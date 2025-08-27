#!/bin/bash -ex

TMPDIR=/tmp/test_ceph-erasure-code-tool.$$
mkdir $TMPDIR
trap "rm -fr $TMPDIR" 0

cd /ceph/build/

PLUGINS=("tsx" "isa")
K=8
M=3
CHUNK_SIZE=$((2*1024*1024)) # 2MB
DATA_SIZE=$((1024*1024*1024)) # 模拟大文件
ENCODE_ROUNDS=1
DECODE_ROUNDS=1

echo "准备测试数据..."
dd if=/dev/urandom of=$TMPDIR/data bs=$DATA_SIZE count=3
cp $TMPDIR/data $TMPDIR/data.orig


# 结果保存数组
declare -A ENCODE_AVG_MAP
declare -A ENCODE_TPUT_MAP
declare -A DECODE_AVG_MAP
declare -A DECODE_TPUT_MAP
declare -A VERIFY_MAP

FILE_SIZE=$(stat -c '%s' $TMPDIR/data.orig)
for PLUGIN in "${PLUGINS[@]}"; do
    ./bin/ceph-erasure-code-tool test-plugin-exists $PLUGIN
    ceph-erasure-code-tool validate-profile \
        plugin=$PLUGIN,technique=reed_sol_van,k=$K,m=$M

    ENCODE_TOTAL=0
    for ((i=0; i<$ENCODE_ROUNDS; i++)); do
        cp $TMPDIR/data.orig $TMPDIR/data
        rm -f $TMPDIR/data.{0..20}
        START=$(date +%s.%N)
        ./bin/ceph-erasure-code-tool encode \
            plugin=$PLUGIN,technique=reed_sol_van,k=$K,m=$M \
            $CHUNK_SIZE \
            $(seq -s, 0 $((K+M-1))) \
            $TMPDIR/data
        END=$(date +%s.%N)
        DURATION=$(awk "BEGIN{print $END - $START}")
        ENCODE_TOTAL=$(awk "BEGIN{print $ENCODE_TOTAL + $DURATION}")
    done
    ENCODE_AVG=$(awk "BEGIN{printf \"%.3f\", $ENCODE_TOTAL / $ENCODE_ROUNDS}")
    ENCODE_MB=$(awk "BEGIN{printf \"%.2f\", $DATA_SIZE / 1048576}")
    ENCODE_TPUT=$(awk "BEGIN{printf \"%.2f\", $ENCODE_MB / $ENCODE_AVG}")
    ENCODE_AVG_MAP[$PLUGIN]=$ENCODE_AVG
    ENCODE_TPUT_MAP[$PLUGIN]=$ENCODE_TPUT

    DECODE_TOTAL=0
    for ((i=0; i<$DECODE_ROUNDS; i++)); do
        # 可选：丢失块索引方案，支持1~3个丢失
        LOST_IDX=(1 4 8)
        # 构造输入块索引（去掉丢失的块）
        INPUT_IDX=()
        for ((j=0; j<K+M; j++)); do
            skip=0
            for lost in "${LOST_IDX[@]}"; do
                if [[ $j -eq $lost ]]; then
                    skip=1
                    break
                fi
            done
            if [[ $skip -eq 0 ]]; then
                INPUT_IDX+=($j)
            fi
        done
        # 生成输入索引字符串
        INPUT_IDX_STR=$(IFS=,; echo "${INPUT_IDX[*]}")
        START=$(date +%s.%N)
        ./bin/ceph-erasure-code-tool decode \
            plugin=$PLUGIN,technique=reed_sol_van,k=$K,m=$M \
            $CHUNK_SIZE \
            $INPUT_IDX_STR \
            $TMPDIR/data
        END=$(date +%s.%N)
        DURATION=$(awk "BEGIN{print $END - $START}")
        DECODE_TOTAL=$(awk "BEGIN{print $DECODE_TOTAL + $DURATION}")
    done
    DECODE_AVG=$(awk "BEGIN{printf \"%.3f\", $DECODE_TOTAL / $DECODE_ROUNDS}")
    DECODE_TPUT=$(awk "BEGIN{printf \"%.2f\", $ENCODE_MB / $DECODE_AVG}")
    DECODE_AVG_MAP[$PLUGIN]=$DECODE_AVG
    DECODE_TPUT_MAP[$PLUGIN]=$DECODE_TPUT

    # 截断并校验
    size=$(stat -c '%s' $TMPDIR/data.orig)
    truncate -s "${size}" $TMPDIR/data
    if cmp $TMPDIR/data.orig $TMPDIR/data; then
        VERIFY_MAP[$PLUGIN]="数据一致性校验通过"
    else
        VERIFY_MAP[$PLUGIN]="数据一致性校验失败"
    fi
done


# 统一输出所有测试结果
echo "\n================= 测试结果汇总 ================="
echo "测试文件大小: $FILE_SIZE 字节"
for PLUGIN in "${PLUGINS[@]}"; do
    echo "插件: $PLUGIN (k=$K, m=$M, 块大小=2MB)"
    echo "编码平均耗时: ${ENCODE_AVG_MAP[$PLUGIN]}s, 吞吐量: ${ENCODE_TPUT_MAP[$PLUGIN]} MB/s"
    echo "解码平均耗时: ${DECODE_AVG_MAP[$PLUGIN]}s, 吞吐量: ${DECODE_TPUT_MAP[$PLUGIN]} MB/s"
    echo "${VERIFY_MAP[$PLUGIN]}"
    echo "----------------------------------------------"
done
echo "所有测试完成。"

echo "所有测试完成。"