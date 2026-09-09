#pragma once

#include <string>
#include <vector>

#include "zl/compiler/type_checker.hpp"
#include "zl/mir/function.hpp"
#include "zl/parser/ast.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// Lowering boundary: AST + semantic analysis -> MIR
// ---------------------------------------------------------------------------
//
// This is the one place the typed AST becomes MIR, and the one place MIR is
// allowed to know the AST exists. Everything downstream of it - the verifier,
// optimisation passes, backends - sees only `zl::mir::Module`.
//
// Two rules define the boundary:
//
//   1. **The MIR lowerer does no type inference.** Every static type it writes
//      comes from `TypeChecker::expressionType`, the same record the checker
//      used to accept the program. Re-deriving types here would create a second
//      opinion about the language, and the two would eventually disagree.
//   2. **Unsupported does not mean wrong.** When the lowerer meets a construct
//      it cannot represent yet, it marks that *function* incomplete with a
//      reason and keeps going. The module stays well-formed MIR; the flag is
//      what stops a backend from treating a partial translation as a whole one.
//      Lowering never throws for a program the type checker accepted.

struct LoweringOptions {
    // Lower members of generic classes as templates whose bodies use TypeParam
    // types. Turning this off skips them, which keeps the module smaller while
    // generic support is still maturing.
    bool lowerGenericTemplates{true};
    // When false, the first unsupported construct fails the whole lowering
    // instead of marking one function incomplete. Useful in tests.
    bool tolerateUnsupported{true};
    // Include class layouts and statics in the module. Backends need them for
    // field resolution; a caller that only wants control flow can skip them.
    bool emitLayouts{true};
};

struct LoweringResult {
    // True when lowering ran to completion. Note that this can be true while
    // individual functions are still incomplete - see `incompleteFunctions`.
    bool success{false};
    Module module;
    // Human-readable notes: what was skipped and why.
    std::vector<std::string> diagnostics;
    // Qualified names of functions that are not a complete translation of
    // their source body.
    std::vector<std::string> incompleteFunctions;

    // True when every lowered function is a complete translation.
    [[nodiscard]] bool complete() const { return incompleteFunctions.empty(); }
};

// Lowers a program that has already been type-checked. `checker` must be the
// same checker instance that checked `program`, because the lowering reads its
// recorded expression types by AST node identity.
[[nodiscard]] LoweringResult lowerProgram(const zl::Program& program, const zl::TypeChecker& checker,
                                          const LoweringOptions& options = {});

} // namespace zl::mir
