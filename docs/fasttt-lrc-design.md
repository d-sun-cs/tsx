# FastTT-LRC：实现说明

代码主体是 `ErasureCodeTsxLrc.h/.cc`。当前 `mode=scaffold` 用ISA全局码贯通流程、用XOR实现局部校验；真正FastTT全局内核尚未接入。

## 1. 布局：原数据只切一次

以32KiB输入、8个数据片、每片4KiB为例。map的key是片号，片内偏移才是字节位置。

| 片号 | 本次输入范围 | 原tsx：8+3 | 新tsx_lrc：8+2+2 |
| --- | --- | --- | --- |
| 0 | [0,4) KiB | D0 | D0 |
| 1 | [4,8) KiB | D1 | D1 |
| 2 | [8,12) KiB | D2 | D2 |
| 3 | [12,16) KiB | D3 | D3 |
| 4 | [16,20) KiB | D4 | D4 |
| 5 | [20,24) KiB | D5 | D5 |
| 6 | [24,28) KiB | D6 | D6 |
| 7 | [28,32) KiB | D7 | D7 |
| 8 | 计算得到 | G+ | G0，当前ISA |
| 9 | 计算得到 | P0 | G1，当前ISA |
| 10 | 计算得到 | G− | L0＝D0 XOR D1 XOR D2 XOR D3 |
| 11 | 计算得到 | 无 | L1＝D4 XOR D5 XOR D6 XOR D7 |

数据满足 `Di[t]=in[i*4096+t]`。新配置的 `m=2` 只计全局校验，实际共有4个校验、12片。原tsx的G±还有Redis尾部；新表中的ISA校验没有实现TT尾部。

## 2. 接口：Ceph准备缓冲，插件填写数据

我们与官方LRC一样继承 `ErasureCode::encode/decode/decode_concat`，只实现所需的低层计算与选片扩展，不修改OSD或ECUtil调用逻辑。上层按条带调用，插件收到的未必是整个对象。

| 低层参数 | 进入函数时的内容 |
| --- | --- |
| `encode_chunks(want, encoded*)` 的want | 要交回的片号；默认写入为全部12片 |
| encoded | 工作map：0～7已有数据，8～11已有空间但尚未填写；基类最后按want保留结果 |
| `decode_chunks(want, chunks, decoded*)` 的want | 要求有效的输出片号；前台通常为全部数据0～7，后台重建为本次缺片 |
| chunks | 实际读回的有效片；缺key也可能只是没读，不能用全零片冒充缺片 |
| decoded | 基类工作map：引用已有片，并为其余片分配空间；有条目不代表内容有效 |

公开encode/decode调用的输出map初始为空；进入_chunks前空间已准备好。chunks与decoded是不同map，已读片通常共享底层字节，对齐重建可能产生复制。计算函数返回0／负错误码，实际结果写进bufferlist。

## 3. Layer：我们维护的编码任务表

官方LRC把若干次编码组织为Layer，层的输入可以包含之前生成的校验。其常见高层布局把数据与全局校验一起局部分组；我们仅对数据分组。Layer是插件内部结构，不是Ceph额外传入的参数。

| 本插件的Layer | 输入 → 输出（外层片号） | 子编码器的编号 |
| --- | --- | --- |
| global | 0～7 → 8、9 | 0～9 |
| local0 | 0、1、2、3 → 10 | 0、1、2、3 → 4 |
| local1 | 4、5、6、7 → 11 | 0、1、2、3 → 4 |

`layers_init()`创建任务；`layer.chunks=data+coding`规定重编号顺序。原始数据只拆一次，各层引用同一批缓冲，并非每层再拆一份文件。调用子编码器前重排编号，返回后映回外层。

## 4. 用D2恢复串起选片、计算与输出

```text
目标want={2}，其余片可读
  → minimum_to_decode选择{0,1,3,10}
  → Ceph只读取这四片，形成chunks
  → decode_chunks用同一规划器决定步骤
  → D2=D0 XOR D1 XOR D3 XOR L0
```

local0子编码器看到的是 `want={2}, chunks={0:D0,1:D1,3:D3,4:L0}`，L0在这里是4而非10。外层decoded起初有12片空间；计算后仅0、1、2、3、10有效，内核删除未填条目，交回这5片，符合Ceph允许返回额外有效片的约定。

局部／全局由 `make_repair_plan(want,available)` 判断，Ceph不传策略标志：

| 情况 | 当前骨架的处理 |
| --- | --- |
| 单D2，输入{0,1,3,10} | 局部0只缺1片，局部恢复 |
| D0、D1，输入{2,3,4,5,6,7,8,9} | 局部能力不足，全局层用2校验恢复 |
| D0、L0都要恢复 | 可以先全局恢复D0，再局部生成L0 |

读前_minimum_to_decode只把计划inputs交给上层；读后decode_chunks按实际chunks重新规划并执行steps。上层负责可用性、实际读写和缓冲管理，分组与校验方程由插件解释。

**少读不等于少分配。** 本例每片4KiB，四输入共16KiB；基类仍新分配其余8片的32KiB，工作表主片约48KiB，结束时清理不能消除之前的峰值。完整前台读还需要D4～D7，因此同样缺D2时通常读8片，而非4片。

## 5. 接下来改哪里

| 函数 | 责任 |
| --- | --- |
| `layers_init()` | 层成员、子编码器与编号关系 |
| `encode_chunks()` | 重排缓冲，执行各层编码并映回 |
| `make_repair_plan()` | 生成真实输入集合及有序恢复步骤 |
| `_minimum_to_decode()` | 将计划输入交给Ceph，落实少读 |
| `decode_chunks()` | 只用有效输入执行步骤，维护known并清理未填条目 |
| `init_fasttt_global_layer()` | 待实现真正G+/G−与显式尾部；旧tsx2是G+/P0，不能直接替代 |

未来TT方案以G+/G−替换8、9，保留两局部校验，并按需生成 `P0=L0 XOR L1`；尾部与联合恢复都要另行实现，不能只换子编码器便宣告完成。`mode=fasttt`目前在初始化返回未实现。

Ceph18基类encode忽略encode_chunks返回值的既有限制仍保留。安装、验证记录、当前仓库状态和接续步骤统一见 [开发交接](agent/fasttt-handoff.md)。
