#include "zl/mir/analysis.hpp"

#include <algorithm>
#include <queue>

namespace zl::mir {

const std::vector<BlockId> ControlFlowGraph::kEmpty{};

ControlFlowGraph::ControlFlowGraph(const Function& function) : function_(function) {
    const std::size_t count = function_.blocks.size();
    order_.reserve(count);
    successors_.resize(count);
    predecessors_.resize(count);
    unwindSuccessors_.resize(count);
    unwindPredecessors_.resize(count);

    for (std::size_t i = 0; i < count; ++i) {
        indexOf_[function_.blocks[i].id] = i;
        order_.push_back(function_.blocks[i].id);
    }

    for (std::size_t i = 0; i < count; ++i) {
        const auto& block = function_.blocks[i];
        for (BlockId successor : block.terminator.successors()) {
            const auto it = indexOf_.find(successor);
            // An edge to a block id that does not exist is recorded as far as
            // this structure is concerned and left for the verifier to report;
            // silently dropping it here would hide exactly the malformed-CFG
            // case the verifier exists to catch.
            if (it == indexOf_.end()) continue;
            successors_[i].push_back(successor);
            predecessors_[it->second].push_back(block.id);
        }
        for (const auto& handler : block.exceptionHandlers) {
            if (handler.block == kNoBlock) continue;
            const auto it = indexOf_.find(handler.block);
            if (it == indexOf_.end()) continue;
            unwindSuccessors_[i].push_back(handler.block);
            unwindPredecessors_[it->second].push_back(block.id);
        }
    }

    computeReachability();
    computeReversePostOrder();
    computeDominators();
    computeDominanceFrontiers();
}

const std::vector<BlockId>& ControlFlowGraph::successors(BlockId id) const {
    const auto it = indexOf_.find(id);
    return it == indexOf_.end() ? kEmpty : successors_[it->second];
}

const std::vector<BlockId>& ControlFlowGraph::predecessors(BlockId id) const {
    const auto it = indexOf_.find(id);
    return it == indexOf_.end() ? kEmpty : predecessors_[it->second];
}

const std::vector<BlockId>& ControlFlowGraph::unwindSuccessors(BlockId id) const {
    const auto it = indexOf_.find(id);
    return it == indexOf_.end() ? kEmpty : unwindSuccessors_[it->second];
}

const std::vector<BlockId>& ControlFlowGraph::unwindPredecessors(BlockId id) const {
    const auto it = indexOf_.find(id);
    return it == indexOf_.end() ? kEmpty : unwindPredecessors_[it->second];
}

void ControlFlowGraph::computeReachability() {
    if (function_.entryBlock == kNoBlock) return;

    std::queue<BlockId> work;
    work.push(function_.entryBlock);
    reachable_.insert(function_.entryBlock);
    while (!work.empty()) {
        const BlockId current = work.front();
        work.pop();
        for (BlockId successor : successors(current)) {
            if (reachable_.insert(successor).second) work.push(successor);
        }
    }

    // Unwind reachability starts from the normally reachable set: a handler is
    // only genuinely reachable if some reachable block installs it.
    //
    // This has to be one traversal following BOTH edge kinds, not a second pass
    // over unwind edges alone. A block reached along an unwind edge is ordinary
    // code from there on - a catch handler jumps to the join block after the try
    // like any other block - so stopping at the handler leaves everything
    // downstream of it looking unreachable. That is not a cosmetic mistake: the
    // join block then gets pruned, and the handler is left jumping nowhere.
    reachableWithUnwind_ = reachable_;
    std::queue<BlockId> unwindWork;
    for (BlockId id : reachable_) unwindWork.push(id);
    while (!unwindWork.empty()) {
        const BlockId current = unwindWork.front();
        unwindWork.pop();
        for (BlockId successor : successors(current)) {
            if (reachableWithUnwind_.insert(successor).second) unwindWork.push(successor);
        }
        for (BlockId successor : unwindSuccessors(current)) {
            if (reachableWithUnwind_.insert(successor).second) unwindWork.push(successor);
        }
    }
}

void ControlFlowGraph::computeReversePostOrder() {
    if (function_.entryBlock == kNoBlock || !isValid(function_.entryBlock)) return;

    std::vector<bool> visited(function_.blocks.size(), false);
    std::vector<BlockId> postOrder;
    postOrder.reserve(function_.blocks.size());

    // Iterative post-order DFS. A recursive walk would overflow the stack on a
    // function with a long chain of blocks, and MIR functions are machine
    // generated rather than written by hand.
    struct Frame { BlockId block; std::size_t next; };
    std::vector<Frame> stack{{function_.entryBlock, 0}};
    visited[indexOf(function_.entryBlock)] = true;

    while (!stack.empty()) {
        Frame& frame = stack.back();
        const std::vector<BlockId>& succs = successors(frame.block);
        if (frame.next < succs.size()) {
            const BlockId next = succs[frame.next++];
            const std::size_t index = indexOf(next);
            if (!visited[index]) {
                visited[index] = true;
                stack.push_back(Frame{next, 0});
            }
            continue;
        }
        postOrder.push_back(frame.block);
        stack.pop_back();
    }

    reversePostOrder_.assign(postOrder.rbegin(), postOrder.rend());
}

void ControlFlowGraph::computeDominators() {
    immediateDominators_.assign(function_.blocks.size(), kNoBlock);
    if (reversePostOrder_.empty()) return;

    // Position of each block in the reverse post-order; -1 for unreachable
    // blocks, which never acquire a dominator.
    std::vector<int> rpoNumber(function_.blocks.size(), -1);
    for (std::size_t i = 0; i < reversePostOrder_.size(); ++i) {
        rpoNumber[indexOf(reversePostOrder_[i])] = static_cast<int>(i);
    }

    const BlockId entry = function_.entryBlock;
    immediateDominators_[indexOf(entry)] = entry;

    // Intersect two already-computed dominator chains by walking them towards
    // the entry in reverse post-order.
    auto intersect = [&](BlockId a, BlockId b) {
        while (a != b) {
            while (rpoNumber[indexOf(a)] > rpoNumber[indexOf(b)]) a = immediateDominators_[indexOf(a)];
            while (rpoNumber[indexOf(b)] > rpoNumber[indexOf(a)]) b = immediateDominators_[indexOf(b)];
        }
        return a;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        // Skip the entry: it is its own dominator by definition.
        for (std::size_t i = 1; i < reversePostOrder_.size(); ++i) {
            const BlockId block = reversePostOrder_[i];
            BlockId newIdom = kNoBlock;
            for (BlockId pred : predecessors(block)) {
                if (immediateDominators_[indexOf(pred)] == kNoBlock) continue;
                newIdom = (newIdom == kNoBlock) ? pred : intersect(pred, newIdom);
            }
            if (newIdom != kNoBlock && immediateDominators_[indexOf(block)] != newIdom) {
                immediateDominators_[indexOf(block)] = newIdom;
                changed = true;
            }
        }
    }
}

void ControlFlowGraph::computeDominanceFrontiers() {
    const std::size_t count = function_.blocks.size();
    // Position in reverse post-order. `reversePostOrder_` already runs
    // entry-first over reachable blocks; numbering it here keeps the dominance
    // frontier loop from searching the order vector repeatedly.
    reversePostOrderNumbers_.assign(count, -1);
    for (std::size_t i = 0; i < reversePostOrder_.size(); ++i) {
        reversePostOrderNumbers_[indexOf(reversePostOrder_[i])] = static_cast<int>(i);
    }

    dominatorTreeChildren_.assign(count, {});
    for (std::size_t i = 0; i < count; ++i) {
        const BlockId block = order_[i];
        const BlockId idom = immediateDominator(block);
        if (idom == kNoBlock || idom == block) continue; // entry or unreachable
        dominatorTreeChildren_[indexOf(idom)].push_back(block);
    }
    for (auto& children : dominatorTreeChildren_) {
        std::sort(children.begin(), children.end());
        children.erase(std::unique(children.begin(), children.end()), children.end());
    }

    // Textbook dominance frontier, in the formulation that walks a join's
    // predecessors up to their common dominator:
    //
    //     for every join block b, for every predecessor p of b, add b to the
    //     frontier of every block on the way from p up to idom(b).
    //
    // The entry block can never appear as a member of a frontier here: the walk
    // stops at `idom(b)` and an entry has no predecessors to start from. (The
    // successor-side formulation gets this wrong unless the root is special
    // cased, which is a bug that shows up as a block parameter on the entry.)
    //
    // Only normal predecessors count. A catch block is entered by an exception,
    // not by falling through, so it is not a join this construction can place a
    // phi at.
    dominanceFrontiers_.assign(count, {});
    for (std::size_t i = 0; i < count; ++i) {
        const BlockId block = order_[i];
        if (!isReachable(block)) continue;
        std::vector<BlockId> predecessors = predecessors_[i];
        std::sort(predecessors.begin(), predecessors.end());
        predecessors.erase(std::unique(predecessors.begin(), predecessors.end()), predecessors.end());
        if (predecessors.size() < 2) continue; // not a join
        const BlockId stop = immediateDominator(block);
        for (BlockId predecessor : predecessors) {
            BlockId runner = predecessor;
            // A join whose immediate dominator is itself (the entry, or an
            // unreachable block) has no frontier to contribute.
            if (stop == kNoBlock || !isReachable(runner)) continue;
            std::size_t guard = 0;
            while (runner != kNoBlock && runner != stop && guard++ <= count + 1) {
                std::vector<BlockId>& frontier = dominanceFrontiers_[indexOf(runner)];
                if (std::find(frontier.begin(), frontier.end(), block) == frontier.end()) {
                    frontier.push_back(block);
                }
                const BlockId next = immediateDominator(runner);
                if (next == runner) break;
                runner = next;
            }
        }
    }
    for (auto& frontier : dominanceFrontiers_) {
        std::sort(frontier.begin(), frontier.end());
        frontier.erase(std::unique(frontier.begin(), frontier.end()), frontier.end());
    }
}

const std::vector<BlockId>& ControlFlowGraph::dominatorTreeChildren(BlockId id) const {
    const auto it = indexOf_.find(id);
    return it == indexOf_.end() ? kEmpty : dominatorTreeChildren_[it->second];
}

const std::vector<BlockId>& ControlFlowGraph::dominanceFrontier(BlockId id) const {
    const auto it = indexOf_.find(id);
    return it == indexOf_.end() ? kEmpty : dominanceFrontiers_[it->second];
}

int ControlFlowGraph::reversePostOrderNumber(BlockId id) const {
    const auto it = indexOf_.find(id);
    if (it == indexOf_.end() || it->second >= reversePostOrderNumbers_.size()) return -1;
    return reversePostOrderNumbers_[it->second];
}

std::vector<BlockId> ControlFlowGraph::deadBlocks() const {
    std::vector<BlockId> dead;
    for (const auto& block : function_.blocks) {
        if (reachable_.count(block.id)) continue;
        if (reachableWithUnwind_.count(block.id)) continue;
        dead.push_back(block.id);
    }
    std::sort(dead.begin(), dead.end());
    return dead;
}

bool ControlFlowGraph::strictlyDominates(BlockId dominator, BlockId block) const {
    return dominator != block && dominates(dominator, block);
}

BlockId ControlFlowGraph::immediateDominator(BlockId id) const {
    const auto it = indexOf_.find(id);
    if (it == indexOf_.end() || it->second >= immediateDominators_.size()) return kNoBlock;
    const BlockId idom = immediateDominators_[it->second];
    // The entry dominates itself; report "no dominator" instead so callers do
    // not have to special-case the fixed point.
    return idom == id ? kNoBlock : idom;
}

std::size_t pruneUnreachableBlocks(Function& function) {
    if (function.blocks.empty()) return 0;
    const ControlFlowGraph cfg(function);

    std::vector<bool> keep(function.blocks.size(), false);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
        if (cfg.reachableWithUnwind().count(function.blocks[i].id)) {
            keep[i] = true;
            ++kept;
        }
    }
    const std::size_t original = function.blocks.size();
    if (kept == original) return 0;

