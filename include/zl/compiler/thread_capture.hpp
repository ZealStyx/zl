#pragma once

#include <string>
#include <unordered_set>

#include "zl/parser/ast.hpp"

namespace zl {

class SymbolTable;

// Free-variable / `this` analysis for a lambda body.
//
// This walks the whole expression/statement subtree (the switch is exhaustive
// over NodeKind, so a new node kind fails to compile rather than silently
// dropping references) and records:
//   - names referenced but not bound by a parameter or a nested binding;
//   - whether the body uses `this`.
//
// The result feeds BOTH closure capture (MakeClosure captures exactly these
// names) and the compile-time thread-boundary check, so missing a node kind is
// a correctness bug, not a cosmetic one.
struct LambdaCaptureRefs {
    std::unordered_set<std::string> names;
    // Direct writes to free bindings. Nested closures only write their own
    // captured copies, so their assignments are not propagated here.
    std::unordered_set<std::string> assignedNames;
    bool usesThis{false};
};

// Walk `body`, seeded with the lambda's own parameter names so they are not
// mistaken for captures. Nested lambdas and match-pattern bindings add their
// own bound names as the walk descends.
[[nodiscard]] LambdaCaptureRefs collectLambdaCaptureRefs(
    const AstNode* body, const std::unordered_set<std::string>& params);

// Whether a captured local must be rejected at a Task.spawn / Thread.start
// boundary. Mirrors the runtime gate (capturesAreExplicitlyShared in
// src/vm/native.cpp): only OBJECT handles whose class is `Shared<T>` or one of
// the runtime synchronisation primitives may cross. Primitives and plain func
// values are captured by value but the language rule disallows them on a thread
// closure (see examples/advanced/Threads.zl), so reject them here with a
// source-level message. A name that does not resolve to a local (a top-level
// func/class name) is not a capture.
[[nodiscard]] bool capturedValueCrossesThreadBoundary(const SymbolTable& symbols,
                                                      const std::string& captured);

} // namespace zl
