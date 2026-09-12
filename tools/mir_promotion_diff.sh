#!/usr/bin/env bash
#
# tools/mir_promotion_diff.sh - differential harness for slot -> block-parameter
# promotion (src/mir/ssa.cpp).
#
# Lowering emits mutable locals in their memory form: a `store` on every path
# that writes them, a `load` where the value is needed. Promotion rewrites that
# into the value form - a block parameter on the join block, with one argument
# per incoming edge. The two forms mean the same thing, so they must behave the
# same.
#
# The bytecode backend gives us a way to check that without a second IR
# interpreter: it lowers a block parameter as the memory form of itself (each
# predecessor stores the argument into the parameter's own local, the block
# reads it, since the VM has no phi instruction). So the same program run
# through `--mir-vm` with promotion off and on must produce byte-identical
# output. Any difference is a promotion bug or a block-parameter lowering bug;
# either way it is ours, and it fails the run.
#
# Programs the backend cannot run at all yet (the fail-closed gaps tracked by
# tools/mir_backend_diff.sh) are skipped: with promotion off they already fail,
# so they carry no information about promotion. Skipping rather than ignoring is
# deliberate - a program that silently starts or stops working is reported as a
# change in the skip count.
#
# Usage:
#   tools/mir_promotion_diff.sh [path/to/zl_language] [files...]
#
# With no files, every .zl under examples/ is used.
#
# Exit status is 0 only when every runnable program behaved identically.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ZL=""
if [[ $# -ge 1 && -x "$1" ]]; then
    ZL="$1"
    shift
elif [[ -x "$ROOT/build/zl_language" ]]; then
    ZL="$ROOT/build/zl_language"
elif command -v zl_language >/dev/null 2>&1; then
    ZL="$(command -v zl_language)"
fi

if [[ -z "$ZL" || ! -x "$ZL" ]]; then
    echo "error: cannot find zl_language (build it first, or pass the path)" >&2
    exit 2
fi

# Directories named `_lib` hold importable module sources rather than runnable
# programs. Without them on the module search path every program that imports a
# sibling module fails to load, and "the backend cannot run it" would silently
# excuse it from the comparison - the same trap tools/mir_backend_diff.sh fell
# into. ZL_EXTRA_ROOTS is honoured by the reference path and by --mir-vm alike.
ROOTS=""
while IFS= read -r libdir; do
    [[ -d "$libdir" ]] || continue
    ROOTS="${ROOTS:+$ROOTS:}$libdir"
done < <(find "$ROOT/examples" -type d -name '_lib' | sort)
export ZL_EXTRA_ROOTS="${ROOTS}${ZL_EXTRA_ROOTS:+:$ZL_EXTRA_ROOTS}"

FILES=("$@")
if [[ ${#FILES[@]} -eq 0 ]]; then
    cd "$ROOT" || exit 2
    # Only runnable programs: `_lib` directories hold importable module
    # sources (no main), which would otherwise be counted as permanent
    # "skips" and bury a real skip - a program the backend genuinely cannot
    # run - under nine lines of noise.
    while IFS= read -r f; do
        FILES+=("$f")
    done < <(find examples -name '*.zl' -not -path '*/_lib/*' | sort)
fi

pass=0
fail=0
skipped=0
failed=()

for f in "${FILES[@]}"; do
    [[ -f "$f" ]] || continue
    # The optimiser is pinned off on both legs so the only thing that differs
    # between them is the IR form being compared.
    off_out="$(ZL_MIR_OPT=0 "$ZL" --mir-vm "$f" 2>/dev/null)"; off_rc=$?
    if [[ $off_rc -ne 0 ]]; then
        # The backend cannot run this program even without promotion, so it
        # cannot tell us anything about promotion.
        skipped=$((skipped + 1))
        continue
    fi
    on_out="$(ZL_MIR_OPT=0 ZL_MIR_PROMOTE=1 "$ZL" --mir-vm "$f" 2>/dev/null)"; on_rc=$?
    if [[ "$off_out" == "$on_out" && $off_rc -eq 0 ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("$f")
        echo "DIFF  $f  (promotion off rc=$off_rc, on rc=$on_rc)"
        diff <(printf '%s\n' "$off_out") <(printf '%s\n' "$on_out") | head -10 | sed 's/^/      /'
    fi
done

echo
echo "identical with and without promotion: $pass"
if [[ $fail -ne 0 ]]; then
    echo "DIFFER: $fail"
    printf '  %s\n' "${failed[@]}"
fi
echo "skipped (backend cannot run them without promotion): $skipped"

[[ $fail -eq 0 ]]
