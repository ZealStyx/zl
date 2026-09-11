#include "zl/mir/dataflow.hpp"

#include <algorithm>

namespace zl::mir {
namespace {

// Slot access, stated once. A slot is memory, so "which opcodes read it" and
// "which opcodes write it" are a property of the opcode, not of the operand
// list: `Store` names a slot and consumes a value, `Load` names a slot and
// produces one. `Move`, `Drop` and `EndBorrow` read the cell and then leave it
// unusable, which makes them both a use and a definition.
bool opcodeWritesSlot(Opcode opcode) {
    switch (opcode) {
        case Opcode::Store:
        case Opcode::Move:
        case Opcode::Drop:
        case Opcode::EndBorrow:
        case Opcode::Borrow: // writes the destination borrow slot
            return true;
        default:
            return false;
    }
}

bool opcodeReadsSlot(Opcode opcode) {
    switch (opcode) {
        case Opcode::Load:
        case Opcode::Move:
        case Opcode::Drop:
        case Opcode::EndBorrow:
            return true;
        default:
            return false;
    }
}

// One instruction's backward liveness step, shared by the block transfer and
// the suspension-point refinement so the two cannot disagree about what an
// instruction reads or writes.
void applyLiveStep(const Instruction& instruction, LivenessFact& live) {
    if (instruction.result != kNoTemp) live.values.erase(tempValue(instruction.result));
    forEachInstructionUse(instruction, [&](const Operand& operand) {
        const ValueId value = valueOf(operand);
        if (value.valid()) live.values.insert(value);
    });
    if (instruction.slot != 0) {
        // Kill before gen, so an opcode that both reads and writes the cell
        // (Move/Drop/EndBorrow) leaves it live on entry rather than dead.
        if (opcodeWritesSlot(instruction.opcode)) live.slots.erase(instruction.slot);
        if (opcodeReadsSlot(instruction.opcode)) live.slots.insert(instruction.slot);
    }
}

std::optional<Constant> makeInt(std::int64_t value) {
    Constant c;
    c.kind = ConstKind::Int;
    c.intValue = value;
    return c;
}
std::optional<Constant> makeDouble(double value) {
    Constant c;
    c.kind = ConstKind::Double;
    c.doubleValue = value;
    return c;
}
std::optional<Constant> makeBool(bool value) {
    Constant c;
    c.kind = ConstKind::Bool;
    c.boolValue = value;
    return c;
}

bool isInt(const std::optional<Constant>& c) { return c && c->kind == ConstKind::Int; }
bool isDouble(const std::optional<Constant>& c) { return c && c->kind == ConstKind::Double; }
bool isBool(const std::optional<Constant>& c) { return c && c->kind == ConstKind::Bool; }

} // namespace

// ---------------------------------------------------------------------------
// DefUseInfo
// ---------------------------------------------------------------------------

DefUseInfo::DefUseInfo(const Function& function) {
    // Parameters are defined by the caller, so their definition point is the
    // entry block. Recording them keeps "is this value defined" one question
    // instead of "parameter or temp?".
    for (ParamId p = 0; p < function.parameters.size(); ++p) {
        noteDefinition(paramValue(p), function.entryBlock, -1, function.parameters[p].type, true);
    }

    for (const auto& block : function.blocks) {
        for (const auto& parameter : block.parameters) {
            noteDefinition(blockParamValue(parameter.id), block.id, -1, parameter.type, false);
        }
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            const auto& instruction = block.instructions[i];
            const long index = static_cast<long>(i);
            if (instruction.result != kNoTemp) {
                noteDefinition(tempValue(instruction.result), block.id, index, instruction.resultType,
                               false);
            }
            forEachInstructionUse(instruction, [&](const Operand& operand) {
                noteUse(operand, block.id, index);
            });
        }
        forEachTerminatorUse(block.terminator, [&](const Operand& operand) {
            noteUse(operand, block.id, -1);
        });
    }
}

