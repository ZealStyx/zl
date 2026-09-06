#!/usr/bin/env bash
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# -lt 1 ]]; then echo "usage: $0 /path/to/zl_language" >&2; exit 2; fi
ZL="$1"; shift
if [[ ! -x "$ZL" ]]; then echo "error: '$ZL' is not executable" >&2; exit 2; fi
ZL="$(cd "$(dirname "$ZL")" && pwd)/$(basename "$ZL")"
status=0
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
for f in "$ROOT"/examples/*.zl; do
  [[ -f "$f" ]] || continue
  echo "--- $(basename "$f") ---"
  if ! "$ZL" "$f" "$@"; then status=1; fi
  echo
done
exit "$status"