    // Old id -> new id, so every reference can be rewritten in one pass.
    std::unordered_map<BlockId, BlockId> renumber;
    std::vector<BasicBlock> survivors;
    survivors.reserve(kept);
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
        if (!keep[i]) continue;
        survivors.push_back(function.blocks[i]);
        renumber[function.blocks[i].id] = static_cast<BlockId>(survivors.size());
    }

    // A survivor must never reference a pruned block: reachability follows every
    // edge out of every kept block, so anything a kept block can reach was kept.
    // If that invariant is ever broken the pruning is wrong, and emitting a jump
    // to kNoBlock would turn a bad analysis into silently malformed MIR - so
    // report it and leave the function untouched instead.
    bool dangling = false;
    auto rewrite = [&](BlockId& target) {
        if (target == kNoBlock) return;
        const auto it = renumber.find(target);
        if (it == renumber.end()) {
            dangling = true;
            return;
        }
        target = it->second;
    };

    for (auto& block : survivors) {
        block.id = renumber[block.id];
        rewrite(block.terminator.target);
        rewrite(block.terminator.elseBlock);
        for (auto& entry : block.terminator.cases) rewrite(entry.block);
        for (auto& handler : block.exceptionHandlers) rewrite(handler.block);
    }
    if (dangling) return 0;

    function.blocks = std::move(survivors);
    rewrite(function.entryBlock);
    function.rebuildEdges();
    return original - kept;
}

bool ControlFlowGraph::dominates(BlockId dominator, BlockId block) const {
    if (!isValid(dominator) || !isValid(block)) return false;
    if (dominator == block) return true;
    BlockId current = block;
    // Walk up the dominator tree. The chain is finite and terminates at the
    // entry, whose immediate dominator is reported as kNoBlock.
    while (current != kNoBlock) {
        const BlockId next = immediateDominator(current);
        if (next == dominator) return true;
        if (next == kNoBlock || next == current) return false;
        current = next;
    }
    return false;
}

} // namespace zl::mir
