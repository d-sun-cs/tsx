#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 [k=8] [m=3] [chunk-KiB=2048] [iterations-per-case=2000]"
  echo "Uses installed plugins; run python3 repro/appendix.py install first."
  exit 0
fi
if (( $# > 4 )); then echo "Expected at most four positional arguments" >&2; exit 2; fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 "$script_dir/repro/appendix.py" coding \
  --k "${1:-8}" --m "${2:-3}" --chunk-kib "${3:-2048}" --iterations "${4:-2000}"
