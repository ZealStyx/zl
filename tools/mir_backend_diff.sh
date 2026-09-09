#!/usr/bin/env bash
#
# tools/mir_backend_diff.sh - differential harness for the MIR -> bytecode
# backend (--mir-vm).
#
# The MIR->bytecode backend must produce the same observable behaviour as the
# reference compiler->VM path for every ZL program it claims to support. This
# script runs each corpus program through BOTH paths and compares (exit code +
# stdout). It fails if any program that is expected to match actually differs.
#
# Programs whose behaviour the backend does not yet reproduce faithfully (they
# use constructs it still stubs at runtime - closures/lambdas, exceptions,
# reflection-generic runtime checks, static fields, nested generic collections)
# are listed in MIR_BACKEND_KNOWN_GAPS below and are reported but do NOT fail
# the run: the whole point of the fail-closed backend is that a reachable
# unsupported function raises loudly instead of miscompiling, so these programs
# fail the reference path with a clear runtime error rather than differing
# silently. When one is fixed it must move out of KNOWN_GAPS into the expected
# list.
#
# Usage:
#   tools/mir_backend_diff.sh [path/to/zl_language]
#
# Exit status is 0 only when every expected-match program matched.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ZL=""
if [[ $# -ge 1 ]]; then
    ZL="$1"
elif [[ -x "$ROOT/build/zl_language" ]]; then
    ZL="$ROOT/build/zl_language"
elif command -v zl_language >/dev/null 2>&1; then
    ZL="$(command -v zl_language)"
fi

if [[ -z "$ZL" || ! -x "$ZL" ]]; then
    echo "error: cannot find zl_language (build it first, or pass the path)" >&2
    exit 2
fi

# Programs that must produce identical output on both paths.
EXPECTED=(
    examples/basics/*.zl
    examples/intermediate/Classes.zl
    examples/intermediate/DataRecords.zl
    examples/intermediate/Encapsulation.zl
    examples/intermediate/Enums.zl
    examples/intermediate/Generics.zl
    examples/intermediate/Inheritance.zl
    examples/intermediate/Interfaces.zl
    examples/intermediate/Modules.zl
    examples/intermediate/OperatorOverloading.zl
    examples/intermediate/Reflection.zl
)

# Programs using constructs the backend still stubs (fail-closed), so they are
# expected NOT to run cleanly yet; report but do not fail the run.
KNOWN_GAPS=(
    examples/intermediate/Closures.zl
    examples/intermediate/CollectionAlgorithms.zl
    examples/intermediate/Exceptions.zl
    examples/intermediate/GenericRuntimeChecks.zl
    examples/intermediate/Lambdas.zl
    examples/intermediate/MatchExpressions.zl
    examples/intermediate/NestedGenerics.zl
    examples/intermediate/StaticMembers.zl
)

pass=0
fail=0
gap_fail=0
failed=()
gaps_that_now_pass=()

run_one() {
    local f="$1"
    local ref_out mir_out ref_rc mir_rc
    ref_out="$("$ZL" "$f" 2>/dev/null)"; ref_rc=$?
    mir_out="$("$ZL" --mir-vm "$f" 2>/dev/null)"; mir_rc=$?
    if [[ "$ref_out" == "$mir_out" && "$ref_rc" == "$mir_rc" ]]; then
        echo "PASS  $f"
        return 0
    else
        echo "DIFF  $f  (ref rc=$ref_rc, mir rc=$mir_rc)"
        return 1
    fi
}

for f in "${EXPECTED[@]}"; do
    [[ -f "$f" ]] || continue
    if run_one "$f"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("$f")
    fi
done

for f in "${KNOWN_GAPS[@]}"; do
    [[ -f "$f" ]] || continue
    if run_one "$f"; then
        gaps_that_now_pass+=("$f")
    else
        gap_fail=$((gap_fail + 1))
    fi
done

echo
echo "expected-match passed: $pass"
if [[ $fail -ne 0 ]]; then
    echo "expected-match FAILED: $fail"
    printf '  %s\n' "${failed[@]}"
fi
echo "known-gaps still failing (expected): $gap_fail"
if [[ ${#gaps_that_now_pass[@]} -ne 0 ]]; then
    echo "WARNING: known gaps now match - move them to EXPECTED:"
    printf '  %s\n' "${gaps_that_now_pass[@]}"
fi

[[ $fail -eq 0 ]]
