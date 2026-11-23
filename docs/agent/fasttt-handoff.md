# FastTT / TSX 开发交接

修订 **r51**，2026-11-23，Asia/Shanghai。本文件用于新agent准确继续开发，保存当前状态及必要依据；人读实现说明在 `docs/fasttt-lrc-design.md`。不需要旧聊天才能理解下面的目标、约束和下一步。

## 0. 当前快照

| 项目 | 当前事实 |
| --- | --- |
| 研究主线 | M1 FastTT局部冗余，M2多线程。当前优先完成M1的存储布局和单线程基线 |
| Ceph基线 | 官方 `v18.2.7@6b0e988052ec84cf2d4a54ff9bbbc5e720b621ad`，detached。五处接入修改由TSX根单补丁重建 |
| TSX仓库 | `d-sun-cs/tsx`，`research/ceph-v18.2.7`，嵌在 `src/erasure-code/tsx` 的独立Git仓库。origin为 `git@github.com:d-sun-cs/tsx.git` |
| 复现基线 | 与本文同一提交包含r50分窗原地解码和r51复用加载、首写优化。上一发布基线 `bb11837af1547de13cd8124e530b7d918e0071cf` 包含融合编码、可变偶数k和原始符号Redis键，r49测该产物。`19bd227` 保留旧摘要格式 |
| M1 | `mode=fasttt` 在内存工具中运行真实G+/G−及组内XOR，局部优先后联合解码。尾部随片传递。`mode=fasttt-redis` 用Redis存尾部并输出k+4片等长分片。`mode=scaffold` 保留ISA-L对照 |
| 论文实验口径 | 每块2MiB（2048KiB），FastTT编码symbol固定256位（32B）。Ceph性能实验的普通FastTT与FastTT-LRC均采用Redis尾部模式 |
| 验证 | Redis模式在2MiB默认片长通过全部298种一至三片擦除。AVX2及EPM、CFWP、NMA、TPS均已逐步落地并回归，见第6节 |
| 系统边界 | 两种FastTT模式仍限内存工具，守护进程初始化时拒绝。Redis模式等长布局已验证，集群侧的元数据生命周期和恢复尚待验证 |

## 1. 开发进度与渐进计划

1. **内存参考阶段已完成。** 先构建真实移位编码，再接入选片、局部恢复、联合求解和校验重建。内存布局与全部298种一至三擦除作为后续回归基线。
2. **Redis等长元数据阶段已完成内存验证。** 参照普通TSX在Redis存移位尾部，用各全局校验首个原始符号及片号定位。后续FastTT-LRC性能使用Redis模式，inline保留为正确性参考。系统持久化另按需求推进。
3. **单线程优化已完成首轮。** AVX2、EPM、CFWP、NMA、TPS分别构建、回归和计时。当前只比较同布局ISA-L LRC scaffold双方均可恢复的用例。已融合全局与局部校验的编码遍历，扩展偶数k。r47按论文实验口径简化尾部模拟，取消整片散列，重点保留Redis命令开销。r49同布局复测确认编码领先、同组双缺片仍有差距。r50把残差和消元分窗并映射到缺片输出，消除整片残差和求商后的搬运。r51对照普通TSX移植单次加载代入、全局校验首写和四符号直接归约。下一步独立验证第5节的一次递推方案。第6节有对照和回归范围。
4. **M2多线程。** 优先并行编码和单擦除，复杂联合求解后置。按输出范围分配唯一写者，处理尾部与初始化，共享只读输入，返回前收齐任务。编码窗口有k−1个单元的写入重叠，联合递推也有前后依赖。使用共享线程预算并计入调用线程，避免嵌套等待，比较同核心预算的吞吐与尾延迟。
5. **M3系统验证。** 先明确Redis持久化、缺键后的选片重试、跨对象共享键的回收与写入顺序，再验证Ceph正常写读、降级读、持久重建和重启。此阶段当前后置。

普通FastTT与论文第III节的代码对应如下。AVX2的32B向量运算是这四项的实现基础。LRC目标显式启用 `-mavx2`，使用非对齐 load/store 兼容Redis重组和临时缓冲。

| 优化 | 普通FastTT | LRC实现 |
| --- | --- | --- |
| EPM | `ErasureCodeTsx.cc` 中 `decode_chunks` 以 `maperasure_ptr` 将缺数据关联到幸存校验，缺校验映射到自身输出，校验按窗初始化该缓冲后原地代入 | 多个缺片共用方程残差。分子对齐到缺片输出，第一项写入新位置，前窗反馈和后续贡献原地XOR。缺G也直接更新其输出 |
| CFWP | 编解码按16个符号划窗，按需初始化，代入领先消元一窗以覆盖移位 | `write_parities` 直接写入G±首次覆盖的范围，只对重叠区读改写。联合解码按窗推进，保留残差历史、移位前视及递推余项 |
| NMA | 编码相邻数据块成对读取，保留上一个符号并在寄存器合并，再更新校验 | `write_parities<true>` 一次加载相邻一对，更新G±及L0/L1，跨组时分别更新两L。解码代入也一次加载符号并更新所有相关方程及待重建G |
| TPS | 单擦除使用垂直遍历快速路径。多擦除使用窗口代入和消元。`decode_one_data_chunk` 展开4个符号 | `write_local`、`write_global` 和 `recover_single` 的直接归约每次处理4个相邻符号。待同时重建G时改用分窗代入，共用一次幸存数据扫描 |