void DefUseInfo::noteDefinition(ValueId value, BlockId block, long index, std::uint32_t type,
                                bool isFunctionParameter) {
    if (!value.valid()) return;
    const auto inserted = definitionIndex_.emplace(value, definitions_.size());
    if (!inserted.second) return; // a redefinition is the verifier's business, not ours
    definitions_.push_back(ValueDefinition{value, block, index, type, isFunctionParameter});
    if (!uses_.count(value)) values_.push_back(value);
}

void DefUseInfo::noteUse(const Operand& operand, BlockId block, long index) {
    const ValueId value = valueOf(operand);
    if (!value.valid()) return; // constants and statics are not values
    if (!uses_.count(value)) values_.push_back(value);
    uses_[value].push_back(ValueUse{value, block, index, operand.type});
    if (!definitionIndex_.count(value)) {
        undefinedUses_.push_back(ValueUse{value, block, index, operand.type});
    }
}

bool DefUseInfo::hasDefinition(ValueId value) const { return definitionIndex_.count(value) != 0; }

const ValueDefinition* DefUseInfo::definition(ValueId value) const {
    const auto it = definitionIndex_.find(value);
    return it == definitionIndex_.end() ? nullptr : &definitions_[it->second];
}

const std::vector<ValueUse>& DefUseInfo::uses(ValueId value) const {
    static const std::vector<ValueUse> empty;
    const auto it = uses_.find(value);
    return it == uses_.end() ? empty : it->second;
}

std::vector<ValueId> DefUseInfo::unusedDefinitions() const {
    std::vector<ValueId> unused;
    for (const auto& definition : definitions_) {
        // A function parameter with no uses is still meaningful: the caller
        // passed it, and the signature is part of the contract.
        if (definition.isFunctionParameter) continue;
        if (uses_.count(definition.value)) continue;
        unused.push_back(definition.value);
    }
    return unused;
}

// ---------------------------------------------------------------------------
// Liveness
// ---------------------------------------------------------------------------

const std::unordered_set<SlotId> LivenessAnalysis::kNoSlots{};
const std::unordered_set<ValueId> LivenessAnalysis::kNoValues{};

namespace {

// The per-block transfer, written the way the backward solver expects: given
// the fact at the block's *exit*, produce the fact at its *entry*. Liveness is
// a backward problem, so every side effect is applied right to left:
//
//     liveIn(S) = (liveOut(S) \ writes(S)) ∪ reads(S)
//
// That one equation, applied to every instruction in reverse and to the
// terminator first, is the whole analysis; the solver only handles the join.
struct LivenessTransfer {
    using State = LivenessFact;

    State boundaryIn() const { return State{}; }  // unused: liveness is backward
    State boundaryOut() const { return State{}; } // nothing is live past the end

    State transfer(const BasicBlock& block, const State& out) const {
        State live = out;

        // The terminator only reads (its edge arguments are evaluated here),
        // and those reads are not visible in any successor's live-in set.
        forEachTerminatorUse(block.terminator, [&](const Operand& operand) {
            const ValueId value = valueOf(operand);
            if (value.valid()) live.values.insert(value);
        });

        for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
            applyLiveStep(*it, live);
        }

        // Block parameters are established on entry, so they are not live
        // before it. Their incoming arguments were counted in the predecessor.
        for (const auto& parameter : block.parameters) {
            live.values.erase(blockParamValue(parameter.id));
        }
        return live;
    }

    void merge(State& into, const State& other) const {
        into.slots.insert(other.slots.begin(), other.slots.end());
        into.values.insert(other.values.begin(), other.values.end());
    }

    bool equal(const State& a, const State& b) const { return a == b; }
};

} // namespace

