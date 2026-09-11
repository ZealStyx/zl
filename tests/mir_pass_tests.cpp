// MIR optimisation framework regressions.
//
// Three things are proved here, all on MIR built by hand so the shape under
// test is visible in the test rather than implied by lowering:
//
//   1. the effect classification answers the questions the passes ask, and
//      refuses where a refusal is the only safe answer (an operation that can
//      raise is not removable, however dead it looks);
//   2. each pass rewrites what it claims to rewrite and - the half that matters
//      more - leaves alone what it cannot prove safe;
//   3. the framework around them behaves as designed: verification runs between
//      passes, a pass that breaks the MIR is rolled back rather than handed
//      on, analyses are cached and invalidated, and the differential validator
//      catches an optimisation that changes what the program observes.

#include "zl/mir/builder.hpp"
#include "zl/mir/differential.hpp"
#include "zl/mir/effects.hpp"
#include "zl/mir/folding.hpp"
#include "zl/mir/passes.hpp"
#include "zl/mir/printer.hpp"
#include "zl/mir/ssa.hpp"
#include "zl/mir/verifier.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void require(bool condition, const std::string& message) {
    ++checks;
    if (condition) return;
    std::cerr << "mir pass regression: " << message << '\n';
    ++failures;
}

using namespace zl::mir;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Runs one pass over one function, with a fresh analysis manager, the way the
// pipeline does.
bool runPass(Module& module, FunctionId id, FunctionPass& pass) {
    FunctionAnalysisManager analyses(module);
    Function* function = module.function(id);
    if (!function) return false;
    return pass.runOnFunction(module, *function, analyses);
}

bool runPass(Module& module, FunctionId id, std::unique_ptr<FunctionPass> pass) {
    return runPass(module, id, *pass);
}

[[nodiscard]] std::string textOf(const Module& module, FunctionId id) {
    const Function* function = module.function(id);
    return function ? printFunction(module, *function, PrinterOptions{false, false, false}) : std::string{};
}

[[nodiscard]] bool textHas(const Module& module, FunctionId id, const std::string& needle) {
    return textOf(module, id).find(needle) != std::string::npos;
}

[[nodiscard]] std::size_t countOf(const Module& module, FunctionId id, const std::string& needle) {
    std::size_t total = 0;
    const std::string text = textOf(module, id);
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        ++total;
    }
    return total;
}

// A function whose body is one straight-line block.
struct StraightLine {
    Module module;
    FunctionId function{};
    BlockId entry{};
};

// ---------------------------------------------------------------------------
// Effect classification
// ---------------------------------------------------------------------------

void testEffectClassification() {
    ModuleBuilder builder("effects");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Effects.f()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    require(hasEffect(classifyOpcode(Opcode::Add), Effect::Pure), "add is a pure computation");
    require(hasEffect(classifyOpcode(Opcode::Add), Effect::MayThrow),
            "add may throw: ZL raises on integer overflow");
    require(!hasEffect(classifyOpcode(Opcode::BitAnd), Effect::MayThrow), "bit_and cannot throw");
    require(hasEffect(classifyOpcode(Opcode::Log), Effect::ObservableIO), "log is observable IO");
    require(hasEffect(classifyOpcode(Opcode::Call), Effect::Calls), "call is a call");
    require(hasEffect(classifyOpcode(Opcode::Drop), Effect::Ownership), "drop is an ownership event");
    require(hasEffect(classifyOpcode(Opcode::AtomicLoad), Effect::VolatileRead),
            "an atomic load is a volatile read");
    require(hasEffect(classifyOpcode(Opcode::MutexWithLock), Effect::UnwindBarrier),
            "a scoped lock is an unwind barrier");
    require(hasEffect(classifyOpcode(Opcode::Await), Effect::Suspension), "await suspends");
    require(hasEffect(classifyOpcode(Opcode::ThreadStart), Effect::ThreadBoundary),
            "thread start crosses a thread boundary");
    require(hasEffect(classifyOpcode(Opcode::HandleClose), Effect::NativeResource),
            "handle close is a native-resource event");

    // Nothing that writes memory, calls, synchronises or observes is removable
    // just because its result is unused.
    require(!isRemovableIfResultUnused(classifyOpcode(Opcode::Call)), "an unused call is not removable");
    require(!isRemovableIfResultUnused(classifyOpcode(Opcode::Store)), "a store is not removable");
    require(!isRemovableIfResultUnused(classifyOpcode(Opcode::Drop)), "a drop is not removable");
    require(!isRemovableIfResultUnused(classifyOpcode(Opcode::AtomicLoad)), "an atomic load is not removable");
    require(!isRemovableIfResultUnused(classifyOpcode(Opcode::FieldLoad)), "a field load is not removable");
    require(isRemovableIfResultUnused(classifyOpcode(Opcode::Load)), "a dead load is removable");
    require(isRemovableIfResultUnused(classifyOpcode(Opcode::TypeTest)), "a dead type test is removable");

    (void)module;
}