每阶段在同一插件入口增量构建并运行对应测试后再继续。增量更新、码转换和THEIA预取保留为候选。

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

父仓库只有下面5个tracked修改；根 `ceph-v18.2.7.patch` 已验证可从官方原文件逐字节重建：

- `src/erasure-code/CMakeLists.txt`：启用TSX接入。
- `src/erasure-code/ErasureCode.cc`：基类encode传播encode_chunks失败并清空输出，Redis写失败可返回到正常入口。
- `src/test/erasure-code/ceph_erasure_code_benchmark.cc/.h`：constant/random输入、seed、字节诊断、插件实际总片数及可选minimum／擦除目标／verify。
- `cmake/modules/BuildBoost.cmake`：官方Boost下载地址；仍锁Boost1.79.0及其原SHA。

不要把父仓库dirty源码当隐藏依赖；有新父修改必须同步可重建补丁。基类只补通用编码错误传播，继续使用继承的encode/decode接口。OSD、ECUtil和官方LRC源码保持原样。当前18只用set/map接口，不引入历史Ceph20的bufferptr新重载或ECBackendL路径。

已使用的构建环境是 `fasttt_reef`：Ubuntu22.04、GCC11.4、CMake3.22.1，挂载当前Ceph为 `/ceph`，网络隔离；宿主缺工具时在该容器构建。运行前重新核验容器／挂载／缓存。普通TSX和FastTT Redis模式使用回环Redis，默认6379。inline FastTT和ISA对照无需连接Redis。本机历史依赖为Redis8.4.0／hiredis1.3.0，Ubuntu包可能不同，论文未固定它们。

持续保留的约束：

- 新增执行脚本直接放TSX根目录；不新建runner／配置层。docs、archive、.vscode等非当前执行入口目录保留。只用普通AVX2主线。旧TSX AVX512 stash `032f48c806f57ab39b89a12080f2ac4f203dd27d`、父旧stash `a52b8f6b4c01efe565d429957ac74c68140c3124` 不自动应用；stash不随clone／普通push迁移。
- 旧约51GiB build在Ceph根 `local_workspace/build-archives/ceph-v20.2.0-avx512-20261101`；不用其缓存／二进制，不删除。论文、结果、备份都留在 `local_workspace/`，不进入源码构建或推送。
- 旧环境备份位于 `local_workspace/environment-backups/2026-11-01-pre-reef/`；精简前备份在 `2026-11-07-ec-simplification/`。备份用于保护原工作，不是新环境必须取得的依赖。
- 不重启其他工作区容器、改旧Redis或清理历史进程；过去容器里有已退出的Z条目，不能当活跃集群。
- GitHub使用仅限TSX仓库的Deploy key。2026-11-15 SSH身份、ls-remote和push dry-run已通过。仓库local `core.sshCommand` 指定 `/home/dsun/.ssh/id_ed25519_github_tsx` 并严格校验独立known_hosts。新环境须单独配置凭据。旧令牌文件Ceph根 `local_workspace/tsx-fine-grained-token.txt` 为空且保留，常规同步使用SSH。
- 文档维护不自动提交／推送。固定完整提交并携带需要的本地差异。配置、私钥、stash、ignored资料、容器和二进制不会随clone迁移。

## 3. 安装与测试：唯一操作说明

新工作区取得固定Ceph和包含第0节TSX提交的研究分支。根目录补丁包含本轮完整的Ceph接入差异。已有r33的父工作树若少新基类hunk，先用 `git apply --reverse --check --exclude=src/erasure-code/ErasureCode.cc` 验证旧四处，再用 `git apply --include=src/erasure-code/ErasureCode.cc` 只补新hunk，两条都传TSX根单补丁。

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

在TSX根增量构建并运行插件接口测试。

```bash
bash recmake.sh 8
bash check_lrc.sh
```

FastTT Redis模式和普通TSX对照先用 `redis-cli ping` 检查PONG。仅在独立环境没有Redis时执行下面第一行。

```bash
redis-server --bind 127.0.0.1 --port 6379 --save '' --appendonly no --daemonize yes
bash coding_benchmark_lrc.sh 2048 20
bash coding_benchmark_lrc.sh 2048 20 scaffold
bash check_correctivity.sh
bash coding_benchmark_cases.sh 8 3 2048 2000
```

