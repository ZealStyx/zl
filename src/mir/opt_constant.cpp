// Constant folding and constant propagation.
//
// Both passes exist because neither subsumes the other, and the reason is a
// property of the constant pool rather than of the algorithm:
//
//   * folding *evaluates* an instruction and interns the result, so it can
//     produce a constant the source never spelled (`2 + 3` -> `5` when `5`
//     appears nowhere in the program);
//   * propagation *reports* only constants the pool already contains, but it
//     carries them across blocks and through block parameters, which folding
//     (a strictly local, instruction-at-a-time rewrite) cannot.
//
// Neither pass deletes anything. Both replace *uses*; the definitions they
// orphan are removed by dead-value elimination, which is the only pass
// entitled to delete an instruction and which re-checks that the instruction
// cannot raise before it does.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "zl/mir/folding.hpp"
#include "zl/mir/passes.hpp"

namespace zl::mir {
namespace {

class ConstantFoldingPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "fold-constants"; }
    [[nodiscard]] std::string description() const override {
        return "evaluates an instruction whose operands are all constants, and "
               "rewrites the uses of its result to the value it computes; refuses "
               "to fold an operation that would raise (overflow, division by zero, "
               "out-of-range shift), so the raise stays in the program";
    }

    bool runOnFunction(Module& module, Function& function, FunctionAnalysisManager& analyses) override {
        std::size_t folded = 0;
        std::size_t usesRewritten = 0;

        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.result == kNoTemp) continue;
                const std::optional<Constant> value = foldInstruction(module, instruction);
                if (!value) continue;
                // The evaluator already checked that the constant has exactly
                // the type this instruction declares, so the substitution
                // cannot retype an operand.
                const ConstId id = module.internConstant(*value);
                const Operand replacement = Operand::constant(id, instruction.resultType);
                usesRewritten +=
                    replaceValueUsesTyped(function, tempValue(instruction.result), replacement);
                ++folded;
            }
        }

        if (usesRewritten == 0) return false; // nothing observable moved

        analyses.invalidate(function);
        note_ = "folded " + std::to_string(folded) + " instruction(s), rewrote " +
                std::to_string(usesRewritten) + " use(s)";
        return true;
    }

    [[nodiscard]] std::string lastNote() const override { return note_; }

private:
    std::string note_;
};

class ConstantPropagationPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "propagate-constants"; }
    [[nodiscard]] std::string description() const override {
        return "replaces a use of a value with the constant every path agrees it "
               "holds, including a block parameter whose incoming edges all hand it "
               "the same constant";
    }

    bool runOnFunction(Module& module, Function& function, FunctionAnalysisManager& analyses) override {
        const ConstantAnalysis& constants = analyses.constants(function);

        std::size_t values = 0;
        std::size_t usesRewritten = 0;
        for (const ValueId value : constants.constantValues()) {
            const std::optional<ConstId> id = constants.constantOf(value);
            if (!id) continue;
            const Constant* constant = module.constant(*id);
            if (!constant) continue;
            const std::uint32_t type = typeOfValue(function, value);
            // The analysis names a constant the pool holds; this is the check
            // that the constant really denotes the value's type, so a
            // substitution can never narrow an operand.
            if (!constantMatchesType(module, *constant, type)) continue;
            const Operand replacement = Operand::constant(*id, type);
            const std::size_t rewritten = replaceValueUsesTyped(function, value, replacement);
            if (rewritten != 0) ++values;
            usesRewritten += rewritten;
        }

        if (usesRewritten == 0) return false;

        analyses.invalidate(function);
        note_ = "propagated " + std::to_string(values) + " constant value(s) into " +
                std::to_string(usesRewritten) + " use(s)";
        return true;
    }

    [[nodiscard]] std::string lastNote() const override { return note_; }

private:
    std::string note_;
};

} // namespace

std::unique_ptr<FunctionPass> createConstantFoldingPass() {
    return std::unique_ptr<FunctionPass>(new ConstantFoldingPass());
}

std::unique_ptr<FunctionPass> createConstantPropagationPass() {
    return std::unique_ptr<FunctionPass>(new ConstantPropagationPass());
}

} // namespace zl::mir
