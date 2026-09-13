#!/usr/bin/env bash
#
# boundary_lint.sh - the compiler boundary, as a check instead of a claim.
#
# docs/pipeline.md states rules about where language semantics stop and code
# generation starts. They are easy to state and easy to break quietly: a
# backend that includes the AST still compiles, it just now has an opinion
# about ZL that MIR is supposed to hold. This script reads the source and
# fails when a rule is broken, so the boundary is enforced by the build
# rather than by review.
#
#   rule 1  a backend reads MIR and nothing else (directly AND transitively)
#   rule 2  the typed lowerer reads recorded types; it never infers
#   rule 3  the front end is constructed in exactly one place: the pipeline
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
rule3_violations=0

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

# --- rule 1: a backend reads MIR and nothing else ---------------------------
#
# Two checks. The first is the direct one: the backend's own #include lines
# must not name a forbidden header. The second walks each backend file's
# quoted-include graph to a fixpoint, so a *permitted* header that later gains
# a frontend include (a shared header reaching the AST) fails the boundary too
# - the backend would compile with a new opinion about ZL that it never asked
# for, and that is exactly what this rule exists to prevent.
#
# What a backend may see, transitively (the allowlist below):
#   zl/mir/*    the boundary itself. The frontend-facing seams
#               (zl/mir/lowering.hpp, zl/mir/mir.hpp) are not backend-safe
#               because they include the AST and the checker, so they can
#               never appear in a closure that passes this check.
#   zl/common/* shared utilities, audited to include nothing frontend.
#   zl/compiler/semantic_types.hpp, zl/compiler/operator_rules.hpp
#               the shared language-semantics tables (the operator rules are
#               keyed by Operator, not by a lexer token, on purpose).
#   zl/compiler/native_catalog.hpp, zl/compiler/native_abi.hpp
#               descriptions of the runtime's ABI: target facts.
#   bytecode tier (vm_backend) only:
#               zl/vm/* and zl/compiler/bytecode.hpp - the runtime and the
#               chunk format the bytecode backend translates TO. The target's
#               types are not language semantics.
#   native tier only:
#               zl/native/* - the native tier's own IR. It must not see the
#               VM: that would be the legacy path in disguise.

scan "rule 1: a backend must not see the AST, the checker, the loader, or the legacy IR" \
     '#include "zl/(parser/|compiler/(type_checker|compiler|module_loader|ir|ir_lowering|ir_optimizer|machine_code|native_compiler|pipeline)\.hpp)' \
     '' \
     backend_files

# Resolves a quoted include the way the compiler does: relative to the
# including file's directory first, then against include/.
resolve_include() {
    local spec="$1" from="$2"
    local dir
    dir=$(dirname "$from")
    if [ -f "$dir/$spec" ]; then
        echo "$dir/$spec"
    elif [ -f "include/$spec" ]; then
        echo "include/$spec"
    fi
}

# What the closure of `file` (a backend file) may contain.
backend_allowed_prefixes() {
    case "$1" in
        src/mir/vm_backend.*|include/zl/mir/vm_backend.*)
            echo "include/zl/mir/ include/zl/common/ include/zl/vm/ \
include/zl/compiler/bytecode.hpp include/zl/compiler/semantic_types.hpp \
include/zl/compiler/operator_rules.hpp include/zl/compiler/native_catalog.hpp \
include/zl/compiler/native_abi.hpp"
            ;;
        *)
            echo "include/zl/mir/ include/zl/common/ include/zl/native/ \
include/zl/compiler/semantic_types.hpp include/zl/compiler/operator_rules.hpp \
include/zl/compiler/native_catalog.hpp include/zl/compiler/native_abi.hpp"
            ;;
    esac
}

