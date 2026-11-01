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
- 保留上游旧脚本供历史查阅，但当前基线使用本目录入口。尤其不要执行旧 `recmake.sh`、覆盖完整 Ceph CMake 的脚本，或把 MemStore 测试当作磁盘端到端结果。
- 不把旧 AVX512 stash 当作迁移补丁。任何算法变化、测试发现和新依赖都应进入此 TSX 分支并同步交接文档；指定实验以完整提交和结果清单为准。