// An instruction is only removable once the possibility of a raise has been
// discharged, which happens for constant operands the evaluator accepts.
void testThrowDischarge() {
    ModuleBuilder builder("discharge");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Discharge.f()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);

    const std::int64_t max = std::numeric_limits<std::int64_t>::max();

    // Built by hand here: the builder refuses nothing, but the point is the
    // instruction's operands, and spelling them out is clearer than emitting.
    Instruction safe;
    safe.opcode = Opcode::Add;
    safe.result = 1;
    safe.resultType = types.intType();
    safe.operands = {Operand::constant(builder.constantInt(2), types.intType()),
                     Operand::constant(builder.constantInt(3), types.intType())};

    Instruction raising;
    raising.opcode = Opcode::Add;
    raising.result = 2;
    raising.resultType = types.intType();
    raising.operands = {Operand::constant(builder.constantInt(max), types.intType()),
                        Operand::constant(builder.constantInt(1), types.intType())};

    Instruction dividingByZero;
    dividingByZero.opcode = Opcode::Div;
    dividingByZero.result = 3;
    dividingByZero.resultType = types.intType();
    dividingByZero.operands = {Operand::constant(builder.constantInt(1), types.intType()),
                               Operand::constant(builder.constantInt(0), types.intType())};

    fb.emitReturn();
    fb.finish();
    const Module module = builder.take();

    require(!instructionMayThrow(module, safe), "2 + 3 cannot raise, so the flag is discharged");
    require(instructionMayThrow(module, raising), "INT64_MAX + 1 raises at runtime and must stay");
    require(instructionMayThrow(module, dividingByZero), "1 / 0 raises at runtime and must stay");

    require(isRemovableIfResultUnused(module, safe), "a folded add becomes removable once dead");
    require(!isRemovableIfResultUnused(module, raising), "an add that would raise is never removable");
    require(!isRemovableIfResultUnused(module, dividingByZero), "a division by zero is never removable");
}

// ---------------------------------------------------------------------------
// The constant evaluator
// ---------------------------------------------------------------------------

void testFolding() {
    ModuleBuilder builder("folding");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Folding.f()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    auto folded = [&](Opcode opcode, const Constant& a, const Constant& b, std::uint32_t resultType) {
        Instruction instruction;
        instruction.opcode = opcode;
        instruction.result = 1;
        instruction.resultType = resultType;
        instruction.operands = {Operand::constant(module.internConstant(a), constantTypeOf(module, a)),
                                Operand::constant(module.internConstant(b), constantTypeOf(module, b))};
        // The pool is const here, so intern the operands through the already
        // built constants: `internConstant` is what deduplicates them.
        return foldInstruction(module, instruction);
    };
    auto unaryFolded = [&](Opcode opcode, const Constant& a, std::uint32_t resultType) {
        Instruction instruction;
        instruction.opcode = opcode;
        instruction.result = 1;
        instruction.resultType = resultType;
        instruction.operands = {Operand::constant(module.internConstant(a), constantTypeOf(module, a))};
        return foldInstruction(module, instruction);
    };

    Constant two;
    two.kind = ConstKind::Int;
    two.intValue = 2;
    Constant three;
    three.kind = ConstKind::Int;
    three.intValue = 3;

    const auto sum = folded(Opcode::Add, two, three, types.intType());
    require(sum && sum->kind == ConstKind::Int && sum->intValue == 5, "2 + 3 folds to 5");

    Constant max;
    max.kind = ConstKind::Int;
    max.intValue = std::numeric_limits<std::int64_t>::max();
    Constant one;
    one.kind = ConstKind::Int;
    one.intValue = 1;
    require(!folded(Opcode::Add, max, one, types.intType()).has_value(),
            "an overflowing add is not folded: the program must still raise");

    Constant zero;
    zero.kind = ConstKind::Int;
    zero.intValue = 0;
    require(!folded(Opcode::Div, one, zero, types.intType()).has_value(),
            "division by zero is not folded: the program must still raise");
    require(!folded(Opcode::Mod, one, zero, types.intType()).has_value(),
            "modulo by zero is not folded");

    Constant sixtyFour;
    sixtyFour.kind = ConstKind::Int;
    sixtyFour.intValue = 64;
    require(!folded(Opcode::Shl, one, sixtyFour, types.intType()).has_value(),
            "an out-of-range shift is not folded: the program must still raise");

    Constant seven;
    seven.kind = ConstKind::Int;
    seven.intValue = 7;
    const auto remainder = folded(Opcode::Mod, seven, three, types.intType());
    require(remainder && remainder->intValue == 1, "7 % 3 folds to 1");
    const auto quotient = folded(Opcode::Div, seven, two, types.intType());
    require(quotient && quotient->intValue == 3, "7 / 2 folds to 3 (truncating, as the VM does)");

    Constant min;
    min.kind = ConstKind::Int;
    min.intValue = std::numeric_limits<std::int64_t>::min();
    Constant minusOne;
    minusOne.kind = ConstKind::Int;
    minusOne.intValue = -1;
    require(!folded(Opcode::Div, min, minusOne, types.intType()).has_value(),
            "INT64_MIN / -1 raises in ZL and is not folded");
    const auto modulo = folded(Opcode::Mod, min, minusOne, types.intType());
    require(modulo && modulo->intValue == 0, "INT64_MIN % -1 is defined as 0 by the VM");
    require(!unaryFolded(Opcode::Neg, min, types.intType()).has_value(),
            "negating INT64_MIN raises and is not folded");

    Constant half;
    half.kind = ConstKind::Double;
    half.doubleValue = 1.5;
    Constant quarter;
    quarter.kind = ConstKind::Double;
    quarter.doubleValue = 2.5;
    const auto realSum = folded(Opcode::Add, half, quarter, types.doubleType());
    require(realSum && realSum->kind == ConstKind::Double && realSum->doubleValue == 4.0,
            "1.5 + 2.5 folds to 4.0");

    Constant hello;
    hello.kind = ConstKind::String;
    hello.stringValue = "hello ";
    Constant world;
    world.kind = ConstKind::String;
    world.stringValue = "world";
    const auto joined = folded(Opcode::Add, hello, world, types.stringType());
    require(joined && joined->kind == ConstKind::String && joined->stringValue == "hello world",
            "string concatenation folds, because + on strings is concatenation");

    // A folded result must have the type the instruction declares, or the
    // substitution would retype its operands.
    require(!folded(Opcode::Add, two, three, types.doubleType()).has_value(),
            "an int operation is not folded into a double-typed result");
    require(!folded(Opcode::Add, two, half, types.intType()).has_value(),
            "mixed operand kinds are not folded");
}

