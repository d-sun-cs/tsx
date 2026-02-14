#!/bin/bash

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
   echo "please change directory to ceph/src/erasure-code/tsx to run this script"
   exit 1
fi

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    echo "Usage: $0 [k] [m] [block-size]"
    echo "  k: data chunks (default: 8)"
    echo "  m: parity chunks (default: 3)"
    echo "  block-size: e.g. 2m, 512k, 4096 (default: 2m; no unit => k)"
    exit 0
fi

# 可选参数：K、M、块大小
K=${1:-8}
M=${2:-3}
BLOCK_SIZE_INPUT=${3:-2m}

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

tsx_plugin="tsx"
if [ "$M" -eq 2 ]; then
    tsx_plugin="tsx2"
elif [ "$M" -eq 4 ]; then
    tsx_plugin="tsx4"
fi

# 定义纠删码池配置
ERASURE_CODE_POOLS=("$tsx_plugin" "isa" "jerasure")

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

BLOCK_SIZE=$(normalize_block_size "$BLOCK_SIZE_INPUT")
STRIPE_UNIT=$(echo "$BLOCK_SIZE" | tr -d 'kKmM')  # 提取数值部分
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

$CEPH_BUILD/bin/ceph config set global osd_max_object_size 3G 

rm -f $CEPH_BUILD/largefile
# dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=16MiB count=64
# dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=8MiB count=32
# dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=64KiB count=1

cd $current_dir