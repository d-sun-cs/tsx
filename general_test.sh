#!/bin/bash

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    echo "Usage: $0 [plugin] [k] [m] [block-size] [test-count]"
    echo "  plugin: tsx, tsx2, tsx4, isa, jerasure (default: tsx)"
    echo "  k: data chunks (default: 8)"
    echo "  m: parity chunks (default: 3)"
    echo "  block-size: e.g. 2m, 512k, 4096 (default: 2m; no unit => k)"
    echo "  test-count: iterations (default: 10)"
    exit 0
fi

# 参数：plugin [k] [m] [block-size] [test-count]
PLUGIN=${1:-tsx}
K=${2:-8}
M=${3:-3}
BLOCK_SIZE_INPUT=${4:-2m}
TEST_COUNT=${5:-10}

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

POOL_PREFIX="${PLUGIN}-${BLOCK_SIZE}"
POOL_NAME="${POOL_PREFIX}-pool"  # 自动拼接 -pool

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
    echo "please change directory to ceph/src/erasure-code/tsx to run this script"
    exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

cd $CEPH_BUILD

if $CEPH_BUILD/bin/ceph osd tree  | grep -q "down"; then
    echo "Error: Found 'down' status in OSD tree. Running restart-osds.sh and exiting."
    echo "Please run this script again after the OSDs have restarted."
    cd $current_dir
    ./restart-osds.sh
    exit 1
fi

# dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=32MiB count=64 oflag=direct
dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=32MiB count=64 

file_size=$(stat -c%s $CEPH_BUILD/largefile)
total_put_time=0
total_get_time=0

# 写测试
for ((i = 1; i <= TEST_COUNT; i++)); do
    $CEPH_BUILD/bin/rados rm largefile -p $POOL_NAME  # 删除之前的测试大文件
    rm -f $CEPH_BUILD/largefile # 删除本地大文件
    # dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=32MiB count=64 oflag=direct # 新建大文件
    dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=32MiB count=64
    start_time=$(date +%s.%N)
    $CEPH_BUILD/bin/rados put largefile $CEPH_BUILD/largefile -p $POOL_NAME
    end_time=$(date +%s.%N)
    put_time=$(python3 -c "print($end_time - $start_time)")
    total_put_time=$(python3 -c "print($total_put_time + $put_time)")
done

avg_put_time=$(python3 -c "print($total_put_time / $TEST_COUNT)")
put_speed=$(python3 -c "print(($file_size / $avg_put_time) / 1024 / 1024)")

$CEPH_BUILD/bin/rados ls -p $POOL_NAME
$CEPH_BUILD/bin/ceph osd map $POOL_NAME largefile

osd_map_output=$($CEPH_BUILD/bin/ceph osd map $POOL_NAME largefile)
osd_ids=($(echo "$osd_map_output" | grep -oP '\[.*?\]' | tr -d '[]' | tr ',' ' '))
osd_to_stop=(${osd_ids[7]} ${osd_ids[9]} ${osd_ids[8]})
# osd_to_stop=(${osd_ids[0]}})

for osd_id in "${osd_to_stop[@]}"; do
    $CEPH_BUILD/bin/init-ceph stop osd.$osd_id
done

# 读测试
for ((i = 1; i <= TEST_COUNT; i++)); do
    rm -f $CEPH_BUILD/my_largefile  # 清空本地文件
    start_time=$(date +%s.%N)
    $CEPH_BUILD/bin/rados get largefile $CEPH_BUILD/my_largefile -p $POOL_NAME
    end_time=$(date +%s.%N)
    get_time=$(python3 -c "print($end_time - $start_time)")
    total_get_time=$(python3 -c "print($total_get_time + $get_time)")
done

avg_get_time=$(python3 -c "print($total_get_time / $TEST_COUNT)")
get_speed=$(python3 -c "print(($file_size / $avg_get_time) / 1024 / 1024)")

# 恢复之前停止的 OSD
for osd_id in "${osd_to_stop[@]}"; do
    $CEPH_BUILD/bin/init-ceph start osd.$osd_id
done

# 校验文件完整性
diff $CEPH_BUILD/largefile $CEPH_BUILD/my_largefile
if [ $? -ne 0 ]; then
    echo "[${POOL_PREFIX}] Test failed! The files largefile and my_largefile are not identical!"
    exit 1
fi

$CEPH_BUILD/bin/rados rm largefile -p $POOL_NAME
rm -f $CEPH_BUILD/my_largefile

echo "[${POOL_PREFIX}] Test passed! The files are identical!"

echo "File Size: $(python3 -c "print($file_size / 1024 / 1024)") MB"
echo "Write Time (avg of $TEST_COUNT runs): $avg_put_time seconds"
echo "Write Speed: $put_speed MB/s"
echo "Read Time (avg of $TEST_COUNT runs): $avg_get_time seconds"
echo "Read Speed: $get_speed MB/s"

cd $current_dir