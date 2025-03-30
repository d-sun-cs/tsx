#!/bin/bash

# 参数校验
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <pool-prefix> <test-count>"
    exit 1
fi

POOL_PREFIX=$1
TEST_COUNT=$2
POOL_NAME="${POOL_PREFIX}-pool"  # 自动拼接 -pool

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
    echo "please change directory to ceph/src/erasure-code/tsx to run this script"
    exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

cd $CEPH_BUILD

if $CEPH_BUILD/bin/ceph osd tree | grep -q "down"; then
    echo "Error: Found 'down' status in OSD tree. Running restart-osds.sh and exiting."
    echo "Please run this script again after the OSDs have restarted."
    cd $current_dir
    ./restart-osds.sh
    exit 1
fi

file_size=$(stat -c%s /tmp/largefile)
total_put_time=0
total_get_time=0

# 写测试
for ((i = 1; i <= TEST_COUNT; i++)); do
    $CEPH_BUILD/bin/rados cleanup -p $POOL_NAME  # 清空存储池
    start_time=$(date +%s.%N)
    $CEPH_BUILD/bin/rados put largefile /tmp/largefile -p $POOL_NAME
    end_time=$(date +%s.%N)
    put_time=$(echo "$end_time - $start_time" | bc)
    total_put_time=$(echo "$total_put_time + $put_time" | bc)
done

avg_put_time=$(echo "$total_put_time / $TEST_COUNT" | bc -l)
put_speed=$(echo "$file_size / $avg_put_time / 1024 / 1024" | bc -l)

$CEPH_BUILD/bin/rados ls -p $POOL_NAME
$CEPH_BUILD/bin/ceph osd map $POOL_NAME largefile 

osd_map_output=$($CEPH_BUILD/bin/ceph osd map $POOL_NAME largefile)
osd_ids=($(echo "$osd_map_output" | grep -oP '\[.*?\]' | tr -d '[]' | tr ',' ' '))
osd_to_stop=(${osd_ids[5]} ${osd_ids[6]} ${osd_ids[7]})

for osd_id in "${osd_to_stop[@]}"; do
    $CEPH_BUILD/bin/ceph osd stop osd.$osd_id
done

# 读测试
for ((i = 1; i <= TEST_COUNT; i++)); do
    rm -f $CEPH_BUILD/mylargefile  # 清空本地文件
    start_time=$(date +%s.%N)
    $CEPH_BUILD/bin/rados get largefile mylargefile -p $POOL_NAME
    end_time=$(date +%s.%N)
    get_time=$(echo "$end_time - $start_time" | bc)
    total_get_time=$(echo "$total_get_time + $get_time" | bc)
done

avg_get_time=$(echo "$total_get_time / $TEST_COUNT" | bc -l)
get_speed=$(echo "$file_size / $avg_get_time / 1024 / 1024" | bc -l)

for osd_id in "${osd_to_stop[@]}"; do
    until ! $CEPH_BUILD/bin/ceph osd tree | grep "osd.$osd_id " | grep -q "down"; do
        $CEPH_BUILD/bin/ceph-osd -i $osd_id --cluster ceph
        sleep 1
    done
done

diff $CEPH_BUILD/mylargefile /tmp/largefile
if [ $? -ne 0 ]; then
    echo "[${POOL_PREFIX}] Test failed! The files largefile and mylargefile are not identical!"
    exit 1
fi

$CEPH_BUILD/bin/rados cleanup -p $POOL_NAME
rm $CEPH_BUILD/mylargefile

echo "[${POOL_PREFIX}] Test passed! The files are identical!"

echo "File Size: $(echo "$file_size / 1024 / 1024" | bc -l) MB"
echo "Write Time (avg of $TEST_COUNT runs): $avg_put_time seconds"
echo "Write Speed: $put_speed MB/s"
echo "Read Time (avg of $TEST_COUNT runs): $avg_get_time seconds"
echo "Read Speed: $get_speed MB/s"

cd $current_dir
