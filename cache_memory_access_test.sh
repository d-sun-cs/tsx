#!/bin/bash

current_dir=$(pwd)
if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
    echo "Error: Please run this script from /ceph/src/erasure-code/tsx directory"
    exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

cd $CEPH_BUILD || exit

# Replace individual valgrind invocations with a loop that captures output,
# extracts "D   refs" 和 "D1  miss rate" 行（保留括号内的读/写拆分），
# 并把每条命令的结果追加到汇总文件中。

summary_file="$(mktemp "/tmp/cachegrind_summary.XXXXXX")"

cmds=(
"./bin/ceph_erasure_code_benchmark -p tsxnaive -P k=8 -P m=3 -S 16777216 -i 2000"
"./bin/ceph_erasure_code_benchmark -p tsxmap -P k=8 -P m=3 -S 16777216 -i 2000"
"./bin/ceph_erasure_code_benchmark -p tsxwin -P k=8 -P m=3 -S 16777216 -i 2000"
"./bin/ceph_erasure_code_benchmark -p tsxmerge -P k=8 -P m=3 -S 16777216 -i 2000"
"./bin/ceph_erasure_code_benchmark -p tsx -P k=8 -P m=3 -S 16777216 -i 2000"

"./bin/ceph_erasure_code_benchmark -p tsxnaive -e 3 -w decode -P k=8 -P m=3 -S 16777216 -i 2000 --erased 2 --erased 5 --erased 9"
"./bin/ceph_erasure_code_benchmark -p tsxmap -e 3 -w decode -P k=8 -P m=3 -S 16777216 -i 2000 --erased 2 --erased 5 --erased 9"
"./bin/ceph_erasure_code_benchmark -p tsxwin -e 3 -w decode -P k=8 -P m=3 -S 16777216 -i 2000 --erased 2 --erased 5 --erased 9"
"./bin/ceph_erasure_code_benchmark -p tsxmerge -e 3 -w decode -P k=8 -P m=3 -S 16777216 -i 2000 --erased 2 --erased 5 --erased 9"
"./bin/ceph_erasure_code_benchmark -p tsx -e 3 -w decode -P k=8 -P m=3 -S 16777216 -i 2000 --erased 2 --erased 5 --erased 9"
)

# 清空/初始化汇总文件
: > "$summary_file"

for cmd in "${cmds[@]}"; do
    tmp_out="$(mktemp "/tmp/cachegrind_out.XXXXXX")"
    # 将 valgrind 的所有输出重定向到临时文件，避免在控制台显示中间输出
    # 注意：valgrind 的输出通常到 stderr，所以重定向 both
    bash -c "valgrind --tool=cachegrind $cmd" &> "$tmp_out"

    # 去掉 valgrind 前缀（==NNN== ）并提取需要的行
    extracted=$(sed -E 's/^==[0-9]+== //' "$tmp_out" \
        | grep -E '^[[:space:]]*D[[:space:]]+refs:|^[[:space:]]*D1[[:space:]]+miss rate:' \
        || true)

    if [[ -z "$extracted" ]]; then
        extracted="(no matching lines found)"
    fi

    {
        printf 'COMMAND: %s\n' "$cmd"
        printf '%s\n\n' "$extracted"
    } >> "$summary_file"

    rm -f "$tmp_out"
done

# 在 CEPh build 目录生成的汇总结果，最后一次性输出到控制台
echo "===== cachegrind 提取汇总 ====="
cat "$summary_file"
rm -f "$summary_file"

cd "$current_dir" || exit