LivenessAnalysis::LivenessAnalysis(const Function& function) {
    ControlFlowGraph cfg(function);
    LivenessTransfer transfer;
    const DataFlowResult<LivenessTransfer> result = solveBackward(cfg, function, transfer);

    for (const auto& block : function.blocks) {
        const auto in = result.in.find(block.id);
        const auto out = result.out.find(block.id);
        if (in != result.in.end()) {
            liveSlotsIn_[block.id] = in->second.slots;
            liveValuesIn_[block.id] = in->second.values;
        }
        if (out != result.out.end()) {
            liveSlotsOut_[block.id] = out->second.slots;
            liveValuesOut_[block.id] = out->second.values;
        }
    }

    // Dead definitions: a value defined inside the body that nothing reads.
    //
    // The test is "no use in the defining block at or after the definition, and
    // not live out of it". Reading live-in would report every value used only in
    // its own block as dead - live-in of a block excludes what that block
    // defines, block parameters included - which is the opposite of useful.
    DefUseInfo defUse(function);
    for (const auto& definition : defUse.definitions()) {
        if (definition.isFunctionParameter) continue;
        bool usedInBlock = false;
        for (const auto& use : defUse.uses(definition.value)) {
            if (use.block != definition.block) continue;
            if (definition.instructionIndex < 0 || use.instructionIndex < 0 ||
                use.instructionIndex > definition.instructionIndex) {
                usedInBlock = true;
                break;
            }
        }
        if (usedInBlock) continue;
        const auto out = liveValuesOut_.find(definition.block);
        if (out != liveValuesOut_.end() && out->second.count(definition.value)) continue;
        deadDefinitions_.push_back(definition.value);
    }
}

const std::unordered_set<SlotId>& LivenessAnalysis::liveSlotsIn(BlockId block) const {
    const auto it = liveSlotsIn_.find(block);
    return it == liveSlotsIn_.end() ? kNoSlots : it->second;
}
const std::unordered_set<SlotId>& LivenessAnalysis::liveSlotsOut(BlockId block) const {
    const auto it = liveSlotsOut_.find(block);
    return it == liveSlotsOut_.end() ? kNoSlots : it->second;
}
const std::unordered_set<ValueId>& LivenessAnalysis::liveValuesIn(BlockId block) const {
    const auto it = liveValuesIn_.find(block);
    return it == liveValuesIn_.end() ? kNoValues : it->second;
}
const std::unordered_set<ValueId>& LivenessAnalysis::liveValuesOut(BlockId block) const {
    const auto it = liveValuesOut_.find(block);
    return it == liveValuesOut_.end() ? kNoValues : it->second;
}
bool LivenessAnalysis::isSlotLiveIn(BlockId block, SlotId slot) const {
    return liveSlotsIn(block).count(slot) != 0;
}
bool LivenessAnalysis::isValueLiveIn(BlockId block, ValueId value) const {
    return liveValuesIn(block).count(value) != 0;
}

// ---------------------------------------------------------------------------
// SuspensionLiveness
// ---------------------------------------------------------------------------

SuspensionLiveness::SuspensionLiveness(const Function& function) {
    LivenessAnalysis liveness(function);
    DefUseInfo defUse(function);
    for (const auto& block : function.blocks) {
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            const Instruction& await = block.instructions[i];
            if (await.opcode != Opcode::Await) continue;
            // Live immediately after the await: the block's live-out fact plus
            // the terminator's reads, walked back over the instructions that
            // follow the await.
            LivenessFact live;
            live.slots = liveness.liveSlotsOut(block.id);
            live.values = liveness.liveValuesOut(block.id);
            forEachTerminatorUse(block.terminator, [&](const Operand& operand) {
                const ValueId value = valueOf(operand);
                if (value.valid()) live.values.insert(value);
            });
            for (std::size_t j = block.instructions.size(); j-- > i + 1;) {
                applyLiveStep(block.instructions[j], live);
            }
            // The await's own result is produced by the resumption, not
            // preserved across the suspension.
            if (await.result != kNoTemp) live.values.erase(tempValue(await.result));

            SuspensionPoint point;
            point.block = block.id;
            point.instructionIndex = static_cast<long>(i);
            for (const ValueId value : live.values) {
                std::uint32_t type = 0;
                if (const ValueDefinition* definition = defUse.definition(value)) {
                    type = definition->type;
                }
                point.values.push_back(SuspendedValue{value, type});
            }
            std::sort(point.values.begin(), point.values.end(),
                      [](const SuspendedValue& a, const SuspendedValue& b) { return a.value < b.value; });
            for (SlotId slot : live.slots) {
                SuspendedSlot suspended;
                suspended.slot = slot;
                if (const Slot* info = function.slot(slot)) {
                    suspended.type = info->type;
                    suspended.ownership = info->ownership;
                    suspended.borrowSource = info->borrowSource;
                }
                point.slots.push_back(std::move(suspended));
            }
            std::sort(point.slots.begin(), point.slots.end(),
                      [](const SuspendedSlot& a, const SuspendedSlot& b) { return a.slot < b.slot; });
            points_.push_back(std::move(point));
        }
    }
}

