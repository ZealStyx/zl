#!/usr/bin/env bash
#
# tools/mir_opt_check_all.sh - static differential sweep over the whole corpus.
#
# The runtime harness (tools/mir_opt_diff.sh) compares programs by running
# them, which is the strongest check there is - and the narrowest, because it
# can only cover programs the MIR bytecode backend can execute. Most of the
# language's most interesting code is not in that set: the stdlib is full of
# generics, async, tasks, locks, atomics and native calls whose functions the
# backend currently stubs.
#
# This harness does not need the backend at all. For every .zl file it runs
# zl --mir-opt-check: lower, optimise, verify, and compare the two modules with
# compareModules. It reports any program whose optimised form is not
# observationally equivalent to the one that went in.
#
# It is the wide check; the runtime harness is the deep one. Neither substitutes
# for the other: this one cannot see values, and that one cannot reach the code
# this one reaches.
#
# A file that cannot be loaded or type checked on its own - a stdlib module
# that only makes sense imported by something else, say - is skipped and
# counted, not failed: it says nothing about the optimiser.
#
# Usage:
#   tools/mir_opt_check_all.sh [path/to/zl_language] [files...]
#
# With no files, every .zl under examples/ and stdlib/ is used.
#
# Exit status is 0 only when every loadable file was equivalent.

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

# `_lib` directories hold importable module sources; without them on the search
# path a program that imports a sibling module fails to load and would be
# counted as a skip instead of being checked.
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
    done < <(find examples stdlib -name '*.zl' -not -path '*/_lib/*' 2>/dev/null | sort)
fi

pass=0
fail=0
skipped=0
divergent=()
skipped_files=()

for f in "${FILES[@]}"; do
    [[ -f "$f" ]] || continue

    # --mir-opt-check prints the optimiser report and the differential verdict
    # to stderr; only the final "equivalent" line matters here, but the whole
    # tail is shown for a file that diverges so the reason is visible.
    out="$("$ZL" --mir-opt-check "$f" 2>&1)"; rc=$?

    if [[ $rc -eq 0 ]]; then
        if [[ "$out" == *"mir-opt-check: equivalent"* ]]; then
            pass=$((pass + 1))
        else
            # Exit 0 without the verdict line is not a pass; treat it as a
            # failure to be looked at rather than trusting the status alone.
            fail=$((fail + 1))
            divergent+=("$f")
            echo "UNVERIFIED  $f"
            printf '%s\n' "$out" | tail -5 | sed 's/^/      /'
        fi
    elif [[ $rc -eq 4 ]]; then
        # Exit 4 is the optimiser's own verdict: not equivalent, or a module
        # that failed to verify after optimisation. Either way, a miscompile.
        fail=$((fail + 1))
        divergent+=("$f")
        echo "DIVERGENT  $f"
        printf '%s\n' "$out" | tail -12 | sed 's/^/      /'
    else
        # The file does not stand on its own - it cannot be loaded, type
        # checked, or lowered - so it carries no information about the
        # optimiser.
        skipped=$((skipped + 1))
        skipped_files+=("$f")
    fi
done

echo
echo "equivalent optimised and unoptimised: $pass"
if [[ $fail -ne 0 ]]; then
    echo "NOT EQUIVALENT: $fail"
    printf '  %s\n' "${divergent[@]}"
fi
if [[ $skipped -ne 0 ]]; then
    echo "skipped (cannot be loaded on their own): $skipped"
    printf '  %s\n' "${skipped_files[@]}"
fi

[[ $fail -eq 0 ]]
