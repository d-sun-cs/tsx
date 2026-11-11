# FastTT / TSX 开发交接

修订 **r25**，2026-11-11，Asia/Shanghai。本文件用于新agent准确继续开发，保存当前状态及必要依据；人读实现说明在 `docs/fasttt-lrc-design.md`。不需要旧聊天才能理解下面的目标、约束和下一步。

## 0. 当前快照

| 项目 | 当前事实 |
| --- | --- |
| 研究主线 | M1：FastTT局部冗余；M2：多线程模式。两者都保留，先解决TT内核／尾部基础 |
| 当前阶段 | 原论文实验流程已打通；正在开发独立 `tsx_lrc`。当前只做纠删码插件／内存测试，不构建完整Ceph运行环境或启动虚拟集群 |
| Ceph基线 | 官方 `v18.2.7@6b0e988052ec84cf2d4a54ff9bbbc5e720b621ad`，detached；父仓库4处接入修改由根单补丁重建 |
| TSX仓库 | `https://github.com/d-sun-cs/tsx.git`，分支 `research/ceph-v18.2.7`；嵌在Ceph的 `src/erasure-code/tsx`，是独立Git仓库，不是submodule |
| 已提交／最后核对发布版本 | `da46287a59f2e56c7c1f88bfed994a575a8012b5`；原tsx/2/4算法仍基于普通AVX2发布基线 `v0.0.1-ipdps@5c85ce5e629905fcf8e660c8490f6b66a4160c69` |
| 尚未提交／推送 | r22的 `ErasureCodeTsxLrc.h/.cc`、`TestErasureCodeTsxLrc.cc`；当前 `AGENTS.md`、设计文档、本文。合计6文件；只拉远端尚不能取得这些改动 |
| M1真实状态 | 固定8+2+2骨架已编译／测试；全局为ISA占位、局部为真实XOR。`mode=fasttt` 返回未实现；TT全局、尾部与完整联合恢复未完成 |
| M2真实状态 | 有输出拥有权／线程预算设计，无实际线程池或多线程内核。链接OpenMP不代表已并行 |
| 最近代码验证 | r22：532次恢复＋64次预期规划拒绝通过，并验证继承的包装、decode_concat和全部返回片有效；原插件6522组合仅沿用r16结果 |
| 本次文档工作 | r25区分两类文档、压缩重复历史并同步导航；算法／测试源码未改，未重跑构建或实验 |
| 运行中任务 | 本轮没有启动构建、实验或服务；接手时仍应核验实际进程，不能由旧日志推断 |

先检查两个仓库的身份与差异，再读第1节；源码行为以当前代码为准，旧build和历史通过不能覆盖当前工作树事实。

## 1. 开发进度与下一步

| 工作 | 已完成 | 尚未完成 |
| --- | --- | --- |
| 环境／原论文基线 | 回归Ceph18与AVX2；插件安装、原纠删码验证和论文附录各阶段流程打通；现已精简为直接shell入口 | 论文硬件上的公平性能比例、多轮正式统计不能继承 |
| M1基础 | 注册、固定布局、Layer映射、共享规划、局部／全局占位恢复、有效输出整理及独立测试 | 真正G+/G−、显式尾部、TT与局部联合恢复、全部三擦除验收 |
| M2 | 明确输出单写者、缓冲生命周期、线程预算和比较方法 | 任务切分、池／调度、并发正确性和性能验证 |
| M3系统验证 | 仅有旧插件的历史BlueStore流程记录 | 新M1/M2实际读片、持久重建、尾部缺失／重启可靠性；当前后置 |

**下一项具体工作：先定义TT主片／尾部契约，再实现 `init_fasttt_global_layer()`。**

1. 核对第6节r22源码哈希和本地diff，确认仍继承基类encode/decode；保留scaffold作为对照。
2. 定义一条带8个B字节数据、G±各B+224字节、L0/L1各B字节的显式内核输入／输出；说明补齐、有效长度、只读输入、scratch、错误返回，不借Redis内容键隐式取得尾部。
3. 先做标量真值／顺序内核，再接真实全局子编码器；未完成前继续返回 `-EOPNOTSUPP`，不能假成功。旧tsx2是G+/P0，不能当作G+/G−。
4. 扩展共享RepairPlan及执行，支持虚拟P0或直接联合方程；严格只给选中输入，逐字节验数据、校验和尾部，再优化／并行。