const SuspensionPoint* SuspensionLiveness::pointAt(BlockId block, long instructionIndex) const {
    for (const auto& point : points_) {
        if (point.block == block && point.instructionIndex == instructionIndex) return &point;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// ConstantAnalysis
// ---------------------------------------------------------------------------

ConstantAnalysis::ConstantAnalysis(const Module& module, const Function& function)
    : module_(module), function_(function) {
    run();
}

std::optional<ConstId> ConstantAnalysis::poolIdOf(const Constant& constant) const {
    for (ConstId id = 1; id <= module_.constants.size(); ++id) {
        const Constant* existing = module_.constant(id);
        if (existing && *existing == constant) return id;
    }
    return std::nullopt;
}

std::optional<ConstId> ConstantAnalysis::evaluateOperand(const Operand& operand) const {
    if (operand.kind == OperandKind::Const) {
        // A constant operand is only "a module constant" if the pool holds it;
        // by construction it does.
        return module_.constant(operand.index) ? std::optional<ConstId>(operand.index) : std::nullopt;
    }
    const ValueId value = valueOf(operand);
    if (!value.valid()) return std::nullopt;
    const auto it = constantIds_.find(value);
    return it == constantIds_.end() ? std::nullopt : std::optional<ConstId>(it->second);
}

// Folding is deliberately small: only operations whose result is the same
// whether or not a runtime check would have fired, and only when the operands
// are both constants the VM would see with the same shape. Anything else
// (string concatenation, division by a zero it cannot see through) reports
// "not known" rather than guessing.
std::optional<Constant> ConstantAnalysis::fold(const Instruction& instruction) const {
    const bool unary = instruction.opcode == Opcode::Neg || instruction.opcode == Opcode::Not ||
                       instruction.opcode == Opcode::BitNot || instruction.opcode == Opcode::Widen;
    if (!unary && instruction.operands.size() != 2) return std::nullopt;
    if (instruction.operands.empty()) return std::nullopt;

    // Operands are folded through the *pool*, so a constant only propagates
    // while the module already spells it out.
    auto constantOfOperand = [&](std::size_t i) -> std::optional<Constant> {
        if (i >= instruction.operands.size()) return std::nullopt;
        const auto id = evaluateOperand(instruction.operands[i]);
        if (!id) return std::nullopt;
        const Constant* c = module_.constant(*id);
        return c ? std::optional<Constant>(*c) : std::nullopt;
    };
    const std::optional<Constant> a = constantOfOperand(0);
    const std::optional<Constant> b = constantOfOperand(1);

    auto compare = [&](auto intOp, auto doubleOp) -> std::optional<Constant> {
        if (isInt(a) && isInt(b)) return makeBool(intOp(a->intValue, b->intValue));
        if (isDouble(a) && isDouble(b)) return makeBool(doubleOp(a->doubleValue, b->doubleValue));
        return std::nullopt;
    };

    switch (instruction.opcode) {
        case Opcode::Add:
            if (isInt(a) && isInt(b)) return makeInt(a->intValue + b->intValue);
            if (isDouble(a) && isDouble(b)) return makeDouble(a->doubleValue + b->doubleValue);
            return std::nullopt;
        case Opcode::Sub:
            if (isInt(a) && isInt(b)) return makeInt(a->intValue - b->intValue);
            if (isDouble(a) && isDouble(b)) return makeDouble(a->doubleValue - b->doubleValue);
            return std::nullopt;
        case Opcode::Mul:
            if (isInt(a) && isInt(b)) return makeInt(a->intValue * b->intValue);
            if (isDouble(a) && isDouble(b)) return makeDouble(a->doubleValue * b->doubleValue);
            return std::nullopt;
        case Opcode::Div:
            if (isInt(a) && isInt(b) && b->intValue != 0) return makeInt(a->intValue / b->intValue);
            if (isDouble(a) && isDouble(b) && b->doubleValue != 0.0)
                return makeDouble(a->doubleValue / b->doubleValue);
            return std::nullopt;
        case Opcode::Mod:
            if (isInt(a) && isInt(b) && b->intValue != 0) return makeInt(a->intValue % b->intValue);
            return std::nullopt;
        case Opcode::Neg:
            if (isInt(a)) return makeInt(-a->intValue);
            if (isDouble(a)) return makeDouble(-a->doubleValue);
            return std::nullopt;
        case Opcode::Not:
            if (isBool(a)) return makeBool(!a->boolValue);
            return std::nullopt;
        case Opcode::BitNot:
            if (isInt(a)) return makeInt(~a->intValue);
            return std::nullopt;
        case Opcode::BitAnd:
            if (isInt(a) && isInt(b)) return makeInt(a->intValue & b->intValue);
            return std::nullopt;
        case Opcode::BitOr:
            if (isInt(a) && isInt(b)) return makeInt(a->intValue | b->intValue);
            return std::nullopt;
        case Opcode::BitXor:
            if (isInt(a) && isInt(b)) return makeInt(a->intValue ^ b->intValue);
            return std::nullopt;
        case Opcode::Shl:
            if (isInt(a) && isInt(b) && b->intValue >= 0 && b->intValue < 64) {
                return makeInt(static_cast<std::int64_t>(static_cast<std::uint64_t>(a->intValue)
                                                         << b->intValue));
            }
            return std::nullopt;
        case Opcode::Shr:
            if (isInt(a) && isInt(b) && b->intValue >= 0 && b->intValue < 64)
                return makeInt(a->intValue >> b->intValue);
            return std::nullopt;
        case Opcode::Widen:
            if (isInt(a)) return makeDouble(static_cast<double>(a->intValue));
            return std::nullopt;
        case Opcode::Eq:
            if (isInt(a) && isInt(b)) return makeBool(a->intValue == b->intValue);
            if (isDouble(a) && isDouble(b)) return makeBool(a->doubleValue == b->doubleValue);
            if (isBool(a) && isBool(b)) return makeBool(a->boolValue == b->boolValue);
            return std::nullopt;
        case Opcode::Ne:
            if (isInt(a) && isInt(b)) return makeBool(a->intValue != b->intValue);
            if (isDouble(a) && isDouble(b)) return makeBool(a->doubleValue != b->doubleValue);
            if (isBool(a) && isBool(b)) return makeBool(a->boolValue != b->boolValue);
            return std::nullopt;
        case Opcode::Lt:
            return compare([](std::int64_t x, std::int64_t y) { return x < y; },
                           [](double x, double y) { return x < y; });
        case Opcode::Le:
            return compare([](std::int64_t x, std::int64_t y) { return x <= y; },
                           [](double x, double y) { return x <= y; });
        case Opcode::Gt:
            return compare([](std::int64_t x, std::int64_t y) { return x > y; },
                           [](double x, double y) { return x > y; });
        case Opcode::Ge:
            return compare([](std::int64_t x, std::int64_t y) { return x >= y; },
                           [](double x, double y) { return x >= y; });
        default:
            return std::nullopt;
    }
}

// What a value's own definition implies, given the constants known right now.
std::optional<ConstId> ConstantAnalysis::evaluate(ValueId value) const {
    if (!value.valid()) return std::nullopt;
    if (value.isParam()) return std::nullopt; // the caller chooses the argument

    if (value.isBlockParam()) {
        const BasicBlock* owner = function_.blockOfParameter(value.index);
        if (!owner) return std::nullopt;
        // Position of this parameter in its block's parameter list.
        std::size_t position = owner->parameters.size();
        for (std::size_t i = 0; i < owner->parameters.size(); ++i) {
            if (owner->parameters[i].id == value.index) { position = i; break; }
        }
        if (position == owner->parameters.size()) return std::nullopt;

        std::optional<ConstId> agreed;
        bool sawEdge = false;
        for (const auto& block : function_.blocks) {
            const auto successors = block.terminator.successors();
            for (std::size_t i = 0; i < successors.size(); ++i) {
                if (successors[i] != owner->id) continue;
                const auto& arguments = block.terminator.argumentsFor(i);
                if (position >= arguments.size()) return std::nullopt; // malformed: say nothing
                const auto argument = evaluateOperand(arguments[position]);
                if (!argument) return std::nullopt;
                if (!agreed) agreed = argument;
                else if (*agreed != *argument) return std::nullopt;
                sawEdge = true;
            }
        }
        return sawEdge ? agreed : std::nullopt;
    }

    // A temp: fold its defining instruction.
    const Instruction* definition = nullptr;
    for (const auto& block : function_.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.result != kNoTemp && instruction.result == value.index) {
                definition = &instruction;
                break;
            }
        }
        if (definition) break;
    }
    if (!definition) return std::nullopt;
    const auto folded = fold(*definition);
    if (!folded) return std::nullopt;
    // Only constants the module already spells out are reported, so a consumer
    // can use the result as an operand as-is.
    return poolIdOf(*folded);
}

