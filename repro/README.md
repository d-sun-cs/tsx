# FastTT：Ceph v18.2.7 实验基线

此目录把独立 TSX 仓库接入官方 Ceph，不需要维护 Ceph fork，也不依赖另一台机器上修改过的 CMake、旧 build、sudo install 或私人 Docker 镜像。后续代码与实验脚本应提交到 TSX 的 `research/ceph-v18.2.7` 分支；复现实验时还须记录该分支当时的完整 commit，不能只记录会移动的分支名。

## 版本依据

论文 *FastTT: Accelerating Shift-XOR Erasure Coding for Data Storage* 的实验环境与 artifact appendix 明确指定 **Ceph v18.2.7、Ubuntu 22.04、GCC 11.4.0、AVX2**。官方 Ceph tag 对应 `6b0e988052ec84cf2d4a54ff9bbbc5e720b621ad`。论文只给出了 TSX 仓库 URL，没有锁定分支／提交；这里以普通 AVX2 的 `v0.0.1-ipdps`／`5c85ce5e629905fcf8e660c8490f6b66a4160c69` 为算法起点。详细身份保存在 `versions.json`。

论文 CPU 是 i9-14900K、64 GB DDR5。本工作区的 Xeon 平台不同；对齐软件版本不等于重新得到论文性能。论文的磁盘端到端 PUT 也不能用旧 `vstart.sh --memstore` 脚本充当复现。

## 从两个仓库重建

先确保所选 TSX 分支已经推送到远端；本地新建的分支不会自动出现在 GitHub。部署具体实验时，将下面 TSX checkout 固定为该实验记录中的 commit。

```bash
git clone --branch v18.2.7 --depth 1 --recurse-submodules \
  https://github.com/ceph/ceph.git ceph
cd ceph
git clone --branch research/ceph-v18.2.7 \
  https://github.com/d-sun-cs/tsx.git src/erasure-code/tsx
python3 src/erasure-code/tsx/repro/reproduce.py prepare
docker build -t fasttt-reef:ubuntu22 -f src/erasure-code/tsx/repro/Dockerfile .
docker run --rm -it --name fasttt-reef \
  --mount type=bind,src="$PWD",dst=/ceph fasttt-reef:ubuntu22
```

容器内执行：

```bash
python3 src/erasure-code/tsx/repro/reproduce.py build --jobs 8
# 新容器内只监听回环地址；不映射宿主机端口，不使用已有 Redis 数据。
redis-server --bind 127.0.0.1 --port 6379 --save '' --appendonly no --daemonize yes
python3 src/erasure-code/tsx/repro/reproduce.py verify
python3 src/erasure-code/tsx/repro/reproduce.py doctor
```

`prepare` 先校验 Ceph HEAD、TSX 基线祖先和递归子模块，然后幂等应用三个补丁；遇到冲突会停止，不会覆盖未知修改：

1. `0001-enable-tsx.patch`：只在官方 EC CMake 中增加 TSX 子目录和插件组目标。
2. `0002-benchmark-input.patch`：保留默认全 `X` 输入，另加 `--input-pattern random --input-seed 1`。以 `mt19937(seed)` 每次取低 8 位生成字节，准备输入在计时循环外；保留官方 exhaustive 擦除后的长度／内容比较，增加首差异位置诊断。
3. `0003-boost-download.patch`：改用 Boost 官方 archive 地址，保留 Ceph 原定 1.79.0 和 SHA256 校验。

父 Ceph 的预期工作区 diff 就是这些补丁，版本身份始终为官方 tag。不要把这些 diff 隐藏成“干净上游”，也不要直接恢复 v20 的 stash。TSX CMake 明确发现 hiredis 头和库，Cerasure 对照项默认关闭；需要单独对照时再安装它的依赖并设置 `WITH_TSX_CERASURE=ON`。

`build` 创建新的 `build/`，使用 GCC 11、RelWithDebInfo 和插件本身的 `-mavx2 -O3`；不加入全局 AVX512／`-march=native`。仅编译 TSX 三种校验数变体、ISA-L、Jerasure 和 benchmark。它保留 BlueStore 编译配置，但尚不等于编译出完整 vstart 集群。以后做集群实验时，应把所需 daemon targets 和明确的磁盘／后端配置补入可复现入口。

`verify` 在 `(6,2)、(6,3)、(8,3)、(10,4)` 上逐一枚举 1..m 个擦除的全部组合，使用 64 KiB／2 MiB 数据片和 constant／random 两类输入，实际比较丢失片恢复内容。`-s` 是整个输入对象大小，所以以 `k * chunk_size` 传入。该检查不覆盖任意长度、Redis 丢失、重启、真实 OSD 故障或 LRC 功能，也不生成论文性能结论。Redis 保存原始实现需要的移位尾部，不应在编解码间清空。

`build/fasttt-build.json`、`fasttt-verification.json` 和 `fasttt-environment.json` 记录两个仓库 HEAD、分支、diff、子模块、补丁和产物哈希、工具链、dpkg 包版本及硬件。复现时另存容器 image ID／digest和命令输出。Dockerfile固定了Ubuntu22.04的amd64 manifest digest，但 apt 仓库会更新，因此这仍不是逐字节一致的依赖镜像锁；GCC 补丁版本以实际记录为准，若不是 11.4.0，不得标为与论文工具链完全一致。