M1完成至少要求12片所有1／2／3擦除共298集合、多长度／数据模式、合法选片与失败路径通过；ISA占位的532成功不是TT验收。系统可用性另按M3记录，不能用内存测试代替。

M2实施顺序：先新内核一线程真值，再编码与单擦除并行。按**输出地址范围**划分任务，包含尾部／初始化，输入共享只读，任务返回前全部结束。旧输入窗口16个32B单元、移位最大7单元会产生7单元输出重叠，不能直接并行旧window循环；一般消元也有前后依赖。

线程预算包含参与计算的调用线程，用共享预算／固定池执行限额；单进程限制不等于全机OSD限额。错误／取消也须收尾所有任务后才能释放缓冲，避免嵌套等待死锁；Redis连接等副作用另核线程安全。比较旧一线程、新一线程、新多线程，并在同核心预算下测并发、吞吐与尾延迟；复杂多擦除并行后置。

增量更新、码转换、THEIA预取保留为候选，不作为当前前置；没有已验收实现。不得自行删掉M1或M2，也不沿用未重新确认的旧日程。

## 2. 仓库、环境与持久约束

以下路径默认相对TSX根；`src/osd/...`、`src/erasure-code/ErasureCode.cc`等显式Ceph路径相对父Ceph根。进入TSX后可识别本机目录：

```bash
TSX_ROOT=$(git rev-parse --show-toplevel)
CEPH_ROOT=$(cd "$TSX_ROOT/../../.." && pwd)
git status --short --branch
git -C "$CEPH_ROOT" status --short --branch
git rev-parse HEAD
git -C "$CEPH_ROOT" rev-parse HEAD
```

父仓库只有下面4个tracked修改；根 `ceph-v18.2.7.patch` 已验证可从官方原文件逐字节重建：

- `src/erasure-code/CMakeLists.txt`：启用TSX接入。
- `src/test/erasure-code/ceph_erasure_code_benchmark.cc/.h`：constant/random输入、seed和首差异诊断。
- `cmake/modules/BuildBoost.cmake`：官方Boost下载地址；仍锁Boost1.79.0及其原SHA。

不要把父仓库dirty源码当隐藏依赖；有新父修改必须同步可重建补丁。Ceph基类、OSD、ECUtil和官方LRC源码均未为当前LRC改动。当前18只用set/map接口，不引入历史Ceph20的bufferptr新重载或ECBackendL路径。

已使用的构建环境是 `fasttt_reef`：Ubuntu22.04、GCC11.4、CMake3.22.1，挂载当前Ceph为 `/ceph`，网络隔离；宿主缺工具时在该容器构建。运行前重新核验容器／挂载／缓存。原TSX需要回环Redis6379；当前ISA骨架check_lrc不用Redis。本机历史依赖为Redis8.4.0／hiredis1.3.0，Ubuntu包可能不同，论文未固定它们。

持续保留的约束：

- 只用普通AVX2主线。旧TSX AVX512 stash `032f48c806f57ab39b89a12080f2ac4f203dd27d`、父旧stash `a52b8f6b4c01efe565d429957ac74c68140c3124` 不自动应用；stash不随clone／普通push迁移。
- 旧约51GiB build在Ceph根 `local_workspace/build-archives/ceph-v20.2.0-avx512-20261101`；不用其缓存／二进制，不删除。论文、结果、备份都留在 `local_workspace/`，不进入源码构建或推送。
- 旧环境备份位于 `local_workspace/environment-backups/2026-11-01-pre-reef/`；精简前备份在 `2026-11-07-ec-simplification/`。备份用于保护原工作，不是新环境必须取得的依赖。
- 不重启其他工作区容器、改旧Redis或清理历史进程；过去容器里有已退出的Z条目，不能当活跃集群。
- 个人GitHub身份为d-sun-cs，认证仅限TSX目标仓库；不把令牌写入Git、URL或日志。Ceph根 `local_workspace/tsx-fine-grained-token.txt` 当前为空；需要时用户重新填写。所有推送成功并核对SHA后只清空内容为0字节，**保留原文件名，不删除**。临时helper仍清理。
- 文档维护不自动提交／推送；本轮6文件未提交。跨工作区必须携带完整dirty差异或后续对应提交，不能只给移动分支名。

