#!/usr/bin/env bash
#
# boundary_lint.sh - the compiler boundary, as a check instead of a claim.
#
# docs/pipeline.md states three rules about where language semantics stop and
# code generation starts. They are easy to state and easy to break quietly: a
# backend that includes the AST still compiles, it just now has an opinion about
# ZL that MIR is supposed to hold. This script reads the source and fails when a
# rule is broken, so the boundary is enforced by the build rather than by
# review.
#
#   rule 1  a backend reads MIR and nothing else
#   rule 2  the typed lowerer reads recorded types; it never infers
#   rule 3  the front end is driven in exactly one place
#   rule 4  the legacy IR is frozen: one consumer, and no new ones
#
# Usage: bash tools/boundary_lint.sh [repo-root]
# Exit:  0 all rules hold, 1 a rule was broken, 2 usage.

set -uo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
if [ ! -d "$ROOT/src" ]; then
    echo "boundary-lint: not a ZL checkout: $ROOT" >&2
    exit 2
fi

cd "$ROOT" || exit 2

violations=0
checked=0

# Reports one broken rule: the file, the line, and what to do about it.
report() {
    local rule="$1" file="$2" line="$3" detail="$4"
    echo "boundary-lint: $rule"
    echo "  $file:$line: $detail"
    violations=$((violations + 1))
}

# Scans every named file for a pattern. `ignore` is an extended regex of
# `file:line:content` matches that are deliberate; it must stay explicit, so an
# exception is visible in the rule that grants it.
scan() {
    local rule="$1" pattern="$2" ignore="$3"
    shift 3
    local files=()
    while IFS= read -r file; do
        [ -n "$file" ] && files+=("$file")
    done < <("$@" 2>/dev/null)

    local file match content
    for file in "${files[@]}"; do
        [ -f "$file" ] || continue
        checked=$((checked + 1))
        while IFS= read -r match; do
            [ -n "$match" ] || continue
            if [ -n "$ignore" ] && printf '%s' "$match" | grep -Eq "$ignore"; then
                continue
            fi
            report "$rule" "$file" "${match%%:*}" "${match#*:}"
        done < <(grep -nE "$pattern" "$file" 2>/dev/null)
    done
}

# --- rule 1: a backend reads MIR and nothing else ---------------------------
#
# The backends are the bytecode translator and everything in the native tier.
# They may see MIR, their own target's types, the shared language-semantics
# tables (zl::OperatorRules, zl::SemanticTypes - the single source of truth for
# what `+` means, which is exactly what stops them re-deciding it), and the
# native catalog (a description of the runtime's ABI, i.e. a target fact). They
# may not see the AST, the parser, the type checker, the module loader, the
# pipeline that drives them, or the legacy IR.
backend_files() {
    ls src/native/*.cpp include/zl/native/*.hpp \
       src/mir/vm_backend.cpp include/zl/mir/vm_backend.hpp 2>/dev/null
}

scan "rule 1: a backend must not see the AST, the checker, the loader, or the legacy IR" \
     '#include "zl/(parser/|compiler/(type_checker|compiler|module_loader|ir|ir_lowering|ir_optimizer|machine_code|native_compiler|pipeline)\.hpp)' \
     '' \
     backend_files

# --- rule 2: the lowerer reads recorded types; it never infers --------------
#
# Lowering is the one file on the boundary that is allowed to see both sides,
# and its contract is that every type it writes comes from the checker's record
# (TypeChecker::expressionType). Any other use of the checker - check(),
# re-resolving an overload, a second opinion - is a second type system.
scan "rule 2: the typed lowerer must read types through expressionType() and nothing else" \
     'checker\.[A-Za-z_]+' \
     'checker\.expressionType\(' \
     bash -c 'ls src/mir/lowering.cpp'

# --- rule 3: the front end is driven in exactly one place -------------------
#
# Stage 1 (ModuleLoader) and stage 2 (TypeChecker) belong to the pipeline. A
# command that constructs them itself has its own roots, its own stdlib-version
# handling and its own exit codes, which is how the CLI grows a second front end.
scan "rule 3: only the pipeline may drive the front end (ModuleLoader / TypeChecker)" \
     '(zl::)?ModuleLoader [a-zA-Z_]|(zl::)?TypeChecker [a-zA-Z_]' \
     '' \
     bash -c 'ls src/main.cpp'

# --- rule 4: the legacy IR is frozen ----------------------------------------
#
# zl::ir is not grown and not ported to; it has exactly one consumer left, the
# portable-C++ emitter for the @native subset, and that consumer prints a note
# saying so. A fifth consumer would be a second lowering path in disguise.
legacy_ir_consumers() {
    grep -ln '#include "zl/compiler/\(ir\|ir_lowering\|ir_optimizer\|machine_code\)\.hpp"' \
        $(ls src/*/*.cpp include/zl/*/*.hpp 2>/dev/null) 2>/dev/null |
        grep -vE '^src/compiler/(ir|ir_lowering|ir_optimizer|machine_code)\.cpp$' |
        grep -vE '^include/zl/compiler/(ir|ir_lowering|ir_optimizer|machine_code)\.hpp$'
}

for file in $(legacy_ir_consumers); do
    checked=$((checked + 1))
    if [ "$file" != "src/compiler/native_compiler.cpp" ]; then
        report "rule 4: the legacy zl::ir must keep exactly one consumer" "$file" 1 \
               "includes the legacy IR; the only permitted consumer is src/compiler/native_compiler.cpp (--emit-native)"
    fi
done

if [ "$violations" -ne 0 ]; then
    echo "boundary-lint: $violations violation(s) of the compiler boundary"
    exit 1
fi

echo "boundary-lint: rules 1-4 hold ($checked files scanned)"