`recmake.sh`检查Ceph SHA、子模块、缓存归属／实验flags，幂等应用单补丁，冲突退出且不删build。显式构建benchmark、8个原TSX族、tsx_lrc及其测试、ISA与Jerasure，关闭BlueStore等系统功能；只安装tsx／isa／jerasure子目录，不构建all／vstart／MON／OSD，不全量安装。Boost1.79.0归档SHA256：`475d589d51a7f8b3ba2ba4eda022b170e562ca3b760ee922c146b6c65856ef39`，首次构建需要下载。

默认产物：Ceph根 `build/bin/ceph_erasure_code_benchmark`、`build/bin/ceph_test_erasure_code_tsx_lrc`；安装插件 `/usr/local/lib/ceph/erasure-code`。安装系统目录需权限，容器用root；也可统一设置绝对 `INSTALL_PREFIX="$CEPH_ROOT/local_workspace/ec-install"`。`BUILD_DIR`可改绝对构建目录，测试可用 `PLUGIN_DIR`；benchmark脚本支持 `CPU`／`RESULT_DIR`，默认允许的最小CPU、OMP_NUM_THREADS=1及Ceph根 `local_workspace/ec-tests/` 独立结果目录。

**固定论文口径。** 每片2MiB（2048KiB，2,097,152B），FastTT每个编码symbol固定256位（32B，AVX2向量宽度），因此每片主区65,536个symbol。symbol宽度由代码常量决定。普通码字有k+m个片。默认实验配置有8数据＋2全局＋2局部，共12片，profile的m=2只数全局片。每次benchmark输入只包含k个数据片，即 `-s $((k*2048*1024))`。Redis模式12片等长，尾部另存。Inline参考版的G±另含224B片内尾部。脚本默认片长均为2048KiB，其他片长属于开发正确性或冒烟检查。

**影响性能比较的设置。** 记录CPU型号与绑核／线程预算、编译器与SIMD路径、输入模式和seed、擦除集合与修复目标、预热与迭代次数，以及Redis是否计入耗时、连接方式与状态。两组比较要对齐计时范围和吞吐分母，尤其要区分按选片修单片和请求整条带。需要改变既定口径或引入会改变结论的配置时，先告知用户并确认。

**正确性。** `check_correctivity.sh`无参数跑(6,2,tsx2)/(6,3,tsx)/(8,3,tsx)/(10,4,tsx4)，各对照ISA cauchy、Jerasure cauchy_good，8/3加naive/map/win/merge；2MiB片、mt19937低8位、seed1，1..m穷举并逐字节比较，48命令／6522组合。单组入口 `bash check_correctivity.sh 6 2 64 random`。它不含tsxse；tsx_lrc的独立接口测试仍用 `check_lrc.sh`，基于benchmark的流程用下述根脚本。

**LRC内存benchmark。** `bash coding_benchmark_lrc.sh [每片KiB=2048] [每项迭代数=2000] [scaffold|fasttt|fasttt-redis]`。默认`fasttt-redis`，用于后续FastTT性能数据。`scaffold`显式选择ISA对照，片内尾部`fasttt`保留为正确性参考。两种FastTT模式都覆盖全220种三擦除。校验输入数和字节量，保留checks.csv、results.csv和命令日志。Redis模式可用 `REDIS_PORT=16379` 指定实验端口，计时包含Redis建连和命令。只在可丢弃Redis上运行 `LRC_REDIS_TEST_PORT=16379 bash check_lrc.sh` 的元数据故障测试。`stripe_GiB_s` 以完整8数据片计，helper_bytes只统计片，不含Redis流量。

**FastTT的Redis口径。** 普通 `tsx/tsx2/tsx4` 的编码用P0首32B作键写尾部，多擦除路径按需取回。LRC按第5节利用各G的首个原始符号写、读尾部，局部修复无需访问Redis。r36报告的LRC Redis列对应旧摘要实现，当前同布局对照用r49。片内尾部列用于此前诊断。独立Cerasure A2有自己的内存框架，结果单列。

**原插件性能。** `coding_benchmark_cases.sh k m 每片KiB 迭代数`；四配置全矩阵163项，编码及各种数据／校验擦除数量组合；默认constant X、固定各类前几个片、跨实现同位置，每项预热2次再计时。`results.csv`存原始时间、KiB、GB/s和GiB/s，`summary.csv`按擦除总数对故障类型吞吐取算术平均；保留命令与日志，失败即停。计时不替代字节正确性检查。全跑：

```bash
for pair in '6 2' '6 3' '8 3' '10 4'; do
  read -r k m <<< "$pair"
  bash coding_benchmark_cases.sh "$k" "$m" 2048 2000
done
```

