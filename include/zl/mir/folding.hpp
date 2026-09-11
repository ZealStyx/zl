#pragma once

// ---------------------------------------------------------------------------
// Compile-time evaluation of MIR instructions
// ---------------------------------------------------------------------------
//
// One question is answered here, for the whole optimiser: **what value does
// this instruction produce, given that all of its operands are constants?**
//
// It is a separate header for the same reason the verifier is separate from
// the builder: the answer has to be *one* answer. The constant analysis
// (`dataflow.hpp`) uses it to report "this value is always this constant";
// the constant-folding pass uses it to rewrite the MIR; the effect
// classification (`effects.hpp`) uses it to decide whether an instruction can
// still raise. Three callers, one evaluator, so they cannot drift.
//
// The evaluator is deliberately more conservative than a textbook constant
// folder, and every refusal is a refusal for a reason:
//
//   * **Overflow is not folded.** ZL's integer arithmetic *raises* on
//     overflow (see `VM::binaryArith`). Folding `INT64_MAX + 1` into a wrapped
//     constant would replace a thrown error with a value, which is exactly the
//     kind of silent behaviour change this optimiser exists to avoid. An
//     operation that would raise reports "not known" and the instruction
//     stays in the MIR, where it raises at runtime.
//   * **Division and modulo by zero are not folded**, for the same reason.
//   * **Out-of-range shifts are not folded** — the VM raises.
//   * **Mixed-kind operands are not folded.** `+` means concatenation as soon
//     as either side is a string (`binaryArith`), so int+double and
//     string+int have a result whose exact spelling is the VM's business.
//     Only same-kind pairs are folded, and only when the instruction's own
//     result type agrees (checked by the caller through `constantTypeOf`).
//   * **Nothing with side effects is folded.** Only opcodes whose effect set
//     is pure are considered; the opcode table in `effects.hpp` is the single
//     statement of which those are.

#include <cstdint>
#include <optional>

#include "zl/mir/function.hpp"
#include "zl/mir/instruction.hpp"
#include "zl/mir/value.hpp"

namespace zl::mir {

// The value an instruction produces when every operand is a constant, or
// nullopt when that is not known (not constant, not foldable, or folding would
// change behaviour).
//
// The result is a `Constant`, not a pool id: a folded value need not already
// exist in the module's constant pool. `Module::internConstant` is how a
// caller turns it into something an operand can name.
[[nodiscard]] std::optional<Constant> foldInstruction(const Module& module, const Instruction& instruction);

// The TypeId a constant denotes, or 0 when the constant's type is not pinned
// down by the constant alone. `Nil` and `EnumMember` report 0 on purpose: a
// nil constant can stand for any nullable type, and an enum member's type is
// the enum, which only the type table knows.
[[nodiscard]] std::uint32_t constantTypeOf(const Module& module, const Constant& constant);

// True when `constant` may stand for a value of `type`. This is the check a
// pass performs before substituting a constant for a value: an operand that
// names a constant must still have the type the instruction claims.
[[nodiscard]] bool constantMatchesType(const Module& module, const Constant& constant, std::uint32_t type);

} // namespace zl::mir
