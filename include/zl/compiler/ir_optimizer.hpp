#pragma once

#include "zl/compiler/ir.hpp"

namespace zl::ir {

// Backend-neutral optimization for the canonical IR. The pass is deliberately
// conservative: it removes unreachable blocks, folds literal primitive
// expressions, and leaves ownership/borrow instructions untouched.
[[nodiscard]] Module optimize(const Module& module);

} // namespace zl::ir
