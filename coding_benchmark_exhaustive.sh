#!/bin/bash

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 <chunk-size-in-KB> <test-count> [k]"
    exit 1
fi

CHUNK_SIZE_KB=$1
TEST_COUNT=$2
K=${3:-8}
BUFFER_SIZE=$((CHUNK_SIZE_KB * K * 1024))

current_dir=$(pwd)
if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
    echo "please change directory to ceph/src/erasure-code/tsx to run this script"
    exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)
export LD_LIBRARY_PATH=$CEPH_BUILD/lib:$LD_LIBRARY_PATH
cd $CEPH_BUILD

ninja_output_ec_tsx=$(ninja ec_tsx)
ninja_output_ec_tsxse=$(ninja ec_tsxse)
ninja_output_benchmark=$(ninja ceph_erasure_code_benchmark)

if [[ "$ninja_output_ec_tsx" != *"no work to do."* || "$ninja_output_benchmark" != *"no work to do."* || "$ninja_output_ec_tsxse" != *"no work to do."* ]]; then
    sudo ninja install
fi

# 计算组合数 C(n, k)
combinations() {
    local n=$1 k=$2 num=1 den=1
    for ((i=1; i<=k; i++)); do
        num=$((num * (n - i + 1)))
        den=$((den * i))
    done
    echo $((num / den))
}

# 计算总测试情况数 (C(n,1) + C(n,2) + C(n,3))
n=$((K + 3))
total_cases=0
for e in 1 2 3; do
    comb=$(combinations $n $e)
    total_cases=$((total_cases + comb))
done

# 计算每个组合的测试次数（向上取整）
per_case_count=$(( (TEST_COUNT + total_cases - 1) / total_cases ))

run_test() {
    local description=$1
    local command=$2
    local is_exhaustive=${3:-0}
    
    echo "$description"
    if [ $is_exhaustive -eq 1 ]; then
        total_seconds=0
        # 分别测试-e1/-e2/-e3三种情况
        for erased_count in 1 2 3; do
            cmd_erased="$command -e $erased_count"
            output=$($cmd_erased)
            echo "$output" | grep -v "^Command"  # 过滤无关输出
            time_val=$(echo "$output" | awk '{print $1}')
            total_seconds=$(python3 -c "print($total_seconds + $time_val)")
        done
        # 计算总数据量 (Buffer大小 × 总情况数 × 每个组合测试次数)
        total_data_mb=$(python3 -c "print(($BUFFER_SIZE * $total_cases * $per_case_count) / (1024 * 1024))")
        avg_throughput=$(python3 -c "print(round($total_data_mb / $total_seconds, 2))")
        echo "Avg Throughput: ${avg_throughput} MB/s"
    else
        output=$($command)
        echo "$output"
        time_val=$(echo "$output" | awk '{print $1}')
        data_kb=$(echo "$output" | awk '{print $2}')
        if [ -n "$time_val" ] && [ -n "$data_kb" ]; then
            throughput=$(python3 -c "print(round($data_kb / 1024 / $time_val, 2))")
            echo "Throughput: ${throughput} MB/s"
        else
            echo "Failed to calculate throughput"
        fi
    fi
}

# 预热测试（保持原逻辑）
warmup_test() {
    $CEPH_BUILD/bin/ceph_erasure_code_benchmark -p isa -P k=$K -P m=3 -s $BUFFER_SIZE -i 2 > /dev/null
    $CEPH_BUILD/bin/ceph_erasure_code_benchmark -e 3 -w decode -p isa -P k=$K -P m=3 -s $BUFFER_SIZE -i 2 --erased 5 --erased 6 --erased 7 > /dev/null
    $CEPH_BUILD/bin/ceph_erasure_code_benchmark -p tsx -P k=$K -P m=3 -s $BUFFER_SIZE -i 2 > /dev/null
    $CEPH_BUILD/bin/ceph_erasure_code_benchmark -e 3 -w decode -p tsx -P k=$K -P m=3 -s $BUFFER_SIZE -i 2 --erased 5 --erased 6 --erased 7 > /dev/null
    $CEPH_BUILD/bin/ceph_erasure_code_benchmark -p tsxse -P k=$K -P m=3 -s $BUFFER_SIZE -i 2 > /dev/null
    $CEPH_BUILD/bin/ceph_erasure_code_benchmark -e 3 -w decode -p tsxse -P k=$K -P m=3 -s $BUFFER_SIZE -i 2 --erased 5 --erased 6 --erased 7 > /dev/null
}

warmup_test

# 解码测试命令模板（使用per_case_count）
DECODE_CMD="$CEPH_BUILD/bin/ceph_erasure_code_benchmark -w decode -P k=$K -P m=3 -s $BUFFER_SIZE -i $per_case_count -E exhaustive"

# 输出测试次数信息
echo "-------------------------------------------"
echo "Encode test count: $TEST_COUNT"
echo "Decode total cases: $total_cases"
echo "Decode per-case count: $per_case_count"
echo "Total decode test count: $(( per_case_count * total_cases ))"
echo "Chunk size: ${CHUNK_SIZE_KB}KB"
echo "Buffer size: $((CHUNK_SIZE_KB * K))KB"
echo "K: $K"

# 正常测试部分
echo "-------------------------------------------"
run_test "TSX encode" "$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p tsx -P k=$K -P m=3 -s $BUFFER_SIZE -i $TEST_COUNT"
run_test "TSX exhaustive decode" "$DECODE_CMD -p tsx" 1
echo "-------------------------------------------"
run_test "ISA encode" "$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p isa -P k=$K -P m=3 -s $BUFFER_SIZE -i $TEST_COUNT"
run_test "ISA exhaustive decode" "$DECODE_CMD -p isa" 1
echo "-------------------------------------------"
run_test "TSXSE encode" "$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p tsxse -P k=$K -P m=3 -s $BUFFER_SIZE -i $TEST_COUNT"
run_test "TSXSE exhaustive decode" "$DECODE_CMD -p tsxse" 1
echo "-------------------------------------------"
run_test "jerasure encode" "$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p jerasure -P technique=reed_sol_van -P k=$K -P m=3 -s $BUFFER_SIZE -i $TEST_COUNT"
run_test "jerasure exhaustive decode" "$DECODE_CMD -p jerasure -P technique=reed_sol_van" 1
echo "-------------------------------------------"

cd "$current_dir"

echo "All tests completed successfully!"