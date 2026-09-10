#!/usr/bin/env bash
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# -lt 1 ]]; then echo "usage: $0 /path/to/zl_language" >&2; exit 2; fi
ZL="$1"; shift
if [[ ! -x "$ZL" ]]; then echo "error: '$ZL' is not executable" >&2; exit 2; fi
ZL="$(cd "$(dirname "$ZL")" && pwd)/$(basename "$ZL")"
status=0
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Examples are organised into topic subdirectories. Files under a `_lib`
# directory are importable module sources with no main(), so skip them but
# expose each one to the importer via --root.
EXAMPLES_DIR="$ROOT/examples"
roots=()
while IFS= read -r libdir; do
  [[ -d "$libdir" ]] && roots+=("--root" "$libdir")
done < <(find "$EXAMPLES_DIR" -type d -name '_lib' 2>/dev/null | sort)

found=0
while IFS= read -r f; do
  [[ -f "$f" ]] || continue
  found=1
  echo "--- ${f#"$EXAMPLES_DIR"/} ---"
  if ! "$ZL" ${roots[@]+"${roots[@]}"} "$f" "$@"; then status=1; fi
  echo
done < <(find "$EXAMPLES_DIR" -name '*.zl' -type f -not -path '*/_lib/*' 2>/dev/null | sort)

if [[ "$found" -eq 0 ]]; then
  echo "no examples found under $EXAMPLES_DIR" >&2
  exit 2
fi

# To also diff each example against its embedded expected output, run
# examples/run_all.sh instead.
exit "$status"
