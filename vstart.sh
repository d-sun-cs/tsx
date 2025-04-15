#!/bin/bash

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
   echo "please change directory to ceph/src/erasure-code/tsx to run this script"
   exit 1
fi

# 检查是否提供了块大小参数
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <block-size1> <block-size2> ... <block-sizeN>"
    exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)
CEPH_SRC=$(cd ../.. && pwd)

cd $CEPH_BUILD

ninja vstart 2>/dev/null

$CEPH_BUILD/bin/init-ceph stop 2>/dev/null
$CEPH_SRC/stop.sh 2>/dev/null
env OSD=11 $CEPH_SRC/vstart.sh -d -n 2>/dev/null

# 从命令行读取块大小参数
BLOCK_SIZES=("$@")

# 循环创建 isa 和 tsx 纠删码配置
for BLOCK_SIZE in "${BLOCK_SIZES[@]}"; do
    STRIPE_UNIT=$(echo $BLOCK_SIZE | tr -d 'kKmM')  # 提取数值部分
    if [[ $BLOCK_SIZE == *k* ]]; then
        STRIPE_UNIT="${STRIPE_UNIT}K"
    elif [[ $BLOCK_SIZE == *m* ]]; then
        STRIPE_UNIT="${STRIPE_UNIT}M"
    fi

    # 创建 tsx 配置
    $CEPH_BUILD/bin/ceph osd erasure-code-profile set tsx-${BLOCK_SIZE}-profile plugin=tsx crush-failure-domain=osd stripe_unit=$STRIPE_UNIT 2>/dev/null
    $CEPH_BUILD/bin/ceph osd pool create tsx-${BLOCK_SIZE}-pool erasure tsx-${BLOCK_SIZE}-profile 2>/dev/null
    $CEPH_BUILD/bin/ceph osd pool set tsx-${BLOCK_SIZE}-pool min_size 8 2>/dev/null

    # 创建 isa 配置
    $CEPH_BUILD/bin/ceph osd erasure-code-profile set isa-${BLOCK_SIZE}-profile plugin=isa k=8 m=3 crush-failure-domain=osd stripe_unit=$STRIPE_UNIT 2>/dev/null
    $CEPH_BUILD/bin/ceph osd pool create isa-${BLOCK_SIZE}-pool erasure isa-${BLOCK_SIZE}-profile 2>/dev/null
    $CEPH_BUILD/bin/ceph osd pool set isa-${BLOCK_SIZE}-pool min_size 8 2>/dev/null
done

$CEPH_BUILD/bin/ceph config set global osd_max_object_size 3G 2>/dev/null

dd if=/dev/zero of=$CEPH_BUILD/largefile bs=1GiB count=1

cd $current_dir