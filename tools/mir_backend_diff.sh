#!/usr/bin/env bash
#
# tools/mir_backend_diff.sh - differential harness for the MIR -> bytecode
# backend (--mir-vm).
#
# The MIR->bytecode backend must produce the same observable behaviour as the
# reference compiler->VM path for every ZL program it claims to support. This
# script runs every runnable example through BOTH paths and compares (exit code
# + stdout). It fails if any program that is expected to match actually differs.
#
# The two paths are: `ZL_COMPILER=ast zl file.zl` (the original AST -> bytecode
# compiler) and `zl --mir-vm file.zl` (source -> MIR -> verify -> optimise ->
# bytecode -> VM). The default `zl file.zl` is the second of those, which is why
# the reference leg has to say which compiler it wants.
#
# The corpus is every .zl directly under an examples/ subdirectory - basics,
# intermediate and advanced alike. (Directories named `_lib` hold importable
# module sources, not runnable programs; they are one level deeper, so the glob
# below does not pick them up, and they are put on the module search path
# instead.)
#
# There are no known gaps today. If the backend stops reproducing some
# construct faithfully, list the affected programs in KNOWN_GAPS below: they
# are reported but do NOT fail the run, on the fail-closed theory that a
# reachable unsupported function must raise loudly instead of miscompiling.
# When a gap is fixed its programs move out of KNOWN_GAPS (back into the
# glob-covered corpus) and the entry is deleted.
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

# Directories named `_lib` hold importable module sources rather than runnable
# programs. They are not examples, but the programs that import them need them on
# the module search path - exactly as examples/run_all.sh arranges. Without this,
# every example that imports a sibling module fails to load on BOTH paths, and
# "both paths failed identically" would be reported as a pass: a green line that
# measured nothing. ZL_EXTRA_ROOTS is honoured by the reference path and by
# --mir-vm alike, so one setting covers both sides of the comparison.
ROOTS=""
while IFS= read -r libdir; do
    [[ -d "$libdir" ]] || continue
    ROOTS="${ROOTS:+$ROOTS:}$libdir"
done < <(find "$ROOT/examples" -type d -name '_lib' | sort)
export ZL_EXTRA_ROOTS="${ROOTS}${ZL_EXTRA_ROOTS:+:$ZL_EXTRA_ROOTS}"

# Programs that must produce identical output on both paths: every runnable
# example. `examples/*/*.zl` covers basics, intermediate and advanced (and any
# future directory) without descending into `_lib` module sources.
EXPECTED=(
    examples/*/*.zl
)

# Programs using constructs the backend still stubs (fail-closed), so they are
# expected NOT to run cleanly yet; report but do not fail the run. Empty today:
# every former gap (closures, lambdas, try/catch, statics, generics end to end)
# now runs identically on both paths. New gaps land here with a comment naming
# the missing construct. Entries are exact file paths; they are skipped from
# the EXPECTED glob when the run executes.
KNOWN_GAPS=()

pass=0
fail=0
gap_fail=0
failed=()
gaps_that_now_pass=()

# Runs once before the sweep: if ZL_COMPILER=ast stops selecting the reference
# compiler, both legs become the MIR pipeline and every comparison below would
# pass vacuously. A harness that cannot tell "identical" from "compared the
# same thing with itself" is worse than no harness, so this fails loudly.
probe=""
for candidate in "${EXPECTED[@]}"; do
    if [[ -f "$candidate" ]]; then probe="$candidate"; break; fi
done
if [[ -z "$probe" ]]; then
    echo "error: no example programs found; nothing to compare" >&2
    exit 2
fi
if ! ZL_COMPILER=ast "$ZL" "$probe" 2>&1 >/dev/null | grep -q "reference compiler"; then
    echo "error: ZL_COMPILER=ast no longer selects the reference compiler;" >&2
    echo "       this harness would compare the MIR pipeline with itself." >&2
    exit 2
fi
if ! "$ZL" --mir-vm "$probe" 2>&1 >/dev/null | grep -q "^pipeline:"; then
    echo "error: --mir-vm no longer runs the MIR pipeline;" >&2
    echo "       this harness would compare the reference path with itself." >&2
    exit 2
fi

run_one() {
    local f="$1"
    local ref_out mir_out ref_rc mir_rc
    # ZL_COMPILER=ast is the reference path: the original AST -> bytecode
    # compiler, kept for exactly this comparison. It is named explicitly
    # because the default path is the MIR pipeline now, and running the
    # pipeline on both sides would be a green line that measured nothing.
    # The guard below is what keeps that from happening silently.
    ref_out="$(ZL_COMPILER=ast "$ZL" "$f" 2>/dev/null)"; ref_rc=$?
    mir_out="$("$ZL" --mir-vm "$f" 2>/dev/null)"; mir_rc=$?
    # Two failures are not agreement. If the reference path cannot even load the
    # program, the comparison carries no information about the backend, so it is
    # reported as a failure of the harness rather than as a pass.
    if [[ $ref_rc -ne 0 ]]; then
        echo "ERROR $f  (reference path failed, rc=$ref_rc - nothing compared)"
        return 1
    fi
    if [[ "$ref_out" == "$mir_out" && "$mir_rc" == 0 ]]; then
        echo "PASS  $f"
        return 0
    else
        echo "DIFF  $f  (ref rc=$ref_rc, mir rc=$mir_rc)"
        return 1
    fi
}

for f in "${EXPECTED[@]}"; do
    [[ -f "$f" ]] || continue
    # A KNOWN_GAPS entry is excluded from the glob: it is run (and reported)
    # by the gap loop below, where a match is a warning rather than a failure.
    in_gaps=false
    for g in "${KNOWN_GAPS[@]}"; do
        if [[ "$f" == "$g" ]]; then in_gaps=true; break; fi
    done
    if $in_gaps; then continue; fi
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
