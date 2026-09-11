#include "zl/mir/ssa.hpp"

#include <algorithm>
#include <deque>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "zl/mir/dataflow.hpp"

namespace zl::mir {

std::string SsaPromotionReport::describe() const {
    std::ostringstream out;
    out << "promoted " << promotedSlots << " slot(s), added " << parametersAdded
        << " block parameter(s), removed " << loadsRemoved << " load(s) and " << storesRemoved
        << " store(s)";
    if (!skipped.empty()) {
        out << "; declined:";
        for (const auto& reason : skipped) out << " " << reason << ";";
    }
    return out.str();
}

namespace {

// ---------------------------------------------------------------------------
// Step 1: which slots can be promoted
// ---------------------------------------------------------------------------

// "Is this slot written on *every* path that reaches this point?" - the classic
// definite-assignment question, asked as a forward must-analysis.
//
// The lattice is two-valued and the discipline is what makes it a must-analysis:
//
//     in[b]  = AND over predecessors' out    (assigned only if every path did)
//     out[b] = writes(b) OR in[b]
//
// Encoding the fact as "the set of blocks that wrote" - the obvious-looking
// choice - is wrong: at a join reached from two different writers the
// intersection is empty even though both paths assigned, so it would decline
// exactly the merge this pass exists to convert. The fact has to be "assigned
// or not", with the entry starting at "not".
//
// A read of the slot may be rewritten into the value form exactly when this
// holds at the read; otherwise the read is of a cell that may never have been
// written and the value form of it would name a value that does not exist.
class DefiniteAssignment {
public:
    using State = bool; // true = assigned on every path reaching here

    explicit DefiniteAssignment(SlotId slot) : slot_(slot) {}

    State boundaryIn() const { return false; } // nothing is assigned before entry
    State boundaryOut() const { return false; } // unused: forward problem

    State transfer(const BasicBlock& block, const State& in) const {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode == Opcode::Store && instruction.slot == slot_) return true;
        }
        return in;
    }

    void merge(State& into, const State& other) const { into = into && other; }
    bool equal(State a, State b) const { return a == b; }

private:
    SlotId slot_;
};

// Every slot mentioned by `Load`/`Store`/`Move`/`Drop`/`Borrow`/`EndBorrow`,
// with what was observed, so the eligibility test is one pass over the body.
struct SlotProfile {
    std::vector<std::pair<BlockId, std::size_t>> stores; // where it is written
    std::vector<std::pair<BlockId, std::size_t>> loads;  // where it is read
    bool touchedInOtherWay{false};
};