原 `repro/`、Python runner和 `run_end_to_end_tests.sh` 已删除，不恢复多层封装。all_test／vstart／general_test／stop等为旧集群脚本，当前不用；update_cmake会覆盖父CMake、update_docker_build会改父容器工具，也不用。当前Xeon8358与论文i9-14900K不同，ISA-L还会运行时选SIMD，单轮数值不能宣称论文性能比例复现；公开空镜像全新依赖构建未验证。

## 4. 当前代码与接口

| 文件或函数 | 职责 |
| --- | --- |
| `ErasureCodePluginTsxLrc.cc` | Registry注册、创建和初始化 |
| `ErasureCodeTsxLrc.cc` | `layers_init` 定义物理片号。`encode_chunks` 写校验。`make_repair_plan` 为minimum和decode共用。`recover_fasttt` 执行移位联合恢复 |
| `TestErasureCodeTsxLrc.cc` | 通过Registry加载.so，覆盖真实校验、只传选中输入、输出字节、错误路径、守护进程模式 |
| `check_lrc.sh`、`coding_benchmark_lrc.sh` | 根目录单机合同测试和内存benchmark入口 |

三种模式使用 `plugin=tsx_lrc m=2`，mode必须显式设置。支持偶数k=4..32，默认8，两个等长局部组，`locality=k/2`（可省略）。`Layout` 推导编号：0～k−1为D，k、k+1为G±，k+2、k+3为L0/L1。总校验数4，默认组仍是{0,1,2,3,10}和{4,5,6,7,11}。scaffold使用ISA Cauchy全局和ISA单校验XOR局部，FastTT自己计算移位和普通XOR。

三种模式继承Ceph18的 `encode/decode/decode_concat`。低层 `encode_chunks` 接收基类准备的全部数据和校验缓冲。成功时公开encode按want筛结果。基类用补丁传播低层错误，失败时清空结果。解码 `chunks` 的key才代表有效输入，`decoded` 的已分配缺片只是工作槽。成功后保留已读片和已恢复片，失败清空输出。已读payload可能共享，计算只写恢复目标。公开decode在目标全部存在时直接返回。

读前 `minimum_to_decode` 只交所需输入集合给ECBackend。读后按实际chunks重新规划。局部D2选{0,1,3,10}，每片4KiB时输入16KiB。基类仍分配其余槽位，不能从少读推出内存峰值减半。全条带数据读取还需D4～D7。普通前台开启fast_read时可能扩大实际读取集合。

## 5. FastTT内存分片和联合解码

FastTT数据长度 `B = 32 * max(1, ceil(原始字节数/(32*k)))`，输入由Ceph基类补零到kB。对32B单元t，越界为0，所有移位为非循环。

```text
L0[t] = XOR(j=0..k/2-1) D_j[t]
L1[t] = XOR(j=k/2..k-1) D_j[t]
G+[t] = XOR(j=0..k-1) D_j[t-j]
G-[t] = XOR(j=0..k-1) D_j[t-(k-1-j)]
```

完整G±各B+32(k−1)字节，D和L均B。`get_chunk_size` 报告B，`decode_concat` 返回补齐后的kB数据。inline模式直接输出完整G±。Redis模式原样输出每个G的前B字节，k+4片均为B。键为 `fasttt:lrc:raw-tail:v1:<片号>:` 加该G首个32B二进制符号，值仅为32(k−1)B的溢出尾部。k=8时每份值224B。同键可覆盖，当前只模拟尾部I/O，实验按条带使用。旧摘要格式的条带须重新编码后运行新基准。

先尝试组内单缺片XOR。仍有全局工作时，`choose_equations` 从可用L0/L1/G+/G−为最多三个剩余缺失数据片选非零行列式的方程，按目标反向裁掉无关步骤。比如D0、D1缺失只需D2～D7、L0、G+。两L异或在代数上等价于P0，代码直接选局部方程。`recover_fasttt` 单方程直接求出移位后的差值，检查缺片覆盖范围外为零。多方程用至多3阶GF(2)移位多项式。每窗16符号（512B），先代入已知数据，再用余子式生成当前窗分子，按最低次项顺序求商。余子式移位非负，本窗只使用已处理的残差，64位移位多项式的次数上界为2(k−1)，窗间只保留实际需要的递推余项。每个方程保留当前残差窗、余子式回看的历史，以及已知数据移位所需的前视。代入时一个AVX2符号最多更新3个目标，其中可包含待重建G。分子按最低次项偏移落到目标数据缓冲，新位置用第一项初始化，反馈和后续项原地XOR。按前缀、有效输出和尾部分窗，窗口之外的反馈直接更新尚未求值的输出或尾部区，首尾一致性检查用定长小缓冲。若还缺G，在代入和求商时同步补齐相应G贡献。非法长度返回EINVAL，规划不可解或首尾余项非零返回EIO。四擦除反例{0,1,8,9}只剩一个组内方程。选片未承诺全局最小。