## 3. 安装与测试：唯一操作说明

新工作区先取得固定Ceph和研究分支；下面只能取得已发布版本，当前未提交改动还需单独携带：

```bash
git clone --branch v18.2.7 --depth 1 --recurse-submodules https://github.com/ceph/ceph.git
cd ceph
git clone --branch research/ceph-v18.2.7 https://github.com/d-sun-cs/tsx.git src/erasure-code/tsx
cd src/erasure-code/tsx
```

使用支持AVX2的Ubuntu22.04／GCC11环境。已有环境可从宿主 `docker exec -it fasttt_reef bash`，容器内进入 `/ceph/src/erasure-code/tsx`。需新容器时，宿主 `bash setup_container.sh` 只挂载当前Ceph并打开Ubuntu22.04 shell；退出后用 `docker start -ai fasttt_ec` 进入，不要求私人镜像或集群初始化。

新容器root安装依赖；已有依赖跳过：

```bash
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  ca-certificates git build-essential gcc-11 g++-11 cmake ninja-build \
  autoconf automake libtool pkg-config nasm curl python3 python3-dev \
  python3-setuptools python3-pip cython3 python3-yaml python3-prettytable python3-sphinx \
  libssl-dev libcurl4-openssl-dev libsnappy-dev zlib1g-dev liblz4-dev libzstd-dev \
  libaio-dev libudev-dev libblkid-dev libkeyutils-dev xfslibs-dev \
  libgoogle-perftools-dev libexpat1-dev libcap-ng-dev libatomic-ops-dev \
  libsqlite3-dev uuid-dev libxml2-dev valgrind libhiredis-dev redis-server
```

挂载ownership导致Git拒绝时，仅在专用实验容器可执行 `git config --global --add safe.directory '*'`；不用旧初始化／依赖脚本，不添加Redis第三方源或改已有Redis持久化。

先 `redis-cli ping`，要求PONG；只有独立实验环境没有Redis时才启动：

```bash
redis-server --bind 127.0.0.1 --port 6379 --save '' --appendonly no --daemonize yes
```

从TSX根运行：

```bash
bash recmake.sh 8
bash check_correctivity.sh
bash check_lrc.sh
bash coding_benchmark_cases.sh 8 3 2048 2000
```

`recmake.sh`检查Ceph SHA、子模块、缓存归属／实验flags，幂等应用单补丁，冲突退出且不删build。显式构建benchmark、8个原TSX族、tsx_lrc及其测试、ISA与Jerasure，关闭BlueStore等系统功能；只安装tsx／isa／jerasure子目录，不构建all／vstart／MON／OSD，不全量安装。Boost1.79.0归档SHA256：`475d589d51a7f8b3ba2ba4eda022b170e562ca3b760ee922c146b6c65856ef39`，首次构建需要下载。

默认产物：Ceph根 `build/bin/ceph_erasure_code_benchmark`、`build/bin/ceph_test_erasure_code_tsx_lrc`；安装插件 `/usr/local/lib/ceph/erasure-code`。安装系统目录需权限，容器用root；也可统一设置绝对 `INSTALL_PREFIX="$CEPH_ROOT/local_workspace/ec-install"`。`BUILD_DIR`可改绝对构建目录，测试可用 `PLUGIN_DIR`；旧基准脚本还支持 `CPU`／`RESULT_DIR`，默认允许的最小CPU、OMP_NUM_THREADS=1及Ceph根 `local_workspace/ec-tests/` 独立结果目录。

**正确性。** `check_correctivity.sh`无参数跑(6,2,tsx2)/(6,3,tsx)/(8,3,tsx)/(10,4,tsx4)，各对照ISA cauchy、Jerasure cauchy_good，8/3加naive/map/win/merge；2MiB片、mt19937低8位、seed1，1..m穷举并逐字节比较，48命令／6522组合。单组入口 `bash check_correctivity.sh 6 2 64 random`。它不含tsxse；tsx_lrc另用 `check_lrc.sh`，旧benchmark按k+m枚举，不能直接用于额外局部片布局。

**性能。** `coding_benchmark_cases.sh k m 每片KiB 迭代数`；四配置全矩阵163项，编码及各种数据／校验擦除数量组合；默认constant X、固定各类前几个片、跨实现同位置，每项预热2次再计时。`results.csv`存原始时间、KiB、GB/s和GiB/s，`summary.csv`按擦除总数对故障类型吞吐取算术平均；保留命令与日志，失败即停。计时不替代字节正确性检查。全跑：

