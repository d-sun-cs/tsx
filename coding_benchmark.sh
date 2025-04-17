#!/bin/bash

# 参数校验
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <block-size-in-KB> <test-count>"
    exit 1
fi

BLOCK_SIZE_KB=$1
TEST_COUNT=$2

# 将块大小从 KB 转换为 B
BLOCK_SIZE=$((BLOCK_SIZE_KB * 1024))

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
    echo "please change directory to ceph/src/erasure-code/tsx to run this script"
    exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

cd $CEPH_BUILD

ninja_output_ec_tsx=$(ninja ec_tsx)
ninja_output_benchmark=$(ninja ceph_erasure_code_benchmark)

if [[ "$ninja_output_ec_tsx" != *"no work to do."* || "$ninja_output_benchmark" != *"no work to do."* ]]; then
    sudo ninja install
fi

# 测试命令

$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p isa -P k=8 -P m=3 -s $BLOCK_SIZE -i $TEST_COUNT
$CEPH_BUILD/bin/ceph_erasure_code_benchmark -e 3 -w decode -p isa -P k=8 -P m=3 -s $BLOCK_SIZE -i $TEST_COUNT --erased 5 --erased 6 --erased 7

$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p jerasure -P k=8 -P m=3 -P technique=reed_sol_van -s $BLOCK_SIZE -i $TEST_COUNT
$CEPH_BUILD/bin/ceph_erasure_code_benchmark -e 3 -w decode -p jerasure -P k=8 -P m=3 -P technique=reed_sol_van -s $BLOCK_SIZE -i $TEST_COUNT --erased 5 --erased 6 --erased 7

$CEPH_BUILD/bin/ceph_erasure_code_benchmark -p tsx -P k=8 -P m=3 -s $BLOCK_SIZE -i $TEST_COUNT
$CEPH_BUILD/bin/ceph_erasure_code_benchmark -e 3 -w decode -p tsx -P k=8 -P m=3 -s $BLOCK_SIZE -i $TEST_COUNT --erased 5 --erased 6 --erased 7


cd $current_dir

echo "All tests completed successfully!"