## 文件与文档边界

- `AGENTS.md` 与 `docs/agent/fasttt-handoff.md` 随 TSX 管理；后者是唯一持续交接正文。文档使用相对路径、代码符号与提交身份，不靠旧聊天恢复状态。
- 论文 PDF、原始实验结果、旧构建、备份留在 Ceph 根下的本地 `local_workspace/`。它们不进入插件源文件列表，也不作为构建输入；不应自动上传论文 PDF 或庞大构建文件。
- 附录的 `coding_benchmark_cases.sh` 与 `run_end_to_end_tests.sh` 已接入本目录入口；其余上游旧脚本保留供历史查阅。尤其不要执行旧 `recmake.sh`、覆盖完整 Ceph CMake 的脚本，或把 MemStore 测试当作磁盘端到端结果。
- 不把旧 AVX512 stash 当作迁移补丁。任何算法变化、测试发现和新依赖都应进入此 TSX 分支并同步交接文档；指定实验以完整提交和结果清单为准。

## 按论文附录实际执行 A1 / A2

在已配置的独立容器内，从 Ceph 根运行：

```bash
# 构建所有 TSX/消融插件与 vstart，再按生成的 EC 安装规则安装。
python3 src/erasure-code/tsx/repro/appendix.py install --cluster --jobs 16
# 该目录必须实际存在 libec_tsx.so / libec_tsx2.so / libec_tsx4.so 等。
ls /usr/local/lib/ceph/erasure-code
python3 src/erasure-code/tsx/repro/appendix.py verify-installed
python3 src/erasure-code/tsx/repro/appendix.py coding --iterations 2000
python3 src/erasure-code/tsx/repro/appendix.py end-to-end --put-iterations 3 --degraded-read
```

`coding` 默认跑全部四组论文参数、FastTT/ISA-L/Jerasure及8+3消融，共163个测项。每种数据／校验擦除类型固定同一组位置供所有实现使用，每测项2000次，先预热两次；按擦除总数分别汇总。默认数据片2MiB，原始日志、完整命令、GB/s与GiB/s均保留。默认固定一个可用CPU、单测量轮次；这是流程验收入口，正式统计可用 `--repeats 3`，并先核验本机ISA-L实际SIMD分派与论文机器的差异。

`install` 只执行生成的 `build/src/erasure-code/cmake_install.cmake`，避免全量安装；默认安装前缀来自CMake缓存。基准与集群明确从安装目录加载插件，不依赖先前的系统插件。

`verify-installed` 对安装后的三实现及(8,3)四消融，使用2MiB随机数据片穷举1..m擦除，逐字节检查恢复；完整矩阵为48条命令、6,522种擦除组合。它与旧 `reproduce.py verify` 的7,464组合矩阵范围不同，不能混为同一个计数。

`end-to-end` 使用官方Ceph18 `src/vstart.sh`，每次创建新的 `/tmp/fasttt-paper-cluster-*` 状态目录，单MON、最多14个OSD、MGR/MDS/RGW关闭，BlueStore使用普通文件，未指定原始块设备。每对象为4条完整stripe（默认48/64/80MiB），PUT后GET并比较SHA256；PUT吞吐按总字节／总时间计算。各k/m采用不同但确定的数据种子，避免原Redis内容键在不同尾部格式间相撞，这不构成尾部命名或可靠性修复。`--degraded-read` 从对象acting映射选一个OSD，核对其进程属于当前独立配置后停止，执行降级GET并比较SHA，再恢复；最终只停止带本次精确配置路径的进程。日志与结果保留，不调用旧的全局stop／删除build脚本。该流程不等于论文硬件、生产故障域或任意故障可靠性验证。

端到端池的ISA-L/Jerasure按原vstart脚本默认值使用 `reed_sol_van`，与内存基准的 `cauchy/cauchy_good` 分开记录。全矩阵共用14个OSD，各池仍按自身k+m放置；这是缩小规模的流程验收，不复述论文磁盘吞吐比例。

论文原入口名称继续可用，已改为上述实现的包装：

```bash
cd src/erasure-code/tsx
bash coding_benchmark_cases.sh 8 3 2048 2000
bash run_end_to_end_tests.sh 8 3 2m 3
```

A2需在可联网的宿主先克隆论文指定的独立仓库，使用 `fasttt` 分支而非缺少入口的 `main`，并核对 `versions.json` 中的提交：

```bash
git clone --branch fasttt https://github.com/d-sun-cs/cerasure.git local_workspace/cerasure-paper
git -C local_workspace/cerasure-paper checkout c9f305294cb8e52af6f5962e3c83cc607a85ba07
# 回到同一个实验容器后：
python3 src/erasure-code/tsx/repro/appendix.py cerasure --jobs 16
```

此入口从Ceph固定GF-Complete子模块复制源码并在本地依赖前缀构建，不修改原子模块，也不覆盖系统ISA-L。随后构建独立Cerasure程序，使用 `bash -e fasttt.sh` 执行附录原脚本的24项比较，校验逐项 `decode success` 与输出完整性。A2中的FastTT是该独立框架中的实现，不能与Ceph插件的测量数据混为一个表。

所有输出默认保存在 `local_workspace/paper-reproduction/` 的独立目录；安装清单保存在 `build/fasttt-installation.json`。输出不会写进插件源码目录。