```bash
for pair in '6 2' '6 3' '8 3' '10 4'; do
  read -r k m <<< "$pair"
  bash coding_benchmark_cases.sh "$k" "$m" 2048 2000
done
```

原 `repro/`、Python runner和 `run_end_to_end_tests.sh` 已删除，不恢复多层封装。all_test／vstart／general_test／stop等为旧集群脚本，当前不用；update_cmake会覆盖父CMake、update_docker_build会改父容器工具，也不用。当前Xeon8358与论文i9-14900K不同，ISA-L还会运行时选SIMD，单轮数值不能宣称论文性能比例复现；公开空镜像全新依赖构建未验证。

## 4. 当前代码与必须保持的契约

| 代码／符号 | 当前职责 |
| --- | --- |
| `ErasureCodePluginTsxLrc.h/.cc` | 注册tsx_lrc；factory创建、init，失败不发布对象 |
| `ErasureCodeTsxLrc.h/.cc` | 固定Layer、ISA子插件、共享规划、两个_chunks接口、known输出整理；没有encode/decode覆写 |
| `TestErasureCodeTsxLrc.cc` | registry加载已安装.so，严格只给计划输入，检查全部返回片字节／输入不变；不直接链接具体类绕过注册 |
| `CMakeLists.txt`、`recmake.sh`、`check_lrc.sh` | ec_tsx_lrc和独立测试的构建／安装／执行；不依赖ec_lrc |
| 原 `ErasureCodeTsx{,2,4}.cc`及Plugin | 保持普通AVX2基线，三种校验数；naive/map/win/merge为消融，tsxse非当前基准必需项 |
| 父 `ErasureCode.cc`、`ECUtil.cc`、`ECBackend.cc/.h` | 缓冲准备、条带循环、选片到实际读请求及结果持久化；当前未为新插件改动 |

当前合法模式 `plugin=tsx_lrc mode=scaffold k=8 m=2 locality=4`，后三项可省略后使用这些默认值；拒绝其他值及l、mapping、layers、自定义CRUSH组。缺mode返回-EINVAL；`mode=fasttt`进入 `init_fasttt_global_layer()` 返回-EOPNOTSUPP(-95)。未完成内核不得返回0或拿ISA占位测FastTT性能。

布局identity：0..7为D0..D7，8/9为ISA cauchy全局G0/G1，10/11为L0/L1。m只计2全局，总片12、数据8、全部校验4。Layer存data/coding/chunks/erasure_code，chunks=data接coding：global为[0..7,8,9]，local0为[0,1,2,3,10]，local1为[4,5,6,7,11]；局部子插件是ISA reed_sol_van的m=1，实际XOR。子调用重编号为本层连续ID，local0的物理10在子层是4。

官方LRC借助通用纠删码接口及自己的Layer组织计算；常见高层布局局部组包含数据和全局校验。本插件仅对数据分组，采用固定C++表，不复制其通用JSON配置器。Layer是本插件内部代码，不由OSD自动生成；Ceph不传local/global指令，分组方程由插件解释。

**参数和缓冲。** 一次原始输入按条带交给encode；数据 `Di[t]=in[i*B+t]`。默认编码want为全部片；encoded进入低层时数据已有值、所有校验已有空间且不保证为零。公开输出map初始空；实际编码输入非空，解码输入同条带／等长／非空。k/m在init确定，不是逐次参数。

解码want是有效输出目标，可含数据或校验；chunks仅有本次实际提供的有效片。基类_decode复制已读bufferlist引用、必要时对齐，并为所有未读ID分配工作空间；两个map不同，已读payload通常共享，改写共享字节可影响输入。预分配条目的存在／长度不代表有效，缺片用缺key表达，不能塞零片。

**选片与执行。** `_minimum_to_decode(want,have)` 和 `decode_chunks(want,chunks,decoded)` 都调 `make_repair_plan()`，请求局部变量不缓存上次策略。计划含inputs和有序steps(layer/helpers/targets)：先局部、再全局、可重复解锁下一层；按当前ISA层缺片数≤校验数形成候选，从want反向裁掉无关依赖；不保证全局最少I/O，也不保证任意三擦除可解。

