#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zl/mir/function.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// Control-flow queries
// ---------------------------------------------------------------------------
//
// Everything here is derived, never stored in the MIR itself. The one exception
// is `Function::edges`, which the verifier cross-checks against these results so
// a stale edge list cannot survive into a backend.
//
// Two edge relations matter and they are kept apart throughout:
//
//   *normal* edges - terminator successors. These are real control flow and
//       are the only edges that participate in dominance and in dataflow.
//   *unwind* edges - exception handler targets. Execution arrives here by
//       raising, not by falling through, so an unwind target is reachable but
//       is not dominated by the block that names it. Treating an unwind edge as
//       a normal edge would make every catch block appear to dominate the code
//       it catches, which is exactly backwards.

class ControlFlowGraph {
public:
    // Builds the query structure for `function`. Blocks are addressed by index
    // into `function.blocks`; `indexOf` maps a BlockId to that index.
    explicit ControlFlowGraph(const Function& function);

    [[nodiscard]] std::size_t blockCount() const { return successors_.size(); }
    [[nodiscard]] bool isValid(BlockId id) const { return indexOf_.count(id) != 0; }
    [[nodiscard]] std::size_t indexOf(BlockId id) const {
        const auto it = indexOf_.find(id);
        return it == indexOf_.end() ? static_cast<std::size_t>(-1) : it->second;
    }
    [[nodiscard]] BlockId idAt(std::size_t index) const { return order_[index]; }

    // Normal-flow successors, in terminator order (duplicates preserved: a
    // Branch whose two targets are the same block really does have two edges).
    [[nodiscard]] const std::vector<BlockId>& successors(BlockId id) const;
    // Normal-flow predecessors.
    [[nodiscard]] const std::vector<BlockId>& predecessors(BlockId id) const;
    // Unwind targets named by this block's handler chain.
    [[nodiscard]] const std::vector<BlockId>& unwindSuccessors(BlockId id) const;
    // Blocks whose handler chain names this block.
    [[nodiscard]] const std::vector<BlockId>& unwindPredecessors(BlockId id) const;

    // Blocks reachable from the entry along normal edges only.
    [[nodiscard]] const std::unordered_set<BlockId>& reachable() const { return reachable_; }
    // Blocks reachable when unwind edges are followed too. Superset of
    // reachable(): a catch block is reachable, just not by falling through.
    [[nodiscard]] const std::unordered_set<BlockId>& reachableWithUnwind() const { return reachableWithUnwind_; }
    [[nodiscard]] bool isReachable(BlockId id) const { return reachable_.count(id) != 0; }

    // Entry-first reverse post-order over normal edges. Blocks unreachable from
    // the entry are omitted, which is what makes this a safe iteration order for
    // forward dataflow.
    [[nodiscard]] const std::vector<BlockId>& reversePostOrder() const { return reversePostOrder_; }

    // Immediate dominator over normal edges, or kNoBlock for the entry and for
    // unreachable blocks. Computed with the iterative Cooper-Harvey-Kennedy
    // algorithm over `reversePostOrder_`.
    [[nodiscard]] BlockId immediateDominator(BlockId id) const;
    [[nodiscard]] bool dominates(BlockId dominator, BlockId block) const;

private:
    void computeReachability();
    void computeReversePostOrder();
    void computeDominators();

    const Function& function_;
    std::unordered_map<BlockId, std::size_t> indexOf_;
    std::vector<BlockId> order_;
    std::vector<std::vector<BlockId>> successors_;
    std::vector<std::vector<BlockId>> predecessors_;
    std::vector<std::vector<BlockId>> unwindSuccessors_;
    std::vector<std::vector<BlockId>> unwindPredecessors_;
    std::unordered_set<BlockId> reachable_;
    std::unordered_set<BlockId> reachableWithUnwind_;
    std::vector<BlockId> reversePostOrder_;
    std::vector<BlockId> immediateDominators_;
    static const std::vector<BlockId> kEmpty;
};

// Removes blocks that no edge reaches from the entry, then renumbers the
// survivors so block ids stay dense and positional, rewriting every reference to
// them (terminator targets, exception handler targets, the entry block) and
// rebuilding the edge list.
//
// Reachability here follows unwind edges as well as normal ones: a catch block
// is not dead just because nothing falls through to it.
//
// Two reasons this lives with the analysis rather than only in a future
// optimiser. A lowering pass that gives up partway through a function leaves
// blocks it created but never wired up, and the MIR invariant "every block is
// reachable" is much easier to hold unconditionally than to hold only for
// functions that lowered cleanly. And any pass that deletes control flow needs
// exactly this, so it should exist before the passes do.
// Returns the number of blocks removed.
std::size_t pruneUnreachableBlocks(Function& function);

} // namespace zl::mir
