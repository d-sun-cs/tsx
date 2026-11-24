#!/usr/bin/env bash
# Both inner parallelism and concurrent requests through a shared plugin instance.
set -euo pipefail
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  echo "Usage: bash coding_benchmark_mt.sh [threads=4] [requests=4] [iterations-per-request=30]"
  echo "Optional CPU_LIST, REDIS_PORT (default 16379), RESULT_DIR, BUILD_DIR, PLUGIN_DIR."
  exit 0
fi
threads=${1:-4}
requests=${2:-4}
iterations=${3:-30}
port=${REDIS_PORT:-16379}
[[ $# -le 3 && "$threads" =~ ^[1-9][0-9]?$ && "$threads" -le 64 &&
   "$requests" =~ ^[1-9][0-9]?$ && "$requests" -le 64 &&
   "$iterations" =~ ^[1-9][0-9]{0,5}$ && "$iterations" -le 100000 &&
   "$port" =~ ^[1-9][0-9]{0,4}$ && "$port" -le 65535 ]] || {
  echo "Expected threads and requests in 1..64, iterations in 1..100000 and a valid Redis port" >&2
  exit 2
}
tsx=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ceph=$(cd "$tsx/../../.." && pwd)
build=${BUILD_DIR:-$ceph/build}
plugins=${PLUGIN_DIR:-${INSTALL_PREFIX:-/usr/local}/lib/ceph/erasure-code}
bench=$build/bin/ceph_erasure_code_benchmark
driver=$build/bin/ceph_test_erasure_code_tsx_mt
[[ -x "$bench" && -x "$driver" && -f "$plugins/libec_tsx_mt.so" ]] || {
  echo "Build and install the EC targets with bash recmake.sh first" >&2
  exit 1
}
if redis-cli -p "$port" ping >/dev/null 2>&1; then
  echo "Redis port $port is occupied. Choose a free REDIS_PORT for this test." >&2
  exit 1
fi
cpus=${CPU_LIST:-$(awk -v limit="$threads" '/Cpus_allowed_list/ {
  n=split($2,ranges,",");
  for (i=1; i<=n && count<limit; ++i) {
    fields=split(ranges[i], ends, "-");
    last=fields == 1 ? ends[1] : ends[2];
    for (cpu=ends[1]; cpu<=last && count<limit; ++cpu) {
      printf "%s%s", count ? "," : "", cpu;
      count++;
    }
  }
}' /proc/self/status)}
export OMP_DYNAMIC=FALSE OMP_WAIT_POLICY=PASSIVE LC_ALL=C
directory=${RESULT_DIR:-$ceph/local_workspace/ec-tests}
mkdir -p "$directory"
out=$(mktemp -d "$directory/tsx-mt-XXXXXXXX")
out=$(realpath "$out")
chmod 755 "$out"
printf 'affinity=%s\nthreads=%s\nconcurrent_requests=%s\nredis_port=%s\n' \
  "$cpus" "$threads" "$requests" "$port" > "$out/settings.txt"
sha256sum "$bench" "$driver" "$plugins/libec_tsx_mt.so" > "$out/binaries.sha256"
redis-server --bind 127.0.0.1 --port "$port" --save '' --appendonly no \
  --daemonize no --dir "$out" > "$out/redis.log" 2>&1 &
redis_pid=$!
trap 'kill "$redis_pid" 2>/dev/null || true; wait "$redis_pid" 2>/dev/null || true' EXIT
for retry in {1..40}; do
  kill -0 "$redis_pid" || { cat "$out/redis.log" >&2; exit 1; }
  if [[ $(redis-cli -p "$port" ping 2>/dev/null) == PONG ]]; then break; fi
  sleep .05
done
[[ $(redis-cli -p "$port" ping 2>/dev/null) == PONG ]]

run() {
  local log=$1
  shift
  printf '%q ' "$@" >> "$out/commands.log"
  printf '\n' >> "$out/commands.log"
  "$@" > "$log" 2> "$log.err" || { cat "$log" "$log.err" >&2; return 1; }
}

run "$out/ceph-single-erasure.log" taskset -c "$cpus" "$bench" \
  --erasure-code-dir "$plugins" -p tsx_mt -P k=8 -P m=3 -P "threads=$threads" -P "redis-port=$port" \
  -s 16777216 -w decode --use-minimum --verify --input-pattern random --input-seed 1 \
  -E exhaustive -e 1 -v -i 1
[[ $(awk '/^decode want=/ {n++} END {print n+0}' "$out/ceph-single-erasure.log") == 11 ]]

budgets=(1)
clients=(1)
if ((threads > 1)); then budgets+=("$threads"); fi
if ((requests > 1)); then clients+=("$requests"); fi
for budget in "${budgets[@]}"; do
  for concurrency in "${clients[@]}"; do
    csv=$out/t$budget-r$concurrency.csv
    run "$csv" taskset -c "$cpus" "$driver" "$plugins" "$port" "$budget" "$concurrency" "$iterations"
    if [[ -e "$out/summary.csv" ]]; then
      tail -n +2 "$csv" >> "$out/summary.csv"
    else
      cp "$csv" "$out/summary.csv"
    fi
  done
done
cat "$out/summary.csv"
echo "PASS tsx_mt. Results in $out"