读前have仅为候选可读状态，返回的inputs经基类转为need（当前单subchunk整片范围）；ECBackend将片号映射到pg_shard/OSD，实际只读这些片。上层不接收steps；读后内核按实际chunks重新规划。后台禁止冗余读；前台若显式启用fast_read可扩大need。上层负责故障／I/O／持久化，不负责本插件分组判断。

例：want={2}、输入{0,1,3,10}，执行局部D2恢复；want={0,1}、输入{2,3,4,5,6,7,8,9}走全局；D0和L0可先全局后局部。每成功一步才把targets加入known；结束仅删除未知输出，保留输入和恢复片；失败清空输出。D2例返回{0,1,2,3,10}，不要求仅want，但所有额外片必须有效。

**上层与空间边界。** 继承基类encode/decode/_decode/decode_concat，不为新插件改变它们。全部目标已读时基类直接返回，不进decode_chunks。decode_concat直接走_decode后只拼k个数据，故内核契约也须支持该路径。Ceph18前台目标为全部k数据；只缺D2时仍需输入{0,1,3,4,5,6,7,10}，不能把单目标4helper宣称为完整前台读取量。

每片4KiB、单D2例读取16KiB，但基类仍新分配其余8片32KiB，工作表主片约48KiB（忽略对齐／临时空间）；内核事后删除7个未填条目只降低后续保留量。少读不等于分配峰值下降。官方LRC也有局部目标完成后留下未填条目的现象，我们在低层清理，无须额外恢复其他组。

**编码错误边界。** Ceph18基类encode忽略encode_chunks返回值，当前与官方LRC一样继承此限制；不再宣称修复公开编码错误传播。配置／未实现模式在init拒绝；未来TT若需运行时错误传播，应另定通用修复，不能悄悄恢复包装覆写。基类直返路径也不会执行低层所有输入检查。

## 5. 真正TT目标与主要技术缺口

目标仍为8数据＋2移位全局＋2数据组局部，不单独存P0。以32B SIMD单元为t，越界输入为零，非循环移位：

```text
L0[t] = D0[t] XOR D1[t] XOR D2[t] XOR D3[t]
L1[t] = D4[t] XOR D5[t] XOR D6[t] XOR D7[t]
G+[t] = XOR(j=0..7) D_j[t-j]
G-[t] = XOR(j=0..7) D_j[t-(7-j)]
P0[t] = L0[t] XOR L1[t]
```

规范化B=32N字节时，G±各需B+224字节，L0/L1各B；非整长度和空输入必须先约定补齐。未来物理8/9用于G±，目前仍是ISA校验，尚未发布TT存储格式。旧三校验视图是[数据,G+,P0,G−]，旧G−编号10，新候选编号9，复用时须显式映射。

三擦除保证是**待代码验收的条件性论证**：若旧G+/P0/G−基码可恢复任意三擦除且尾部完整，两L都在时合成P0；任一L缺时把P0也视作缺失，总共至多三擦除仍可投影到旧基码，恢复数据再生成L。不能把缺P0置零，不能宣称任意四擦除或12片任取8片。反例D0/D1/G+/G−同时丢失，只剩一个组内XOR方程。

联合恢复候选：D0/D1丢失，读D2..D7、L0、G+共8片，得 `S[t]=D0[t] XOR D1[t]`、`R[t]=D0[t] XOR D1[t-1]`，按 `D0[t]=R[t] XOR D1[t-1]`、`D1[t]=S[t] XOR D0[t]` 顺序恢复，D1[-1]=0且须验尾约束。其他数据对需重新推导；“两L拼P0再解”不是唯一最优路径。

**首要阻碍是尾部／等长片。** ECUtil按stripe_width/k定义主片并断言各片等长，简单把get_chunk_size加224也会改变原数据边界，并不能消除新移位尾部。先显式内核，再单独设计Ceph等长序列化／I/O偏移及对象－条带－版本身份；有效尾部缺失不能当零，需错误或重新规划。新格式接入后须做正常写读、降级读、持久重建和重启／尾部缺失验证；普通降级读本身不等于写回修复。

**旧TSX复用前必须处理的事实。** 原8+3主片编号8=G+、9=P0、10=G−；k=8时G±尾部各224B，另分配448B，经同步Redis SET／GET保存，key为P0首32B内容而非对象／条带／版本。一般解码才GET，单擦除快路径可能提前返回；旧tsx2／tsx4的校验与尾容量不同，不能套448B。