# Walks the quoted-include closure of one backend file and reports every
# reachable header that is not on that tier's allowlist, with the chain that
# reached it. `parents` is a "child<-parent" map as a space-separated string
# (filenames in this tree contain no whitespace), which keeps the check
# portable to old bash.
transitive_boundary() {
    local root="$1"
    local visited=""
    local frontier=" $root "
    # First-parent map as "child<-parent" entries, space-separated (filenames
    # in this tree contain no whitespace); membership is a substring test on
    # the padded string, which is exact for that reason.
    local parents=""
    local current spec resolved parent next

    while [ -n "$(echo "$frontier" | tr -d ' ')" ]; do
        next=""
        for current in $frontier; do
            case " $visited " in *" $current "*) continue ;; esac
            visited="$visited$current "
            while IFS= read -r spec; do
                [ -n "$spec" ] || continue
                resolved=$(resolve_include "$spec" "$current")
                [ -n "$resolved" ] || continue
                case " $visited " in *" $resolved "*) continue ;; esac
                # Record the first parent (shortest path): it is the chain a
                # newcomer will want to break.
                case " $parents" in
                    *" $resolved<-"*) ;;
                    *) parents="$parents $resolved<-$current" ;;
                esac
                next="$next $resolved"
            done < <(grep -oE '#[[:space:]]*include[[:space:]]*"[^"]+"' "$current" 2>/dev/null \
                         | sed -E 's/.*"([^"]+)".*/\1/')
        done
        frontier="$next"
    done

    local h allowed prefix
    for h in $visited; do
        [ "$h" = "$root" ] && continue
        case "$h" in
            src/*) continue ;;  # a translation unit, not a header the backend sees
        esac
        allowed=0
        for prefix in $(backend_allowed_prefixes "$root"); do
            case "$h" in
                "$prefix"*) allowed=1; break ;;
            esac
        done
        [ "$allowed" -eq 1 ] && continue
        # Walk parents back to the backend file for the chain.
        local chain="$h" cur="$h"
        while [ "$cur" != "$root" ]; do
            parent=$(printf '%s\n' "$parents" | tr ' ' '\n' \
                | awk -v c="$cur" 'index($0, c "<-") == 1 { sub(c "<-", "", $0); print; exit }')
            [ -n "$parent" ] || break
            chain="$chain <- $parent"
            cur="$parent"
        done
        report "rule 1: a backend must not see the AST, the checker, the loader, or the legacy IR" \
            "$root" 1 "transitively includes a forbidden frontend header ($chain)"
    done
}

for bf in $(backend_files); do
    [ -f "$bf" ] || continue
    transitive_boundary "$bf"
done

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

# --- rule 3: the front end is constructed in exactly one place --------------
#
# Stage 1 (ModuleLoader) and stage 2 (TypeChecker) belong to the pipeline. A
# command that constructs them itself has its own roots, its own stdlib-version
# handling and its own exit codes, which is how the CLI grows a second front
# end.
#
# The rule is about CONSTRUCTION, and it scans the whole production tree
# (src/ and include/), not one file. A construction must name the type -
# `ModuleLoader loader`, `new ModuleLoader`, `make_unique<zl::TypeChecker>`,
# an alias - so the check is structural: any production file that is not the
# pipeline driver and mentions a construction pattern fails, no matter how the
# declaration is spaced or line-broken.
#
# Deliberately NOT violations:
#   * the front end's own implementation files (module_loader.*,
#     type_checker.*) - they DEFINE the types; they do not drive them. They
#     need no allowlist entry, and if they ever constructed a second instance
#     of themselves this rule would catch it.
#   * references: `const zl::TypeChecker&` parameters (the typed lowerer, the
#     overload resolver). Holding the pipeline's instance by reference is how
#     the lowerer reads recorded types; rule 2 governs what it may read.
#   * mentions in comments and string literals.
#
# The single allowlist entry is the file that is architecturally responsible
# for front-end ownership: the pipeline driver.

front_end_driver_files() {
    echo "src/compiler/pipeline.cpp"
}

front_end_scan_files() {
    find src include -type f \( -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null | sort
}

# Strips comments (string-literal aware) and reports construction patterns as
# "lineno <TAB> label <TAB> original line". Patterns:
#   value declaration / by-value ownership:  T name  /  T name(...)  /  T name{...}
#                                           /  T name = ...   /  parameters
#   the same split across two lines:         T <newline> name(...)
#   heap construction:                       new T
#   smart-pointer construction:              make_unique<T>(...) / make_shared<T>
#   aliasing a name to smuggle it in:        typedef T X;  /  using X = T;
# Qualification (zl::, ::zl::) is optional on every pattern, so unqualified
# code in or outside namespace zl is covered. Pointer and reference
# declarations (T*, T&, const T&) are references, not constructions, and are
# not flagged.
FRONT_END_AWK='
FNR == 1 { in_comment = 0; in_str = 0; pending = 0 }
{
    line = $0
    sub(/\r$/, "", line)
    code = ""
    i = 1
    n = length(line)
    while (i <= n) {
        c = substr(line, i, 1)
        d = (i < n) ? substr(line, i + 1, 1) : ""
        if (in_comment) {
            if (c == "*" && d == "/") { in_comment = 0; i += 2 } else i++
            continue
        }
        if (in_str) {
            if (c == "\\") i += 2
            else { if ((in_str == 1 && c == "\x27") || (in_str == 2 && c == "\"")) in_str = 0; i++ }
            continue
        }
        if (c == "/" && d == "/") break
        if (c == "/" && d == "*") { in_comment = 1; code = code "  "; i += 2; continue }
        if (c == "\x27") { in_str = 1; i++; continue }
        if (c == "\"") { in_str = 2; i++; continue }
        code = code c
        i++
    }
    trimmed = code
    gsub(/[ \t]/, "", trimmed)
    if (trimmed == "") next
    if (pending) {
        pending = 0
        if (trimmed ~ /^[A-Za-z_][A-Za-z0-9_]*[ \t]*[;=({,)]/)
            print NR "\tdeclaration that started with a front-end type on the previous line\t" line
    }
    if (code ~ /(^|[^A-Za-z0-9_])(::zl::|zl::)?(ModuleLoader|TypeChecker)[ \t]*$/) pending = 1
    if (code ~ /(^|[^A-Za-z0-9_])(::zl::|zl::)?(ModuleLoader|TypeChecker)[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*[;=({,)]/)
        print NR "\tconstruction or by-value ownership of the front end\t" line
    if (code ~ /(^|[^A-Za-z0-9_])new[ \t]+(::zl::|zl::)?(ModuleLoader|TypeChecker)([^A-Za-z0-9_]|$)/)
        print NR "\theap construction of the front end\t" line
    if (code ~ /(make_unique|make_shared)[ \t]*<[^\n>]*(::zl::|zl::)?(ModuleLoader|TypeChecker)([^A-Za-z0-9_]|$)/)
        print NR "\tsmart-pointer construction of the front end\t" line
    if (code ~ /typedef[ \t]+(::zl::|zl::)?(ModuleLoader|TypeChecker)([^A-Za-z0-9_]|$)/)
        print NR "\ttypedef of a front-end type\t" line
    if (code ~ /using[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*(::zl::|zl::)?(ModuleLoader|TypeChecker)([^A-Za-z0-9_]|$)/)
        print NR "\taliasing a front-end type\t" line
}
'

drivers=$(front_end_driver_files | tr '\n' ' ')
for file in $(front_end_scan_files); do
    [ -f "$file" ] || continue
    checked=$((checked + 1))
    case " $drivers" in
        *" $file "*) continue ;;
    esac
    while IFS="$(printf '\t')" read -r lineno label content; do
        [ -n "$lineno" ] || continue
        rule3_violations=$((rule3_violations + 1))
        report "rule 3: only the pipeline may drive the front end (ModuleLoader / TypeChecker)" \
            "$file" "$lineno" "$label: $content"
    done < <(awk "$FRONT_END_AWK" "$file" 2>/dev/null)
done
if [ "$rule3_violations" -gt 0 ]; then
    echo "boundary-lint: the front end is constructed in exactly one place: src/compiler/pipeline.cpp."
    echo "  Route the work through zl::pipeline::Pipeline (or take the front end by const"
    echo "  reference where rule 2 allows it) instead of constructing it."
fi

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
