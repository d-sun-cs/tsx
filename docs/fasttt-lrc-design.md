# FastTT-LRC：当前代码实现要点

实现主体是 `ErasureCodeTsxLrc.h/.cc`。当前 `mode=scaffold` 的全局层用ISA占位，局部层实际执行XOR；真正TT内核和尾部尚未接入。下面只讲实际参数、缓冲和调用关系。

## 先统一片号和片内索引

以一次 **32KiB编码单元、k=8、每片B=4096字节** 为例。表内片号是map的key，不是OSD编号；两个插件采用不同的校验编号，不能混用。

原tsx的k=8,m=3对应11片；新tsx_lrc的k=8,m=2,locality=4对应12片，m只计全局校验，另有两个局部校验。

| map片号i | 原tsx默认8+3 | 新tsx_lrc的8+2+2 | 数据片来自本次in的范围 |
| --- | --- | --- | --- |
| 0 | D0 | D0 | [0,4) KiB |
| 1 | D1 | D1 | [4,8) KiB |
| 2 | D2 | D2 | [8,12) KiB |
| 3 | D3 | D3 | [12,16) KiB |
| 4 | D4 | D4 | [16,20) KiB |
| 5 | D5 | D5 | [20,24) KiB |
| 6 | D6 | D6 | [24,28) KiB |
| 7 | D7 | D7 | [28,32) KiB |
| 8 | G+主片 | G0主片，当前ISA | 校验由数据计算 |
| 9 | P0主片，无移位 | G1主片，当前ISA | 校验由数据计算 |
| 10 | G−主片 | L0＝XOR(D0…D3) | 校验由数据计算 |
| 11 | 无此片 | L1＝XOR(D4…D7) | 校验由数据计算 |

`encoded[i].c_str()[t]` 中，i是片号，t是片内字节偏移；对数据片有 `Di[t]=in[i*B+t]`，`0≤t<B`。校验字节是方程结果，不是原始in中的另一段。原tsx的G+/G−另有Redis尾部，不包含在表内B字节主片中。

## 编码：上层先准备缓冲，再调用我们的计算

```text
上层的in（本次32KiB逻辑数据）和want_to_encode
  → encode() → encode_prepare()
  → encode_chunks(want_to_encode, &work)
  → encode()把结果交回上层
```

| encode_chunks参数 | 进入函数时是什么 | 我们负责什么 |
| --- | --- | --- |
| `const set<int>& want_to_encode` | 希望交回上层的片号集合，例如{0…11}或只要校验{8,9,10,11}；没有数据字节 | 按目标计算所需结果；当前骨架会执行全部三层，外层再筛选输出 |
| `map<int,bufferlist>* encoded` | 输入输出共用的工作表；0…7已有D0…D7，8…11已分配B字节空间但内容尚无效 | 保留数据，填好G0、G1、L0、L1；不能把新分配空间当作全零 |

`encode_prepare()` 根据片数与 `get_chunk_size(in.length())` 切分／补零，并准备连续对齐的缓冲。不是让encode_chunks再切一次in，也不是由want决定工作表只放几个条目。所有条目准备好后，本例内部表有12片；当前LRC的外层encode最终只发布want中的结果。

调用外层 `encode(want,in,&out)` 时out为空；进入encode_chunks时，其encoded参数已经指向准备好的工作表。直接调用低层encode_chunks就要自行满足这个准备条件。

两个计算函数都返回 **0表示成功、负errno表示失败**；实际输出写在缓冲中。当前LRC覆写encode以传播内核错误，避免Ceph18基类忽略encode_chunks返回值。

## 解码：目标、已读输入、工作输出是三件事

```cpp
int decode_chunks(const set<int>& want_to_read,
                  const map<int,bufferlist>& chunks,
                  map<int,bufferlist>* decoded);
```

| 参数 | 准确含义 |
| --- | --- |
| `want_to_read` | 本次必须得到有效内容的目标片号；例如{2}要D2，{10}要L0。不是输入helper集合，也不必都是丢失片 |
| `chunks` | 本次真正提供的有效输入，key仍是物理片号；缺少key表示本次不可用，可能丢失，也可能只是没读 |
| `decoded` | 基类准备的输入／输出工作表：已提供片引用其数据，其余片已分配恢复空间；我们的计算填写需要恢复的内容 |

缺片用“不提供该key”表示，不能塞一个全零片代替；全零也可能是真实的有效数据。

