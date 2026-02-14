#!/bin/bash

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    echo "Usage: $0 [k] [m] [block-size] [test-count]"
    echo "  k: data chunks (default: 8)"
    echo "  m: parity chunks (default: 3)"
    echo "  block-size: e.g. 2m, 512k, 4096 (default: 2m; no unit => k)"
    echo "  test-count: iterations for general_test.sh (default: 10)"
    exit 0
fi

K=${1:-8}
M=${2:-3}
BLOCK_SIZE_INPUT=${3:-2m}
TEST_COUNT=${4:-10}

normalize_block_size() {
    local size_raw="$1"
    local size_norm
    if [[ "$size_raw" =~ [kKmM]$ ]]; then
        size_norm=$(echo "$size_raw" | tr 'KM' 'km')
    else
        size_norm="${size_raw}k"
    fi
    echo "$size_norm"
}

BLOCK_SIZE=$(normalize_block_size "$BLOCK_SIZE_INPUT")

tsx_plugin="tsx"
if [ "$M" -eq 2 ]; then
    tsx_plugin="tsx2"
elif [ "$M" -eq 4 ]; then
    tsx_plugin="tsx4"
fi

OUTPUT_FILE="/ceph/src/erasure-code/tsx/output.txt"

echo "Running vstart.sh with K=$K M=$M BLOCK_SIZE=$BLOCK_SIZE..."
bash vstart.sh "$K" "$M" "$BLOCK_SIZE"

echo "Running general_test.sh for $tsx_plugin, isa, and jerasure..."
echo "Output will be written to $OUTPUT_FILE"
echo "" > "$OUTPUT_FILE"

echo "===========================================================" >> "$OUTPUT_FILE"
echo "Test Configuration: K=$K M=$M BLOCK_SIZE=$BLOCK_SIZE TEST_COUNT=$TEST_COUNT" >> "$OUTPUT_FILE"
echo "===========================================================" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"

echo "Running $tsx_plugin test..."
echo "--- Testing $tsx_plugin ---" >> "$OUTPUT_FILE"
bash general_test.sh "$tsx_plugin" "$K" "$M" "$BLOCK_SIZE" "$TEST_COUNT" >> "$OUTPUT_FILE" 2>&1
echo "" >> "$OUTPUT_FILE"

echo "Running isa test..."
echo "--- Testing isa ---" >> "$OUTPUT_FILE"
bash general_test.sh isa "$K" "$M" "$BLOCK_SIZE" "$TEST_COUNT" >> "$OUTPUT_FILE" 2>&1
echo "" >> "$OUTPUT_FILE"

echo "Running jerasure test..."
echo "--- Testing jerasure ---" >> "$OUTPUT_FILE"
bash general_test.sh jerasure "$K" "$M" "$BLOCK_SIZE" "$TEST_COUNT" >> "$OUTPUT_FILE" 2>&1
echo "" >> "$OUTPUT_FILE"

echo "All tests completed. Results saved to $OUTPUT_FILE"