void ConstantAnalysis::run() {
    // A value-based fixpoint: repeatedly re-evaluate every value from its
    // definition until nothing new becomes constant. The lattice only moves
    // from "unknown" to "constant", so it terminates; the outer guard is a
    // belt-and-braces bound rather than a limit the analysis should ever reach.
    std::vector<ValueId> definitionOrder;
    for (ParamId p = 0; p < function_.parameters.size(); ++p) definitionOrder.push_back(paramValue(p));
    for (const auto& block : function_.blocks) {
        for (const auto& parameter : block.parameters) definitionOrder.push_back(blockParamValue(parameter.id));
        for (const auto& instruction : block.instructions) {
            if (instruction.result != kNoTemp) definitionOrder.push_back(tempValue(instruction.result));
        }
    }

    bool changed = true;
    std::size_t guard = 0;
    const std::size_t guardLimit = definitionOrder.size() * 4 + 64;
    while (changed && guard++ < guardLimit) {
        changed = false;
        for (ValueId value : definitionOrder) {
            if (constantIds_.count(value)) continue;
            const auto constant = evaluate(value);
            if (!constant) continue;
            constantIds_[value] = *constant;
            constantValues_.push_back(value);
            changed = true;
        }
    }
}

std::optional<ConstId> ConstantAnalysis::constantOf(ValueId value) const {
    const auto it = constantIds_.find(value);
    return it == constantIds_.end() ? std::nullopt : std::optional<ConstId>(it->second);
}

std::optional<ConstId> ConstantAnalysis::constantOf(const Operand& operand) const {
    if (operand.kind == OperandKind::Const) {
        return module_.constant(operand.index) ? std::optional<ConstId>(operand.index) : std::nullopt;
    }
    const ValueId value = valueOf(operand);
    return value.valid() ? constantOf(value) : std::nullopt;
}

// ---------------------------------------------------------------------------
// Reachability helpers
// ---------------------------------------------------------------------------

std::vector<BlockId> reachableBlocks(const ControlFlowGraph& cfg) {
    std::vector<BlockId> out(cfg.reversePostOrder().begin(), cfg.reversePostOrder().end());
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<BlockId> deadBlocks(const ControlFlowGraph& cfg) { return cfg.deadBlocks(); }

} // namespace zl::mir
