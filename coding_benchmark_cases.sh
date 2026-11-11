#!/usr/bin/env bash
# Direct EC benchmark: fixed erasure positions shared by all implementations.
set -euo pipefail
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  echo "Usage: bash coding_benchmark_cases.sh [k=8] [m=3] [chunk-KiB=2048] [iterations-per-case=2000]"
  exit 0
fi
[[ $# -le 4 ]] || { echo "Expected at most four arguments" >&2; exit 2; }
k=${1:-8}; m=${2:-3}; chunk=${3:-2048}; iterations=${4:-2000}
case "$k,$m" in
  6,2) fasttt=tsx2;; 6,3|8,3) fasttt=tsx;; 10,4) fasttt=tsx4;;
  *) echo "Supported k,m: 6,2 / 6,3 / 8,3 / 10,4" >&2; exit 2;;
esac
[[ "$chunk" =~ ^[1-9][0-9]*$ && "$chunk" -le 209715 && "$iterations" =~ ^[1-9][0-9]*$ ]] || {
  echo "Expected positive chunk-KiB and iterations" >&2; exit 2;
}
(( iterations <= 2147483647 / (k*chunk) )) || { echo "Benchmark KiB counter would overflow" >&2; exit 2; }
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
out=$(mktemp -d "${RESULT_DIR:-$ceph/local_workspace/ec-tests}/coding-$k-$m-XXXXXXXX")
chmod 755 "$out"
printf 'plugin,k,m,workload,data_lost,parity_lost,seconds,KiB,GB_s,GiB_s\n' > "$out/results.csv"
# The benchmark prepares input outside its timer; explicit --erased decode is timed only.
measure() {
  local work=$1 data=$2 parity=$3
  shift 3
  local stem=$out/$plugin-$work-$data-$parity
  local cmd=(taskset -c "$cpu" "$bench" --erasure-code-dir "$plugins"
    "${args[@]}" -s "$((k*chunk*1024))" -w "$work" "$@")
  printf '%q ' "${cmd[@]}" -i "$iterations" >> "$out/commands.log"; printf '\n' >> "$out/commands.log"
  if ! "${cmd[@]}" -i 2 > "$stem.warmup.log" 2>&1 ||
     ! "${cmd[@]}" -i "$iterations" > "$stem.log" 2>&1; then
    tail -40 "$stem"*.log >&2; echo "FAILED: $stem" >&2; exit 1
  fi
  awk -v p="$plugin" -v k="$k" -v m="$m" -v w="$work" -v d="$data" -v c="$parity" \
    -v expected="$((iterations*k*chunk))" '
    NF==2 && $1 ~ /^[0-9]+(\.[0-9]+)?$/ && $2 ~ /^[0-9]+$/ {s=$1; b=$2; n++}
    END {if(n!=1 || s<=0 || b!=expected) exit 1;
      printf "%s,%d,%d,%s,%d,%d,%.9f,%.0f,%.6f,%.6f\n",p,k,m,w,d,c,s,b,b*1024/s/1e9,b/s/1048576}
  ' "$stem.log" >> "$out/results.csv" || { echo "Invalid timing output: $stem.log" >&2; exit 1; }
  echo "PASS $plugin $work, data_lost=$data parity_lost=$parity"
}
algorithms=("$fasttt" isa jerasure)
if [[ "$k,$m" == 8,3 ]]; then algorithms+=(tsxnaive tsxmap tsxwin tsxmerge); fi
for plugin in "${algorithms[@]}"; do
  args=(-p "$plugin" -P "k=$k" -P "m=$m")
  case "$plugin" in isa) args+=(-P technique=cauchy);; jerasure) args+=(-P technique=cauchy_good);; esac
  measure encode 0 0
  for ((e=1; e<=m; e++)); do
    for ((data=0; data<=e; data++)); do
      parity=$((e-data)); erased=()
      for ((i=0; i<data; i++)); do erased+=(--erased "$i"); done
      for ((i=0; i<parity; i++)); do erased+=(--erased "$((k+i))"); done
      measure decode "$data" "$parity" -e "$e" "${erased[@]}"
    done
  done
done
awk -F, 'NR>1 {e=$5+$6; id=$1 "," $4 "," e; total[id]+=$10; n[id]++}
  END {print "plugin,workload,erasures,mean_GiB_s"; for(id in total) printf "%s,%.6f\n",id,total[id]/n[id]}
' "$out/results.csv" > "$out/summary.csv"
echo "PASS: $(($(wc -l < "$out/results.csv")-1)) measurements. Logs and CSV: $out"
