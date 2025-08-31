#!/bin/bash

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 <chunk-size-in-KB> <test-count> [k]"
    exit 1
fi

CHUNK_SIZE_KB=$1
TEST_COUNT=$2
K=${3:-8}  # Default k=8
M=3        # Fixed m for Reed-Solomon
TOTAL_CHUNKS=$((K + M))

BUFFER_SIZE=$((CHUNK_SIZE_KB * K * 1024))

# Validate current directory
current_dir=$(pwd)
if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
    echo "Error: Please run this script from ceph/src/erasure-code/tsx directory"
    exit 1
fi

# Set up build environment
CEPH_BUILD=$(cd ../../../build && pwd)
export LD_LIBRARY_PATH=$CEPH_BUILD/lib:$LD_LIBRARY_PATH
cd $CEPH_BUILD || exit

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

# Test cases configuration (data chunks : parity chunks)
declare -A TEST_CASES=(
    # 1块丢失
    ["1data-0parity"]="1:0"
    ["0data-1parity"]="0:1"
    # 2块丢失
    ["2data-0parity"]="2:0"
    ["1data-1parity"]="1:1"
    ["0data-2parity"]="0:2"
    # 3块丢失
    ["3data-0parity"]="3:0"
    ["2data-1parity"]="2:1"
    ["1data-2parity"]="1:2"
    ["0data-3parity"]="0:3"
)
ORDERED_CASES=(
    "1data-0parity"
    "0data-1parity"
    "2data-0parity"
    "1data-1parity"
    "0data-2parity"
    "3data-0parity"
    "2data-1parity"
    "1data-2parity"
    "0data-3parity"
)

