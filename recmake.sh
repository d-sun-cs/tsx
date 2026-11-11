#!/usr/bin/env bash
# Run inside an Ubuntu 22.04 build environment. Only build EC plugins and benchmark.
set -euo pipefail
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  echo "Usage: bash recmake.sh [jobs=8]  (BUILD_DIR / INSTALL_PREFIX may be set)"
  exit 0
fi
jobs=${1:-8}
[[ $# -le 1 && "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "Expected a positive job count" >&2; exit 2; }
tsx=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ceph=$(cd "$tsx/../../.." && pwd)
build=${BUILD_DIR:-$ceph/build}
prefix=${INSTALL_PREFIX:-/usr/local}
[[ "$build" == /* && "$prefix" == /* ]] || { echo "Use absolute BUILD_DIR and INSTALL_PREFIX" >&2; exit 2; }
[[ $(git -C "$ceph" rev-parse HEAD) == 6b0e988052ec84cf2d4a54ff9bbbc5e720b621ad ]] || {
  echo "Use official Ceph v18.2.7 with TSX in src/erasure-code/tsx" >&2; exit 1;
}
if git -C "$ceph" submodule status --recursive | grep -E '^[-+U]' >/dev/null; then
  echo "Align submodules with git submodule update --init --recursive" >&2; exit 1
fi
if [[ -f "$build/CMakeCache.txt" ]]; then
  grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$ceph" "$build/CMakeCache.txt" || {
    echo "Build cache belongs to another checkout; choose a new BUILD_DIR" >&2; exit 1;
  }
  if grep -E '^CMAKE_C(X[X])?_FLAGS[^=]*=.*(-mavx512|-march=native)' "$build/CMakeCache.txt"; then
    echo "Experimental compiler flags found; choose a new BUILD_DIR" >&2; exit 1
  fi
fi
# Idempotent integration: never overwrite conflicting parent edits.
if ! git -C "$ceph" apply --reverse --check "$tsx/ceph-v18.2.7.patch" 2>/dev/null; then
  git -C "$ceph" apply --check "$tsx/ceph-v18.2.7.patch"
  git -C "$ceph" apply "$tsx/ceph-v18.2.7.patch"
fi
cmake -S "$ceph" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=gcc-11 -DCMAKE_CXX_COMPILER=g++-11 \
  -DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS= -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DWITH_PYTHON3=3.10 -DWITH_SYSTEM_BOOST=OFF -DBOOST_J="$jobs" \
  -DWITH_TESTS=ON -DWITH_EC_ISA_PLUGIN=ON -DWITH_TSX_CERASURE=OFF \
  -DWITH_MANPAGE=OFF -DWITH_MGR=OFF -DWITH_MGR_DASHBOARD_FRONTEND=OFF \
  -DWITH_RADOSGW=OFF -DWITH_RBD=OFF -DWITH_CEPHFS=OFF -DWITH_LIBCEPHFS=OFF \
  -DWITH_LIBCEPHSQLITE=OFF -DWITH_KRBD=OFF -DWITH_RDMA=OFF -DWITH_FUSE=OFF \
  -DWITH_JAEGER=OFF -DWITH_LTTNG=OFF -DWITH_BABELTRACE=OFF -DWITH_OPENLDAP=OFF \
  -DWITH_SYSTEMD=OFF -DWITH_LIBURING=OFF -DWITH_BLUESTORE=OFF -DWITH_CCACHE=OFF
cmake --build "$build" --parallel "$jobs" --target \
  ceph_erasure_code_benchmark ec_tsx ec_tsx2 ec_tsx4 ec_tsxse \
  ec_tsxnaive ec_tsxmap ec_tsxwin ec_tsxmerge ec_isa ec_jerasure \
  ec_tsx_lrc ceph_test_erasure_code_tsx_lrc
# Install only these three plugin subdirectories, not the whole Ceph project.
for plugin in tsx isa jerasure; do
  cmake -P "$build/src/erasure-code/$plugin/cmake_install.cmake"
done
echo "Installed EC plugins: $prefix/lib/ceph/erasure-code"
echo "Benchmark: $build/bin/ceph_erasure_code_benchmark"
echo "LRC scaffold test: bash $tsx/check_lrc.sh"