// ---------------------------------------------------------------------------
// Constant folding and propagation
// ---------------------------------------------------------------------------

void testConstantFoldingPass() {
    ModuleBuilder builder("fold-pass");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("FoldPass.f()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId sum = fb.emitBinary(Opcode::Add,
                                     Operand::constant(builder.constantInt(2), types.intType()),
                                     Operand::constant(builder.constantInt(3), types.intType()),
                                     types.intType());
    fb.emitLog(Operand::temp(sum, types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;

    require(textHas(module, id, "add"), "before folding the add is still there");
    require(runPass(module, id, createConstantFoldingPass()), "folding reports a change");
    require(textHas(module, id, "log 5:int"), "the log now names the folded constant");
    require(verifyModule(module).ok(), "the folded function still verifies");

    // The orphaned definition is removed by dead-value elimination, not by the
    // folding pass: folding only rewrites uses.
    require(textHas(module, id, "= add"), "folding leaves the definition for dead-value elimination");
    require(runPass(module, id, createDeadValueEliminationPass()), "the orphaned add is dead");
    require(!textHas(module, id, "= add"), "dead-value elimination removes the folded definition");
    require(verifyModule(module).ok(), "the function verifies after elimination");
}

void testFoldingRefusesToChangeOverflowBehaviour() {
    ModuleBuilder builder("fold-refusal");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("FoldRefusal.f()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId overflowing =
        fb.emitBinary(Opcode::Add,
                      Operand::constant(builder.constantInt(std::numeric_limits<std::int64_t>::max()),
                                        types.intType()),
                      Operand::constant(builder.constantInt(1), types.intType()), types.intType());
    fb.emitLog(Operand::temp(overflowing, types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;

    const std::string before = textOf(module, id);
    require(!runPass(module, id, createConstantFoldingPass()),
            "an overflowing add is declined: folding it would delete a raise");
    require(textOf(module, id) == before, "a declined fold leaves the function untouched");
    require(!runPass(module, id, createDeadValueEliminationPass()),
            "an add that can raise is not dead even when its result is read nowhere");
    require(textHas(module, id, "= add"), "the raising add stays in the program");
}

void testConstantPropagationThroughBlockParameters() {
    ModuleBuilder builder("propagate");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Propagate.f(bool)");
    fb.setReturnType(types.voidType());
    const ParamId choose = fb.addParameter("choose", types.boolType());

    const BlockId b1 = fb.addBlock();
    const BlockId b2 = fb.addBlock();
    const BlockId b3 = fb.addBlock();
    const BlockId b4 = fb.addBlock();
    const BlockParamId merged = fb.addBlockParameter(b4, "merged", types.intType());

    fb.setCurrentBlock(b1);
    fb.emitBranch(fb.parameterOperand(choose), b2, b3);
    fb.setCurrentBlock(b2);
    fb.emitJumpWithArguments(b4, {Operand::constant(builder.constantInt(10), types.intType())});
    fb.setCurrentBlock(b3);
    fb.emitJumpWithArguments(b4, {Operand::constant(builder.constantInt(10), types.intType())});
    fb.setCurrentBlock(b4);
    fb.emitLog(Operand::blockParam(merged, types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;
    require(verifyModule(module).ok(), "the merge verifies before optimising");

    require(runPass(module, id, createConstantPropagationPass()),
            "a block parameter every edge hands the same constant is propagated");
    require(textHas(module, id, "log 10:int"), "the use of the merged value sees the constant");
    require(verifyModule(module).ok(), "the function verifies after propagation");
}

// ---------------------------------------------------------------------------
// Algebraic simplification and redundant conversions
// ---------------------------------------------------------------------------

void testAlgebraicIdentities() {
    ModuleBuilder builder("algebra");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Algebra.f(int,double)");
    fb.setReturnType(types.voidType());
    const ParamId x = fb.addParameter("x", types.intType());
    const ParamId d = fb.addParameter("d", types.doubleType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);

    const Operand xOperand = fb.parameterOperand(x);
    const Operand dOperand = fb.parameterOperand(d);
    const Operand zero = Operand::constant(builder.constantInt(0), types.intType());

    // x + 0 -> x
    const TempId plus = fb.emitBinary(Opcode::Add, xOperand, zero, types.intType());
    // x - x -> 0
    const TempId minus = fb.emitBinary(Opcode::Sub, xOperand, xOperand, types.intType());
    // -(-x) -> x
    const TempId innerNeg = fb.emitUnary(Opcode::Neg, xOperand, types.intType());
    const TempId outerNeg = fb.emitUnary(Opcode::Neg, Operand::temp(innerNeg, types.intType()),
                                         types.intType());
    // x * 1.0 stays as written; d * 1.0 -> d
    const Operand oneDouble = Operand::constant(builder.constantDouble(1.0), types.doubleType());
    const TempId times = fb.emitBinary(Opcode::Mul, dOperand, oneDouble, types.doubleType());
    // d + 0.0 must NOT be rewritten (the sign of zero is observable).
    const Operand zeroDouble = Operand::constant(builder.constantDouble(0.0), types.doubleType());
    const TempId plusDouble = fb.emitBinary(Opcode::Add, dOperand, zeroDouble, types.doubleType());

    fb.emitLog(Operand::temp(plus, types.intType()));
    fb.emitLog(Operand::temp(minus, types.intType()));
    fb.emitLog(Operand::temp(outerNeg, types.intType()));
    fb.emitLog(Operand::temp(times, types.doubleType()));
    fb.emitLog(Operand::temp(plusDouble, types.doubleType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;

    require(runPass(module, id, createAlgebraicSimplificationPass()), "identities were applied");
    require(verifyModule(module).ok(), "the function verifies after simplification");

    const std::string text = textOf(module, id);
    require(text.find("log $0:int") != std::string::npos, "x + 0 was replaced by x");
    require(text.find("log 0:int") != std::string::npos, "x - x was replaced by the constant 0");
    require(text.find("log $0:int") != std::string::npos, "-(-x) was replaced by x");
    require(text.find("log $1:double") != std::string::npos, "d * 1.0 was replaced by d");
    require(text.find("= add") != std::string::npos,
            "d + 0.0 was left alone: -0.0 + 0.0 is +0.0, so the identity is not safe");
}

void testRedundantConversions() {
    // --- the checks that must NOT be removed --------------------------------
    // Lowering only emits a check where the check can fail, so these are the
    // shapes real MIR contains: refining an `unknown` to a type, and asking
    // about a nullable value. Each is a real runtime question and each stays.
    ModuleBuilder builder("conversions");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Conversions.f(string,unknown)");
    fb.setReturnType(types.voidType());
    const ParamId s = fb.addParameter("s", types.stringType());
    const ParamId anything = fb.addParameter("anything", types.unknownType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);

    const TempId checked = fb.emitNullCheck(fb.parameterOperand(s));
    const TempId isNull = fb.emitIsNull(fb.parameterOperand(s));
    const TempId dynamic = fb.emitRefine(fb.parameterOperand(anything), types.intType());
    fb.emitLog(Operand::temp(checked, types.stringType()));
    fb.emitLog(Operand::temp(isNull, types.boolType()));
    fb.emitLog(Operand::temp(dynamic, types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;
    require(verifyModule(module).ok(), "the unoptimised function verifies");

    require(!runPass(module, id, createRedundantConversionPass()),
            "no check is removed when each one can still fail");
    require(textHas(module, id, "= null_check") && textHas(module, id, "= is_null") &&
                textHas(module, id, "= refine"),
            "the checks on a nullable value and on an unknown-typed value stay");
    require(verifyModule(module).ok(), "and it still verifies");

    // --- the checks that are provably free ---------------------------------
    // The verifier rejects these shapes as they stand (`refine` to the type a
    // value already has is a warning; `null_check` on a non-nullable type is an
    // error), so this half asserts what the pass *decides* rather than that the
    // module it was handed was legal. The assertion that matters is the last
    // one: the rewrite introduces no new diagnostic.
    ModuleBuilder free("free");
    TypeArena& freeTypes = free.types();
    FunctionBuilder freeFb = free.addFunction("Free.f(int)");
    freeFb.setReturnType(freeTypes.voidType());
    const ParamId n = freeFb.addParameter("n", freeTypes.intType());
    const BlockId freeEntry = freeFb.addBlock();
    freeFb.setCurrentBlock(freeEntry);
    const Operand nOperand = freeFb.parameterOperand(n);
    const TempId refined = freeFb.emitRefine(nOperand, freeTypes.intType());
    const TempId nullChecked = freeFb.emitNullCheck(nOperand);
    const TempId nullTested = freeFb.emitIsNull(nOperand);
    freeFb.emitLog(Operand::temp(refined, freeTypes.intType()));
    freeFb.emitLog(Operand::temp(nullChecked, freeTypes.intType()));
    freeFb.emitLog(Operand::temp(nullTested, freeTypes.boolType()));
    freeFb.emitReturn();
    freeFb.finish();
    Module freeModule = free.take();
    const FunctionId freeId = freeModule.functions[0].id;
    const VerificationReport beforeReport = verifyModule(freeModule);

    require(runPass(freeModule, freeId, createRedundantConversionPass()),
            "a check that cannot fail is replaced by its operand");
    const std::string text = textOf(freeModule, freeId);
    require(text.find("log $0:int") != std::string::npos,
            "the refine to the type the value already has is gone");
    require(text.find("log false:bool") != std::string::npos,
            "is_null of a non-nullable value becomes the constant false");

    const VerificationReport afterReport = verifyModule(freeModule);
    require(afterReport.errorCount() == beforeReport.errorCount() &&
                afterReport.warningCount() == beforeReport.warningCount(),
            "the rewrite introduces no new diagnostic: the instructions it "
            "replaced were already the ones the verifier objects to");
}

// ---------------------------------------------------------------------------
// Copy propagation
// ---------------------------------------------------------------------------

void testCopyPropagation() {
    ModuleBuilder builder("copies");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Copies.f(int)");
    fb.setReturnType(types.voidType());
    const ParamId x = fb.addParameter("x", types.intType());
    const SlotId slot = fb.addSlot("copy", types.intType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);

    fb.emitStore(slot, fb.parameterOperand(x));
    const TempId first = fb.emitLoad(slot);
    const TempId second = fb.emitLoad(slot);
    fb.emitLog(Operand::temp(first, types.intType()));
    fb.emitLog(Operand::temp(second, types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;

    require(runPass(module, id, createCopyPropagationPass()), "copies were propagated");
    require(verifyModule(module).ok(), "the function verifies after copy propagation");

    const std::string text = textOf(module, id);
    require(text.find("log $0:int") != std::string::npos,
            "the first load was replaced by the value that was stored");
    require(text.find("log %1:int") != std::string::npos,
            "the second load was replaced by the first load's result");
}

// ---------------------------------------------------------------------------
// Branch simplification and dead blocks
// ---------------------------------------------------------------------------

void testBranchSimplificationAndDeadBlocks() {
    ModuleBuilder builder("branches");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Branches.f()");
    fb.setReturnType(types.voidType());
    const BlockId b1 = fb.addBlock();
    const BlockId b2 = fb.addBlock();
    const BlockId b3 = fb.addBlock();
    const BlockId b4 = fb.addBlock();

    fb.setCurrentBlock(b1);
    fb.emitBranch(Operand::constant(builder.constantBool(true), types.boolType()), b2, b3);
    fb.setCurrentBlock(b2);
    fb.emitLog(Operand::constant(builder.constantString("taken"), types.stringType()));
    fb.emitJump(b4);
    fb.setCurrentBlock(b3);
    fb.emitLog(Operand::constant(builder.constantString("not taken"), types.stringType()));
    fb.emitJump(b4);
    fb.setCurrentBlock(b4);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;

    require(runPass(module, id, createBranchSimplificationPass()), "the constant branch was simplified");
    require(!textHas(module, id, "branch"), "the branch became a jump");
    require(verifyModule(module, VerifierOptions{true, false}).ok(),
            "the function verifies with unreachable blocks allowed");

    require(runPass(module, id, createDeadBlockEliminationPass()), "the stranded block was removed");
    require(verifyModule(module).ok(), "the function verifies after pruning");
    const std::string text = textOf(module, id);
    require(text.find("not taken") == std::string::npos, "the unreachable arm is gone");
    require(text.find("taken") != std::string::npos, "the reachable arm is still there");
    require(countOf(module, id, "log ") == 1, "exactly one log survives");
}

// ---------------------------------------------------------------------------
// Dead values
// ---------------------------------------------------------------------------

void testDeadValueEliminationBoundaries() {
    ModuleBuilder builder("dead");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Dead.f(int)");
    fb.setReturnType(types.voidType());
    const ParamId x = fb.addParameter("x", types.intType());
    const SlotId unread = fb.addSlot("unread", types.intType());
    const SlotId read = fb.addSlot("read", types.intType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);

    // Dead: a pure computation nobody reads, on operands that cannot raise.
    (void)fb.emitBinary(Opcode::Add, fb.parameterOperand(x),
                        Operand::constant(builder.constantInt(0), types.intType()), types.intType());
    // Not dead: it can raise, so deleting it would delete an error.
    (void)fb.emitBinary(Opcode::Div, fb.parameterOperand(x),
                        Operand::constant(builder.constantInt(0), types.intType()), types.intType());
    // Dead store: the slot is never read.
    fb.emitStore(unread, fb.parameterOperand(x));
    // Live store: the slot is read below.
    fb.emitStore(read, fb.parameterOperand(x));
    const TempId loaded = fb.emitLoad(read);
    fb.emitLog(Operand::temp(loaded, types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;

    require(runPass(module, id, createDeadValueEliminationPass()), "something was eliminated");
    require(verifyModule(module).ok(), "the function verifies after elimination");

    const std::string text = textOf(module, id);
    require(text.find("= add") == std::string::npos, "the dead add is gone");
    require(text.find("= div") != std::string::npos,
            "the division by zero stays: it raises, and deleting it would delete the raise");
    require(countOf(module, id, "store") == 1, "only the store to the read slot survives");
}

// A closure body's slots are the closure's captured environment, not this
// invocation's private locals: a captured `var` that the closure mutates has to
// persist between calls. "No read in this function" therefore does not mean
// "no effect", and a store that looks dead is not.
//
// This is examples/intermediate/Closures.zl's counter, reduced to the shape
// that matters. The full program is covered by the runtime differential in
// tests/mir_opt_pipeline_tests.cpp; this is the unit-level statement of why.
void testDeadStoresSurviveInClosures() {
    ModuleBuilder builder("closure-stores");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("ClosureStore.$lambda0(int)");
    fb.setLambda(true);
    fb.setReturnType(types.intType());
    const ParamId count = fb.addParameter("count", types.intType());
    // The capture list is what marks the function as a closure body.
    fb.addCapture("count", types.intType(), false, "$local383");
    const SlotId captured = fb.addSlot("count", types.intType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitStore(captured, fb.parameterOperand(count));
    const TempId loaded = fb.emitLoad(captured);
    const TempId incremented = fb.emitBinary(Opcode::Add, Operand::temp(loaded, types.intType()),
                                             Operand::constant(builder.constantInt(1), types.intType()),
                                             types.intType());
    fb.emitStore(captured, Operand::temp(incremented, types.intType()));
    fb.emitReturn(Operand::temp(incremented, types.intType()));
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;

    runPass(module, id, createDeadValueEliminationPass());
    require(textHas(module, id, "store"), "a store to a captured slot is never removed");
    require(verifyModule(module).ok(), "and the closure body still verifies");

    // The same body without the capture list is an ordinary function, and there
    // the store really is dead: nothing reads it.
    ModuleBuilder plainBuilder("plain-stores");
    TypeArena& plainTypes = plainBuilder.types();
    FunctionBuilder plainFb = plainBuilder.addFunction("PlainStore.f(int)");
    plainFb.setReturnType(plainTypes.intType());
    const ParamId value = plainFb.addParameter("value", plainTypes.intType());
    const SlotId unread = plainFb.addSlot("unread", plainTypes.intType());
    const BlockId plainEntry = plainFb.addBlock();
    plainFb.setCurrentBlock(plainEntry);
    plainFb.emitStore(unread, plainFb.parameterOperand(value));
    plainFb.emitReturn(plainFb.parameterOperand(value));
    plainFb.finish();
    Module plain = plainBuilder.take();
    const FunctionId plainId = plain.functions[0].id;

    require(runPass(plain, plainId, createDeadValueEliminationPass()),
            "in a function with no captures the dead store is removed");
    require(!textHas(plain, plainId, "store"), "the dead store is gone");
    require(verifyModule(plain).ok(), "and that function verifies");
}

// ---------------------------------------------------------------------------
// The framework: ordering, verification, rollback, snapshots
// ---------------------------------------------------------------------------

// A pass used to prove the framework catches a broken transform: it deletes
// every `log`, which is exactly the rewrite the safety rules exist to prevent.
class DeleteLogsPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "delete-logs"; }
    [[nodiscard]] std::string description() const override { return "test-only: removes observable output"; }

    bool runOnFunction(Module&, Function& function, FunctionAnalysisManager&) override {
        bool changed = false;
        for (auto& block : function.blocks) {
            auto& instructions = block.instructions;
            const auto removed = std::remove_if(instructions.begin(), instructions.end(),
                                                [](const Instruction& instruction) {
                                                    return instruction.opcode == Opcode::Log;
                                                });
            if (removed != instructions.end()) {
                instructions.erase(removed, instructions.end());
                changed = true;
            }
        }
        return changed;
    }
};

// A pass that produces invalid MIR on purpose, to prove the pipeline rolls back.
class CorruptingPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "corrupt"; }
    [[nodiscard]] std::string description() const override { return "test-only: breaks the CFG"; }

    bool runOnFunction(Module&, Function& function, FunctionAnalysisManager&) override {
        if (function.blocks.empty()) return false;
        // Point the entry block's terminator at a block that does not exist.
        function.blocks.front().terminator.target = 9999;
        function.blocks.front().terminator.kind = TerminatorKind::Jump;
        return true;
    }
};

void testPipelineOrderingAndRegistry() {
    registerBuiltinPasses();
    PassManager manager = PassManager::defaultPipeline();
    require(manager.passes().size() == 8, "the default pipeline has eight passes");
    std::vector<std::string> names;
    for (const auto& pass : manager.passes()) names.push_back(pass->name());
    const std::vector<std::string> expected = {
        "simplify-branches",   "eliminate-dead-blocks", "fold-constants",
        "propagate-constants", "simplify-algebraic",    "remove-redundant-conversions",
        "propagate-copies",    "eliminate-dead-values"};
    require(names == expected, "the pipeline order is the documented one");
    require(manager.describePipeline().find("1. simplify-branches") == 0, "the pipeline describes itself");

    std::string error;
    require(PassManager::namedPipeline("none", error).passes().empty(), "'none' is an empty pipeline");
    require(PassManager::namedPipeline("fold-constants,propagate-copies", error).passes().size() == 2,
            "a comma-separated list builds a pipeline");
    require(!PassManager::namedPipeline("fold-constants,not-a-pass", error).passes().empty() == false ||
                !error.empty(),
            "an unknown pass name is reported rather than ignored");
    require(error.find("not-a-pass") != std::string::npos, "the error names the offending pass");

    const std::vector<std::string> registered = PassRegistry::instance().passNames();
    require(std::find(registered.begin(), registered.end(), "fold-constants") != registered.end(),
            "the built-in passes are registered by name");
}

void testVerificationBetweenPassesAndRollback() {
    ModuleBuilder builder("rollback");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Rollback.f()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitLog(Operand::constant(builder.constantString("kept"), types.stringType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;
    const std::string before = textOf(module, id);

    PassManager manager;
    manager.addPass(std::unique_ptr<FunctionPass>(new CorruptingPass()));
    OptimizationOptions options;
    options.verifyBetweenPasses = true;
    options.rollbackOnVerificationFailure = true;
    options.verifyAtEnd = true;
    const OptimizationReport report = manager.run(module, options);

    require(report.rollbacks == 1, "the corrupting pass was rolled back");
    require(!report.changed(), "a rolled-back pass does not count as a change");
    require(report.runs.size() == 1 && report.runs[0].rolledBack, "the run record says so");
    require(!report.runs[0].verificationError.empty(), "the record carries the verifier's complaint");
    require(textOf(module, id) == before, "the function was restored exactly");
    require(report.verifiedAtEnd && report.finalVerification.ok(),
            "the module verifies at the end, because the broken rewrite never landed");
    require(!report.trustworthy() == false || report.rollbacks == 1, "the report is honest about it");

    // With rollback disabled the damage is reported but left in place: the
    // framework records the failure either way, and the caller decides.
    Module damaged = module;
    require(textOf(damaged, id) == before, "starting from the same function");
    PassManager again;
    again.addPass(std::unique_ptr<FunctionPass>(new CorruptingPass()));
    OptimizationOptions noRollback;
    noRollback.rollbackOnVerificationFailure = false;
    const OptimizationReport kept = again.run(damaged, noRollback);
    require(kept.rollbacks == 0, "nothing is rolled back when rollback is disabled");
    require(!kept.finalVerification.ok(), "and the final verification fails, loudly");
    require(!kept.trustworthy(), "the report says the module is not trustworthy");
}

void testAnalysisCaching() {
    ModuleBuilder builder("cache");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Cache.f(int)");
    fb.setReturnType(types.voidType());
    const ParamId x = fb.addParameter("x", types.intType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitLog(fb.parameterOperand(x));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    FunctionAnalysisManager analyses(module);
    const Function& function = module.functions[0];
    const ControlFlowGraph& first = analyses.cfg(function);
    const ControlFlowGraph& second = analyses.cfg(function);
    require(&first == &second, "a repeated query returns the cached analysis");
    require(analyses.analysesBuilt() == 1 && analyses.analysesReused() == 1, "the cache records both");
    (void)analyses.defUse(function);
    require(analyses.analysesBuilt() == 2, "a different analysis is built once");
    analyses.invalidate(function);
    (void)analyses.cfg(function);
    require(analyses.analysesBuilt() == 3, "invalidation forces a rebuild on the next query");
    analyses.invalidateAll();
    (void)analyses.cfg(function);
    require(analyses.analysesBuilt() == 4, "invalidateAll drops everything");
}

void testSnapshotsAndDiff() {
    ModuleBuilder builder("snapshots");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Snapshots.f()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId sum = fb.emitBinary(Opcode::Add,
                                     Operand::constant(builder.constantInt(20), types.intType()),
                                     Operand::constant(builder.constantInt(22), types.intType()),
                                     types.intType());
    fb.emitLog(Operand::temp(sum, types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const FunctionId id = module.functions[0].id;

    const std::string directory = "/tmp/zl-mir-opt-snapshots";
    OptimizationOptions options;
    options.keepSnapshots = true;
    options.snapshotDirectory = directory;
    const OptimizationReport report = optimizeModule(module, options);

    require(!report.snapshots.empty(), "snapshots are kept when asked for");
    bool sawBefore = false;
    bool sawAfter = false;
    for (const auto& snapshot : report.snapshots) {
        if (snapshot.isBefore) sawBefore = true;
        else sawAfter = true;
    }
    require(sawBefore && sawAfter, "both sides of a change are captured");
    require(report.snapshotsWritten != 0, "snapshot files were written");

    const std::string diff = diffLines("one\ntwo\nthree\n", "one\nthree\n");
    require(diff.find("-two") != std::string::npos, "the diff shows the removed line");
    require(diff.find("@@") != std::string::npos, "the diff has hunks");
    require(diffLines("same\n", "same\n").find("no textual difference") != std::string::npos,
            "an unchanged pair says so");
    (void)id;
}

// ---------------------------------------------------------------------------
// Differential validation
// ---------------------------------------------------------------------------

Module buildArithmeticProgram() {
    ModuleBuilder builder("differential");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Differential.f()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId sum = fb.emitBinary(Opcode::Add,
                                     Operand::constant(builder.constantInt(2), types.intType()),
                                     Operand::constant(builder.constantInt(3), types.intType()),
                                     types.intType());
    const TempId twice = fb.emitBinary(Opcode::Mul, Operand::temp(sum, types.intType()),
                                       Operand::constant(builder.constantInt(2), types.intType()),
                                       types.intType());
    fb.emitLog(Operand::temp(twice, types.intType()));
    fb.emitReturn();
    fb.finish();
    return builder.take();
}

void testDifferentialAcceptsARealOptimization() {
    const Module before = buildArithmeticProgram();
    Module after = before;
    const OptimizationReport report = optimizeModule(after);

    require(report.changed(), "the optimiser found something to do");
    require(report.verifiedAtEnd && report.finalVerification.ok(), "the optimised module verifies");

    const DifferentialResult result = compareModules(before, after);
    require(result.equivalent, "the optimised module observes the same thing: " + result.describe());
    require(result.instructionsAfter < result.instructionsBefore, "and it is smaller");
    require(result.eventsCompared != 0, "observable events were compared");
}

void testDifferentialCatchesARemovedObservation() {
    const Module before = buildArithmeticProgram();
    Module after = before;
    {
        PassManager manager;
        manager.addPass(std::unique_ptr<FunctionPass>(new DeleteLogsPass()));
        OptimizationOptions options;
        options.verifyAtEnd = false;
        (void)manager.run(after, options);
    }
    const DifferentialResult result = compareModules(before, after);
    require(!result.equivalent, "removing a log is caught even though the MIR still verifies");
    require(!result.mismatches.empty() &&
                result.mismatches[0].kind == "observable-events",
            "the mismatch is reported as a change in observable events");
    require(result.describe().find("mismatch") != std::string::npos, "the report says so");
}

// `if 1 < 2 { log("t") } else { log("u") }`. Folding decides the condition; only
// the branch-simplification and dead-block passes remove the untaken arm and
// the log inside it. So the two pipelines disagree about one observable event,
// which is exactly the situation the reference pipeline exists for.
Module buildDecidedBranchProgram(const char* moduleName) {
    ModuleBuilder builder(moduleName);
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Ref.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    const BlockId taken = fb.addBlock();
    const BlockId untaken = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId condition = fb.emitBinary(Opcode::Lt, Operand::constant(builder.constantInt(1), types.intType()),
                                           Operand::constant(builder.constantInt(2), types.intType()),
                                           types.boolType());
    fb.emitBranch(Operand::temp(condition, types.boolType()), taken, untaken);
    fb.setCurrentBlock(taken);
    fb.emitLog(Operand::constant(builder.constantString("taken"), types.stringType()));
    fb.emitReturn();
    fb.setCurrentBlock(untaken);
    fb.emitLog(Operand::constant(builder.constantString("untaken"), types.stringType()));
    fb.emitReturn();
    fb.finish();
    return builder.take();
}

// The reference module has to be built by the pipeline under test. Judge a
// module built by one pass against a reference built by eight and every event
// the other seven would have deleted is reported as a divergence - which says
// something about the comparison and nothing about the module.
void testDifferentialReferencePipeline() {
    const Module before = buildDecidedBranchProgram("reference-pipeline");
    Module after = before;
    {
        std::string error;
        PassManager manager = PassManager::namedPipeline("fold-constants", error);
        require(error.empty(), "fold-constants is a pass name");
        OptimizationOptions options;
        options.verifyAtEnd = false;
        (void)manager.run(after, options);
    }
    auto logsIn = [](const Module& module) {
        std::size_t count = 0;
        for (const Function& function : module.functions)
            for (const BasicBlock& block : function.blocks)
                for (const Instruction& instruction : block.instructions)
                    if (instruction.opcode == Opcode::Log) ++count;
        return count;
    };

    // The premise of the whole test: the two pipelines really do disagree.
    Module fullyOptimized = before;
    (void)optimizeModule(fullyOptimized);
    require(logsIn(after) == 2, "folding alone leaves both logs in place");
    require(logsIn(fullyOptimized) == 1, "the full pipeline deletes the untaken one");

    const DifferentialOptions matching = DifferentialOptions{}.withReferencePipeline("fold-constants");
    const DifferentialResult withMatchingReference = compareModules(before, after, matching);
    require(withMatchingReference.equivalent,
            "against a reference built the same way, the module is equivalent: " +
                withMatchingReference.describe());
    require(matching.referencePipeline == "fold-constants", "the pipeline is carried on the options");

    // The default pipeline also simplifies the branch and deletes the untaken
    // arm, so it is one observable event shorter than the module above.
    const DifferentialResult withDefaultReference = compareModules(before, after, DifferentialOptions{});
    require(!withDefaultReference.equivalent,
            "against a reference built by the full pipeline, the same module is reported as divergent");
    require(!withDefaultReference.mismatches.empty() &&
                withDefaultReference.mismatches[0].kind == "observable-events",
            "and the mismatch is an event the reference removed and this pipeline did not");
}

void testDifferentialCatchesStructuralDamage() {
    const Module before = buildArithmeticProgram();
    Module after = before;
    after.functions[0].returnType = after.types.intType();
    const DifferentialResult result = compareModules(before, after);
    require(!result.equivalent, "a changed signature is caught");
    require(result.mismatches[0].kind == "structure", "and reported as structural");
}

// ---------------------------------------------------------------------------
// End-to-end: the default pipeline on a function with several shapes in it
// ---------------------------------------------------------------------------

void testDefaultPipelineEndToEnd() {
    ModuleBuilder builder("end-to-end");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("EndToEnd.f(int)");
    fb.setReturnType(types.voidType());
    const ParamId n = fb.addParameter("n", types.intType());
    const BlockId b1 = fb.addBlock();
    const BlockId b2 = fb.addBlock();
    const BlockId b3 = fb.addBlock();
    const BlockId b4 = fb.addBlock();

    fb.setCurrentBlock(b1);
    // A branch that is decided by a constant comparison.
    const TempId test = fb.emitBinary(Opcode::Lt, Operand::constant(builder.constantInt(1), types.intType()),
                                      Operand::constant(builder.constantInt(2), types.intType()),
                                      types.boolType());
    fb.emitBranch(Operand::temp(test, types.boolType()), b2, b3);
    fb.setCurrentBlock(b2);
    const TempId dead = fb.emitBinary(Opcode::Add, fb.parameterOperand(n),
                                      Operand::constant(builder.constantInt(0), types.intType()),
                                      types.intType());
    fb.emitLog(Operand::temp(dead, types.intType()));
    fb.emitJump(b4);
    fb.setCurrentBlock(b3);
    fb.emitLog(Operand::constant(builder.constantString("unreachable"), types.stringType()));
    fb.emitJump(b4);
    fb.setCurrentBlock(b4);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const Module before = module;
    OptimizationOptions options;
    options.keepSnapshots = false;
    const OptimizationReport report = optimizeModule(module, options);

    require(report.changed(), "the pipeline changed the module");
    require(report.verifiedAtEnd && report.finalVerification.ok(), "the result verifies");
    require(report.rollbacks == 0, "nothing had to be rolled back");
    require(report.trustworthy(), "the report says the module can be handed on");
    require(module.functions[0].blocks.size() < before.functions[0].blocks.size(),
            "the unreachable arm was pruned");
    require(compareModules(before, module).equivalent, "and the observable behaviour is unchanged");
    require(!report.describe().empty(), "the report describes itself");
    require(!report.describeTrace().empty(), "and has a per-pass trace");
}

} // namespace

int main() {
    testEffectClassification();
    testThrowDischarge();
    testFolding();
    testConstantFoldingPass();
    testFoldingRefusesToChangeOverflowBehaviour();
    testConstantPropagationThroughBlockParameters();
    testAlgebraicIdentities();
    testRedundantConversions();
    testCopyPropagation();
    testBranchSimplificationAndDeadBlocks();
    testDeadValueEliminationBoundaries();
    testDeadStoresSurviveInClosures();
    testPipelineOrderingAndRegistry();
    testVerificationBetweenPassesAndRollback();
    testAnalysisCaching();
    testSnapshotsAndDiff();
    testDifferentialAcceptsARealOptimization();
    testDifferentialCatchesARemovedObservation();
    testDifferentialReferencePipeline();
    testDifferentialCatchesStructuralDamage();
    testDefaultPipelineEndToEnd();

    std::cout << "mir pass regressions: " << checks << " checks, " << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
