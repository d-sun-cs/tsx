#!/usr/bin/env bash
# Exhaustive recovery checks use the benchmark's byte comparisons, not its timing path.
set -euo pipefail
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  echo "Usage: bash check_correctivity.sh [k m [chunk-KiB=2048] [pattern=random]]"
  echo "No arguments: all four paper configurations, including 8+3 ablations."
  exit 0
fi
[[ $# == 0 || $# == 2 || $# == 3 || $# == 4 ]] || { echo "Expected k m [chunk-KiB] [pattern]" >&2; exit 2; }
cases=("6 2" "6 3" "8 3" "10 4")
if (( $# )); then
  case "$1 $2" in
    '6 2'|'6 3'|'8 3'|'10 4') cases=("$1 $2");;
    *) echo "Supported k,m: 6,2 / 6,3 / 8,3 / 10,4" >&2; exit 2;;
  esac
fi
chunk=${3:-2048}
pattern=${4:-random}
[[ "$chunk" =~ ^[1-9][0-9]*$ && "$chunk" -le 209715 ]] || { echo "Invalid chunk-KiB" >&2; exit 2; }
[[ "$pattern" == random || "$pattern" == constant ]] || { echo "Use random or constant input" >&2; exit 2; }
tsx=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ceph=$(cd "$tsx/../../.." && pwd)
build=${BUILD_DIR:-$ceph/build}
plugins=${PLUGIN_DIR:-${INSTALL_PREFIX:-/usr/local}/lib/ceph/erasure-code}
bench=$build/bin/ceph_erasure_code_benchmark
test -x "$bench" || { echo "Run bash recmake.sh first" >&2; exit 1; }
[[ $(redis-cli ping) == PONG ]] || { echo "Start the experiment's local Redis first" >&2; exit 1; }
cpu=${CPU:-$(awk '/Cpus_allowed_list/ {split($2,a,/[,-]/); print a[1]}' /proc/self/status)}
export OMP_NUM_THREADS=1
mkdir -p "${RESULT_DIR:-$ceph/local_workspace/ec-tests}"
out=$(mktemp -d "${RESULT_DIR:-$ceph/local_workspace/ec-tests}/correctness-XXXXXXXX")
chmod 755 "$out"
printf 'plugin,k,m,erasures,combinations,status\n' > "$out/results.csv"
groups=0
combinations=0
for pair in "${cases[@]}"; do
  read -r k m <<< "$pair"
  case "$pair" in
    '6 2') fasttt=tsx2;; '6 3'|'8 3') fasttt=tsx;; '10 4') fasttt=tsx4;;
    *) echo "Supported k,m: 6,2 / 6,3 / 8,3 / 10,4" >&2; exit 2;;
  esac
  algorithms=("$fasttt" isa jerasure)
  if [[ "$pair" == '8 3' ]]; then algorithms+=(tsxnaive tsxmap tsxwin tsxmerge); fi
  for plugin in "${algorithms[@]}"; do
    args=(-p "$plugin" -P "k=$k" -P "m=$m")
    case "$plugin" in isa) args+=(-P technique=cauchy);; jerasure) args+=(-P technique=cauchy_good);; esac
    for ((e=1; e<=m; e++)); do
      cmd=(taskset -c "$cpu" "$bench" --erasure-code-dir "$plugins" "${args[@]}"
        -s "$((k*chunk*1024))" -i 1 -w decode -v -E exhaustive -e "$e"
        --input-pattern "$pattern" --input-seed 1)
      printf '%q ' "${cmd[@]}" >> "$out/commands.log"; printf '\n' >> "$out/commands.log"
      log=$out/$plugin-$k-$m-e$e.log
      if ! "${cmd[@]}" > "$log" 2>&1; then
        tail -40 "$log" >&2; echo "FAILED: $log" >&2; exit 1
      fi
      count=1
      for ((i=1; i<=e; i++)); do count=$((count*(k+m-i+1)/i)); done
      combinations=$((combinations+count)); groups=$((groups+1))
      printf '%s,%s,%s,%s,%s,PASS\n' "$plugin" "$k" "$m" "$e" "$count" >> "$out/results.csv"
      echo "PASS $plugin ($k,$m), erasures=$e, combinations=$count"
    done
  done
done
echo "PASS: $groups commands, $combinations erasure combinations. Logs: $out"