`RedisTails` 每次调用按需建立连接，默认 `127.0.0.1:6379`（profile `redis-port` 可调）。编码用MSET写两份尾部后，通过bufferlist引用保留同一缓冲的主片。联合恢复只GET所选G，主片只读，尾部直接读取该Redis回复持有的字节。解码按窗消耗这两个区域。重建G后写回尾部。局部修复没有Redis操作。选中键缺失返ENOENT，长度、连接或Redis回复异常返EIO，暂不重选。实验键无自动过期和回收。两种FastTT模式仅接受 `CODE_ENVIRONMENT_UTILITY` 和 `CODE_ENVIRONMENT_UTILITY_NODOUT`，系统验证按第1节推进。

**下一项单独实验。** 对同组双缺片且选中一局部一全局方程，令A为全局移位较小的片、B为较大的片，移位差为δ。扣掉已知贡献并对齐全局方程后有 `S[t]=A[t] XOR B[t]`、`R[t]=A[t] XOR B[t−δ]`。可只递推 `B[t]=S[t] XOR R[t] XOR B[t−δ]`，再用 `A[t]=S[t] XOR B[t]` 得到另一片。覆盖G±、非相邻片和首尾约束。当前仍执行通用求商，以本轮作为测量基线。

## 6. 验证结果与版本边界

r38 逐阶段日志与源码快照在 `local_workspace/ec-tests/fasttt-lrc-avx2-stages-20261122-VXD12nyj/`。各步固定2MiB、32B符号、CPU 0、单线程，`fasttt-redis` 使用独立Redis 16379，每项100次。每阶段跑合同测试、全部298种一至三片擦除和六类计时，以下是单轮毫秒/次。

| 阶段 | 编码 | D2 | 同组D0+D1 | D0+L0 | 同组D0+D1+D2 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 00-baseline | 29.976 | 4.403 | 17.707 | 15.670 | 39.330 |
| 01-avx2 | 7.826 | 0.717 | 5.723 | 4.567 | 10.823 |
| 02-epm | 7.793 | 0.716 | 5.010 | 4.559 | 10.017 |
| 03-cfwp | 6.643 | 0.716 | 3.777 | 3.775 | 6.839 |
| 04-nma | 6.049 | 0.717 | 3.781 | 3.758 | 6.762 |
| 05-tps | 5.409 | 0.408 | 3.713 | 3.279 | 6.881 |
| 05-tps 三轮中位 | 5.455 | 0.407 | 3.759 | 3.292 | 6.794 |

- 窗口阶段增加17符号的非整窗测试。最终通过5种输入长度的1,490次inline恢复、scaffold 532次恢复／64次预期拒绝、Redis 303次恢复／1次预期拒绝。单方程首尾篡改也按预期拒绝。安装产物确认含256位 `vpxor`。4KiB的Redis fixture与220种三擦除无内存访问错误；benchmark禁用插件退出清理，因此进程退出会报告16B注册对象，日志在 `final-validation/`。
- 针对EPM追加D0+D1+G+，只读D2～D7、G−、L0，三轮各200次，中位数AVX2阶段8.814ms，EPM阶段8.175ms，最终6.155ms。源码快照单独构建到 `mapping-ab/`。表中其他阶段为单轮探索数据。计时包含插件分配、Redis与摘要，吞吐分母为整条带8×2MiB。

r40 在 `19bd227` 用旧摘要模式已安装产物复测。Xeon 8358、CPU 0、单线程、每片2MiB，按模式轮换执行，三轮每项200次。解码只请求缺片，先用随机数据逐字节校验，再用常量数据计时，各插件按自身规则选片。数据为毫秒/次中位数。

| 用例 | FastTT-LRC Redis | ISA-L LRC scaffold（同布局） | ISA-L Cauchy 8+4（同主片容量） |
| --- | ---: | ---: | ---: |
| 编码 | 5.454 | 1.651 | 1.401 |
| 局部 D2 | 0.393 | 0.361 | 1.431 |
| 局部 L0 | 0.370 | 0.436 | 1.433 |
| 跨组 D0+D4 | 0.755 | 0.673 | 1.396 |
| 同组 D0+D1 | 3.724 | 0.864 | 1.396 |
| D0+L0 | 3.284 | 1.022 | 1.429 |
| 同组 D0+D1+D2 | 6.896 | 不可恢复 | 1.388 |
| D0+D1+G+ | 6.100 | 不可恢复 | 1.384 |

D2和L0的两种LRC均读4片，ISA 8+4读8片。跨组D0+D4可独立做两次局部修复。同组D0+D1均读8片。D0+L0的FastTT读8片、scaffold读9片。ISA 8+4按同片号匹配，它的8～11都是全局校验。FastTT全局路径包含Redis和整片摘要；这些对照尚未拆分耗时。命令、三轮数据、选片与校验日志在 `local_workspace/ec-tests/fasttt-lrc-matched-20261122-QkZ1wyVi/`。

