#pragma once

// ---------------------------------------------------------------------------
// Side-effect classification for MIR instructions
// ---------------------------------------------------------------------------
//
// Every optimisation in this layer answers one question before it rewrites
// anything: *what does this instruction do, besides compute its result?* This
// header is the single answer, stated per opcode and refined per instruction,
// so that no pass has to invent its own notion of "safe to remove", "safe to
// reorder", or "observable". Where the classification cannot prove an
// instruction is safe, the passes leave it alone - the framework's rule is
// that an unproven optimisation is a skipped optimisation, never a guess.
//
// The model is a bit set. Each bit names one *way* an instruction can interact
// with the world beyond its operands, which is the thing a pass must reason
// about:
//
//   Pure          - computes a value from its operands and nothing else.
//   ReadsMemory   - reads a slot, a field, an element, or a static.
//   WritesMemory  - writes one.
//   Allocation    - creates a new GC-managed value (identity is fresh).
//   RuntimeCheck  - exists to raise: a type assertion, a null check, a
//                   handle-validity check. Removing one removes an error.
//   MayThrow      - may transfer control along an unwind edge.
//   ObservableIO  - visible outside the program (log).
//   Synchronization - takes part in the concurrency order (locks, atomics,
//                   channels, semaphores, conditions, shared cells).
//   VolatileRead  - two reads of the same thing may differ (atomic load,
//                   is_alive, size/available).
//   Calls         - transfers control to another ZL function or native.
//   TaskEffect    - creates, awaits, blocks on, cancels or ignores a task.
//   ThreadBoundary - carries values to another thread.
//   Suspension    - parks the async frame on the scheduler.
//   NativeResource - a handle/callback/FFI lifecycle event: deterministic,
//                   outside the collector, and ordered.
//   Ownership     - a move, borrow, end_borrow or drop: an ownership event
//                   the language's lifetime rules are stated in terms of.
//   UnwindBarrier - establishes a scope whose cleanup must run on the way out
//                   (a scoped lock, a cleanup block).
//
// Two consequences are worth stating because they are the point:
//
//   * Arithmetic is `Pure | MayThrow`, not `Pure`. ZL raises on integer
//     overflow, on division by zero and on an out-of-range shift, so an
//     `add` is a computation *and* a possible unwind. An instruction is only
//     removable once `MayThrow` has been discharged - which
//     `classifyInstruction` does when the operands are constants and
//     `foldInstruction` proves the operation cannot raise.
//
//   * The concurrency and ownership families are never removable, never
//     reorderable and never deduplicated. A redundant-looking `atomic_load`
//     is not redundant (the value may change between two reads) and a
//     redundant-looking `drop` is not either (releasing a resource exactly
//     once is the whole contract).

#include <cstdint>
#include <string>

#include "zl/mir/function.hpp"
#include "zl/mir/instruction.hpp"

namespace zl::mir {

enum class Effect : std::uint32_t {
    None = 0,
    Pure = 1u << 0,
    ReadsMemory = 1u << 1,
    WritesMemory = 1u << 2,
    Allocation = 1u << 3,
    RuntimeCheck = 1u << 4,
    MayThrow = 1u << 5,
    ObservableIO = 1u << 6,
    Synchronization = 1u << 7,
    VolatileRead = 1u << 8,
    Calls = 1u << 9,
    TaskEffect = 1u << 10,
    ThreadBoundary = 1u << 11,
    Suspension = 1u << 12,
    NativeResource = 1u << 13,
    Ownership = 1u << 14,
    UnwindBarrier = 1u << 15,
};

using EffectSet = std::uint32_t;

constexpr EffectSet effectsOf(Effect effect) noexcept { return static_cast<EffectSet>(effect); }

constexpr EffectSet operator|(EffectSet left, Effect right) noexcept {
    return left | static_cast<EffectSet>(right);
}
constexpr EffectSet operator|(Effect left, Effect right) noexcept {
    return static_cast<EffectSet>(left) | static_cast<EffectSet>(right);
}
constexpr EffectSet& operator|=(EffectSet& left, Effect right) noexcept { return left = left | right; }

[[nodiscard]] constexpr bool hasEffect(EffectSet effects, Effect effect) noexcept {
    return (effects & static_cast<EffectSet>(effect)) != 0;
}

[[nodiscard]] const char* effectName(Effect effect) noexcept;

// "pure reads-memory may-throw", for diagnostics and dumps. Stable order, so a
// printed effect set is comparable between two runs of the optimiser.
[[nodiscard]] std::string describeEffects(EffectSet effects);

// The static classification of an opcode: everything an instruction with this
// opcode *can* do, before anything is known about its operands.
[[nodiscard]] EffectSet classifyOpcode(Opcode opcode) noexcept;

// The classification of one instruction: the opcode's static effects refined
// by what can be proved about this instance. The only refinement today is
// `MayThrow` - discharged when every operand is a constant and folding proves
// the operation cannot raise - which is what lets dead arithmetic be deleted
// after it has been folded, and *only* then.
[[nodiscard]] EffectSet classifyInstruction(const Module& module, const Instruction& instruction);

// --- the questions a pass actually asks -----------------------------------

// True when the instruction may still transfer control along an unwind edge.
// A pass that deletes, sinks or hoists code must ask this first.
[[nodiscard]] bool instructionMayThrow(const Module& module, const Instruction& instruction);

// True when an instruction whose result is unused may simply be deleted.
//
// This is the narrowest rule in the framework on purpose: an instruction
// qualifies only when reading memory is the most it can do and it cannot
// raise. Everything else - a call, a store, a lock, a drop, a check, an
// allocation, an `await` - stays, even when nothing uses its result, because
// each one is a thing the program is *for* rather than a step towards one.
[[nodiscard]] bool isRemovableIfResultUnused(EffectSet effects) noexcept;
[[nodiscard]] bool isRemovableIfResultUnused(const Module& module, const Instruction& instruction);

// True when the instruction's effect is externally visible, which is what the
// differential validator counts (see differential.hpp).
[[nodiscard]] bool isObservableEffect(EffectSet effects) noexcept;

} // namespace zl::mir
