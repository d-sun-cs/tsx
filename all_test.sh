#!/bin/bash

# 参数校验
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <blocksize> <test-count>"
    exit 1
fi

BLOCKSIZE=$1
TEST_COUNT=$2

# 存储池配置
pool_cnt=3
pool_prefix_arr=(jerasure tsx isa)

# 生成存储池全称数组
for ((i=0; i<pool_cnt; i++)); do
    pool_name_arr[i]="${pool_prefix_arr[i]}-${BLOCKSIZE}-pool"
done

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

# 初始化计时数组
for ((i=0; i<pool_cnt; i++)); do
    total_put_time_arr[i]=0
    total_get_time_arr[i]=0
done

# 依次对每个存储池进行测试
for ((pool_idx=0; pool_idx<pool_cnt; pool_idx++)); do
    POOL_NAME=${pool_name_arr[pool_idx]}
    # 写测试
    for ((i = 1; i <= TEST_COUNT; i++)); do
        $CEPH_BUILD/bin/rados rm largefile -p $POOL_NAME  # 删除之前的测试大文件
        rm -f $CEPH_BUILD/largefile # 删除本地大文件
        dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=32MiB count=64 
        start_time=$(date +%s.%N)
        $CEPH_BUILD/bin/rados put largefile $CEPH_BUILD/largefile -p $POOL_NAME
        end_time=$(date +%s.%N)
        put_time=$(python3 -c "print($end_time - $start_time)")
        total_put_time_arr[pool_idx]=$(python3 -c "print(${total_put_time_arr[pool_idx]} + $put_time)")
    done

    avg_put_time_arr[pool_idx]=$(python3 -c "print(${total_put_time_arr[pool_idx]} / $TEST_COUNT)")
    put_speed_arr[pool_idx]=$(python3 -c "print(($file_size / ${avg_put_time_arr[pool_idx]}) / 1024 / 1024)")

    $CEPH_BUILD/bin/rados ls -p $POOL_NAME
    $CEPH_BUILD/bin/ceph osd map $POOL_NAME largefile

    osd_map_output=$($CEPH_BUILD/bin/ceph osd map $POOL_NAME largefile)
    osd_ids=($(echo "$osd_map_output" | grep -oP '\[.*?\]' | tr -d '[]' | tr ',' ' '))
    # osd_to_stop=(${osd_ids[2]} ${osd_ids[9]})
    osd_to_stop=(${osd_ids[1]} ${osd_ids[5]} ${osd_ids[9]})
    # osd_to_stop_arr[pool_idx]="${osd_to_stop[@]}"

    # 停止对应osd
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
        total_get_time_arr[pool_idx]=$(python3 -c "print(${total_get_time_arr[pool_idx]} + $get_time)")
    done

    avg_get_time_arr[pool_idx]=$(python3 -c "print(${total_get_time_arr[pool_idx]} / $TEST_COUNT)")
    get_speed_arr[pool_idx]=$(python3 -c "print(($file_size / ${avg_get_time_arr[pool_idx]}) / 1024 / 1024)")

    # 恢复之前停止的 OSD
    for osd_id in "${osd_to_stop[@]}"; do
        $CEPH_BUILD/bin/init-ceph start osd.$osd_id
    done

    # 校验文件完整性
    diff $CEPH_BUILD/largefile $CEPH_BUILD/my_largefile
    if [ $? -ne 0 ]; then
        echo "[${pool_prefix_arr[pool_idx]}] Test failed! The files largefile and my_largefile are not identical!"
        exit 1
    fi

    $CEPH_BUILD/bin/rados rm largefile -p $POOL_NAME
    rm -f $CEPH_BUILD/my_largefile
done

# 输出所有存储池的结果
for ((pool_idx=0; pool_idx<pool_cnt; pool_idx++)); do
    echo "[${pool_prefix_arr[pool_idx]}] Test passed! The files are identical!"
    echo "File Size: $(python3 -c "print($file_size / 1024 / 1024)") MB"
    echo "Write Time (avg of $TEST_COUNT runs): ${avg_put_time_arr[pool_idx]} seconds"
    echo "Write Speed: ${put_speed_arr[pool_idx]} MB/s"
    echo "Read Time (avg of $TEST_COUNT runs): ${avg_get_time_arr[pool_idx]} seconds"
    echo "Read Speed: ${get_speed_arr[pool_idx]} MB/s"
done

cd $current_dir