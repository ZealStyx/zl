#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zl/mir/analysis.hpp"
#include "zl/mir/function.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// Data-flow: definitions, uses, liveness, constants
// ---------------------------------------------------------------------------
//
// Everything here is *derived*: built from a function, never stored in it, and
// never mutating it. The point is that a pass can ask "who defined this value",
// "where else is it used", "is it live past this point", or "is it always this
// constant" without each pass growing its own half-right walk over the IR.
//
// The whole file rests on one definition - `forEachValueUse` below - of what
// counts as a use. Def-use chains, liveness and the constant analysis all go
// through it, so they cannot disagree about whether, say, a terminator's block
// parameter arguments are uses (they are: the predecessor evaluates them).
//
// Value space: a *value* is a parameter, a block parameter, or a temp
// (`ValueId` in value.hpp). Slots are not values - they are memory - so slot
// liveness is tracked in a parallel map keyed by SlotId.

// ---------------------------------------------------------------------------
// Walking every use
// ---------------------------------------------------------------------------

// Every operand the instruction reads. Instructions never read their own
// result, so a result temp is not a use.
template <typename InstructionT, typename F>
void forEachInstructionUse(InstructionT& instruction, F&& visit) {
    for (auto& operand : instruction.operands) {
        if (!operand.isNone()) visit(operand);
    }
}

// Every operand the terminator reads: its returned / thrown / tested value plus
// every block-parameter argument on every edge. The arguments belong here
// because the terminator's block is what evaluates them.
template <typename TerminatorT, typename F>
void forEachTerminatorUse(TerminatorT& terminator, F&& visit) {
    if (!terminator.value.isNone()) visit(terminator.value);
    for (auto& arguments : terminator.edgeArguments) {
        for (auto& argument : arguments) {
            if (!argument.isNone()) visit(argument);
        }
    }
}

// Every operand read anywhere in a function, in program order (block by block,
// instructions then terminator). Exception-handler catch slots are deliberately
// not included: the runtime writes them on entry to a catch block, which the
// CFG models as an unwind edge rather than as a use.
template <typename FunctionT, typename F>
void forEachValueUse(FunctionT& function, F&& visit) {
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) forEachInstructionUse(instruction, visit);
        forEachTerminatorUse(block.terminator, visit);
    }
}

// ---------------------------------------------------------------------------
// Definition / use tracking
// ---------------------------------------------------------------------------

struct ValueDefinition {
    ValueId value;
    // The block the definition is established in. For a function parameter this
    // is the entry block: the caller supplied it before execution started.
    BlockId block{kNoBlock};
    // Position within the block; -1 for a function parameter and for a block
    // parameter, both of which are defined on entry rather than by an
    // instruction.
    long instructionIndex{-1};
    std::uint32_t type{0};
    // True when the value is defined outside the function body (a parameter).
    bool isFunctionParameter{false};
};

struct ValueUse {
    ValueId value;
    BlockId block{kNoBlock};
    // Position within the block; -1 for a use in the terminator or on an edge.
    long instructionIndex{-1};
    std::uint32_t type{0};
};

// Definitions and uses for one function, queried in both directions.
//
// Deterministic by construction: definitions are collected in program order and
// uses in program order, so a dump or a diagnostic never depends on hash
// iteration order.
class DefUseInfo {
public:
    explicit DefUseInfo(const Function& function);

    [[nodiscard]] bool hasDefinition(ValueId value) const;
    [[nodiscard]] const ValueDefinition* definition(ValueId value) const;
    [[nodiscard]] const std::vector<ValueUse>& uses(ValueId value) const;
    [[nodiscard]] bool isUsed(ValueId value) const { return !uses(value).empty(); }

