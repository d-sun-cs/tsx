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
pool_prefix_arr=(jerasure isa tsx)
# 定义9种OSD丢失情况 (数据块:校验块)
declare -A ERASURE_CASES=(
    ["1data-0parity"]="1:0"
    ["0data-1parity"]="0:1"
    ["2data-0parity"]="2:0"
    ["1data-1parity"]="1:1"
    ["0data-2parity"]="0:2"
    ["3data-0parity"]="3:0"
    ["2data-1parity"]="2:1"
    ["1data-2parity"]="1:2"
    ["0data-3parity"]="0:3"
)
ORDERED_CASES=(
    "3data-0parity"
    "2data-0parity"
    "1data-1parity"
    "0data-2parity"
    "1data-0parity"
    "0data-1parity"
    "2data-1parity"
    "1data-2parity"
    "0data-3parity"
)
NUM_CASES=${#ORDERED_CASES[@]}
PER_CASE_COUNT=$(( (TEST_COUNT + NUM_CASES - 1) / NUM_CASES ))  # 向上取整

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

# 创建测试文件
dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=32MiB count=96
file_size=$(stat -c%s $CEPH_BUILD/largefile)

# 初始化计时数组
for ((i=0; i<pool_cnt; i++)); do
    total_put_time_arr[i]=0
    total_get_time_arr[i]=0
    # declare -a "get_time_${i}=()"
done

# 函数：随机选择OSD停止
select_erased_osds() {
    local k=8 m=3
    local data_to_erase=$1
    local parity_to_erase=$2
    local osd_list=("${@:3}")
    local erased_osds=()
    
    printf "DEBUG: osd_list: %s\n" "${osd_list[*]}" >&2

    # 随机选择数据块OSD (前8个)
    if [ $data_to_erase -gt 0 ]; then
        mapfile -t data_osds < <(shuf -e "${osd_list[@]:0:$k}" -n $data_to_erase)
        erased_osds+=("${data_osds[@]}")
    fi

    # 随机选择校验块OSD (后3个)
    if [ $parity_to_erase -gt 0 ]; then
        mapfile -t parity_osds < <(shuf -e "${osd_list[@]:$k:$m}" -n $parity_to_erase)
        erased_osds+=("${parity_osds[@]}")
    fi

    printf "DEBUG: erased_osds: %s\n" "${erased_osds[*]}" >&2

    echo "${erased_osds[@]}"
}

# 依次对每个存储池进行测试
for ((pool_idx=0; pool_idx<pool_cnt; pool_idx++)); do
    POOL_NAME=${pool_name_arr[pool_idx]}
    
    # ===== 写测试 (保持不变) =====
    for ((i = 1; i <= TEST_COUNT; i++)); do
        $CEPH_BUILD/bin/rados rm largefile -p $POOL_NAME
        rm -f $CEPH_BUILD/largefile
        dd if=/dev/urandom of=$CEPH_BUILD/largefile bs=32MiB count=96
        start_time=$(date +%s.%N)
        $CEPH_BUILD/bin/rados put largefile $CEPH_BUILD/largefile -p $POOL_NAME
        end_time=$(date +%s.%N)
        put_time=$(python3 -c "print($end_time - $start_time)")
        total_put_time_arr[pool_idx]=$(python3 -c "print(${total_put_time_arr[pool_idx]} + $put_time)")
    done

    avg_put_time_arr[pool_idx]=$(python3 -c "print(${total_put_time_arr[pool_idx]} / $TEST_COUNT)")
    put_speed_arr[pool_idx]=$(python3 -c "print(($file_size / ${avg_put_time_arr[pool_idx]}) / 1024 / 1024)")

    # ===== 获取OSD映射 =====
    $CEPH_BUILD/bin/rados ls -p $POOL_NAME
    osd_map_output=$($CEPH_BUILD/bin/ceph osd map $POOL_NAME largefile)
    echo "$osd_map_output"
    osd_ids=($(echo "$osd_map_output" | grep -oP '\[.*?\]' | tr -d '[]' | tr ',' ' '))
    osd_ids=("${osd_ids[@]:0:11}")
    echo "${osd_ids[@]}"
    
    # ===== 读测试 (扩展为9种情况) =====
    case_get_times=0
    for case in "${ORDERED_CASES[@]}"; do
        IFS=':' read -r data_chunks parity_chunks <<< "${ERASURE_CASES[$case]}"
        echo -e "\nTesting ${POOL_NAME} case: $case (Erase: $data_chunks data + $parity_chunks parity)"
        

        # 随机选择要停止的OSD
        osd_to_stop=($(select_erased_osds $data_chunks $parity_chunks "${osd_ids[@]}"))
        echo "${osd_to_stop[@]}"
        # 停止OSD
        for osd_id in "${osd_to_stop[@]}"; do
            $CEPH_BUILD/bin/init-ceph stop osd.$osd_id 
        done
            
        for ((i = 1; i <= PER_CASE_COUNT; i++)); do
            # 执行读测试
            rm -f $CEPH_BUILD/my_largefile
            start_time=$(date +%s.%N)
            $CEPH_BUILD/bin/rados get largefile $CEPH_BUILD/my_largefile -p $POOL_NAME
            end_time=$(date +%s.%N)
            get_time=$(python3 -c "print($end_time - $start_time)")
            
            # 记录结果
            total_get_time_arr[pool_idx]=$(python3 -c "print(${total_get_time_arr[pool_idx]} + $get_time)")
            # eval "get_time_${pool_idx}+=($get_time)"
            ((case_get_times++))
        done

                    
        # 恢复OSD
        for osd_id in "${osd_to_stop[@]}"; do
            $CEPH_BUILD/bin/init-ceph restart osd.$osd_id 
        done
        
        # 文件校验
        diff $CEPH_BUILD/largefile $CEPH_BUILD/my_largefile || {
            echo "[ERROR] Files differ in case $case! Stopping test."
            exit 1
        }
    done

    # ===== 计算读性能 =====
    avg_get_time_arr[pool_idx]=$(python3 -c "print(${total_get_time_arr[pool_idx]} / $case_get_times)")
    get_speed_arr[pool_idx]=$(python3 -c "print(($file_size / ${avg_get_time_arr[pool_idx]}) / 1024 / 1024)")

    # ===== 清理 =====
    $CEPH_BUILD/bin/rados rm largefile -p $POOL_NAME
    rm -f $CEPH_BUILD/my_largefile
done

# ===== 结果输出 =====
echo -e "\n\n======================== TEST RESULTS ========================"
for ((pool_idx=0; pool_idx<pool_cnt; pool_idx++)); do
    echo -e "\n[${pool_prefix_arr[pool_idx]}] Performance Summary:"
    echo "------------------------------------------------"
    echo "PUT Statistics (avg of $TEST_COUNT runs):"
    echo "  Time: ${avg_put_time_arr[pool_idx]:0:6}s"
    echo "  Speed: ${put_speed_arr[pool_idx]:0:6} MB/s"
    
    echo -e "\nGET Statistics (avg of 9 cases x $PER_CASE_COUNT runs):"
    echo "  Time: ${avg_get_time_arr[pool_idx]:0:6}s"
    echo "  Speed: ${get_speed_arr[pool_idx]:0:6} MB/s"
    
    # 详细解码性能（按情况分类）
    # eval "times=(\"\${get_time_${pool_idx}[@]}\")"
    # echo -e "\nDetailed GET performance by case:"
    # start_idx=0
    # for case in "${ORDERED_CASES[@]}"; do
    #     case_times=("${times[@]:$start_idx:$PER_CASE_COUNT}")
    #     case_avg=$(python3 -c "import statistics; print(statistics.mean(${case_times[*]}))")
    #     case_speed=$(python3 -c "print(($file_size / $case_avg) / 1024 / 1024)")
    #     echo "  $case: ${case_avg:0:6}s (${case_speed:0:6} MB/s)"
    #     ((start_idx += PER_CASE_COUNT))
    # done
done
echo "========================================================"

cd $current_dir