# Calculate test distribution
NUM_CASES=${#TEST_CASES[@]}
PER_CASE_COUNT=$(( (TEST_COUNT + NUM_CASES - 1) / NUM_CASES ))

# Print test configuration
echo ""
echo -n "----------------------------------------------------------"
echo "Test Configuration:"
echo "  Chunk size:      ${CHUNK_SIZE_KB}KB"
echo "  Buffer size:     $((CHUNK_SIZE_KB * K))KB"
echo "  Total tests:     $TEST_COUNT"
echo "  Per-case tests:  $PER_CASE_COUNT"
echo "  Erasure scheme:  K=${K}, M=${M}, Total chunks=${TOTAL_CHUNKS}"
echo "Test Cases:"
for case in "${!TEST_CASES[@]}"; do
    IFS=':' read -r d p <<< "${TEST_CASES[$case]}"
    echo "  $case - Missing: ${d} data chunk(s) + ${p} parity chunk(s)"
done

# Define plugin names and their technique parameters
# PLUGINS=("tsx" "isa" "jerasure" "tsxnaive" "tsxmap" "tsxwin" "tsxmerge")
# TECHNIQUES=("null" "cauchy" "cauchy_good" "null" "null" "null" "null")
PLUGINS=("tsx" "tsxmerge")
TECHNIQUES=("null" "null")


# Test execution function
run_test() {
    local description="$1"
    local command="$2"
    local is_decode=${3:-0}
    local throughput_variable="$4"  # Optional variable to store result
    
    if [ $is_decode -eq 1 ]; then
        echo "[DECODE] $description..."
    else
        echo "[ENCODE] $description..."
    fi
    
    local output
    output=$($command 2>&1)
    echo "$output"
    local time=$(echo "$output" | grep -oP '^\d+\.\d+')
    local total_kb=$(echo "$output" | grep -oP '\d+$')

    if [[ -n "$time" && -n "$total_kb" ]]; then
        local throughput
        throughput=$(python3 -c "print(round($total_kb / 1024 / $time, 2))")
        echo "Time: ${time}s | Throughput: ${throughput} MB/s"
        
        # Store result if variable name provided
        if [ -n "$throughput_variable" ]; then
            eval "$throughput_variable=$throughput"
        fi
    else
        echo "FAILED: Could not parse output"
    fi
}

# Generate random erasure indexes
select_erased_indexes() {
    local data_to_erase=$1
    local parity_to_erase=$2
    local erased_indexes=()
    
    # Select random data chunks (0 to k-1)
    if [ $data_to_erase -gt 0 ]; then
        mapfile -t data_indexes < <(shuf -i 0-$((K-1)) -n $data_to_erase)
        erased_indexes+=("${data_indexes[@]}")
    fi

    # Select random parity chunks (k to k+m-1)
    if [ $parity_to_erase -gt 0 ]; then
        mapfile -t parity_indexes < <(shuf -i $K-$((TOTAL_CHUNKS-1)) -n $parity_to_erase)
        erased_indexes+=("${parity_indexes[@]}")
    fi

    # Format as --erased arguments
    local erased_args=""
    for idx in "${erased_indexes[@]}"; do
        erased_args+=" --erased $idx"
    done
    
    echo "$erased_args"
}

# Warmup run to load libraries
warmup_test() {
    local erased_args=$(select_erased_indexes 1 2)  # Fixed pattern for warmup
    echo "Running warmup tests..."
    for i in "${!PLUGINS[@]}"; do
        plugin="${PLUGINS[$i]}"
        technique="${TECHNIQUES[$i]}"
        cmd="$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p $plugin"
        if [ "$technique" != "null" ]; then
            cmd+=" -P technique=$technique"
        fi
        cmd+=" -P k=$K -P m=$M -s $BUFFER_SIZE -i 2"
        $cmd > /dev/null
        cmd="$CEPH_BUILD/bin/ceph_erasure_code_benchmark -e 3 -w decode -p $plugin"
        if [ "$technique" != "null" ]; then
            cmd+=" -P technique=$technique"
        fi
        cmd+=" -P k=$K -P m=$M -s $BUFFER_SIZE -i 2 $erased_args"
        $cmd > /dev/null
    done
}

warmup_test

# 初始化编码性能变量
declare -A encode_results
declare -A decode_total

for plugin in "${PLUGINS[@]}"; do
    encode_results[$plugin]=0
    decode_total[$plugin]=0
done

# Encoding performance tests
echo -e "\n----------------------------------------------------------"
echo "ENCODING PERFORMANCE TESTS"
for i in "${!PLUGINS[@]}"; do
    plugin="${PLUGINS[$i]}"
    technique="${TECHNIQUES[$i]}"
    desc="$plugin"
    cmd="$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p $plugin"
    if [ "$technique" != "null" ]; then
        cmd+=" -P technique=$technique"
    fi
    cmd+=" -P k=$K -P m=$M -s $BUFFER_SIZE -i $TEST_COUNT"
    run_test "$desc" "$cmd" 0 "encode_results[$plugin]"
done

# Decoding performance tests
echo -e "\n----------------------------------------------------------"
echo "DECODING PERFORMANCE TESTS"
echo "Running all test cases for each decoder..."

tested_cases=0

for case in "${ORDERED_CASES[@]}"; do
    echo "Processing '$case' case... "
    IFS=':' read -r data_chunks parity_chunks <<< "${TEST_CASES[$case]}"
    total_erased=$((data_chunks + parity_chunks))
    erased_args=$(select_erased_indexes $data_chunks $parity_chunks)
    for i in "${!PLUGINS[@]}"; do
        plugin="${PLUGINS[$i]}"
        technique="${TECHNIQUES[$i]}"
        thpt=0
        desc="$plugin"
        cmd="$CEPH_BUILD/bin/ceph_erasure_code_benchmark -e $total_erased -w decode -p $plugin"
        if [ "$technique" != "null" ]; then
            cmd+=" -P technique=$technique"
        fi
        cmd+=" -P k=$K -P m=$M -s $BUFFER_SIZE -i $PER_CASE_COUNT $erased_args"
        run_test "$desc" "$cmd" 1 thpt
        decode_total[$plugin]=$(python3 -c "print(${decode_total[$plugin]} + $thpt)")
    done
    ((tested_cases++))
    echo "Completed '$case' case"
done

# Calculate decode averages
declare -A decode_avg
if [ $tested_cases -gt 0 ]; then
    for plugin in "${PLUGINS[@]}"; do
        decode_avg[$plugin]=$(python3 -c "print(round(${decode_total[$plugin]} / $tested_cases, 2))")
    done
fi

# Display performance summary
echo "----------------------------------------------------------"
echo -e "\nPERFORMANCE SUMMARY"
echo "ENCODING RESULTS:"
for plugin in "${PLUGINS[@]}"; do
    echo "  $plugin: ${encode_results[$plugin]} MB/s"
done
echo ""
echo "DECODING RESULTS (Average of $tested_cases cases):"
for plugin in "${PLUGINS[@]}"; do
    echo "  $plugin: ${decode_avg[$plugin]} MB/s"
done

# Return to original directory
cd "$current_dir" || exit
echo -e "\nAll tests completed successfully!"