解读对照时注明维度。同布局scaffold用于比较两边均可恢复的用例。它未覆盖FastTT联合方程支持的所有三擦除。无局部组的ISA-L Cauchy 8+4用于评估相同主片容量下的局部读片与容错取舍。2MiB片长下它通过全部495种四擦除。FastTT-LRC在Redis元数据可用时保证任意三擦除，{0,1,8,9}只剩L0能约束D0、D1，实际返回EIO。见同目录 `coverage-4uftV9Pu/`。若按任意三擦除的保证匹配，可用ISA-L Cauchy 8+3，记录它少一片全局校验且没有局部组。FastTT的Redis元数据另计。

r42 路径分测只对照同布局ISA-L LRC。Xeon 8358 CPU 0、单线程、2MiB/片、三轮各300次，随机输入预验。原产物重测总耗时为FastTT/ISA-L的中位毫秒。库函数计时与临时插件内分段计时分别执行，源码和日志保存在 `local_workspace/ec-tests/fasttt-lrc-path-costs-20261122-7SYmavh6/`，正式产物使用原哈希。

- 编码 5.561/1.647。FastTT 两次整片SHA-256 3.342ms，建连和MSET约0.081ms；G±计算0.826ms，两局部校验0.790ms，尾部封装拷贝0.363ms。普通8+3以P0首32B作键并复用插件连接，直接处理移位尾部；P0与G±还共用同一批输入加载。该轮LRC实现单独扫描两个局部校验，r44已融合。
- D2 0.380/0.406，两边均四输入XOR，计时内无Redis和摘要。D0+D1 3.826/0.878，FastTT一次整片SHA-256 1.679ms、全局/局部残差拷贝合计0.605ms、窗口联合求解1.338ms。ISA-L重用解码表，对每个偏移做无跨符号递推的GF线性组合。D0+L0 3.456/1.042，FastTT单方程计算0.697ms，仍会散列完整G。

r43 用已安装插件各测793种一至四片擦除，随机2MiB/片，目标为全部缺片。按最少选片仅供计划输入，成功逐字节对照，拒绝为EIO且输出为空。按擦除数1/2/3/4，FastTT Redis通过12/12、66/66、220/220、389/495，ISA-L scaffold通过12/12、66/66、188/220、181/495。scaffold拒绝的32种三擦除为同组三数据片（8种）或同组两数据片加一全局校验（24种）。限制来自分层规划。四个校验同失两边都可用8个数据片重建，纯校验擦除的15种非空子集全部通过。FastTT Redis的全局恢复依赖选中校验的尾部元数据，局部修复没有此依赖。逐掩码、分片类型汇总和临时测试源在 `local_workspace/ec-tests/fasttt-lrc-erasure-coverage-20261122-lFeBDjXB/`。

r44 的 `encode_chunks` 直接调用 `write_parities<true>`。每个16符号窗内，前两对更新L0，后两对更新L1，同时写G±；局部输出的第一对直接写入，第二对累加。双全局片重建走 `write_parities<false>`，组内单片恢复走 `write_local`。Redis尾部封装在校验生成后执行。

以已发布 `19bd227` 的产物为基线，2MiB、CPU 0、单线程、Redis模式交替跑五轮各300次，编码中位 5.408→4.903ms/次（减少9.3%）。另三轮插桩中，整片SHA-256 3.335→3.338ms，扣除SHA和Redis建连/命令后的时间 1.938→1.398ms（含编码及拷贝/分配）。合同测试、2MiB的303次Redis恢复和两种已安装插件各793种一至四片擦除复测通过，逐掩码结果与r43一致。原始数据和基线/融合产物在 `local_workspace/ec-tests/fasttt-lrc-fused-encode-20261122-0TmNDhIR/`。

r45 把k推广至4～32的偶数，locality默认k/2，m=2与两个局部组固定。最小k来自ISA子组至少两数据片，最大k来自64位移位多项式。k=6时D2、D3跨组，融合编码分别更新L0、L1。`coding_benchmark_lrc.sh` 仍复测默认8+2+2。其他k直接在benchmark设 `-P k=<偶数> -P m=2 -s <k×2097152>`，擦除D0、D1时加 `--use-minimum --decode-only-erased --erased 0 --erased 1 -w decode`。

- 三种模式通过k=4..32的布局参考和代表性恢复。安装后k=8两种模式各793种一至四擦除结果与选片逐项匹配r43。FastTT Redis在2MiB还穷举通过k=6的175种、k=10的469种一至三擦除，默认脚本通过303次，Redis合同测试Memcheck零错误。k=8编码A/B三轮各300次中位 4.878→4.884ms/次。
- 同组D0+D1两边均选k片，k=6/8/10分别为12/16/20MiB。FastTT选k−2数据片、G+、L0，scaffold选k−2数据片、两个全局校验。k=8的单轮200次计时为 3.748/0.905ms/次。命令与数据在 `local_workspace/ec-tests/fasttt-lrc-variable-k-20261122-xjZhnBYa/final-rerun/`。