    // Every definition, in program order (parameters first, then each block's
    // parameters and instruction results).
    [[nodiscard]] const std::vector<ValueDefinition>& definitions() const { return definitions_; }
    // Every value defined or used, in the same deterministic order.
    [[nodiscard]] const std::vector<ValueId>& values() const { return values_; }
    // Uses of values this function never defines. Empty for verified MIR; a
    // pass that drops a definition by accident finds out here.
    [[nodiscard]] const std::vector<ValueUse>& undefinedUses() const { return undefinedUses_; }
    // Values that are defined but never read.
    [[nodiscard]] std::vector<ValueId> unusedDefinitions() const;

private:
    void noteDefinition(ValueId value, BlockId block, long index, std::uint32_t type,
                        bool isFunctionParameter);
    void noteUse(const Operand& operand, BlockId block, long index);

    std::unordered_map<ValueId, std::size_t> definitionIndex_;
    std::unordered_map<ValueId, std::vector<ValueUse>> uses_;
    std::vector<ValueDefinition> definitions_;
    std::vector<ValueId> values_;
    std::vector<ValueUse> undefinedUses_;
};

// ---------------------------------------------------------------------------
// A basic monotone data-flow solver
// ---------------------------------------------------------------------------
//
// The textbook recipe, written once so each analysis is just a lattice plus a
// transfer function. `Analysis` supplies:
//
//   using State = ...;                                  (copyable, comparable)
//   State boundaryIn() const;     - the fact at function entry  (forward)
//   State boundaryOut() const;    - the fact at function exit   (backward)
//   State transfer(const BasicBlock&, const State& in) const;
//   void  merge(State& into, const State& other) const;  - the join, monotone
//   bool  equal(const State&, const State&) const;
//
// `transfer` is `in -> out` for the forward solver and `out -> in` for the
// backward one: each solver hands the transfer the fact it already knows and
// takes back the other half.
//
// Only blocks reachable from the entry participate, in reverse post-order; that
// is what keeps "unreachable" from being indistinguishable from "no inputs".

template <typename Analysis>
struct DataFlowResult {
    using State = typename Analysis::State;
    std::unordered_map<BlockId, State> in;
    std::unordered_map<BlockId, State> out;
};

template <typename Analysis>
DataFlowResult<Analysis> solveForward(const ControlFlowGraph& cfg, const Function& function,
                                      Analysis& analysis) {
    DataFlowResult<Analysis> result;
    std::unordered_map<BlockId, bool> computed;
    const std::size_t blockCount = function.blocks.size();

    for (std::size_t guard = 0; guard < blockCount + 8; ++guard) {
        bool changed = false;
        for (BlockId id : cfg.reversePostOrder()) {
            const BasicBlock* block = function.block(id);
            if (!block) continue;

            typename Analysis::State input;
            bool haveInput = false;
            if (id == function.entryBlock) {
                input = analysis.boundaryIn();
                haveInput = true;
            }
            for (BlockId pred : cfg.predecessors(id)) {
                const auto it = result.out.find(pred);
                if (it == result.out.end()) continue;
                if (!haveInput) {
                    input = it->second;
                    haveInput = true;
                } else {
                    analysis.merge(input, it->second);
                }
            }
            if (!haveInput) continue; // unreachable: no fact

            typename Analysis::State output = analysis.transfer(*block, input);
            const auto previousIn = result.in.find(id);
            const auto previousOut = result.out.find(id);
            const bool fresh = previousOut == result.out.end();
            if (fresh || !analysis.equal(previousIn->second, input) ||
                !analysis.equal(previousOut->second, output)) {
                changed = true;
            }
            result.in[id] = std::move(input);
            result.out[id] = std::move(output);
        }
        if (!changed) break;
    }
    return result;
}

