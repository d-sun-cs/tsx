# FastTT: High-Performance Two-Tone Shift-XOR Erasure Coding for Ceph

This repository contains the artifact for "FastTT: Accelerating Shift-XOR Erasure Coding for Data Storage".

## Current Ceph 18.2.7 Baseline

Use [repro/README.md](repro/README.md) on `research/ceph-v18.2.7` for the pinned
Ceph release, minimal integration patches, fresh AVX2 build, and deterministic
correctness verification. This workflow supersedes the historical setup scripts
below. It does not require a private container image or an edited Ceph fork.

## Development and AI Agent Handoff

Start with [AGENTS.md](AGENTS.md) and the single maintained
[FastTT handoff document](docs/agent/fasttt-handoff.md). The handoff contains the
current source baseline, implementation map, research plans, evidence, and next
steps. Its paths are relative to this repository so it can move with a checkout.
The instructions below are preserved as the historical artifact workflow;
use the current baseline instructions above for new workspaces.

## Execution Environment Reference

| Step | Location | Commands/Scripts |
|------|----------|------------------|
| 1 | **HOST** | Manual: `git clone` ceph, `cd`, `git clone` tsx |
| 2 | **HOST** | `bash setup_container.sh` or manual setup + `python3 build-with-container.py` |
| 3 | **INSIDE CONTAINER** | `init_container_env.sh`, then `install_dependencies.sh` |
| 4 | **INSIDE CONTAINER** | `bash recmake.sh` (compilation) |
| 5 | **INSIDE CONTAINER** | `coding_benchmark_cases.sh`, `general_test.sh`, `fasttt.sh` (Cerasure) |

## Prerequisites

- Docker
- Python 3
- Git
- Sufficient disk space (~100GB for Ceph build)
- System with AVX2 CPU support (required for optimizations)

## Quick Start

### Step 1: Clone Ceph and tsx Plugin (Host Machine)

**Note:** These commands must be run manually first since the scripts are not yet available locally.

```bash
# Clone Ceph v18.2.7
git clone --branch v18.2.7 --depth 1 --recurse-submodules https://github.com/ceph/ceph.git

# Navigate to erasure-code directory
cd ceph/src/erasure-code/

# Clone tsx plugin
git clone https://github.com/d-sun-cs/tsx.git

# All scripts and tools are now available in the tsx directory
```

### Step 2: Build Docker Container (Host Machine)

After cloning, run the automated setup script:

```bash
cd tsx
bash setup_container.sh
```

This script will:
- Run initialization scripts (update_cmake.sh, update_docker_build.sh)
- Create the Docker container with compilation environment
- Drop you into the container

**Alternative Manual Setup:**

```bash
cd tsx
bash update_cmake.sh
cd ../../..
python3 ./src/script/build-with-container.py -d ubuntu22.04 -e interactive --keep-container
```

### Step 3: Initialize Container Environment and Install Dependencies

**Note:** All commands below must be executed **inside the container**

**Automated Setup (Recommended):**

```bash
# First, basic environment setup
bash init_container_env.sh

# Then, install Redis and hiredis
bash install_dependencies.sh
```

**Manual Setup (Alternative):**

```bash
# Prepare environment
cd
apt install sudo -y
git config --global --add safe.directory "*"

# Install Redis and hiredis (see install_dependencies.sh for details)
```

### Step 4: Compile the tsx Plugin (Inside Container)

Navigate to the tsx plugin directory and compile:

```bash
cd /ceph/src/erasure-code/tsx/
bash recmake.sh
```

**Troubleshooting:** If you encounter ninja build errors, manually compile in the build directory:
```bash
cd /ceph/build
ninja
```

Repeat until compilation succeeds.

### Step 5: Run Benchmarks (Inside Container)

#### 5.1 Reproduce Tables I, III, V (Encoding/Decoding Throughput - Ceph Benchmark)

Navigate to the tsx plugin directory and run the benchmark suite for different (k,m) configurations:

```bash
cd /ceph/src/erasure-code/tsx/
bash coding_benchmark_cases.sh
bash coding_benchmark_cases.sh 6 2
bash coding_benchmark_cases.sh 6 3
bash coding_benchmark_cases.sh 10 4
```

#### 5.2 Reproduce Tables II, IV (Ablation Study - Cerasure Framework)