r46 复核原版与LRC解码。普通FastTT一般路径把幸存校验映射到目标恢复缓冲，按窗初始化、代入和消元，Redis通过P0首32B取两份尾部。LRC先局部规划，联合求解前复制校验残差、读取选中的G尾部并校验完整摘要，再按窗构造各缺片的分子并递推。同组D0+D1的r42计时显示整片SHA约1.68ms/个G，Redis建连和GET合计约0.10ms，联合窗口约1.34ms。

`RedisTails::load` 对空vector先assign约2MiB再insert尾部，当前GCC11实测扩容并再拷贝前缀。仅在独立探针插件预留完整长度，保持同一SHA、选片和数据格式，合同测试通过。2MiB、单线程、五轮各300次，D0+D1 3.841→3.627ms，D0+L0 3.259→3.029ms。该轮仅做探针，r47已一并预留容量。记录在 `local_workspace/ec-tests/fasttt-lrc-decode-audit-20261122-evXiftRa/`。

r47 回归普通TSX的原始符号键思路。LRC每个G的首符号加片号作键，保留MSET/GET实测，值只存移位尾部。移除整片SHA-256和插件的OpenSSL链接，编码保留原缓冲中的主片，解码预留一次容量。二进制键测试、尾部字节对照、缺键/短值、尾部重建及三个模式k=4..32合同测试通过。2MiB默认脚本通过303次，安装产物两种模式各793种擦除状态及选片与r43一致，k=6/10的175/469种一至三擦除通过，Redis合同测试Memcheck零错误。

2MiB、CPU 0、单线程、Redis模式五轮各300次，摘要版→原始键版中位毫秒/次：编码 4.863→1.108，D0+D1 3.764→1.843，D0+L0 3.281→1.423。另三轮插桩中，建连加Redis命令编码约0.105ms，D0+D1约0.100ms。日志、产物和对照在 `local_workspace/ec-tests/fasttt-lrc-raw-tail-20261122-bHdZdkWB/`。

r49 在 `bb11837` 已安装产物复测，仅测同布局双方支持的用例。默认8+2+2、2MiB/片、CPU 0单线程。每组轮换顺序，五轮各300次，随机输入逐字节预验、常量输入计时。FastTT原始符号键版/ISA-L scaffold中位毫秒/次：编码1.085/1.631（14.40/9.58GiB/s），D2 0.369/0.365，L0 0.366/0.408，跨组D0+D4 0.722/0.670，同组D0+D1 1.851/0.879，D0+L0 1.366/1.031，G+重建0.778/0.695。D0+D1两边读8片，D0+L0与G+分别读8/9片。纳入同期旧摘要产物仅复核改版收益，原始键版仍实测Redis建连、MSET/GET。合同通过，默认脚本FastTT 303次、scaffold 83次。时间、范围、选片和命令见 `local_workspace/ec-tests/fasttt-lrc-raw-tail-matched-20261123-W9VFOBQp/`。

r50 基于 `bb11837` 分两步：①裁掉整片残差复制，按窗读主片和Redis尾部并保留余子式历史；②分子按窗累加到缺片输出，反馈传播后符号已在最终位置。D0+D1少分配两份整片残差（2MiB＋2MiB＋224B），省掉每个结果从分子窗搬出的独立写回。2MiB、CPU 0单线程、Redis模式交替五轮各300次，三阶段中位毫秒/次：D0+D1 1.854→1.400→1.280，D0+D3 1.852→1.306→1.248，D0+D1+G+ 2.307→1.858→1.733，D0+D1+D2 3.023→2.428→2.244，D0+L0 1.397→1.134→1.134，局部D2 0.364→0.363→0.364。六用例的读量和helper集合三阶段一致。

两步均过合同测试和默认303次。最终安装产物默认k=8两种模式各793种逐掩码结果一致，k=6/10分别175/469种一至三擦除通过，Redis合同测试Memcheck零错误。命令、源码快照和计时范围在 `local_workspace/ec-tests/fasttt-lrc-mapped-output-20261123-ixpJUzYM/`。

r51 逐项验证原版的省访存做法。以r50为基线，同组D0+D1代入的幸存数据从8份片等效加载降为6份，D0+D1+D2从11份降为5份。单位为代入阶段显式符号加载，Ceph选片字节数不变。G±首次覆盖直接写，编码中位耗时约1.07ms。四符号归约复测D0+L0约1.138→1.124ms、单G重建0.767→0.752ms。分子首写后仍需初始化未来可能收到反馈的位置。单缺数据同时重建G也复用窗口代入。