调用外层decode时，传入的out为空；当前LRC包装在内部建立work，并把 `&work` 作为底层decoded传入，它不是外层out这个map。

**用D2丢失、只恢复D2代入：** 规划器选择D0、D1、D3、L0，调用方只提供这四片：

```text
want_to_read = {2}
chunks = {0:D0, 1:D1, 3:D3, 10:L0}

进入decode_chunks时，decoded工作表为：
  0:D0     1:D1     2:未填空间  3:D3
  4:未填空间 5:未填空间 6:未填空间 7:未填空间
  8:未填空间 9:未填空间 10:L0   11:未填空间
```

每个条目长度都是B；“未填空间”没有有效数据，不能读来参与计算。D4等片可能健康，只是这次没有读取。判断可用输入要看chunks和已完成的恢复步骤，不能看decoded是否存在某个key。

计算 `decoded[2]=D0 XOR D1 XOR D3 XOR L0` 后，工作表中的0、1、2、3、10有效；本例其他空间仍未计算。当前LRC外层decode从私有工作表中只取目标，交回上层 `out={2:D2}`，不会发布那些未填空间。通用Ceph接口允许额外交回有效片，但不能把未完成片称为有效输出。目标本来就在chunks里时可直接引用；所有目标都已提供时，基类不会再调用decode_chunks。

## 调到子编码器时，索引还要再换一次

`Layer`是我们维护的内部任务表，不是Ceph强制接口。`layers_init()`创建三层；`chunks=data+coding`规定物理片号到本层连续编号的映射：

| Layer | 外层物理片号顺序 | 子编码器看到的编号 |
| --- | --- | --- |
| 全局8+2 | [0,1,2,3,4,5,6,7,8,9] | [0,1,2,3,4,5,6,7,8,9] |
| 局部0：4+1 | [0,1,2,3,10] | [0,1,2,3,4] |
| 局部1：4+1 | [4,5,6,7,11] | [0,1,2,3,4] |

上面的D2恢复在局部0子编码器里变成：

```text
layer_want    = {2}
layer_chunks  = {0:D0, 1:D1, 3:D3, 4:L0}
layer_decoded = {0:D0, 1:D1, 2:未填空间, 3:D3, 4:L0}
```

这里L0的key是4，不是10；子编码器写回本层2后，我们再映回物理片2。原非LRC的tsx中，编码局部指针数组chunks[i]对应encoded[i]；解码的data[i]对应decoded[i]，coding[j]对应decoded[k+j]。因此k=8时，coding[1]是物理片9的P0，不能理解成数据片1。

## 上层给什么，又取走什么

系统写入路径中，`ECUtil::encode()`把整理好的逻辑数据按条带循环交给encode；大对象不一定只调用一次。上层按片号拼接各条带的结果，再写对应shard。索引i代表该条带的片号，不是整个文件的一段固定大小切片。

恢复时，上层先通过 `minimum_to_decode()`取得所需片号，再读取相同条带位置的字节，调用 `decode(want,chunks,&out,chunk_size)`；chunk_size表示单片大小B。当前基类／LRC骨架实际按收到的片长度准备工作缓冲。接口没有对象名、OSD号或磁盘地址参数，k/m也已在实例初始化时确定。

完整数据读还可走 `decode_concat()`：以全部数据片0…7为目标，恢复后按顺序拼接。当前Ceph18前台通常需要全部k片数据，所以前面的{2}是单片恢复／独立测试例子，不代表完整前台读只需要四个输入。

## 写代码时的直接落点

| 函数 | 我们负责的内容 |
| --- | --- |
| `layers_init()` | 固定各层的输入／输出编号，创建子编码器 |
| `encode_chunks()` | 按Layer重排编码工作缓冲、调用子编码器、映回结果 |
| `make_repair_plan()` | 根据want和available确定inputs及有序恢复步骤 |
| `_minimum_to_decode()` | 把同一计划的输入ID返回给调用方，落实局部少读 |
| `decode_chunks()` | 只使用步骤中的helpers恢复targets，避免使用未填空间 |
| `init_fasttt_global_layer()` | 后续接入G+/G−与显式尾部；已有tsx2是G+/P0，不能直接替换 |

TT与局部校验的联合恢复还需扩展共享规划／执行；只替换全局子编码器不会自动补全路径。接入时保留错误传播和有效输出裁剪。检查命令仍为：

```bash
bash recmake.sh 8
bash check_lrc.sh
```

完整接续记录见 [handoff](agent/fasttt-handoff.md)。