Clone and run the Cerasure comparison framework:

```bash
cd
git clone https://github.com/d-sun-cs/cerasure.git
bash fasttt.sh
```

#### 5.3 Reproduce Table VI (End-to-End Storage Performance)

Back in the tsx directory, initialize a mini-cluster and run the end-to-end benchmark:

```bash
cd /ceph/src/erasure-code/tsx/
bash vstart.sh
bash general_test.sh
bash general_test.sh isa
bash general_test.sh jerasure
bash vstart.sh 6 2
bash general_test.sh tsx2 6 2
bash general_test.sh isa 6 2
bash general_test.sh jerasure 6 2
bash vstart.sh 6 3
bash general_test.sh tsx 6 3
bash general_test.sh isa 6 3
bash general_test.sh jerasure 6 3
bash vstart.sh 10 4
bash general_test.sh tsx4 10 4
bash general_test.sh isa 10 4
bash general_test.sh jerasure 10 4
```

This measures the end-to-end PUT performance and throughput in the Ceph storage system.

To simplify the workflow, you can use the wrapper script to run `vstart.sh` and all three
`general_test.sh` runs with the same parameters:

```bash
cd /ceph/src/erasure-code/tsx/
bash run_end_to_end_tests.sh
bash run_end_to_end_tests.sh 6 2
bash run_end_to_end_tests.sh 6 3
bash run_end_to_end_tests.sh 10 4
```

**Note:** When using `run_end_to_end_tests.sh`, the detailed output from `general_test.sh` 
is written to `/ceph/src/erasure-code/tsx/output.txt` to keep the console output clean. 
Check this file for test results, throughput measurements, and any error messages.

## File Structure

### Setup and Initialization Scripts (Host & Container)

- `setup_container.sh`: **[HOST]** Automated Docker container creation (run from tsx directory after cloning)
- `init_container_env.sh`: **[CONTAINER]** Basic environment initialization (install sudo, git config)
- `install_dependencies.sh`: **[CONTAINER]** Automated Redis and hiredis installation

### Compilation and Benchmarking Scripts

- `update_cmake.sh`: Updates CMakeLists.txt for tsx plugin integration (called by setup_container.sh)
- `update_docker_build.sh`: Updates Docker build configuration (called by setup_container.sh)
- `recmake.sh`: Recompiles the tsx plugin within the Ceph build system
- `coding_benchmark_cases.sh`: Runs comprehensive encoding/decoding benchmarks for different (k,m) configurations
- `vstart.sh`: Initializes a mini-cluster for end-to-end testing
- `general_test.sh`: Runs end-to-end storage performance benchmarks
- `run_end_to_end_tests.sh`: Convenience wrapper to run vstart + general_test for tsx/isa/jerasure

## Additional Notes

### Prerequisites and Initial Setup

Step 1 requires manual execution since scripts are not yet available locally:
```bash
git clone --branch v18.2.7 --depth 1 --recurse-submodules https://github.com/ceph/ceph.git
cd ceph/src/erasure-code/
git clone https://github.com/d-sun-cs/tsx.git
cd tsx
```

All subsequent scripts are located within the tsx directory.

### Container Scripts

- `init_container_env.sh`: Basic environment setup (installs sudo, configures git)
- `install_dependencies.sh`: Installs Redis and hiredis with persistence disabled for benchmarking

### Configuration Notes

- Redis operations are configured to disable disk persistence for faster benchmarking
- Container must remain running during all compilation and benchmark steps
- For reproducibility, benchmarks use 2000 iterations (as per paper settings)
- The tsx plugin supports various (k,m) configurations; results scale accordingly

### Troubleshooting Common Issues

1. **Ninja compilation errors**: Manually navigate to `/ceph/build` and run `ninja` to continue interrupted builds
2. **Redis connection issues**: Verify Redis is running with `redis-cli ping` (should return PONG)
3. **Plugin not found**: Ensure `recmake.sh` completed successfully and check `/ceph/build/lib/` for compiled plugin
4. **Container not found**: Verify container name in Docker before re-running `setup_container.sh`

## References

For detailed methodology and additional context, see:
- [Artifact Description and Evaluation (ADAE.tex)](ADAE.tex)
- Original paper: FastTT - Accelerating Two-Tone Erasure Codes
