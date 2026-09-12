#!/usr/bin/env bash
#
# tools/backend_diff.sh - differential harness for backend selection.
#
# The claim this harness exists to check is the one the pipeline header makes:
#
#     **choosing a backend cannot change what a ZL program means.**
#
# Every program in the corpus is compiled three times and run three times, and
# all three runs must produce byte-identical output and the same exit status:
#
#   1. the reference path     - ZL_COMPILER=ast zl file.zl
#                               (the original AST -> bytecode compiler, kept
#                                unchanged for exactly this comparison)
#   2. the bytecode backend   - zl --backend bytecode file.zl
#                               (source -> MIR -> verify -> optimise -> bytecode)
#   3. the native backend     - zl --backend native file.zl
#                               (the same pipeline, with native IR and machine
#                                code generated for the subset of functions the
#                                native tier can prove it handles; execution
#                                still goes through the VM, which is the only
#                                execution driver in this phase)
#
# plus one structural check per program: `--pipeline-report` compiles the
# program once per backend and asserts the two backends were handed the *same*
# MIR module (equal digests). That is what makes the runtime comparison above
# meaningful rather than a coincidence: same semantics in, backend-dependent
# code out.
#
# The optimiser is pinned off on both MIR legs (ZL_MIR_OPT=0). It has its own
# harness (tools/mir_opt_diff.sh); leaving it on here would mean a divergence
# could belong to either the optimiser or a backend, and a harness that cannot
# say which is a harness that gets ignored.
#
# Usage:
#   tools/backend_diff.sh [path/to/zl_language] [files...]
#
# With no files, every .zl under examples/ is used.
#
# Exit status is 0 only when every program behaved identically on all three
# paths and every parity check passed.
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

# `_lib` directories hold importable module sources rather than runnable
# programs; they belong on the module search path. Without them every program
# that imports a sibling module would fail to load on all three legs, and
# "all three failed identically" is not a comparison.
ROOTS=""
while IFS= read -r libdir; do
    [[ -d "$libdir" ]] || continue
    ROOTS="${ROOTS:+$ROOTS:}$libdir"
done < <(find "$ROOT/examples" -type d -name '_lib' | sort)
export ZL_EXTRA_ROOTS="${ROOTS}${ZL_EXTRA_ROOTS:+:$ZL_EXTRA_ROOTS}"

cd "$ROOT" || exit 2

FILES=("$@")
if [[ ${#FILES[@]} -eq 0 ]]; then
    for f in examples/*/*.zl; do
        [[ -f "$f" ]] && FILES+=("$f")
    done
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "error: no programs to compare" >&2
    exit 2
fi

# Vacuity guards. Each leg must be verifiably the path it claims to be; if one
# stops being distinguishable, this harness would be comparing a path with
# itself and reporting a confident green line.
probe="${FILES[0]}"
if ! ZL_COMPILER=ast "$ZL" "$probe" 2>&1 >/dev/null | grep -q "reference compiler"; then
    echo "error: ZL_COMPILER=ast no longer selects the reference compiler" >&2
    exit 2
fi
if ! ZL_PIPELINE_VERBOSE=1 "$ZL" --backend bytecode "$probe" 2>&1 >/dev/null | grep -q "^pipeline:"; then
    echo "error: --backend bytecode does not run the pipeline" >&2
    exit 2
fi
if ! ZL_PIPELINE_VERBOSE=1 "$ZL" --backend native "$probe" 2>&1 >/dev/null | grep -q "backend native"; then
    echo "error: --backend native does not select the native backend" >&2
    exit 2
fi

pass=0
fail=0
failed=()

for f in "${FILES[@]}"; do
    [[ -f "$f" ]] || continue

    ref_out="$(ZL_COMPILER=ast "$ZL" "$f" 2>/dev/null)"; ref_rc=$?
    bc_out="$(ZL_MIR_OPT=0 "$ZL" --backend bytecode "$f" 2>/dev/null)"; bc_rc=$?
    native_out="$(ZL_MIR_OPT=0 "$ZL" --backend native "$f" 2>/dev/null)"; native_rc=$?

    if [[ $ref_rc -ne 0 ]]; then
        echo "ERROR $f  (reference path failed, rc=$ref_rc - nothing compared)"
        fail=$((fail + 1))
        failed+=("$f")
        continue
    fi

    if [[ "$ref_out" != "$bc_out" || $bc_rc -ne 0 ]]; then
        echo "DIFF  $f  (reference rc=$ref_rc, bytecode rc=$bc_rc)"
        fail=$((fail + 1))
        failed+=("$f")
        continue
    fi
    if [[ "$ref_out" != "$native_out" || $native_rc -ne 0 ]]; then
        echo "DIFF  $f  (reference rc=$ref_rc, native rc=$native_rc)"
        fail=$((fail + 1))
        failed+=("$f")
        continue
    fi

    # The structural half: the same MIR, whichever backend is selected.
    parity="$("$ZL" --pipeline-report "$f" 2>/dev/null)" || parity=""
    if ! grep -q "identical: the backend choice is a code-generation decision only" <<<"$parity"; then
        echo "PARITY $f  (--pipeline-report did not confirm identical MIR for both backends)"
        fail=$((fail + 1))
        failed+=("$f")
        continue
    fi

    echo "PASS  $f"
    pass=$((pass + 1))
done

echo
echo "identical on all three paths: $pass"
if [[ $fail -ne 0 ]]; then
    echo "FAILED: $fail"
    printf '  %s\n' "${failed[@]}"
fi

[[ $fail -eq 0 ]]
