#!/usr/bin/env bash
# Optional host-side shell: dependency commands are documented in the handoff.
set -euo pipefail
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  echo "Usage: bash setup_container.sh [image=ubuntu:22.04] [name=fasttt_ec]"
  exit 0
fi
[[ $# -le 2 ]] || { echo "Expected at most image and container name" >&2; exit 2; }
tsx=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ceph=$(cd "$tsx/../../.." && pwd)
test -f "$ceph/src/erasure-code/ErasureCode.h"
exec docker run -it --name "${2:-fasttt_ec}" \
  --mount "type=bind,source=$ceph,target=/ceph" \
  --workdir /ceph/src/erasure-code/tsx "${1:-ubuntu:22.04}" bash
