// Algebraic simplification, redundant-conversion removal, local copy
// propagation and branch simplification.
//
// These four are grouped because they share one shape: each rewrites a
// *definition* into something simpler without ever deleting it, and each
// declines rather than guessing. The passes that delete things are elsewhere
// (opt_dead.cpp), which keeps the rule "only a pass whose name says
// elimination removes anything" true of the whole pipeline.
//
// The safety argument per rewrite is stated at the rewrite. The two that
// matter most, because they are the ones a textbook simplifier gets wrong in
// ZL:
//
//   * an identity that can *introduce* a raise is not applied. `x * 2` is not
//     rewritten to `x + x` (it can overflow where the original could not), and
//     `x - x` is rewritten to `0` because it cannot;
//   * floating point identities are limited to the ones that hold for every
//     value of `x`, NaN and negative zero included. `x + 0.0` is *not*
//     `x` - `(-0.0) + 0.0` is `+0.0` - so it is left alone.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zl/mir/analysis.hpp"
#include "zl/mir/passes.hpp"

namespace zl::mir {
namespace {

const Constant* constantOf(const Module& module, const Operand& operand) {
    if (operand.kind != OperandKind::Const) return nullptr;
    return module.constant(operand.index);
}

bool isIntConstant(const Module& module, const Operand& operand, std::int64_t value) {
    const Constant* constant = constantOf(module, operand);
    return constant != nullptr && constant->kind == ConstKind::Int && constant->intValue == value;
}

bool isDoubleConstant(const Module& module, const Operand& operand, double value) {
    const Constant* constant = constantOf(module, operand);
    return constant != nullptr && constant->kind == ConstKind::Double && constant->doubleValue == value;
}

Constant intConstant(std::int64_t value) {
    Constant constant;
    constant.kind = ConstKind::Int;
    constant.intValue = value;
    return constant;
}

Constant doubleConstant(double value) {
    Constant constant;
    constant.kind = ConstKind::Double;
    constant.doubleValue = value;
    return constant;
}

Constant boolConstant(bool value) {
    Constant constant;
    constant.kind = ConstKind::Bool;
    constant.boolValue = value;
    return constant;
}

// A type whose values the MIR cannot reason about: `unknown` (semantic analysis
// did not resolve it) and an unsubstituted `T`. A value of either may be
// anything at runtime, so no check on it is provably incapable of failing.
bool isOpaqueType(const Module& module, std::uint32_t type) {
    const Type* resolved = module.types.find(type);
    if (!resolved) return true;
    return resolved->kind == TypeKind::Unknown || resolved->kind == TypeKind::TypeParam;
}

// ---------------------------------------------------------------------------
// Algebraic identities
// ---------------------------------------------------------------------------
// Returns the operand the instruction's result can be replaced by, or
// nullopt when no identity applies. Every rule here is one that cannot
// introduce or remove a raise: the replacement computes the same value on
// every input the original accepts.
std::optional<Operand> tryAlgebraicIdentity(Module& module, const Function& function,
                                            const Instruction& instruction) {
    const std::uint32_t intType = module.types.intType();
    const std::uint32_t doubleType = module.types.doubleType();
    const std::uint32_t boolType = module.types.boolType();
    const std::uint32_t resultType = instruction.resultType;

    // --- double negation: -(-x) == x, !(!x) == x ---------------------------
    // Safe for integers too, and the reason is worth stating: reaching the
    // outer negation means the inner one completed, and the inner one raises
    // on the single input (INT64_MIN) for which the outer would also raise. So
    // the pair cannot overflow where either instruction alone could.
    if (instruction.operands.size() == 1) {
        const Operand& inner = instruction.operands[0];
        const Opcode wanted = (instruction.opcode == Opcode::Neg)  ? Opcode::Neg
                            : (instruction.opcode == Opcode::Not)  ? Opcode::Not
                                                                   : Opcode::Nop;
        if (wanted != Opcode::Nop && inner.kind == OperandKind::Temp) {
            const Instruction* definition = definingInstruction(function, valueOf(inner));
            if (definition != nullptr && definition->opcode == wanted &&
                definition->operands.size() == 1 && definition->operands[0].type == resultType) {
                return definition->operands[0];
            }
        }
        return std::nullopt;
    }

    if (instruction.operands.size() != 2) return std::nullopt;
    const Operand& left = instruction.operands[0];
    const Operand& right = instruction.operands[1];
    const bool bothInt = left.type == intType && right.type == intType && resultType == intType;
    const bool bothDouble = left.type == doubleType && right.type == doubleType && resultType == doubleType;

    if (bothInt) {
        const bool same = left == right;
        switch (instruction.opcode) {
            case Opcode::Add:
                if (isIntConstant(module, left, 0)) return right;
                if (isIntConstant(module, right, 0)) return left;
                return std::nullopt;
            case Opcode::Sub:
                if (isIntConstant(module, right, 0)) return left;
                // x - x is 0 for every x, with no overflow: the one input that
                // could overflow (INT64_MIN - 0) is excluded by the "same
                // operand" condition, since x == 0 makes the left side 0.
                if (same) return Operand::constant(module.internConstant(intConstant(0)), intType);
                return std::nullopt;
            case Opcode::Mul:
                if (isIntConstant(module, left, 1)) return right;
                if (isIntConstant(module, right, 1)) return left;
                // x * 0 is 0 for every x, including INT64_MIN: the VM's
                // overflow check never fires for a zero operand.
                if (isIntConstant(module, left, 0)) return left;
                if (isIntConstant(module, right, 0)) return right;
                return std::nullopt;
            case Opcode::Div:
                // x / 1: no division by zero, no INT64_MIN/-1 overflow.
                if (isIntConstant(module, right, 1)) return left;
                return std::nullopt;
            case Opcode::Mod:
                if (isIntConstant(module, right, 1)) return Operand::constant(module.internConstant(intConstant(0)), intType);
                return std::nullopt;
            case Opcode::BitAnd:
                if (isIntConstant(module, left, -1)) return right;
                if (isIntConstant(module, right, -1)) return left;
                if (isIntConstant(module, left, 0)) return left;
                if (isIntConstant(module, right, 0)) return right;
                if (same) return left;
                return std::nullopt;
            case Opcode::BitOr:
                if (isIntConstant(module, left, 0)) return right;
                if (isIntConstant(module, right, 0)) return left;
                if (isIntConstant(module, left, -1)) return left;
                if (isIntConstant(module, right, -1)) return right;
                if (same) return left;
                return std::nullopt;
            case Opcode::BitXor:
                if (isIntConstant(module, left, 0)) return right;
                if (isIntConstant(module, right, 0)) return left;
                if (same) return Operand::constant(module.internConstant(intConstant(0)), intType);
                return std::nullopt;
            case Opcode::Shl:
            case Opcode::Shr:
            case Opcode::Ushr:
                // A shift by zero is the identity and cannot raise, whereas the
                // general shift raises outside 0..63.
                if (isIntConstant(module, right, 0)) return left;
                return std::nullopt;
            default:
                break;
        }
    }

    if (bothDouble) {
        switch (instruction.opcode) {
            case Opcode::Mul:
                if (isDoubleConstant(module, left, 1.0)) return right;
                if (isDoubleConstant(module, right, 1.0)) return left;
                return std::nullopt;
            case Opcode::Div:
                if (isDoubleConstant(module, right, 1.0)) return left;
                return std::nullopt;
            case Opcode::Sub:
                // x - 0.0 is x for every x including -0.0 (IEEE: -0.0 - +0.0
                // is -0.0). x + 0.0 is deliberately absent: it turns -0.0 into
                // +0.0, and the sign of zero is observable.
                if (isDoubleConstant(module, right, 0.0)) return left;
                return std::nullopt;
            default:
                break;
        }
    }

    // --- self-comparison ---------------------------------------------------
    // Only where the comparison is total and NaN cannot appear: integers and
    // booleans for equality, integers alone for ordering (ZL's ordering rules
    // are numeric). On doubles every one of these is false for NaN, so a
    // "x == x -> true" rewrite would be wrong exactly where it matters.
    if (resultType == boolType && left == right) {
        const bool integral = left.type == intType;
        const bool boolean = left.type == boolType;
        switch (instruction.opcode) {
            case Opcode::Eq:
                if (integral || boolean) return Operand::constant(module.internConstant(boolConstant(true)), boolType);
                return std::nullopt;
            case Opcode::Ne:
                if (integral || boolean) return Operand::constant(module.internConstant(boolConstant(false)), boolType);
                return std::nullopt;
            case Opcode::Le:
            case Opcode::Ge:
                if (integral) return Operand::constant(module.internConstant(boolConstant(true)), boolType);
                return std::nullopt;
            case Opcode::Lt:
            case Opcode::Gt:
                if (integral) return Operand::constant(module.internConstant(boolConstant(false)), boolType);
                return std::nullopt;
            default:
                break;
        }
    }

    return std::nullopt;
}

class AlgebraicSimplificationPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "simplify-algebraic"; }
    [[nodiscard]] std::string description() const override {
        return "applies identities that hold for every value of their operands "
               "(`x + 0`, `x * 1`, `x & ~0`, `x - x`, `-(-x)`), skipping any that "
               "could introduce or remove a raise";
    }

