#!/bin/bash

# 参数校验
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 <chunk-size-in-KB> <test-count> [k]"
    exit 1
fi

CHUNK_SIZE_KB=$1
TEST_COUNT=$2
K=${3:-8}  # 如果第3个参数为空，默认k=8

# 计算 buffer size
BUFFER_SIZE=$((CHUNK_SIZE_KB * K * 1024))

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
    echo "please change directory to ceph/src/erasure-code/tsx to run this script"
    exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

export LD_LIBRARY_PATH=$CEPH_BUILD/lib:$LD_LIBRARY_PATH

cd $CEPH_BUILD

# 定义需要编译的目标数组
NINJA_TARGETS=("ec_tsx" "ec_tsxse" "ec_tsxnaive" "ec_tsxmap" "ec_tsxwin" "ec_tsxmerge" "ceph_erasure_code_benchmark")

needs_install=false

for target in "${NINJA_TARGETS[@]}"; do
    ninja_output=$(ninja "$target")
    if [[ "$ninja_output" != *"no work to do."* ]]; then
        needs_install=true
    fi
done

if $needs_install; then
    sudo ninja install
fi

# 测试命令
echo "-------------------------------------------"
echo "Chunk size: ${CHUNK_SIZE_KB}KB"
echo "Buffer size: $((CHUNK_SIZE_KB * K))KB"
echo "Test count: $TEST_COUNT"
echo "K: $K"

run_test() {
    local description=$1
    local command=$2

    echo "$description"
    output=$($command)
    echo "$output"

    # 提取时间和总量
    time=$(echo "$output" | grep -oP '^\d+\.\d+')
    total_kb=$(echo "$output" | grep -oP '\d+$')

    if [[ -n "$time" && -n "$total_kb" ]]; then
        throughput=$(python3 -c "print(round($total_kb / 1024 / $time, 2))")
        gbps=$(python3 -c "print('{:.2f}'.format($throughput / 1024))")
        echo "Throughput: ${throughput} MB/s (${gbps} GB/s)"
    else
        echo "Failed to calculate throughput for $description"
    fi
}

# Define plugin names and their technique parameters
# PLUGINS=("tsx" "isa" "jerasure" "tsxnaive" "tsxmap" "tsxwin" "tsxmerge")
# TECHNIQUES=("null" "cauchy" "cauchy_good" "null" "null" "null" "null")
PLUGINS=("tsx" "tsxse" "tsxmerge"  "isa")
TECHNIQUES=("null" "null" "null" "cauchy")


# 在正式测试前进行预热
warmup_test() {
    local plugin=$1
    local technique=$2
    local cmd="$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p $plugin"
    if [ "$technique" != "null" ]; then
        cmd+=" -P technique=$technique"
    fi
    cmd+=" -P k=$K -P m=3 -s $BUFFER_SIZE -i 2"
    $cmd > /dev/null 2>&1
    cmd="$CEPH_BUILD/bin/ceph_erasure_code_benchmark -e 3 -w decode -p $plugin"
    if [ "$technique" != "null" ]; then
        cmd+=" -P technique=$technique"
    fi
    cmd+=" -P k=$K -P m=3 -s $BUFFER_SIZE -i 2 --erased 5 --erased 6 --erased 7"
    $cmd > /dev/null 2>&1
}

for i in "${!PLUGINS[@]}"; do
    warmup_test "${PLUGINS[$i]}" "${TECHNIQUES[$i]}"
done

# 恢复正常测试部分
# 指定要擦除的块索引
ERASED_IDX=(0 9)
# 拼接 --erased 参数
ERASED_ARGS=""
for idx in "${ERASED_IDX[@]}"; do
    ERASED_ARGS+=" --erased $idx"
done

echo "-------------------------------------------"
for i in "${!PLUGINS[@]}"; do
    plugin="${PLUGINS[$i]}"
    technique="${TECHNIQUES[$i]}"
    # encode
    desc="$plugin encode"
    cmd="$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p $plugin"
    if [ "$technique" != "null" ]; then
        cmd+=" -P technique=$technique"
    fi
    cmd+=" -P k=$K -P m=3 -s $BUFFER_SIZE -i $TEST_COUNT"
    run_test "$desc" "$cmd"
    # decode
    desc="$plugin decode"
    cmd="$CEPH_BUILD/bin/ceph_erasure_code_benchmark -e ${#ERASED_IDX[@]} -w decode -p $plugin"
    if [ "$technique" != "null" ]; then
        cmd+=" -P technique=$technique"
    fi
    cmd+=" -P k=$K -P m=3 -s $BUFFER_SIZE -i $TEST_COUNT $ERASED_ARGS"
    run_test "$desc" "$cmd"
    echo "-------------------------------------------"
done

cd $current_dir

echo "All tests completed successfully!"