template <typename Analysis>
DataFlowResult<Analysis> solveBackward(const ControlFlowGraph& cfg, const Function& function,
                                       Analysis& analysis) {
    DataFlowResult<Analysis> result;
    const std::size_t blockCount = function.blocks.size();

    for (std::size_t guard = 0; guard < blockCount + 8; ++guard) {
        bool changed = false;
        // Reverse of reverse post-order: successors are visited before
        // predecessors, so a straight-line function converges in one pass.
        const auto& order = cfg.reversePostOrder();
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const BlockId id = *it;
            const BasicBlock* block = function.block(id);
            if (!block) continue;

            typename Analysis::State output;
            bool haveOutput = false;
            for (BlockId successor : cfg.successors(id)) {
                const auto found = result.in.find(successor);
                if (found == result.in.end()) continue;
                if (!haveOutput) {
                    output = found->second;
                    haveOutput = true;
                } else {
                    analysis.merge(output, found->second);
                }
            }
            if (!haveOutput) output = analysis.boundaryOut();

            typename Analysis::State input = analysis.transfer(*block, output);
            const auto previousIn = result.in.find(id);
            const auto previousOut = result.out.find(id);
            const bool fresh = previousIn == result.in.end();
            if (fresh || !analysis.equal(previousIn->second, input) ||
                !analysis.equal(previousOut->second, output)) {
                changed = true;
            }
            result.in[id] = std::move(input);
            result.out[id] = std::move(output);
        }
        if (!changed) break;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Liveness
// ---------------------------------------------------------------------------
//
// Two live sets per block, computed in one backward pass:
//
//   * live *slots* - memory cells whose current contents may still be read.
//     This is the one that matters for ZL locals, which live in slots.
//   * live *values* - SSA values (parameters, block parameters, temps) that may
//     still be read. A value's liveness says how far its definition's result
//     travels, which is what a pass needs before it rewrites uses.
//
// "Live in" means "read along some path out of the block before being
// overwritten"; "live out" is the same at the block's exit.

struct LivenessFact {
    std::unordered_set<SlotId> slots;
    std::unordered_set<ValueId> values;

    friend bool operator==(const LivenessFact& a, const LivenessFact& b) noexcept {
        return a.slots == b.slots && a.values == b.values;
    }
};

class LivenessAnalysis {
public:
    explicit LivenessAnalysis(const Function& function);

    [[nodiscard]] const std::unordered_set<SlotId>& liveSlotsIn(BlockId block) const;
    [[nodiscard]] const std::unordered_set<SlotId>& liveSlotsOut(BlockId block) const;
    [[nodiscard]] const std::unordered_set<ValueId>& liveValuesIn(BlockId block) const;
    [[nodiscard]] const std::unordered_set<ValueId>& liveValuesOut(BlockId block) const;
    [[nodiscard]] bool isSlotLiveIn(BlockId block, SlotId slot) const;
    [[nodiscard]] bool isValueLiveIn(BlockId block, ValueId value) const;
    // Values defined in the function but never live anywhere: their definitions
    // are dead. Reported, not removed - this is an analysis, not an optimiser.
    [[nodiscard]] const std::vector<ValueId>& deadDefinitions() const { return deadDefinitions_; }

private:
    std::unordered_map<BlockId, std::unordered_set<SlotId>> liveSlotsIn_;
    std::unordered_map<BlockId, std::unordered_set<SlotId>> liveSlotsOut_;
    std::unordered_map<BlockId, std::unordered_set<ValueId>> liveValuesIn_;
    std::unordered_map<BlockId, std::unordered_set<ValueId>> liveValuesOut_;
    std::vector<ValueId> deadDefinitions_;
    static const std::unordered_set<SlotId> kNoSlots;
    static const std::unordered_set<ValueId> kNoValues;
};

// ---------------------------------------------------------------------------
// Suspension liveness
// ---------------------------------------------------------------------------
//
// `await` is the only suspension point: it parks the async frame on the
// scheduler and resumes later with the payload. Everything live across that
// park - every SSA value still to be read plus every slot still to be loaded,
// together with each one's type and lifetime contract - must survive the round
// trip, which is what this analysis reports, one point per Await.
//
// The sets are derived from block liveness refined to the instruction: the
// live-out fact of the block, walked back over the terminator and the
// instructions after the await. The await's own result is excluded (it is
// produced by the resumption, not preserved across the suspension), as are
// constants and statics (not frame state). Borrow slots appear with their
// BORROW ownership and borrow source intact, so a backend can see that the
// borrow binding itself - not just the value - is part of the preserved frame;
// the verifier separately proves every such borrow roots in owned-frame
// storage. Both lists are deterministically ordered (values by ValueId, slots
// by SlotId).

struct SuspendedValue {
    ValueId value;
    std::uint32_t type{0};
};

struct SuspendedSlot {
    SlotId slot{0};
    std::uint32_t type{0};
    zl::OwnershipKind ownership{zl::OwnershipKind::GC};
    std::string borrowSource;
};

struct SuspensionPoint {
    BlockId block{kNoBlock};
    long instructionIndex{-1};
    std::vector<SuspendedValue> values;
    std::vector<SuspendedSlot> slots;
};

class SuspensionLiveness {
public:
    explicit SuspensionLiveness(const Function& function);

    // One entry per Await in the function, in program order.
    [[nodiscard]] const std::vector<SuspensionPoint>& points() const { return points_; }
    // The point at this Await, or nullptr when the instruction is not one.
    [[nodiscard]] const SuspensionPoint* pointAt(BlockId block, long instructionIndex) const;

private:
    std::vector<SuspensionPoint> points_;
};

// ---------------------------------------------------------------------------
// Constant values
// ---------------------------------------------------------------------------
//
// A basic forward data-flow over the "is this value a known constant
// everywhere" lattice:
//
//     unknown                          <- no information; also the join of two
//                                         different constants
//     constant k
//
// A temp is constant when its defining instruction is a pure operation whose
// operands are all constants and whose result can be folded; a block parameter
// is constant when every incoming edge hands it the same constant. Nothing is
// rewritten - the analysis only *reports* what is already known, which is the
// part of constant propagation a later pass can rely on.
//
// The fixpoint runs over values rather than over blocks, because a value's
// constant-ness depends on other values' constant-ness, not on where it sits:
// the edge arguments make a block parameter's value depend on its
// predecessors, and following the value graph handles that without a second
// notion of "the fact flowing along an edge".
class ConstantAnalysis {
public:
    explicit ConstantAnalysis(const Module& module, const Function& function);

    // The constant this value always has, or nullopt when it is not known to be
    // constant. The result names an existing module constant, so it can be used
    // as an operand directly.
    [[nodiscard]] std::optional<ConstId> constantOf(ValueId value) const;
    [[nodiscard]] std::optional<ConstId> constantOf(const Operand& operand) const;
    // Values known to be constant, in definition order.
    [[nodiscard]] const std::vector<ValueId>& constantValues() const { return constantValues_; }

private:
    void run();
    // The constant a *definition* implies right now, or nullopt.
    [[nodiscard]] std::optional<ConstId> evaluate(ValueId value) const;
    [[nodiscard]] std::optional<ConstId> evaluateOperand(const Operand& operand) const;
    [[nodiscard]] std::optional<Constant> fold(const Instruction& instruction) const;
    [[nodiscard]] std::optional<ConstId> poolIdOf(const Constant& constant) const;

    const Module& module_;
    const Function& function_;
    std::unordered_map<ValueId, ConstId> constantIds_;
    std::vector<ValueId> constantValues_;
};

// ---------------------------------------------------------------------------
// Reachability helpers
// ---------------------------------------------------------------------------
//
// Named wrappers over ControlFlowGraph so a pass reads as intent rather than as
// "walk reversePostOrder and compare sizes":
//
//   reachableBlocks - reachable from the entry along normal edges.
//   liveBlocks      - reachable when unwind edges count too (a catch block is
//                     live but not normally reachable).
//   deadBlocks      - neither: nothing can enter them at all.
[[nodiscard]] std::vector<BlockId> reachableBlocks(const ControlFlowGraph& cfg);
[[nodiscard]] std::vector<BlockId> deadBlocks(const ControlFlowGraph& cfg);

} // namespace zl::mir