    bool runOnFunction(Module& module, Function& function, FunctionAnalysisManager& analyses) override {
        std::size_t rewritten = 0;
        std::size_t uses = 0;
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.result == kNoTemp) continue;
                const std::optional<Operand> replacement =
                    tryAlgebraicIdentity(module, function, instruction);
                if (!replacement) continue;
                if (replacement->type != instruction.resultType) continue; // never retype an operand
                uses += replaceValueUsesTyped(function, tempValue(instruction.result), *replacement);
                ++rewritten;
            }
        }
        if (uses == 0) return false;
        analyses.invalidate(function);
        note_ = "applied " + std::to_string(rewritten) + " identity/identities, rewrote " +
                std::to_string(uses) + " use(s)";
        return true;
    }

    [[nodiscard]] std::string lastNote() const override { return note_; }

private:
    std::string note_;
};

// ---------------------------------------------------------------------------
// Redundant conversions
// ---------------------------------------------------------------------------
// A conversion that no longer converts is a copy; a check that cannot fail is
// one the program does not need. Both are replaced by their operand rather
// than deleted, so the instruction simply stops having uses and dead-value
// elimination takes it away later.
std::optional<Operand> tryRedundantConversion(Module& module, const Instruction& instruction) {
    switch (instruction.opcode) {
        case Opcode::Widen: {
            // int -> double only. An operand that is already a double makes
            // this a copy.
            if (instruction.operands.size() != 1) return std::nullopt;
            const Operand& operand = instruction.operands[0];
            if (operand.type != module.types.doubleType()) return std::nullopt;
            if (instruction.resultType != module.types.doubleType()) return std::nullopt;
            return operand;
        }
        case Opcode::Refine: {
            // A runtime type assertion to the type the value already has. The
            // assertion cannot fail when the static type is one the MIR can
            // reason about, so the result is the operand.
            if (instruction.operands.size() != 1) return std::nullopt;
            const Operand& operand = instruction.operands[0];
            if (operand.type != instruction.resultType) return std::nullopt;
            if (isOpaqueType(module, operand.type)) return std::nullopt;
            return operand;
        }
        case Opcode::NullCheck: {
            // `null` is not a value of a non-nullable type, so the check
            // cannot raise and the result is the operand.
            if (instruction.operands.size() != 1) return std::nullopt;
            const Operand& operand = instruction.operands[0];
            if (operand.type != instruction.resultType) return std::nullopt;
            if (isOpaqueType(module, operand.type)) return std::nullopt;
            if (module.types.isNullable(operand.type)) return std::nullopt;
            return operand;
        }
        case Opcode::IsNull: {
            // The same fact, as a value: a non-nullable value is never null.
            if (instruction.operands.size() != 1) return std::nullopt;
            const Operand& operand = instruction.operands[0];
            if (instruction.resultType != module.types.boolType()) return std::nullopt;
            if (isOpaqueType(module, operand.type)) return std::nullopt;
            if (module.types.isNullable(operand.type)) return std::nullopt;
            return Operand::constant(module.internConstant(boolConstant(false)),
                                     module.types.boolType());
        }
        default:
            return std::nullopt;
    }
}

class RedundantConversionPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "remove-redundant-conversions"; }
    [[nodiscard]] std::string description() const override {
        return "replaces a conversion or check that provably cannot fail "
               "(`widen` of a double, `refine` to the type a value already has, "
               "`null_check` on a non-nullable type) with its operand";
    }

    bool runOnFunction(Module& module, Function& function, FunctionAnalysisManager& analyses) override {
        std::size_t rewritten = 0;
        std::size_t uses = 0;
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.result == kNoTemp) continue;
                const std::optional<Operand> replacement = tryRedundantConversion(module, instruction);
                if (!replacement) continue;
                if (replacement->type != instruction.resultType) continue;
                uses += replaceValueUsesTyped(function, tempValue(instruction.result), *replacement);
                ++rewritten;
            }
        }
        if (uses == 0) return false;
        analyses.invalidate(function);
        note_ = "removed " + std::to_string(rewritten) + " redundant conversion(s)/check(s), rewrote " +
                std::to_string(uses) + " use(s)";
        return true;
    }

    [[nodiscard]] std::string lastNote() const override { return note_; }

private:
    std::string note_;
};

// ---------------------------------------------------------------------------
// Local copy propagation
// ---------------------------------------------------------------------------
// Three shapes, one claim each: "this value and that value are provably the
// same". All three are *local* - the proof lives in one block, or in the edges
// entering one block - which is what makes the claim checkable without a
// whole-function data-flow framework, and therefore what makes it safe.
class CopyPropagationPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "propagate-copies"; }
    [[nodiscard]] std::string description() const override {
        return "propagates copies whose source is visible locally: store-to-load "
               "forwarding, a repeated load of a slot nothing wrote in between, and "
               "a block parameter every incoming edge hands the same value";
    }

    bool runOnFunction(Module& module, Function& function, FunctionAnalysisManager& analyses) override {
        (void)module;
        std::size_t uses = 0;
        uses += propagateThroughBlockParameters(function, analyses);
        uses += propagateWithinBlocks(function);
        if (uses == 0) return false;
        analyses.invalidate(function);
        note_ = "propagated " + std::to_string(uses) + " copied use(s)";
        return true;
    }

    [[nodiscard]] std::string lastNote() const override { return note_; }

