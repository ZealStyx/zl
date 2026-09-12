#!/usr/bin/env bash
#
# tools/mir_opt_diff.sh - runtime differential harness for the MIR optimiser.
#
# The optimiser's whole claim is that the program it hands back means what the
# program it was handed meant. `zl --mir-opt-check` checks that by reading the
# two modules; this checks it by running them.
#
# Every program in the corpus is lowered twice: once as-is and once through
# the optimiser. Both are translated to bytecode and executed, and their
# output - and exit status - must match exactly. A program whose behaviour
# changed is a miscompile, and the harness fails rather than reporting a
# statistic.
#
# This is the backstop for the two things the static comparison in
# differential.hpp deliberately does not count, and cannot:
#
#   * runtime *checks* (`refine`, `null_check`, a division by zero). The
#     optimiser is allowed to delete one it has proved cannot fail, so the
#     static comparison ignores them - which means only a run can catch a
#     check it deleted on insufficient evidence;
#   * *values*. Rewriting `x + 0` to the wrong value changes no observable
#     event, so no amount of comparing event lists will notice. A run
#     notices immediately.
#
# Programs the backend cannot run at all are skipped for the same reason
# tools/mir_promotion_diff.sh skips them: with the optimiser off they already
# fail, so they carry no information about the optimiser.
#
# Usage:
#   tools/mir_opt_diff.sh [path/to/zl_language] [files...]
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
# programs; without them on the module search path every program that imports a
# sibling module fails to load, and "the backend cannot run it" would silently
# excuse it from the comparison.
ROOTS=""
while IFS= read -r libdir; do
    [[ -d "$libdir" ]] || continue
    ROOTS="${ROOTS:+$ROOTS:}$libdir"
done < <(find "$ROOT/examples" -type d -name '_lib' | sort)
export ZL_EXTRA_ROOTS="${ROOTS}${ZL_EXTRA_ROOTS:+:$ZL_EXTRA_ROOTS}"

FILES=("$@")
if [[ ${#FILES[@]} -eq 0 ]]; then
    cd "$ROOT" || exit 2
    while IFS= read -r f; do
        FILES+=("$f")
    done < <(find examples -name '*.zl' -not -path '*/_lib/*' | sort)
fi

pass=0
fail=0
skipped=0
failed=()
broken=()

for f in "${FILES[@]}"; do
    [[ -f "$f" ]] || continue

    # ZL_MIR_OPT is pinned on both legs: the pipeline enables the optimiser on
    # the run path by default, and "compare optimised with optimised" is the
    # kind of green line this harness exists to avoid.
    off_out="$(ZL_MIR_OPT=0 "$ZL" --mir-vm "$f" 2>/dev/null)"; off_rc=$?
    if [[ $off_rc -ne 0 ]]; then
        # The backend cannot run this program even unoptimised, so it cannot
        # tell us anything about the optimiser.
        skipped=$((skipped + 1))
        continue
    fi

    # The optimiser refuses to hand back a module that does not verify, and
    # --mir-vm with ZL_MIR_OPT=1 exits 4 when that happens. Treat that as a
    # failure rather than a skip: it is the optimiser, not the backend, that
    # could not run it.
    on_out="$(ZL_MIR_OPT=1 "$ZL" --mir-vm "$f" 2>/dev/null)"; on_rc=$?
    if [[ $on_rc -eq 4 ]]; then
        fail=$((fail + 1))
        broken+=("$f")
        echo "OPT-FAILED  $f  (the optimiser refused its own output)"
        continue
    fi

    if [[ "$off_out" == "$on_out" && $off_rc -eq $on_rc ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("$f")
        echo "DIFF  $f  (unoptimised rc=$off_rc, optimised rc=$on_rc)"
        diff <(printf '%s\n' "$off_out") <(printf '%s\n' "$on_out") | head -10 | sed 's/^/      /'
    fi
done

echo
echo "identical optimised and unoptimised: $pass"
if [[ $fail -ne 0 ]]; then
    echo "DIFFER: $fail"
    printf '  %s\n' "${failed[@]}"
    if [[ ${#broken[@]} -gt 0 ]]; then
        printf '  optimiser refused: %s\n' "${broken[@]}"
    fi
fi
echo "skipped (backend cannot run them unoptimised): $skipped"

[[ $fail -eq 0 ]]
