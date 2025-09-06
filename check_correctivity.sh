#!/bin/bash

current_dir=$(pwd)
if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
    echo "Error: Please run this script from ceph/src/erasure-code/tsx directory"
    exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

cd $CEPH_BUILD || exit

NINJA_TARGETS=("ec_tsx" "ec_tsxse" "ec_tsxnaive" "ec_tsxmap" "ec_tsxwin" "ec_tsxmerge" "ceph_erasure_code_benchmark")

needs_install=false

for target in "${NINJA_TARGETS[@]}"; do
    ninja_output=$(ninja "$target")
    if [[ "$ninja_output" != *"no work to do."* ]]; then
        needs_install=true
    fi
done

if $needs_install; then
    sudo ninja install
fi

# 定义测试参数
k=8
m=3
s=$((k * 2 * 1024 * 1024))  # 根据公式计算s值
error_modes=(1 2 3)
algorithms=("tsxnaive" "tsxmap" "tsxwin" "tsxmerge" "tsx" "tsxse")

# 初始化测试结果跟踪
all_tests_passed=true

# 遍历所有算法和错误模式
for algo in "${algorithms[@]}"; do
    for e in "${error_modes[@]}"; do
        echo "------------------------------------------------"
        echo "Testing algorithm: $algo with error mode: $e"
        echo "------------------------------------------------"
        
        # 运行测试并捕获输出
        output=$(./bin/ceph_erasure_code_benchmark -w decode -p "$algo" -P k=$k -P m=$m -s $s -i 1 -v -E exhaustive -e $e 2>&1)
        
        # 检查输出中是否包含"different"
        if grep -q "different" <<< "$output"; then
            echo -e "\033[31mFAILURE: Data mismatch detected in $algo (e=$e)\033[0m"
            all_tests_passed=false
        else
            echo -e "\033[32mSUCCESS: $algo passed verification (e=$e)\033[0m"
        fi
        
        # 输出分隔线
        echo "================================================"
    done
done

# 返回原始目录
cd "$current_dir" || exit

# 输出最终测试结果
if $all_tests_passed; then
    echo -e "\n\033[42mALL TESTS PASSED SUCCESSFULLY\033[0m"
    exit 0
else
    echo -e "\n\033[41mSOME TESTS FAILED\033[0m"
    exit 1
fi