private:
    // A block parameter whose every reachable incoming edge hands it the same
    // value is that value. The argument must be the same on *every* edge
    // because the block does not know which one it came from.
    std::size_t propagateThroughBlockParameters(Function& function, FunctionAnalysisManager& analyses) {
        const ControlFlowGraph& cfg = analyses.cfg(function);
        std::size_t uses = 0;

        for (const auto& block : function.blocks) {
            if (block.parameters.empty()) continue;
            if (!cfg.isReachable(block.id)) continue;

            // Every reachable predecessor, each with the arguments it passes.
            std::vector<const std::vector<Operand>*> incoming;
            for (const BlockId predecessor : cfg.predecessors(block.id)) {
                if (!cfg.isReachable(predecessor)) continue;
                const BasicBlock* from = function.block(predecessor);
                if (!from) continue;
                const auto successors = from->terminator.successors();
                for (std::size_t index = 0; index < successors.size(); ++index) {
                    if (successors[index] != block.id) continue;
                    incoming.push_back(&from->terminator.argumentsFor(index));
                }
            }
            if (incoming.empty()) continue;

            for (std::size_t position = 0; position < block.parameters.size(); ++position) {
                const BlockParameter& parameter = block.parameters[position];
                std::optional<Operand> agreed;
                bool usable = true;
                for (const std::vector<Operand>* arguments : incoming) {
                    if (position >= arguments->size()) { usable = false; break; }
                    const Operand& candidate = (*arguments)[position];
                    if (candidate.isNone()) { usable = false; break; }
                    if (!agreed) { agreed = candidate; continue; }
                    if (!(*agreed == candidate)) { usable = false; break; }
                }
                if (!usable || !agreed) continue;
                // The parameter's declared type is what every use site sees, so
                // the substitution is only legal when the argument agrees with
                // it - which is also the condition under which the value exists
                // on every path.
                if (agreed->type != parameter.type) continue;
                uses += replaceValueUsesTyped(function, blockParamValue(parameter.id), *agreed);
            }
        }
        return uses;
    }

    // Store-to-load forwarding and repeated loads, inside one block.
    //
    // A ZL local is a slot, and nothing outside the function can write one:
    // closures capture by value, and a call gets its own frame. So "what does
    // this slot hold here" is answerable by looking backwards in the block for
    // the last thing that wrote it, which is exactly what this does.
    std::size_t propagateWithinBlocks(Function& function) {
        std::size_t uses = 0;

        for (auto& block : function.blocks) {
            std::unordered_map<SlotId, Operand> available;

            for (auto& instruction : block.instructions) {
                if (instruction.opcode == Opcode::Store && instruction.slot != 0 &&
                    !instruction.operands.empty()) {
                    available[instruction.slot] = instruction.operands[0];
                    continue;
                }
                if (instruction.opcode == Opcode::Load && instruction.slot != 0 &&
                    instruction.result != kNoTemp) {
                    const auto found = available.find(instruction.slot);
                    if (found != available.end() && found->second.type == instruction.resultType) {
                        // The slot holds this value here, so the load produces it.
                        uses += replaceValueUsesTyped(function, tempValue(instruction.result),
                                                      found->second);
                    }
                    // Whether or not it was forwarded, the slot now holds this
                    // load's result: a second load can read that instead.
                    available[instruction.slot] =
                        Operand::temp(instruction.result, instruction.resultType);
                    continue;
                }
                // Anything else that writes the cell invalidates what we knew.
                // A `move` also empties it; a `borrow` writes a different
                // binding into it; a `drop` releases it.
                switch (instruction.opcode) {
                    case Opcode::Move:
                    case Opcode::Borrow:
                    case Opcode::EndBorrow:
                    case Opcode::Drop:
                        if (instruction.slot != 0) available.erase(instruction.slot);
                        break;
                    default:
                        break;
                }
            }
        }
        return uses;
    }

    std::string note_;
};

