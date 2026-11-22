#!/usr/bin/env bash
# Direct installed-plugin contract test with optional Redis metadata checks.
set -euo pipefail
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  echo "Usage: bash check_lrc.sh  (BUILD_DIR / INSTALL_PREFIX / PLUGIN_DIR may be set)"
  echo "Set LRC_REDIS_TEST_PORT to a disposable Redis port to test metadata failures."
  exit 0
fi
[[ $# == 0 ]] || { echo "Expected no positional arguments" >&2; exit 2; }
tsx=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ceph=$(cd "$tsx/../../.." && pwd)
test_bin=${BUILD_DIR:-$ceph/build}/bin/ceph_test_erasure_code_tsx_lrc
test -x "$test_bin" || { echo "Run bash recmake.sh first" >&2; exit 1; }
plugins=${PLUGIN_DIR:-${INSTALL_PREFIX:-/usr/local}/lib/ceph/erasure-code}
"$test_bin" --check-daemon-profile --erasure-code-dir "$plugins"
if [[ -n "${LRC_REDIS_TEST_PORT:-}" ]]; then
  "$test_bin" "--check-redis=$LRC_REDIS_TEST_PORT" --erasure-code-dir "$plugins"
fi
exec "$test_bin" --erasure-code-dir "$plugins"
