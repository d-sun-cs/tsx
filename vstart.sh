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

# 定义纠删码池配置
ERASURE_CODE_POOLS=("tsx" "isa" "jerasure")
K=8
M=3

CEPH_BUILD=$(cd ../../../build && pwd)
CEPH_SRC=$(cd ../.. && pwd)

cd $CEPH_BUILD

ninja vstart 

# $CEPH_BUILD/bin/init-ceph stop 
$CEPH_SRC/stop.sh 
rm -rf out dev
# env OSD=11 $CEPH_SRC/vstart.sh -d -n 
env OSD=$((K + M)) $CEPH_SRC/vstart.sh -n -X --memstore
# env OSD=11 $CEPH_SRC/vstart.sh -n -X

# 从命令行读取块大小参数
BLOCK_SIZES=("$@")

# 循环创建纠删码配置
for BLOCK_SIZE in "${BLOCK_SIZES[@]}"; do
    STRIPE_UNIT=$(echo $BLOCK_SIZE | tr -d 'kKmM')  # 提取数值部分
    if [[ $BLOCK_SIZE == *k* ]]; then
        STRIPE_UNIT="${STRIPE_UNIT}K"
    elif [[ $BLOCK_SIZE == *m* ]]; then
        STRIPE_UNIT="${STRIPE_UNIT}M"
    fi

    for POOL in "${ERASURE_CODE_POOLS[@]}"; do
        $CEPH_BUILD/bin/ceph osd erasure-code-profile set ${POOL}-${BLOCK_SIZE}-profile plugin=${POOL} k=$K m=$M crush-failure-domain=osd stripe_unit=$STRIPE_UNIT 
        $CEPH_BUILD/bin/ceph osd pool create ${POOL}-${BLOCK_SIZE}-pool erasure ${POOL}-${BLOCK_SIZE}-profile 
        $CEPH_BUILD/bin/ceph osd pool set ${POOL}-${BLOCK_SIZE}-pool min_size $K 
    done
done

$CEPH_BUILD/bin/ceph config set global osd_max_object_size 3G 

rm -f $CEPH_BUILD/largefile
# dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=16MiB count=64
# dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=8MiB count=32
# dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=64KiB count=1

cd $current_dir