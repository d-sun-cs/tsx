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

ninja vstart 

# $CEPH_BUILD/bin/init-ceph stop 
$CEPH_SRC/stop.sh 
rm -rf out dev
# env OSD=11 $CEPH_SRC/vstart.sh -d -n 
env OSD=11 $CEPH_SRC/vstart.sh -n 

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
    $CEPH_BUILD/bin/ceph osd erasure-code-profile set tsx-${BLOCK_SIZE}-profile plugin=tsx crush-failure-domain=osd stripe_unit=$STRIPE_UNIT 
    $CEPH_BUILD/bin/ceph osd pool create tsx-${BLOCK_SIZE}-pool erasure tsx-${BLOCK_SIZE}-profile 
    $CEPH_BUILD/bin/ceph osd pool set tsx-${BLOCK_SIZE}-pool min_size 8 

    # 创建 tsxse 配置
    $CEPH_BUILD/bin/ceph osd erasure-code-profile set tsxse-${BLOCK_SIZE}-profile plugin=tsxse crush-failure-domain=osd stripe_unit=$STRIPE_UNIT 
    $CEPH_BUILD/bin/ceph osd pool create tsxse-${BLOCK_SIZE}-pool erasure tsxse-${BLOCK_SIZE}-profile 
    $CEPH_BUILD/bin/ceph osd pool set tsxse-${BLOCK_SIZE}-pool min_size 8 

    # 创建 isa 配置
    $CEPH_BUILD/bin/ceph osd erasure-code-profile set isa-${BLOCK_SIZE}-profile plugin=isa k=8 m=3 crush-failure-domain=osd stripe_unit=$STRIPE_UNIT 
    $CEPH_BUILD/bin/ceph osd pool create isa-${BLOCK_SIZE}-pool erasure isa-${BLOCK_SIZE}-profile 
    $CEPH_BUILD/bin/ceph osd pool set isa-${BLOCK_SIZE}-pool min_size 8 
done

$CEPH_BUILD/bin/ceph config set global osd_max_object_size 3G 

# dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=16MiB count=64
dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=8MiB count=32
# dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=64KiB count=1

cd $current_dir