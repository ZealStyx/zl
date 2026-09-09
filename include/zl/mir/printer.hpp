#pragma once

#include <string>

#include "zl/mir/function.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// MIR textual form
// ---------------------------------------------------------------------------
//
// A readable dump of a module. Two jobs: it is how a person checks that lowering
// produced what it meant to, and it is the stable shape that MIR regression
// tests assert against. It is not a serialisation format - there is no parser -
// and it deliberately prints every type inline so a reader never has to go
// looking up an id.

struct PrinterOptions {
    // Print source locations next to instructions.
    bool locations{true};
    // Print the materialised control-flow edge list after each function, so a
    // reader can see the CFG the verifier checked rather than only the one the
    // terminators imply.
    bool edges{true};
    // Print the slot table and the type table before the body.
    bool declarations{true};
};

[[nodiscard]] std::string printModule(const Module& module, const PrinterOptions& options = {});
[[nodiscard]] std::string printFunction(const Module& module, const Function& function,
                                        const PrinterOptions& options = {});
[[nodiscard]] std::string printInstruction(const Module& module, const Function& function,
                                           const Instruction& instruction);
[[nodiscard]] std::string printOperand(const Module& module, const Operand& operand);
[[nodiscard]] std::string printTerminator(const Module& module, const Terminator& terminator);

} // namespace zl::mir
