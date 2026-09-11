#pragma once
#include "zl/mir/verifier.hpp"

namespace zl::mir {
// Supplemental non-mutating analyses used by the verifier. Structural/type
// verification remains the authority; these analyses never repair MIR.
[[nodiscard]] SafetyProperty instructionProperty(Opcode opcode) noexcept;
void appendSafetyAnalysis(const Module& module, const Function& function,
                          const VerifierOptions& options, VerificationReport& report);
} // namespace zl::mir