- 内容键有碰撞／覆盖风险，独立Redis无持久化，PG状态不会自动感知尾部键丢失；新方案又不持久存P0，不能原样保留旧查键耦合。
- 静态发现reply类型／长度／缺键检查不足，rctx初始化和连接空指针路径有问题，部分尾缓冲／PadChunkPtr释放缺失；尚非本轮实际故障复现，修改时验证。
- 主编码固定32B单元、16单元窗口，无完整remainder；k/m动态值与固定数组不统一，奇数k及任意长度不保证支持。当前已验仅指定偶数k／对齐长度。
- 旧优化CFWP／NMA／EPM／TPS关系到窗口覆盖、寄存器复用、擦除映射和单擦除快路径；一般解码代入领先消元一窗口。提取内核须保留这些依赖，不能只重命名旧插件或直接并行窗口。

## 6. 验证结果与版本边界

**最近有效代码版本：da46287＋r22三个源码dirty修改。** r23～r25只维护文档／导航，源码哈希如下，可据此核对旧通过是否适用：

```text
ErasureCodeTsxLrc.h 47426b8295c30db0394fa2e5bacc858b1b991f81d2c94d66d4efc4b8b17866d8
ErasureCodeTsxLrc.cc 82c269caa64e20f1d883c0bd2e0baf144900b2a3de3490f4807932a9da7ee1cf
TestErasureCodeTsxLrc.cc 6026f52008a6d33b4df8d79057df5ffc8c00e357b80e7380c50fa77ab809f989
libec_tsx_lrc.so bb12a2b5a06bdaa02a31537cdf36410192a71fe1d0e06f6ee7b12ae7c29e0acb
ceph_test_erasure_code_tsx_lrc b9b41bf6badaf9239ed50ece076da4400cfc5086916f2c98115f6224a0bc2f03
```

2026-11-11/r22在fasttt_reef中增量构建ec_tsx_lrc和测试（5步）、执行tsx子目录安装，从/tmp运行check_lrc，均退出0。两长度4096／65536B，随机mt19937 seed=17+片长，每长度穷举298集合：266恢复、32明确拒绝，共532成功＋64预期拒绝。拒绝分别为同组3数据8种、同组2数据＋1全局24种；是逐层规划限制，不证明数学不可解。单／双擦除78种每长度全部恢复。

测试只传minimum输入，逐字节检查目标和**所有额外返回片**、输入保持不变；覆盖4helper、无关组故障、全局后备、全局→局部、decode_concat全数据恢复、仅编码校验子集、坏ID／内核长度错误／未准备空间、失败清理及四擦除反例。输出包含 `532 repairs + 64 expected planner rejections` 与 `valid-output pruning, inherited wrappers/decode_concat and kernel error paths`。公开空输入／非空out不作为受支持调用。

同日官方LRC对照：k=4/m=2/l=3、4KiB片，want={0}，minimum={1,2,3}；公开decode仍返回0..7，直接低层哨兵实验确认4..7未被写入。由此撤回初版encode/decode包装，在低层清理有效输出；不是修改Ceph上层的理由。

日志／旧.so／临时探针位于Ceph根 `local_workspace/ec-tests/lrc-default-wrappers-20261111-90phCRas/`：build.log、install.log、test.log、official-probe.log、benchmark.csv。这里只是本地证据，不形成新runner；理解结果无需取得该目录。

r22另做同核包装短计时：CPU0、OMP_NUM_THREADS=1、constant X、预热100、每项5000次、before/after交替3轮，包含每次空结果map的分配释放；局部解码固定4helper。每调用耗时中位数，单位微秒：

| 每片 | 操作 | 修改前 → 修改后 |
| --- | --- | --- |
| 4KiB | encode | 12.3296 → 7.1346 |
| 4KiB | local decode | 6.5065 → 6.3528 |
| 64KiB | encode | 70.6486 → 72.6352 |
| 64KiB | local decode | 15.6886 → 12.8290 |

64KiB编码约+2.8%差异未归因；此为烟测，不承诺零回归／算法提速。代码没有增加整片计算／复制或helper数量；正式性能另做独占多轮统计。

