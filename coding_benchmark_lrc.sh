#!/usr/bin/env bash
# In-memory ISA-backed TSX-LRC checks and timing; no Redis or Ceph daemons.
set -euo pipefail
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  echo "Usage: bash coding_benchmark_lrc.sh [chunk-KiB=64] [iterations-per-case=2000]"
  echo "Tests plugin=tsx_lrc mode=scaffold (ISA globals + XOR locals), fixed 8+2+2."
  exit 0
fi
[[ $# -le 2 ]] || { echo "Expected chunk-KiB [iterations]" >&2; exit 2; }
chunk=${1:-64}; iterations=${2:-2000}
[[ "$chunk" =~ ^[1-9][0-9]*$ && "$chunk" -le 209715 && "$iterations" =~ ^[1-9][0-9]*$ ]] || {
  echo "Expected positive chunk-KiB and iterations" >&2; exit 2;
}
(( iterations <= 2147483647 / (8*chunk) )) || { echo "Benchmark KiB counter would overflow" >&2; exit 2; }
tsx=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ceph=$(cd "$tsx/../../.." && pwd)
bench=${BUILD_DIR:-$ceph/build}/bin/ceph_erasure_code_benchmark
plugins=${PLUGIN_DIR:-${INSTALL_PREFIX:-/usr/local}/lib/ceph/erasure-code}
test -x "$bench" || { echo "Run bash recmake.sh first" >&2; exit 1; }
cpu=${CPU:-$(awk '/Cpus_allowed_list/ {split($2,a,/[,-]/); print a[1]}' /proc/self/status)}
export OMP_NUM_THREADS=1
mkdir -p "${RESULT_DIR:-$ceph/local_workspace/ec-tests}"
out=$(mktemp -d "${RESULT_DIR:-$ceph/local_workspace/ec-tests}/lrc-benchmark-XXXXXXXX")
chmod 755 "$out"
base=(taskset -c "$cpu" "$bench" --erasure-code-dir "$plugins"
  -p tsx_lrc -P mode=scaffold -P k=8 -P m=2 -P locality=4
  -s "$((8*chunk*1024))" --use-minimum --decode-only-erased)
printf 'case,combinations,status\n' > "$out/checks.csv"
printf 'plugin,mode,case,workload,erased,helper_count,helper_bytes,seconds,us_per_call,stripe_KiB,stripe_GiB_s\n' > "$out/results.csv"
run() {
  local log=$1
  shift
  printf '%q ' "$@" >> "$out/commands.log"; printf '\n' >> "$out/commands.log"
  if ! "$@" > "$log" 2>&1; then
    tail -40 "$log" >&2; echo "FAILED: $log" >&2; exit 1
  fi
}
# Every single/double erasure, including local parity IDs 10 and 11.
for e in 1 2; do
  log=$out/exhaustive-e$e.log
  run "$log" "${base[@]}" -w decode -i 1 -E exhaustive -e "$e" -v --verify \
    --input-pattern random --input-seed 1
  grep -Fxq 'layout data_chunks=8 coding_chunks=4 total_chunks=12' "$log"
  expected=12; if (( e == 2 )); then expected=66; fi
  count=$(awk '/^decode want=/ {n++} END {print n+0}' "$log")
  [[ "$count" == "$expected" ]] || { echo "Incomplete exhaustive coverage: $log" >&2; exit 1; }
  printf 'exhaustive-e%s,%s,PASS\n' "$e" "$count" >> "$out/checks.csv"
  echo "PASS scaffold: $count byte-checked combinations with $e erasures"
done
measure() {
  local name=$1 work=$2 helpers=$3
  shift 3
  local args=() id faults='' helper_bytes=0 actual
  for id in "$@"; do args+=(--erased "$id"); faults+="${faults:++}$id"; done
  # Verify one random-input run before timing constant-input runs separately.
  run "$out/$name-check.log" "${base[@]}" -w "$work" "${args[@]}" -i 1 -v --verify \
    --input-pattern random --input-seed 1
  if [[ "$work" == decode ]]; then
    read -r actual helper_bytes < <(awk '/^decode want=/ {
      for(i=1;i<=NF;i++) {if($i ~ /^helper_count=/) {split($i,a,"="); n=a[2]}
        if($i ~ /^helper_bytes=/) {split($i,a,"="); b=a[2]}} print n,b
    }' "$out/$name-check.log")
    [[ "$actual" == "$helpers" && "$helper_bytes" == "$((helpers*chunk*1024))" ]] || {
      echo "Unexpected helper selection: $out/$name-check.log" >&2; exit 1;
    }
    printf '%s,1,PASS\n' "$name" >> "$out/checks.csv"
  fi
  run "$out/$name-warmup.log" "${base[@]}" -w "$work" "${args[@]}" -i 2
  run "$out/$name.log" "${base[@]}" -w "$work" "${args[@]}" -i "$iterations"
  awk -v name="$name" -v work="$work" -v faults="$faults" -v helpers="$helpers" \
    -v bytes="$helper_bytes" -v iterations="$iterations" -v expected="$((iterations*8*chunk))" '
    NF==2 && $1 ~ /^[0-9]+(\.[0-9]+)?$/ && $2 ~ /^[0-9]+$/ {s=$1; b=$2; n++}
    END {if(n!=1 || s<=0 || b!=expected) exit 1;
      printf "tsx_lrc,scaffold,%s,%s,%s,%d,%d,%.9f,%.6f,%.0f,%.6f\n",
        name,work,faults,helpers,bytes,s,s*1e6/iterations,b,b/s/1048576}
  ' "$out/$name.log" >> "$out/results.csv" || { echo "Invalid timing: $out/$name.log" >&2; exit 1; }
  echo "PASS $name: $iterations timed $work calls"
}
measure encode encode 0
measure local-data decode 4 2
measure local-parity decode 4 10
measure global-pair decode 8 0 1
measure global-then-local decode 9 0 10
measure three-erasures decode 9 0 4 8
# These are known limits of the current layer-by-layer scaffold planner.
for faults in '0 1 2' '0 1 8'; do
  read -r -a ids <<< "$faults"
  args=(); for id in "${ids[@]}"; do args+=(--erased "$id"); done
  name=reject-${faults// /-}
  cmd=("${base[@]}" -w decode "${args[@]}" -i 1 --verify --input-pattern random)
  printf '%q ' "${cmd[@]}" >> "$out/commands.log"; printf '\n' >> "$out/commands.log"
  if "${cmd[@]}" > "$out/$name.log" 2>&1; then status=0; else status=$?; fi
  [[ "$status" == 251 ]] && grep -Fxq 'minimum_to_decode failed: -5' "$out/$name.log" || {
    cat "$out/$name.log" >&2; echo "Expected -EIO for $faults, got $status" >&2; exit 1;
  }
  printf '%s,1,EXPECTED_REJECTION\n' "$name" >> "$out/checks.csv"
  echo "EXPECTED_REJECTION scaffold planner: $faults"
done
echo "PASS: 83 byte-checked repairs, 2 expected rejections, 6 timing cases. Logs and CSV: $out"
echo "Timing includes selection/allocation; stripe_GiB_s counts all 8 data shards, not helper I/O."
