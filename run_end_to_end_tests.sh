#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 [k=8] [m=3] [chunk-size=2m] [PUT-iterations=3]"
  echo "Creates an isolated file-backed BlueStore cluster; checks normal and degraded GET."
  exit 0
fi
if (( $# > 4 )); then echo "Expected at most four positional arguments" >&2; exit 2; fi
chunk="${3:-2m}"
if [[ ! "$chunk" =~ ^([0-9]+)([kKmM]?)$ ]]; then echo "Invalid chunk size" >&2; exit 2; fi
chunk_kib=$((10#${BASH_REMATCH[1]}))
case "${BASH_REMATCH[2]}" in m|M) chunk_kib=$((chunk_kib*1024));; esac
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 "$script_dir/repro/appendix.py" end-to-end --degraded-read \
  --k "${1:-8}" --m "${2:-3}" --chunk-kib "$chunk_kib" --put-iterations "${4:-3}"