// ---------------------------------------------------------------------------
// Branch simplification
// ---------------------------------------------------------------------------
// Control flow that has already been decided. Every rewrite here keeps the
// edge arguments of the path it keeps, because a block parameter is fed by
// those arguments and dropping them would leave the target's parameters
// undefined on the path that survives.
class BranchSimplificationPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "simplify-branches"; }
    [[nodiscard]] std::string description() const override {
        return "rewrites decided control flow (a branch or switch on a constant, a "
               "branch whose arms agree, a jump through an empty block) and leaves "
               "the blocks it strands for the pruning pass";
    }

    bool runOnFunction(Module& module, Function& function, FunctionAnalysisManager&) override {
        std::size_t rewritten = 0;

        for (auto& block : function.blocks) {
            Terminator& terminator = block.terminator;

            if (terminator.kind == TerminatorKind::Branch) {
                const Constant* condition = constantOf(module, terminator.value);
                if (condition != nullptr && condition->kind == ConstKind::Bool) {
                    const std::size_t takenIndex = condition->boolValue ? 0 : 1;
                    const BlockId target = takenIndex == 0 ? terminator.target : terminator.elseBlock;
                    // Copy before clobbering: the argument list lives on the
                    // terminator we are about to overwrite.
                    std::vector<Operand> arguments = terminator.argumentsFor(takenIndex);
                    terminator.kind = TerminatorKind::Jump;
                    terminator.target = target;
                    terminator.elseBlock = kNoBlock;
                    terminator.cases.clear();
                    terminator.value = Operand::none();
                    terminator.edgeArguments.clear();
                    terminator.edgeArguments.push_back(std::move(arguments));
                    ++rewritten;
                    continue;
                }
                // Both arms the same block: the branch is not decided, but its
                // outcome is. Only safe when the two paths also hand the target
                // the same arguments - otherwise "which arguments" is exactly
                // the question the branch was answering.
                if (terminator.target == terminator.elseBlock &&
                    terminator.argumentsFor(0) == terminator.argumentsFor(1)) {
                    std::vector<Operand> arguments = terminator.argumentsFor(0);
                    terminator.kind = TerminatorKind::Jump;
                    terminator.elseBlock = kNoBlock;
                    terminator.value = Operand::none();
                    terminator.edgeArguments.clear();
                    terminator.edgeArguments.push_back(std::move(arguments));
                    ++rewritten;
                }
                continue;
            }

            if (terminator.kind == TerminatorKind::Switch) {
                const Constant* subject = constantOf(module, terminator.value);
                if (subject == nullptr || subject->kind != ConstKind::Int) continue;
                // Find the case that matches, in order; the default takes the
                // value when none does. The default's arguments are the last
                // entry in `edgeArguments` (see Terminator::successors).
                std::size_t takenIndex = terminator.cases.size(); // default
                for (std::size_t i = 0; i < terminator.cases.size(); ++i) {
                    if (terminator.cases[i].value == subject->intValue) { takenIndex = i; break; }
                }
                std::vector<Operand> arguments = terminator.argumentsFor(takenIndex);
                const BlockId target = takenIndex < terminator.cases.size()
                                           ? terminator.cases[takenIndex].block
                                           : terminator.target;
                terminator.kind = TerminatorKind::Jump;
                terminator.target = target;
                terminator.cases.clear();
                terminator.value = Operand::none();
                terminator.edgeArguments.clear();
                terminator.edgeArguments.push_back(std::move(arguments));
                ++rewritten;
                continue;
            }

            if (terminator.kind == TerminatorKind::Jump) {
                // Jump threading, in the one shape that is free of subtleties:
                // the target is an empty block with no parameters that does
                // nothing but jump onwards. A block with parameters, with
                // instructions, with an exception handler chain, or with a
                // non-normal kind (a catch or cleanup block) is left alone -
                // each of those does something on the way through.
                const BasicBlock* target = function.block(terminator.target);
                if (target == nullptr || target == &block) continue;
                if (!target->instructions.empty() || !target->parameters.empty()) continue;
                if (!target->exceptionHandlers.empty() || target->kind != BlockKind::Normal) continue;
                if (target->terminator.kind != TerminatorKind::Jump) continue;
                if (target->terminator.target == target->id) continue;
                const BasicBlock* onward = function.block(target->terminator.target);
                if (onward == nullptr || onward == &block) continue;
                // The block we skip passes nothing (it has no parameters, so
                // this jump carries no arguments), which means we may only
                // retarget to a block that also expects nothing. Jumping
                // straight to a block with parameters would arrive without the
                // arguments its parameters are defined by.
                if (!onward->parameters.empty()) continue;
                terminator.target = onward->id;
                ++rewritten;
            }
        }

        if (rewritten == 0) return false;
        function.rebuildEdges();
        note_ = "simplified " + std::to_string(rewritten) + " terminator(s)";
        return true;
    }

    [[nodiscard]] std::string lastNote() const override { return note_; }

private:
    std::string note_;
};

} // namespace

std::unique_ptr<FunctionPass> createAlgebraicSimplificationPass() {
    return std::unique_ptr<FunctionPass>(new AlgebraicSimplificationPass());
}

std::unique_ptr<FunctionPass> createRedundantConversionPass() {
    return std::unique_ptr<FunctionPass>(new RedundantConversionPass());
}

std::unique_ptr<FunctionPass> createCopyPropagationPass() {
    return std::unique_ptr<FunctionPass>(new CopyPropagationPass());
}

std::unique_ptr<FunctionPass> createBranchSimplificationPass() {
    return std::unique_ptr<FunctionPass>(new BranchSimplificationPass());
}

} // namespace zl::mir
