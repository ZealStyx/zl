#!/usr/bin/env bash
#
# boundary_lint_tests.sh - regressions for the boundary lint itself.
#
# The lint is only as good as its negatives: a rule that passes on the tree
# and on every broken tree it is supposed to catch is a claim, not a check.
# Each case below builds a minimal ZL-shaped checkout in a temporary
# directory, breaks exactly one rule, and asserts the lint fails while naming
# the offending file and line. The positive case proves the legitimate
# pipeline driver is still accepted, and comments/strings/references do not
# trip the construction net.
#
# Usage: bash tests/boundary_lint_tests.sh [repo-root]

set -uo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
LINT="$ROOT/tools/boundary_lint.sh"
if [ ! -f "$LINT" ]; then
    echo "boundary-lint-tests: missing $LINT" >&2
    exit 2
fi

failures=0
checks=0

expect_fail() {  # $1 = case name; the lint output is asserted to name $2:$3
    local name="$1" file="$2" line="$3" output code
    output=$(bash "$LINT" "$WORK" 2>&1)
    code=$?
    checks=$((checks + 1))
    if [ "$code" -ne 1 ]; then
        echo "FAIL: $name: expected exit 1, got $code"
        echo "$output" | sed 's/^/    /'
        failures=$((failures + 1))
        return
    fi
    if ! printf '%s\n' "$output" | grep -q "$file:$line:"; then
        echo "FAIL: $name: diagnostic does not name $file:$line"
        echo "$output" | sed 's/^/    /'
        failures=$((failures + 1))
        return
    fi
    echo "PASS: $name"
}

expect_pass() {
    local name="$1" output code
    output=$(bash "$LINT" "$WORK" 2>&1)
    code=$?
    checks=$((checks + 1))
    if [ "$code" -ne 0 ]; then
        echo "FAIL: $name: expected the boundary to hold, got:"
        echo "$output" | sed 's/^/    /'
        failures=$((failures + 1))
        return
    fi
    echo "PASS: $name"
}

# Builds the minimal skeleton a case starts from: the legitimate pipeline
# driver (the only file allowed to construct the front end), an empty main,
# and empty MIR/parser headers so the include machinery has something to
# walk.
new_worktree() {
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/zl-boundary-lint-test.XXXXXX")
    mkdir -p "$WORK/src/compiler" "$WORK/src/mir" "$WORK/src/native" \
             "$WORK/include/zl/mir" "$WORK/include/zl/parser" "$WORK/include/zl/compiler" \
             "$WORK/include/zl/vm"
    cat > "$WORK/src/compiler/pipeline.cpp" <<'EOF'
namespace zl::pipeline {
struct Pipeline {
    void run() {
        zl::ModuleLoader loader("", {});
        auto checker = std::make_unique<zl::TypeChecker>();
    }
};
}
EOF
    echo "int main() { return 0; }" > "$WORK/src/main.cpp"
    printf '#pragma once\n' > "$WORK/include/zl/mir/type.hpp"
    printf '#pragma once\n' > "$WORK/include/zl/mir/vm_backend.hpp"
    printf '#pragma once\n' > "$WORK/include/zl/parser/ast.hpp"
    printf '#pragma once\n' > "$WORK/include/zl/compiler/type_checker.hpp"
    printf '#pragma once\n' > "$WORK/include/zl/compiler/module_loader.hpp"
    printf '#pragma once\n' > "$WORK/include/zl/vm/value.hpp"
}

cleanup() {
    [ -n "${WORK:-}" ] && rm -rf "$WORK"
}
trap cleanup EXIT

# --- the baseline: the legitimate driver is the only constructor -----------
new_worktree
expect_pass "the pipeline driver constructs the front end and passes"

# --- rule 3: construction outside the driver --------------------------------
new_worktree
cat > "$WORK/src/compiler/bytecode.cpp" <<'EOF'
void drive() {
    ModuleLoader loader;
}
EOF
expect_fail "unqualified ModuleLoader construction" "src/compiler/bytecode.cpp" 2

new_worktree
cat > "$WORK/src/native/select.cpp" <<'EOF'
void drive() {
    TypeChecker checker;
}
EOF
expect_fail "unqualified TypeChecker construction" "src/native/select.cpp" 2

new_worktree
cat > "$WORK/src/main.cpp" <<'EOF'
void drive() {
    zl::ModuleLoader loader;
    zl::TypeChecker checker;
}
EOF
expect_fail "qualified zl::ModuleLoader construction" "src/main.cpp" 2
expect_fail "qualified zl::TypeChecker construction" "src/main.cpp" 3

new_worktree
cat > "$WORK/src/main.cpp" <<'EOF'
void drive() {
    ::zl::ModuleLoader loader;
}
EOF
expect_fail "fully-qualified ::zl::ModuleLoader construction" "src/main.cpp" 2

# Formatting must not be an escape hatch.
new_worktree
cat > "$WORK/src/main.cpp" <<'EOF'
void drive() {
    zl::TypeChecker    checker;
    auto* heap = new ModuleLoader;
    auto unique = std::make_unique<zl::TypeChecker>();
    ModuleLoader
        loader;
    using LoaderAlias = ModuleLoader;
}
EOF
expect_fail "extra spacing around a declaration" "src/main.cpp" 2
expect_fail "new-expression construction" "src/main.cpp" 3
expect_fail "make_unique construction" "src/main.cpp" 4
expect_fail "declaration split across lines" "src/main.cpp" 6
expect_fail "aliasing a front-end type" "src/main.cpp" 7

# Mentions that are not constructions must stay legal.
new_worktree
cat > "$WORK/src/main.cpp" <<'EOF'
// example: ModuleLoader loader; (a comment, not code)
/* block: TypeChecker checker; new ModuleLoader(); make_unique<zl::TypeChecker>() */
const char* message = "ModuleLoader loader; TypeChecker checker;";
void reference(const zl::TypeChecker& checker, zl::ModuleLoader* loader) {
    (void)checker; (void)loader;
}
int main() { return 0; }
EOF
expect_pass "comments, string literals, and references are not constructions"

# --- rule 1: a backend may not acquire a frontend header transitively -------
new_worktree
printf '#include "zl/parser/ast.hpp"\n' >> "$WORK/include/zl/mir/type.hpp"
printf '#include "zl/mir/type.hpp"\n' >> "$WORK/include/zl/mir/vm_backend.hpp"
printf '#include "zl/mir/vm_backend.hpp"\n' > "$WORK/src/mir/vm_backend.cpp"
expect_fail "backend header acquires the AST through a shared MIR header" \
    "src/mir/vm_backend.cpp" 1

# The direct spelling is still caught at its own line.
new_worktree
printf '#include "zl/parser/ast.hpp"\n' > "$WORK/src/native/select.cpp"
expect_fail "backend file directly includes the AST" "src/native/select.cpp" 1

# The native tier must not see the bytecode target either.
new_worktree
printf '#include "zl/vm/value.hpp"\n' > "$WORK/src/native/select.cpp"
expect_fail "native tier acquires the VM runtime" "src/native/select.cpp" 1

# --- the real tree holds -----------------------------------------------------
checks=$((checks + 1))
if output=$(bash "$LINT" "$ROOT" 2>&1) && printf '%s\n' "$output" | grep -q "rules 1-4 hold"; then
    echo "PASS: the repository itself passes the boundary lint"
else
    echo "FAIL: the repository does not pass its own boundary lint:"
    echo "$output" | sed 's/^/    /'
    failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
    echo "boundary-lint-tests: $failures of $checks cases failed"
    exit 1
fi
echo "boundary-lint-tests: $checks cases passed"