bool slotIsEligible(const Function& function, SlotId slot, const SlotProfile& profile,
                    std::string& reason) {
    const Slot* info = function.slot(slot);
    if (!info) {
        reason = "slot " + std::to_string(slot) + " does not exist";
        return false;
    }
    if (info->isCatchBinding) {
        reason = "slot '" + info->name + "' is a catch binding (filled in by the runtime)";
        return false;
    }
    for (const auto& block : function.blocks) {
        for (const auto& handler : block.exceptionHandlers) {
            if (handler.catchSlot == slot) {
                reason = "slot '" + info->name + "' receives a caught value";
                return false;
            }
        }
    }
    if (profile.touchedInOtherWay) {
        reason = "slot '" + info->name + "' is moved, dropped or borrowed, not just stored";
        return false;
    }
    if (info->ownership != zl::OwnershipKind::GC) {
        reason = "slot '" + info->name + "' has non-GC storage";
        return false;
    }
    if (info->type == 0) {
        reason = "slot '" + info->name + "' has no type";
        return false;
    }
    if (profile.stores.empty()) {
        reason = "slot '" + info->name + "' is never stored";
        return false;
    }
    if (profile.loads.empty()) {
        reason = "slot '" + info->name + "' is never loaded";
        return false;
    }
    // Every stored value must already carry the slot's declared type. A Load
    // produces the *slot's* type - that is the storage contract the store was
    // checked against (a dynamic value crosses through a refine before it is
    // stored). The value form hands the stored operand itself to later uses,
    // so promoting a slot whose stores are retyped relative to it would let
    // uses observe the raw value's type and silently drop the declared one:
    // a slot of type `unknown` holding a `list<int>` would start reading as
    // `list<int>` with no refine anywhere. Such a slot stays in memory form,
    // where every read is typed by the contract.
    for (const auto& [blockId, index] : profile.stores) {
        const BasicBlock* block = function.block(blockId);
        if (!block || index >= block->instructions.size()) continue;
        const Instruction& store = block->instructions[index];
        if (store.operands.empty() || store.operands[0].isNone()) continue;
        if (store.operands[0].type != info->type) {
            reason = "slot '" + info->name + "' stores a value whose static type (type " +
                     std::to_string(store.operands[0].type) + ") differs from the slot's declared type (type " +
                     std::to_string(info->type) + ")";
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Operand surgery
// ---------------------------------------------------------------------------

std::size_t replaceValueUses(Function& function, ValueId from, const Operand& to) {
    if (!from.valid()) return 0;
    std::size_t replaced = 0;
    auto rewrite = [&](Operand& operand) {
        if (valueOf(operand) == from) {
            operand = to;
            ++replaced;
        }
    };
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            for (auto& operand : instruction.operands) rewrite(operand);
        }
        auto& terminator = block.terminator;
        rewrite(terminator.value);
        for (auto& arguments : terminator.edgeArguments) {
            for (auto& argument : arguments) rewrite(argument);
        }
    }
    return replaced;
}

void removeInstruction(Function& function, BlockId block, std::size_t index) {
    BasicBlock* target = function.block(block);
    if (!target || index >= target->instructions.size()) return;
    target->instructions.erase(target->instructions.begin() + static_cast<long>(index));
}

// ---------------------------------------------------------------------------
// Promotion
// ---------------------------------------------------------------------------

namespace {

// One promoted slot's value at a point in the dominator walk.
struct CurrentValue {
    bool defined{false};
    Operand operand;
};

// A parameter created by this pass, and which slot it stands for. SlotId is
// 1-based, so 0 means "not created for a slot" (never happens here).
struct CreatedParameter {
    BlockParamId id{kNoBlockParam};
    SlotId slot{0};
};

bool promoteOnCopy(const Function& original, Function& working, SsaPromotionReport& report) {
    // --- 1. profile every slot ------------------------------------------
    std::unordered_map<SlotId, SlotProfile> profiles;
    for (const auto& block : working.blocks) {
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            const Instruction& instruction = block.instructions[i];
            if (instruction.slot == 0) continue;
            SlotProfile& profile = profiles[instruction.slot];
            switch (instruction.opcode) {
                case Opcode::Store:
                    profile.stores.emplace_back(block.id, i);
                    break;
                case Opcode::Load:
                    profile.loads.emplace_back(block.id, i);
                    break;
                case Opcode::Move:
                case Opcode::Drop:
                case Opcode::Borrow:
                case Opcode::EndBorrow:
                case Opcode::StaticStore:
                case Opcode::StaticLoad:
                    profile.touchedInOtherWay = true;
                    break;
                default:
                    break;
            }
        }
    }

    ControlFlowGraph cfg(working);

    // Stable order: slot ids ascending, so the report and the generated
    // parameter numbering do not depend on hash order.
    std::vector<SlotId> slots;
    for (const auto& [slot, _] : profiles) slots.push_back(slot);
    std::sort(slots.begin(), slots.end());

    std::vector<SlotId> eligible;
    for (SlotId slot : slots) {
        std::string reason;
        if (slotIsEligible(working, slot, profiles[slot], reason)) eligible.push_back(slot);
        else report.skipped.push_back(reason);
    }
    if (eligible.empty()) return false;

    // Definite assignment, per slot, to make sure no read is rewritten into a
    // value that may not exist.
    std::unordered_map<SlotId, DataFlowResult<DefiniteAssignment>> assignment;
    std::vector<SlotId> promotable;
    for (SlotId slot : eligible) {
        DefiniteAssignment analysis(slot);
        assignment[slot] = solveForward<DefiniteAssignment>(cfg, working, analysis);
        const DataFlowResult<DefiniteAssignment>& flow = assignment[slot];
        bool safe = true;
        for (const auto& block : working.blocks) {
            const auto it = flow.in.find(block.id);
            if (it == flow.in.end()) {
                // No fact at all means the block is unreachable, so the read
                // (if any) is dead code this pass cannot reason about. Decline
                // rather than rewrite a path nothing discusses.
                for (const auto& instruction : block.instructions) {
                    if (instruction.opcode == Opcode::Load && instruction.slot == slot) {
                        safe = false;
                        const Slot* info = working.slot(slot);
                        report.skipped.push_back(
                            "slot '" + (info ? info->name : std::to_string(slot)) + "' is read in b" +
                            std::to_string(block.id) + ", which is unreachable");
                        break;
                    }
                }
                if (!safe) break;
                continue;
            }
            // Within the block, an earlier store counts for a later load.
            bool assigned = it->second;
            for (const auto& instruction : block.instructions) {
                if (instruction.opcode == Opcode::Store && instruction.slot == slot) assigned = true;
                if (instruction.opcode == Opcode::Load && instruction.slot == slot && !assigned) {
                    safe = false;
                    const Slot* info = working.slot(slot);
                    report.skipped.push_back("slot '" + (info ? info->name : std::to_string(slot)) +
                                             "' is read in block b" + std::to_string(block.id) +
                                             " where it may not have been written");
                    break;
                }
            }
            if (!safe) break;
        }
        if (safe) promotable.push_back(slot);
    }
    if (promotable.empty()) return false;

    // --- 2. place block parameters at the iterated dominance frontier ----
    std::unordered_map<SlotId, std::unordered_set<BlockId>> writeBlocks;
    for (SlotId slot : promotable) {
        for (const auto& [block, index] : profiles[slot].stores) {
            (void)index;
            writeBlocks[slot].insert(block);
        }
    }

    std::unordered_map<BlockId, std::vector<CreatedParameter>> parametersByBlock;
    BlockParamId nextParameterId = working.nextBlockParameterId();

    // Place parameters, then keep only the slots whose placement is complete:
    // every parameter's block must be *definitely assigned* on entry, which is
    // exactly the condition under which every incoming edge has a value to hand
    // the parameter. A loop header whose back edge defines a value but whose
    // entry edge does not would otherwise get a parameter with nothing to pass
    // it, and the value form of that slot does not exist. Placement is a
    // superset of what is usable, so it is filtered here rather than by
    // guessing up front.
    std::vector<SlotId> usable;
    for (SlotId slot : promotable) {
        std::deque<BlockId> worklist(writeBlocks[slot].begin(), writeBlocks[slot].end());
        std::unordered_set<BlockId> placed;
        std::unordered_set<BlockId> queued(worklist.begin(), worklist.end());
        std::size_t guard = 0;
        const std::size_t guardLimit = (working.blocks.size() + 4) * (working.blocks.size() + 4) + 16;
        while (!worklist.empty() && guard++ < guardLimit) {
            const BlockId block = worklist.front();
            worklist.pop_front();
            for (BlockId frontier : cfg.dominanceFrontier(block)) {
                if (placed.count(frontier)) continue;
                placed.insert(frontier);
                BasicBlock* target = working.block(frontier);
                if (!target) continue;
                const Slot* info = working.slot(slot);
                BlockParameter parameter;
                parameter.id = nextParameterId++;
                parameter.name = (info ? info->name : std::string("slot")) + "$" +
                                 std::to_string(parameter.id);
                parameter.type = info ? info->type : 0;
                target->parameters.push_back(parameter);
                parametersByBlock[frontier].push_back(CreatedParameter{parameter.id, slot});
                if (!writeBlocks[slot].count(frontier) && queued.insert(frontier).second) {
                    worklist.push_back(frontier);
                }
            }
        }

        // Filter: a parameter is usable only where the slot is definitely
        // assigned on entry to its block.
        bool complete = true;
        for (const auto& [block, parameters] : parametersByBlock) {
            for (const auto& parameter : parameters) {
                if (parameter.slot != slot) continue;
                const auto fact = assignment[slot].in.find(block);
                if (fact == assignment[slot].in.end() || !fact->second) { complete = false; break; }
            }
            if (!complete) break;
        }
        if (!complete) {
            // Undo this slot's placement: drop its parameters from the blocks
            // and from the bookkeeping. Nothing else has been rewritten yet.
            for (auto& [block, parameters] : parametersByBlock) {
                BasicBlock* target = working.block(block);
                std::vector<CreatedParameter> keep;
                for (const auto& parameter : parameters) {
                    if (parameter.slot != slot) { keep.push_back(parameter); continue; }
                    if (target) {
                        auto& list = target->parameters;
                        list.erase(std::remove_if(list.begin(), list.end(),
                                                  [&](const BlockParameter& p) {
                                                      return p.id == parameter.id;
                                                  }),
                                   list.end());
                    }
                }
                parameters = std::move(keep);
            }
            const Slot* info = working.slot(slot);
            report.skipped.push_back("slot '" + (info ? info->name : std::to_string(slot)) +
                                     "' is not definitely assigned at one of its merge points");
            continue;
        }
        usable.push_back(slot);
        report.parametersAdded += placed.size();
    }
    promotable = std::move(usable);
    if (report.parametersAdded == 0) return false;

    // --- 3. rename -------------------------------------------------------
    //
    // Walk the dominator tree from the entry. For each slot, `current` is the
    // value the slot holds on this walk; a Load reads it, a Store updates it,
    // and a block parameter pushes its own value on entry. Because the walk is
    // in dominator order, every block sees the values defined on the way in,
    // and the join blocks see the parameters instead of a stale cell.
    std::unordered_map<SlotId, std::vector<CurrentValue>> stacks;
    for (SlotId slot : promotable) stacks[slot]; // ensure a (possibly empty) stack

    // Which slot each created parameter stands for, to fill edge arguments.
    std::unordered_map<BlockParamId, SlotId> parameterSlot;
    for (const auto& [block, parameters] : parametersByBlock) {
        (void)block;
        for (const auto& parameter : parameters) parameterSlot[parameter.id] = parameter.slot;
    }

    bool failed = false;
    std::string failure;

    // A depth-first walk of the dominator tree with explicit enter/exit
    // actions. This is the shape the classic renaming algorithm needs: a
    // block's pushes have to be popped when the walk leaves its subtree, or a
    // sibling would see a value that does not reach it. Iterative rather than
    // recursive because MIR functions are machine-generated and a long
    // dominator chain would overflow the stack.
    struct Frame {
        BlockId block{kNoBlock};
        std::size_t nextChild{0};
        // One entry per push, in order, so the exit action unwinds exactly what
        // this block added.
        std::vector<SlotId> pushedSlots;
    };
    std::vector<Frame> walk;
    if (working.entryBlock != kNoBlock && cfg.isValid(working.entryBlock)) {
        walk.push_back(Frame{working.entryBlock, 0, {}});
    }

    while (!walk.empty() && !failed) {
        Frame& frame = walk.back();
        BasicBlock* block = working.block(frame.block);
        if (!block) {
            walk.pop_back();
            continue;
        }

        if (frame.nextChild == 0) {
            // --- enter ------------------------------------------------------
            for (const auto& parameter : block->parameters) {
                const auto it = parameterSlot.find(parameter.id);
                if (it == parameterSlot.end()) continue; // made by another pass
                stacks[it->second].push_back(
                    CurrentValue{true, Operand::blockParam(parameter.id, parameter.type)});
                frame.pushedSlots.push_back(it->second);
            }

            std::vector<std::size_t> loadsToErase;
            std::vector<std::size_t> storesToErase;
            for (std::size_t i = 0; i < block->instructions.size(); ++i) {
                Instruction& instruction = block->instructions[i];
                if (instruction.opcode == Opcode::Load && stacks.count(instruction.slot)) {
                    const auto& stack = stacks[instruction.slot];
                    if (stack.empty() || !stack.back().defined) {
                        failed = true;
                        failure = "slot " + std::to_string(instruction.slot) +
                                  " is loaded before any definition reaches it";
                        break;
                    }
                    // Replacing every use of the load's result with the value
                    // the load read is exactly what the load meant, so this is
                    // sound for uses anywhere the load's result is valid.
                    const Operand value = stack.back().operand;
                    if (instruction.result != kNoTemp) {
                        replaceValueUses(working, tempValue(instruction.result), value);
                    }
                    loadsToErase.push_back(i);
                    continue;
                }
                if (instruction.opcode == Opcode::Store && stacks.count(instruction.slot)) {
                    if (instruction.operands.empty() || instruction.operands[0].isNone()) {
                        failed = true;
                        failure = "slot " + std::to_string(instruction.slot) +
                                  " is stored without a value";
                        break;
                    }
                    stacks[instruction.slot].push_back(
                        CurrentValue{true, instruction.operands[0]});
                    frame.pushedSlots.push_back(instruction.slot);
                    storesToErase.push_back(i);
                    continue;
                }
            }
            if (failed) break;

            // Erase in one descending pass. Erasing the loads first and then
            // indexing the stores with positions collected before that would
            // delete the wrong instructions - every index after the first
            // removal has shifted.
            std::vector<std::size_t> toErase;
            toErase.insert(toErase.end(), loadsToErase.begin(), loadsToErase.end());
            toErase.insert(toErase.end(), storesToErase.begin(), storesToErase.end());
            std::sort(toErase.begin(), toErase.end());
            toErase.erase(std::unique(toErase.begin(), toErase.end()), toErase.end());
            for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
                removeInstruction(working, frame.block, *it);
            }
            report.loadsRemoved += loadsToErase.size();
            report.storesRemoved += storesToErase.size();

            // --- fill the arguments this block passes along its edges ------
            Terminator& terminator = block->terminator;
            const auto successors = terminator.successors();
            if (terminator.edgeArguments.size() < successors.size())
                terminator.edgeArguments.resize(successors.size());
            for (std::size_t edge = 0; edge < successors.size(); ++edge) {
                const BasicBlock* target = working.block(successors[edge]);
                if (!target) continue;
                std::vector<Operand> arguments;
                arguments.reserve(target->parameters.size());
                for (const auto& parameter : target->parameters) {
                    const auto slotIt = parameterSlot.find(parameter.id);
                    if (slotIt == parameterSlot.end()) {
                        failed = true;
                        failure = "block b" + std::to_string(target->id) +
                                  " has a parameter this pass cannot supply an argument for";
                        break;
                    }
                    const auto& stack = stacks[slotIt->second];
                    if (stack.empty() || !stack.back().defined) {
                        failed = true;
                        failure = "no value reaches block b" + std::to_string(target->id) +
                                  " for block parameter ^" + std::to_string(parameter.id);
                        break;
                    }
                    arguments.push_back(stack.back().operand);
                }
                if (failed) break;
                terminator.edgeArguments[edge] = std::move(arguments);
            }
            if (failed) break;
        }

        // --- descend, or exit ---------------------------------------------
        const std::vector<BlockId>& children = cfg.dominatorTreeChildren(frame.block);
        if (frame.nextChild < children.size()) {
            const BlockId child = children[frame.nextChild++];
            walk.push_back(Frame{child, 0, {}});
            continue;
        }
        for (SlotId slot : frame.pushedSlots) {
            auto& stack = stacks[slot];
            if (!stack.empty()) stack.pop_back();
        }
        walk.pop_back();
    }

    if (failed) {
        // Put the copy back the way it was and report nothing promoted: a
        // shape this pass cannot handle must not be half-rewritten.
        working = original;
        report.parametersAdded = 0;
        report.loadsRemoved = 0;
        report.storesRemoved = 0;
        report.skipped.push_back("promotion abandoned: " + failure);
        return false;
    }

    report.promotedSlots = promotable.size();
    return true;
}

} // namespace

SsaPromotionReport promoteSlotsToBlockParameters(Function& function) {
    SsaPromotionReport report;

    // An unwind edge is a path the dominator tree does not model, so "the value
    // reaching a catch block" is not something this construction can answer.
    // Decline rather than guess.
    for (const auto& block : function.blocks) {
        if (!block.exceptionHandlers.empty()) {
            report.skipped.push_back("function has exception handler chains");
            return report;
        }
        if (!block.parameters.empty()) {
            // A function that already merges values (lowered or promoted
            // earlier) is left alone: this pass is a one-shot constructor, not
            // a fixed point, and running it twice would add parameters for
            // slots whose loads are already gone.
            report.skipped.push_back("function already has block parameters");
            return report;
        }
    }

    Function working = function;
    if (promoteOnCopy(function, working, report)) {
        function = std::move(working);
        report.changed = true;
    } else {
        report.promotedSlots = 0;
        report.changed = false;
        report.parametersAdded = 0;
        report.loadsRemoved = 0;
        report.storesRemoved = 0;
    }
    return report;
}

} // namespace zl::mir