保留的阶段证据，不能合并计数或当作新代码已通过：

| 阶段／版本 | 结果及边界 | Ceph根下的主要证据 |
| --- | --- | --- |
| 2026-11-08/r16原插件回归 | 48命令／6522组合，2MiB随机seed1，四配置＋对照／消融；r22未重跑原算法 | `local_workspace/ec-tests/post-lrc-baseline-20261108.log`、`correctness-gynpzfsC/` |
| 2026-11-07/r15脚本精简 | EC构建安装成功；6522正确性及四配置163项各10次计时通过；非2000次正式性能 | `local_workspace/ec-tests/build-20261107.log`、`correctness-20261107.log`、`coding-20261107.log` |
| 2026-11-02/r14论文附录 | 163项各2000次；安装后6522组合；独立Cerasure24项成功；旧插件BlueStore12池36次PUT/GET＋12次单OSD降级GET，SHA全一致 | `local_workspace/paper-reproduction/appendix-report-20261102.md`及同目录原始结果 |
| 2026-11-01/r8环境回归 | 官方18＋AVX2新构建，4配置×2长度(64KiB/2MiB)×constant/random，48命令／7464组合；不同矩阵，不与6522累加 | `local_workspace/reef-*.log`与环境备份 |

附录完成后提交 `8ad62fe0fbdc1f84f07ea662f9b0ccff544753fb` 已随研究分支发布，可查旧工具历史但不恢复当前入口。Cerasure为独立 `d-sun-cs/cerasure` fasttt分支 `c9f305294cb8e52af6f5962e3c83cc607a85ba07`，GF-Complete来自Ceph固定子模块；独立框架与Ceph插件分开统计。旧系统测试是文件承载BlueStore，正式对照reed_sol_van，正常停止；不等于新LRC、替换重建或Redis丢失／重启可靠性通过。

## 7. 后续维护这两份文档

**人读设计**只保留当前实现、关键函数／参数和少量贯穿例子；不放实验流水、凭据过程或仓库进度。**本交接**保存可继续开发的目标／阶段、版本与dirty状态、运行入口、必要技术结论、已验证边界、未决问题和下一项任务。核心事实直接写清，源码位置／日志是定位与补充，不用旧聊天或另一报告替代必要上下文。

更新按“当前事实优先”进行：修改相应状态和结论，合并重复内容；普通问答不逐轮建章节。历史只保留仍影响实现的决策原因、失败教训和验证边界，已撤回的包装／旧Ceph教程不与现状并列。不要为显得详细重复同一结论，也不要为压字数删掉恢复工作所需信息。周报等单次总结不写进核心文档。

发生代码进展、重要结论、目标／范围／环境／分支变化或交接时，联动第0节、相关技术结论、第6节证据与第1节下一步；区分“计划／代码存在／编译／正确性／性能／系统验收”。仅聊天解释且无新事实时，不必新增历史记录或实验。版本／代码变化后再运行适当检查，文档编辑不机械重跑构建。

本文由主agent单写。写前以排他创建取得 `docs/agent/.fasttt-handoff.lock`，记录维护者／起始时间／所基修订；持锁重读最新正文再合并。已有锁先核验实际持有者，不能仅因超时删除；其他agent只交发现。保存到同目录唯一临时文件，校验UTF-8／围栏／相对入口／当前状态，原子替换后释放锁。锁／临时文件不入Git；修订变化则重读合并，不覆盖。本文保存成功才可声称已同步。

收尾给出真实的改动、验证和未完成项；若有运行任务，写清观察／停止方法。提交／推送依当次授权，成功后核对远端SHA；不能把未提交文档当已发布。README和AGENTS仅导航，不维护另一套台账。

关键演进只保留影响当前开发的节点：

- 2026-10-29～11-01：停用AVX512实验，按论文统一Ceph18和普通AVX2，保护旧资料并验证新基线。
- 2026-11-07～08：删除repro多层编排，保留直接shell；建立固定tsx_lrc骨架与独立测试，真正TT仍留空。
- 2026-11-11/r21～22：研究分支发布到da46287；本地随后移除包装覆写，继承基类并由低层整理有效输出，验证结果见第6节。
- 2026-11-11/r25：按用户要求区分文档受众，合并r23/r24的有效结论并压缩重复历史；本轮仅文档／导航整理，源码哈希不变。
