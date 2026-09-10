#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "zl/mir/analysis.hpp"
#include "zl/mir/function.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// Explicit data flow: slot --> block parameter
// ---------------------------------------------------------------------------
//
// MIR carries values in two interchangeable forms:
//
//   * memory - a `Slot`, written by `Store` and read by `Load`. This is what
//     lowering produces, because ZL `var`s are mutable and reassignable;
//   * a value - a `BlockParameter`, handed to a block by its incoming edges.
//     This is the SSA form, and it is what makes data flow explicit rather than
//     hidden behind a cell.
//
// This pass converts the first into the second for the slots where it is
// provably safe. A program that conceptually writes the same variable on two
// branches and reads it after they join
//
//     if condition { x = 10 } else { x = 20 }
//     return x
//
// comes out of lowering as `store slot/%10`, `store slot/%20`, `load slot`; after
// this pass it is a block parameter at the join, taking `%10` on the one edge and
// `%20` on the other, and the load reads that parameter. The merged value is
// then visible to every consumer, verifier included, instead of being something
// a later pass would have to reconstruct by watching stores.
//
// The algorithm is the classic one, and it uses the machinery in
// `analysis.hpp`:
//
//   1. decide which slots may be promoted (below);
//   2. place a block parameter at the iterated dominance frontier of the blocks
//      that write the slot - precisely the join points where two definitions
//      meet;
//   3. rename: walk the dominator tree from the entry keeping, per slot, the
//      value currently in it; replace each `Load` with that value, each `Store`
//      with an update of it, and fill in every successor's edge argument.
//
// Promotion is applied on a copy and only committed when it completes, so a
// shape this pass cannot handle leaves the function exactly as it was rather
// than half-rewritten.

struct SsaPromotionReport {
    // Slots that became block parameters.
    std::size_t promotedSlots{0};
    // Block parameters created (one per join point per slot).
    std::size_t parametersAdded{0};
    std::size_t loadsRemoved{0};
    std::size_t storesRemoved{0};
    // Slots left in memory form, with the reason. Anything here is a *declined*
    // opportunity, never an error: the function is still correct.
    std::vector<std::string> skipped;
    // True when the function was rewritten.
    bool changed{false};

    [[nodiscard]] std::string describe() const;
};

// Promotes every safely-promotable slot in `function` to a block parameter.
//
// A slot is promoted only when all of the following hold, each of which removes
// a way the rewrite could change meaning:
//
//   * the slot is not a catch binding and is never named by an exception
//     handler - the runtime writes those outside the CFG;
//   * the function has no exception handler chains at all: an unwind edge is a
//     path the dominator tree does not model, so "the value reaching here" is
//     not defined on it;
//   * the slot's storage is plain GC: an `owned`/`borrow`/`shared` slot carries
//     a lifetime contract whose release is a memory operation, not a value;
//   * nothing reads the slot before it is written on any path (checked as
//     reaching definitions), so no read of an uninitialised cell is introduced
//     as a read of a value that does not exist;
//   * the slot is only ever touched by `Load` and `Store` - a `Move`, `Drop`,
//     `Borrow` or `EndBorrow` on it means memory is being managed through it.
//
// The result is expected to still verify; the caller re-verifies.
[[nodiscard]] SsaPromotionReport promoteSlotsToBlockParameters(Function& function);

// ---------------------------------------------------------------------------
// Operand surgery
// ---------------------------------------------------------------------------
//
// Small, exact rewrites used by the pass above and by any later one.

// Replaces every use of `from` with `to`, in instructions and in terminators
// (including block-parameter arguments). Returns the number of uses rewritten.
std::size_t replaceValueUses(Function& function, ValueId from, const Operand& to);

// Removes the instruction at `index` in `block`. The caller is responsible for
// the instruction having no effect worth keeping (a pure load, say); this does
// not check.
void removeInstruction(Function& function, BlockId block, std::size_t index);

} // namespace zl::mir