同口径五轮各300次，r50→r51中位毫秒/次为D0+D1 1.273→1.135，D0+D1+D2 2.242→2.023，D0+D1+G+ 1.753→1.684，D0+G++L0 2.337→1.460，D3+G−+L0 2.342→1.462。流式写入D2在只写时0.363→0.349ms，随即校验读回0.528→0.609ms，恢复结果供同次联合计算时1.546→1.599ms，保留普通写入。按窗清零待重建G使同组双缺片和三缺片分别慢约6%和5%，保留整片初始化。最终安装产物过默认303次及合同，两模式各793种状态／选片与基线一致，k=6/10分别通过175/469种，Redis合同Memcheck零错误。原始记录在 `local_workspace/ec-tests/fasttt-lrc-memory-traffic-20261123-foQtRNQb/`，`summary.csv` 为最终同轮对照。

r37确认Ceph的普通FastTT和LRC性能统一使用Redis。LRC benchmark省略mode的2MiB冒烟测试已验证默认走`fasttt-redis`，通过303次字节校验。日志在 `local_workspace/ec-tests/fasttt-lrc-redis-default-SNhjBmXy/default.log`。

r36 在Xeon 8358上按2MiB／32B、CPU 0、单线程跑三轮，每项100次。普通163项／轮、LRC 18项／轮。另做匹配擦除的ISA 8+4对照、8+3的11个单擦除位置、部分2,000次复核和Cerasure 24条用例。报告和原始数据在 `local_workspace/ec-tests/fasttt-perf-initial-UzUXGWio/report.md`。

- 原论文趋势部分复现。以8+3为例，FastTT编码15.11 GiB/s vs ISA-L 13.26，一擦除23.92 vs 21.75，多擦除落后；2,000次复核及位置扫描均未恢复论文单擦除倍率。型号和ISA-L SIMD调度无法从这轮单独归因。
- LRC同布局scaffold／inline／Redis编码分别1.734／26.152／30.036ms，D2局部修复0.395／4.387／4.409ms。已在反汇编中确认参考XOR循环逐字节工作。Redis路径的整片SHA-256对照约1.68ms/片。跨组选局部修两个数据片较同组联合快约一倍。

早期验证证据。1～64KiB的Redis回归、断连失败和元数据损坏用例在 `local_workspace/ec-tests/fasttt-lrc-redis-nq41fxAT/`。内存参考版与ISA对照在 `local_workspace/ec-tests/fasttt-lrc-core-20261121-XX1zmktH/`，其中ISA表缓存退出的malloc／delete不匹配已用旧插件对照重现。

旧基线证据仍分开使用。r27普通插件48命令／6522组合与8/3的70项计时在 `local_workspace/ec-tests/lrc-benchmark-integration-20261114-C2Ymok3Q/`。r22基类包装与官方LRC额外未填输出对照见 `local_workspace/ec-tests/lrc-default-wrappers-20261111-90phCRas/`。论文附录旧插件流程记录见 `local_workspace/paper-reproduction/appendix-report-20261102.md`，含163项各2000次、独立Cerasure和历史BlueStore验证。它们与本轮FastTT-LRC内存测试的适用范围不同。

## 7. 后续维护这两份文档

**人读设计**只保留当前实现、关键函数／参数和少量贯穿例子；不放实验流水、凭据过程或仓库进度。**本交接**保存可继续开发的目标／阶段、版本与dirty状态、运行入口、必要技术结论、已验证边界、未决问题和下一项任务。核心事实直接写清，源码位置／日志是定位与补充，不用旧聊天或另一报告替代必要上下文。

更新按“当前事实优先”进行：修改相应状态和结论，合并重复内容；普通问答不逐轮建章节。历史只保留仍影响实现的决策原因、失败教训和验证边界，已撤回的包装／旧Ceph教程不与现状并列。不要为显得详细重复同一结论，也不要为压字数删掉恢复工作所需信息。周报等单次总结不写进核心文档。

发生代码进展、重要结论、目标／范围／环境／分支变化或交接时，联动第0节、相关技术结论、第6节证据与第1节下一步；区分“计划／代码存在／编译／正确性／性能／系统验收”。仅聊天解释且无新事实时，不必新增历史记录或实验。版本／代码变化后再运行适当检查，文档编辑不机械重跑构建。

本文由主agent单写。写前以排他创建取得 `docs/agent/.fasttt-handoff.lock`，记录维护者／起始时间／所基修订；持锁重读最新正文再合并。已有锁先核验实际持有者，不能仅因超时删除；其他agent只交发现。保存到同目录唯一临时文件，校验UTF-8／围栏／相对入口／当前状态，原子替换后释放锁。锁／临时文件不入Git；修订变化则重读合并，不覆盖。本文保存成功才可声称已同步。

收尾给出真实的改动、验证和未完成项；若有运行任务，写清观察／停止方法。提交／推送依当次授权，成功后核对远端SHA；不能把未提交文档当已发布。README和AGENTS仅导航，不维护另一套台账。
