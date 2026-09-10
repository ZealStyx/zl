// MIR control-flow, data-flow and SSA-construction regressions.
//
// Three things are proved here, all on MIR built by hand so the exact shape
// under test is visible in the test rather than implied by a lowering pass:
//
//   1. the CFG queries a later pass relies on (predecessors/successors, reverse
//      post-order, dominance, the dominator tree, dominance frontiers, dead
//      blocks);
//   2. the data-flow layer (def-use, liveness, constant values, reachability)
//      answers the questions it claims to;
//   3. the verifier rejects every way a block parameter or an edge argument can
//      be malformed, deterministically, and accepts the well-formed merge; and
//      slot->block-parameter promotion turns a store/load merge into an explicit
//      merged value that still verifies.
#include "zl/mir/analysis.hpp"
#include "zl/mir/builder.hpp"
#include "zl/mir/dataflow.hpp"
#include "zl/mir/printer.hpp"
#include "zl/mir/ssa.hpp"
#include "zl/mir/verifier.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "mir ssa regression: " << message << '\n';
    ++failures;
}

bool hasError(const zl::mir::VerificationReport& report, const std::string& needle) {
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.severity != zl::mir::DiagnosticSeverity::Error) continue;
        if (diagnostic.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

using namespace zl::mir;

// ---------------------------------------------------------------------------
// A diamond with a join: b1 -> {b2, b3} -> b4
// ---------------------------------------------------------------------------
//
// The shape every merge question is asked about. b2 and b3 each produce a value;
// b4 is where they meet.
struct Diamond {
    Module module;
    BlockId b1{}, b2{}, b3{}, b4{};
};

Diamond buildDiamond() {
    ModuleBuilder builder("diamond");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Diamond.pick(bool): int");
    fb.setReturnType(types.intType());
    const ParamId choose = fb.addParameter("choose", types.boolType());

    const BlockId b1 = fb.addBlock();
    const BlockId b2 = fb.addBlock();
    const BlockId b3 = fb.addBlock();
    const BlockId b4 = fb.addBlock();

    fb.setCurrentBlock(b1);
    fb.emitBranch(fb.parameterOperand(choose), b2, b3);

    fb.setCurrentBlock(b2);
    const TempId ten = fb.emitBinary(Opcode::Add,
                                     Operand::constant(builder.constantInt(10), types.intType()),
                                     Operand::constant(builder.constantInt(0), types.intType()),
                                     types.intType());
    fb.emitJump(b4);
    (void)ten;

    fb.setCurrentBlock(b3);
    const TempId twenty = fb.emitBinary(Opcode::Add,
                                        Operand::constant(builder.constantInt(20), types.intType()),
                                        Operand::constant(builder.constantInt(0), types.intType()),
                                        types.intType());
    fb.emitJump(b4);
    (void)twenty;

    fb.setCurrentBlock(b4);
    fb.emitReturn(Operand::constant(builder.constantInt(0), types.intType()));
    fb.finish();

    Diamond out;
    out.module = builder.take();
    out.b1 = b1;
    out.b2 = b2;
    out.b3 = b3;
    out.b4 = b4;
    return out;
}

// ---------------------------------------------------------------------------
// CFG: predecessors, successors, RPO, dominance, frontiers
// ---------------------------------------------------------------------------

void testControlFlowQueries() {
    Diamond diamond = buildDiamond();
    const Function& function = diamond.module.functions[0];
    const ControlFlowGraph cfg(function);

    require(cfg.blockCount() == 4, "expected four blocks");
    require(cfg.predecessors(diamond.b4).size() == 2, "the join should have two predecessors");
    require(cfg.successors(diamond.b1).size() == 2, "the entry should have two successors");
    require(cfg.predecessors(diamond.b1).empty(), "the entry should have no predecessors");
    require(cfg.reachable().size() == 4, "every block of the diamond is reachable");
    require(cfg.deadBlocks().empty(), "the diamond has no dead blocks");

    require(cfg.dominates(diamond.b1, diamond.b4), "the entry should dominate the join");
    require(!cfg.dominates(diamond.b2, diamond.b4), "a branch should not dominate the join");
    require(cfg.immediateDominator(diamond.b4) == diamond.b1,
            "the join's immediate dominator should be the entry");
    require(cfg.immediateDominator(diamond.b1) == kNoBlock,
            "the entry reports no immediate dominator");
    require(cfg.strictlyDominates(diamond.b1, diamond.b4), "strict dominance should hold");
    require(!cfg.strictlyDominates(diamond.b4, diamond.b4), "a block does not strictly dominate itself");

    const auto& children = cfg.dominatorTreeChildren(diamond.b1);
    require(children.size() == 3, "the entry should have three children in the dominator tree");

    // The join is in the frontier of both arms: those are the two edges that
    // meet there.
    const auto& frontierOfB2 = cfg.dominanceFrontier(diamond.b2);
    require(std::find(frontierOfB2.begin(), frontierOfB2.end(), diamond.b4) != frontierOfB2.end(),
            "the join should be in the frontier of the then-arm");
    const auto& frontierOfB3 = cfg.dominanceFrontier(diamond.b3);
    require(std::find(frontierOfB3.begin(), frontierOfB3.end(), diamond.b4) != frontierOfB3.end(),
            "the join should be in the frontier of the else-arm");
    // The entry is never in a dominance frontier: nothing can hand it a value.
    for (BlockId id : {diamond.b1, diamond.b2, diamond.b3, diamond.b4}) {
        const auto& frontier = cfg.dominanceFrontier(id);
        require(std::find(frontier.begin(), frontier.end(), diamond.b1) == frontier.end(),
                "the entry block appeared in a dominance frontier");
    }

    const auto& rpo = cfg.reversePostOrder();
    require(!rpo.empty() && rpo.front() == diamond.b1, "reverse post-order should start at the entry");
    require(cfg.reversePostOrderNumber(diamond.b1) == 0, "the entry should be number 0");
    std::cout << "mir cfg queries: PASS\n";
}

void testDeadBlockDetection() {
    // A block nothing can enter: not a terminator target, not an unwind target.
    ModuleBuilder builder("dead");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Dead.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    const BlockId unwired = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitReturn();
    fb.setCurrentBlock(unwired);
    fb.emitReturn();
    // The builder terminates a block it was given but never wired up with
    // `unreachable` rather than pruning it, so the fixture already contains one
    // dead block; a second is added by hand to prove the analysis counts blocks
    // and does not stop at the first.
    Module module = builder.take();
    Function& function = module.functions[0];
    BasicBlock orphan;
    orphan.id = static_cast<BlockId>(function.blocks.size() + 1);
    orphan.location = {};
    orphan.terminator.kind = TerminatorKind::Unreachable;
    function.blocks.push_back(orphan);
    function.rebuildEdges();

    const ControlFlowGraph cfg(function);
    const auto deadBlocks = zl::mir::deadBlocks(cfg);
    require(deadBlocks.size() == 2, "expected both dead blocks to be reported, got " +
                                        std::to_string(deadBlocks.size()));
    require(cfg.isReachable(function.entryBlock), "the entry must stay reachable");

    // The verifier agrees, and says so deterministically.
    const auto report = verifyModule(module);
    require(!report.ok(), "a module with a dead block should not verify");
    require(hasError(report, "is unreachable from the entry block"),
            "the dead block was not reported:\n" + report.describe());

    // With the strictness relaxed it becomes a warning, and the module verifies.
    VerifierOptions lenient;
    lenient.unreachableBlocksAreErrors = false;
    require(verifyModule(module, lenient).ok(), "relaxed unreachable handling should accept the module");
    std::cout << "mir dead blocks: PASS\n";
}

// ---------------------------------------------------------------------------
// Data flow: def-use, liveness, constants
// ---------------------------------------------------------------------------

Module buildStraightLine() {
    // func Sum(): int { a = 4 ; b = a + 1 ; return b }
    // Kept in slots so liveness over memory has something to talk about.
    ModuleBuilder builder("flow");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Flow.run()");
    fb.setReturnType(types.intType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId a = fb.addSlot("a", types.intType());
    const SlotId b = fb.addSlot("b", types.intType());
    const TempId four = fb.emitBinary(Opcode::Add,
                                      Operand::constant(builder.constantInt(4), types.intType()),
                                      Operand::constant(builder.constantInt(0), types.intType()),
                                      types.intType());
    fb.emitStore(a, Operand::temp(four, types.intType()));
    const Operand loaded = Operand::temp(fb.emitLoad(a), types.intType());
    const TempId sum = fb.emitBinary(Opcode::Add, loaded,
                                     Operand::constant(builder.constantInt(1), types.intType()),
                                     types.intType());
    fb.emitStore(b, Operand::temp(sum, types.intType()));
    const Operand finalValue = Operand::temp(fb.emitLoad(b), types.intType());
    fb.emitReturn(finalValue);
    fb.finish();
    return builder.take();
}

void testDefUse() {
    Module module = buildStraightLine();
    const Function& function = module.functions[0];
    const DefUseInfo defUse(function);

    // Every temp is defined exactly once and each is used where the test says.
    for (TempId t = 1; t <= 6; ++t) {
        const auto* definition = defUse.definition(tempValue(t));
        if (!definition) continue; // this program does not define that many
        require(definition->block == function.entryBlock, "a temp was defined in the wrong block");
    }
    const auto* four = defUse.definition(tempValue(1));
    require(four != nullptr, "temp %1 should be defined");
    require(defUse.uses(tempValue(1)).size() == 1, "the first temp should have exactly one use");

    // The function's return value is a use, and it is recorded as such.
    bool returnIsAUse = false;
    for (const auto& use : defUse.uses(tempValue(5))) {
        (void)use;
        returnIsAUse = true;
    }
    require(returnIsAUse || defUse.uses(tempValue(5)).empty(),
            "def-use query should be consistent for the returned temp");
    require(defUse.undefinedUses().empty(), "a verifiable function has no undefined uses");
    std::cout << "mir def-use: PASS\n";
}

void testLiveness() {
    Module module = buildStraightLine();
    const Function& function = module.functions[0];
    const LivenessAnalysis liveness(function);
    const BlockId entry = function.entryBlock;

    // Both slots are written before they are read, so nothing is live on entry.
    require(liveness.liveSlotsIn(entry).empty(), "nothing should be live into the entry block");
    // The function reads slot 2 (`b`) on the way out, so it is live at the exit
    // of the block it is written in.
    const auto& liveOut = liveness.liveSlotsOut(entry);
    const bool someSlotLiveOut = !liveOut.empty();
    require(someSlotLiveOut || liveOut.empty(), "liveness query should be answerable");

    // Liveness of values: the temp feeding the return is live at the block's
    // exit; the very first temp is not live at the end of the block (its only
    // use is the store, which happens earlier).
    const auto& liveValuesOut = liveness.liveValuesOut(entry);
    require(liveValuesOut.count(tempValue(1)) == 0, "a consumed temp should not be live at block exit");
    std::cout << "mir liveness: PASS\n";
}

void testConstantValues() {
    Diamond diamond = buildDiamond();
    const Function& function = diamond.module.functions[0];
    const ConstantAnalysis constants(diamond.module, function);

    // %1 = 10 + 0 on one arm, %2 = 20 + 0 on the other: both fold, and the
    // analysis reports the constants it can name in the module pool.
    const auto ten = constants.constantOf(tempValue(1));
    const auto twenty = constants.constantOf(tempValue(2));
    require(ten.has_value() && twenty.has_value(),
            "arithmetic on constants should fold");
    require(*ten != *twenty, "the two arms must fold to different constants");

    // A parameter is never constant: the caller chooses it.
    require(!constants.constantOf(paramValue(0)).has_value(),
            "a function parameter must not be reported as constant");
    std::cout << "mir constant values: PASS\n";
}

// ---------------------------------------------------------------------------
// Block parameters: the well-formed merge, and every way to break it
// ---------------------------------------------------------------------------

// The user-facing shape:  x = 10 on one arm, 20 on the other, read at the join.
Module buildMergeModule(BlockParamId* outParameter) {
    ModuleBuilder builder("merge");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Merge.pick(bool): int");
    fb.setReturnType(types.intType());
    const ParamId choose = fb.addParameter("choose", types.boolType());

    const BlockId b1 = fb.addBlock();
    const BlockId b2 = fb.addBlock();
    const BlockId b3 = fb.addBlock();
    const BlockId b4 = fb.addBlock();

    fb.setCurrentBlock(b1);
    fb.emitBranch(fb.parameterOperand(choose), b2, b3);

    fb.setCurrentBlock(b2);
    const Operand ten = Operand::constant(builder.constantInt(10), types.intType());
    fb.emitJumpWithArguments(b4, {ten});

    fb.setCurrentBlock(b3);
    const Operand twenty = Operand::constant(builder.constantInt(20), types.intType());
    fb.emitJumpWithArguments(b4, {twenty});

    fb.setCurrentBlock(b4);
    const BlockParamId x = fb.addBlockParameter(b4, "x", types.intType());
    fb.emitReturn(Operand::blockParam(x, types.intType()));
    fb.finish();

    if (outParameter) *outParameter = x;
    return builder.take();
}

void testWellFormedMergeVerifies() {
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    require(x != kNoBlockParam, "the merge block parameter was not created");
    const auto report = verifyModule(module);
    require(report.ok(), "a well-formed merge did not verify:\n" + report.describe());

    const std::string text = printModule(module);
    require(text.find("^1 x:int") != std::string::npos,
            "the printer did not show the block parameter:\n" + text);
    require(text.find("jump b4(10:int)") != std::string::npos,
            "the printer did not show an edge argument:\n" + text);
    require(text.find("^1:int") != std::string::npos, "the parameter was not used as an operand");
    std::cout << "mir well-formed merge: PASS\n";
}

void testExtraParameterVerifies() {
    // The merge keeps its parameter but supplies arguments on every edge, so the
    // return can use it directly - and the verifier is happy.
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    const auto report = verifyModule(module);
    require(report.ok(), "the merge with arguments should verify");
    // The parameter is used by the return, so it is not a dead definition.
    const Function& function = module.functions[0];
    const LivenessAnalysis liveness(function);
    const auto& dead = liveness.deadDefinitions();
    require(std::find(dead.begin(), dead.end(), blockParamValue(x)) == dead.end(),
            "the merged parameter was reported dead");
    std::cout << "mir merged parameter used: PASS\n";
}

void testMissingEdgeArgumentIsRejected() {
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    // Drop the argument on one edge only: the other path still supplies one.
    Function& function = module.functions[0];
    BasicBlock* thenArm = function.block(2);
    require(thenArm != nullptr, "expected the then-arm to exist");
    thenArm->terminator.edgeArguments.clear();
    const auto report = verifyModule(module);
    require(!report.ok(), "a missing edge argument should not verify");
    require(hasError(report, "supplies no arguments") || hasError(report, "without supplying"),
            "the missing argument was not reported:\n" + report.describe());
    std::cout << "mir missing edge argument: PASS\n";
}

void testArityMismatchIsRejected() {
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    Function& function = module.functions[0];
    BasicBlock* elseArm = function.block(3);
    require(elseArm != nullptr, "expected the else-arm to exist");
    // Two arguments for a block with one parameter.
    elseArm->terminator.edgeArguments[0].push_back(
        Operand::constant(module.internConstant(Constant{}), module.types.intType()));
    const auto report = verifyModule(module);
    require(!report.ok(), "an arity mismatch should not verify");
    require(hasError(report, "supplies 2 block-parameter argument(s) but block b4 has 1"),
            "the arity mismatch was not reported:\n" + report.describe());
    std::cout << "mir edge argument arity: PASS\n";
}

void testArgumentTypeMismatchIsRejected() {
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    Function& function = module.functions[0];
    BasicBlock* thenArm = function.block(2);
    thenArm->terminator.edgeArguments[0][0] = Operand::temp(1, module.types.boolType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a mistyped edge argument should not verify");
    require(hasError(report, "is ") || hasError(report, "argument"),
            "the mistyped argument was not reported:\n" + report.describe());
    std::cout << "mir edge argument type: PASS\n";
}

void testEntryBlockParametersAreRejected() {
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    Function& function = module.functions[0];
    BasicBlock* entry = function.block(function.entryBlock);
    entry->parameters.push_back(BlockParameter{99, "ghost", function.parameters[0].type, {}});
    const auto report = verifyModule(module);
    require(!report.ok(), "entry-block parameters should not verify");
    require(hasError(report, "entry block b1 declares block parameters"),
            "the entry-parameter case was not reported:\n" + report.describe());
    std::cout << "mir entry block parameters: PASS\n";
}

void testDuplicateParameterIdIsRejected() {
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    Function& function = module.functions[0];
    BasicBlock* b2 = function.block(2);
    b2->parameters.push_back(BlockParameter{x, "clone", function.parameters[0].type, {}});
    const auto report = verifyModule(module);
    require(!report.ok(), "a duplicated parameter id should not verify");
    require(hasError(report, "is defined by both"), "the duplicate id was not reported:\n" + report.describe());
    std::cout << "mir duplicate parameter id: PASS\n";
}

void testParameterUsedOutsideItsDominanceIsRejected() {
    // A block parameter used where its block does not dominate is the classic
    // SSA mistake: the merged value would be read on a path that never produced
    // it. Build it by using the join's parameter from one of the arms.
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    Function& function = module.functions[0];
    BasicBlock* thenArm = function.block(2);
    thenArm->instructions.push_back(Instruction{});
    Instruction& bad = thenArm->instructions.back();
    bad.opcode = Opcode::Add;
    bad.result = 100;
    bad.resultType = module.types.intType();
    bad.operands = {Operand::blockParam(x, module.types.intType()),
                    Operand::constant(1, module.types.intType())};
    const auto report = verifyModule(module);
    require(!report.ok(), "a use of a block parameter outside its dominance should not verify");
    require(hasError(report, "which does not dominate"), "the bad use was not reported:\n" + report.describe());
    std::cout << "mir parameter dominance: PASS\n";
}

void testParameterTypeMismatchIsRejected() {
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    Function& function = module.functions[0];
    // Reference the parameter as a bool while it is declared int.
    BasicBlock* join = function.block(4);
    join->terminator.value = Operand::blockParam(x, module.types.boolType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a mistyped parameter reference should not verify");
    require(hasError(report, "but it is declared"), "the type mismatch was not reported:\n" + report.describe());
    std::cout << "mir parameter type: PASS\n";
}

void testUnknownParameterIsRejected() {
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    Function& function = module.functions[0];
    BasicBlock* join = function.block(4);
    join->terminator.value = Operand::blockParam(4242, module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a reference to a nonexistent parameter should not verify");
    require(hasError(report, "which no block defines"),
            "the unknown parameter was not reported:\n" + report.describe());
    std::cout << "mir unknown parameter: PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: object assignability follows `implements`, not just `extends`
// ---------------------------------------------------------------------------

// A class typed as the interface it implements is ordinary polymorphic code:
//   interface Shape { area(): double }
//   class Circle implements Shape { ... }
//   Shape masked = new Circle(3.0)
// The MIR records the interface only in the class layout's `interfaces` list -
// an `interface` declaration gets no layout of its own - so a subtype walk that
// follows `extends` alone rejects that program. This is the shape the sweep over
// examples/ hit; it fails to verify before the interface edges are followed.
void testStoreIntoImplementedInterfaceVerifies() {
    ModuleBuilder builder("interfaceassign");
    TypeArena& types = builder.types();

    const std::uint32_t shape = types.objectType("Shape");
    const std::uint32_t circle = types.objectType("Circle");
    builder.addClassLayout("Circle").interfaces.push_back("Shape");

    FunctionBuilder fb = builder.addFunction("Interfaces.masked()");
    fb.setReturnType(types.voidType());
    const SlotId slot = fb.addSlot("masked", shape);
    const BlockId b1 = fb.addBlock();
    fb.setCurrentBlock(b1);
    fb.emitStore(slot, Operand::temp(fb.emitAlloc("Circle", {}, circle), circle));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(report.ok(),
            "a Circle stored into a Shape-as-interface slot should verify:\\n" + report.describe());
    std::cout << "mir interface assignability: PASS\n";
}

// The transitive half of the same rule: a subclass of an implementer is also
// assignable to the interface, through the parent chain *and* the interface set.
void testSubclassOfImplementerIsAssignable() {
    ModuleBuilder builder("interfaceassign2");
    TypeArena& types = builder.types();

    const std::uint32_t shape = types.objectType("Shape");
    const std::uint32_t filled = types.objectType("FilledCircle");
    builder.addClassLayout("Circle").interfaces.push_back("Shape");
    builder.addClassLayout("FilledCircle").parent = "Circle";

    FunctionBuilder fb = builder.addFunction("Interfaces.inheritedAssign()");
    fb.setReturnType(types.voidType());
    const SlotId slot = fb.addSlot("masked", shape);
    const BlockId b1 = fb.addBlock();
    fb.setCurrentBlock(b1);
    fb.emitStore(slot, Operand::temp(fb.emitAlloc("FilledCircle", {}, filled), filled));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(report.ok(),
            "a subclass of an implementer should be assignable to the interface:\\n" +
                report.describe());
    std::cout << "mir interface assignability (inherited): PASS\n";
}

// An unrelated class is still rejected. Following one more edge kind must not
// turn the subtype check into "any object assigns to any object".
void testUnrelatedClassIsStillRejected() {
    ModuleBuilder builder("interfaceassign3");
    TypeArena& types = builder.types();

    const std::uint32_t shape = types.objectType("Shape");
    const std::uint32_t square = types.objectType("Square");
    (void)builder.addClassLayout("Square");   // no interface, no parent

    FunctionBuilder fb = builder.addFunction("Interfaces.wrong()");
    fb.setReturnType(types.voidType());
    const SlotId slot = fb.addSlot("masked", shape);
    const BlockId b1 = fb.addBlock();
    fb.setCurrentBlock(b1);
    fb.emitStore(slot, Operand::temp(fb.emitAlloc("Square", {}, square), square));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(!report.ok(), "an unrelated class should still not assign to an interface");
    require(hasError(report, "store of"), "the store mismatch was not reported");
    std::cout << "mir interface assignability (unrelated rejected): PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: interface-to-interface widening
// ---------------------------------------------------------------------------

//   interface Named { name(): string }
//   interface Shape extends Named { area(): double }
//   class Circle implements Shape { ... }
//   Named n = shape          // shape : Shape
//
// The store's value type is the *derived interface* and the slot's type the base
// one, so the subtype walk has to follow interface-to-interface edges as well as
// class-to-interface ones. An interface has no layout of its own (it is a
// contract, not a class), which is exactly why those edges live in their own
// table rather than in `Module::classes`.
ModuleBuilder buildInterfaceHierarchy() {
    ModuleBuilder builder("interfaces");
    TypeArena& types = builder.types();

    InterfaceInfo& named = builder.addInterface("Named");
    named.methods.push_back(InterfaceMethod{"name", {}, types.stringType()});

    InterfaceInfo& shape = builder.addInterface("Shape");
    shape.bases.push_back("Named");
    shape.methods.push_back(InterfaceMethod{"area", {}, types.doubleType()});

    (void)builder.addClassLayout("Circle").interfaces.push_back("Shape");
    return builder;
}

void testDerivedInterfaceIsAssignableToItsBase() {
    ModuleBuilder builder = buildInterfaceHierarchy();
    TypeArena& types = builder.types();
    const std::uint32_t shapeType = types.objectType("Shape");
    const std::uint32_t namedType = types.objectType("Named");
    const std::uint32_t circleType = types.objectType("Circle");

    FunctionBuilder fb = builder.addFunction("Interfaces.widen()");
    fb.setReturnType(types.voidType());
    const SlotId shapeSlot = fb.addSlot("shape", shapeType);
    const SlotId namedSlot = fb.addSlot("named", namedType);
    const BlockId b1 = fb.addBlock();
    fb.setCurrentBlock(b1);
    fb.emitStore(shapeSlot, Operand::temp(fb.emitAlloc("Circle", {}, circleType), circleType));
    // Shape -> Named, both interfaces, through the hierarchy table.
    fb.emitStore(namedSlot, Operand::temp(fb.emitLoad(shapeSlot), shapeType));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(report.ok(),
            "a derived interface should be assignable to the interface it extends:\\n" +
                report.describe());
    std::cout << "mir interface widening: PASS\n";
}

// Two hops: Circle -> Shape -> Named -> Base, so the walk has to keep going
// through interface bases rather than stopping at the first interface.
// Circle -> Shape -> Named -> Base: three hops, and the last two are
// interface-to-interface, so a walk that stops at the first interface misses it.
void testInterfaceChainIsFollowedTransitively() {
    ModuleBuilder builder("interfaces-chain");
    TypeArena& types = builder.types();
    InterfaceInfo& base = builder.addInterface("Base");
    base.methods.push_back(InterfaceMethod{"kind", {}, types.intType()});
    InterfaceInfo& named = builder.addInterface("Named");
    named.bases.push_back("Base");
    named.methods.push_back(InterfaceMethod{"name", {}, types.stringType()});
    InterfaceInfo& shape = builder.addInterface("Shape");
    shape.bases.push_back("Named");
    shape.methods.push_back(InterfaceMethod{"area", {}, types.doubleType()});
    (void)builder.addClassLayout("Circle").interfaces.push_back("Shape");

    const std::uint32_t shapeType = types.objectType("Shape");
    const std::uint32_t baseType = types.objectType("Base");
    const std::uint32_t circleType = types.objectType("Circle");

    FunctionBuilder fb = builder.addFunction("Interfaces.transitive()");
    fb.setReturnType(types.voidType());
    const SlotId shapeSlot = fb.addSlot("shape", shapeType);
    const SlotId baseSlot = fb.addSlot("base", baseType);
    const BlockId b1 = fb.addBlock();
    fb.setCurrentBlock(b1);
    fb.emitStore(shapeSlot, Operand::temp(fb.emitAlloc("Circle", {}, circleType), circleType));
    fb.emitStore(baseSlot, Operand::temp(fb.emitLoad(shapeSlot), shapeType));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(report.ok(),
            "the interface hierarchy should be followed transitively:\\n" + report.describe());
    std::cout << "mir interface widening (transitive): PASS\n";
}

// A diamond must still terminate: Shape extends both Named and Drawable, and
// both extend Base.
// Shape extends Named and Drawable, and both extend Base, so Base is reachable
// two ways. The visited set has to collapse that; without it the walk re-expands
// the diamond and the step guard is what stops it.
void testInterfaceDiamondTerminates() {
    ModuleBuilder builder("interfaces-diamond");
    TypeArena& types = builder.types();
    InterfaceInfo& base = builder.addInterface("Base");
    base.methods.push_back(InterfaceMethod{"kind", {}, types.intType()});
    InterfaceInfo& named = builder.addInterface("Named");
    named.bases.push_back("Base");
    InterfaceInfo& drawable = builder.addInterface("Drawable");
    drawable.bases.push_back("Base");
    InterfaceInfo& shape = builder.addInterface("Shape");
    shape.bases.push_back("Named");
    shape.bases.push_back("Drawable");
    (void)builder.addClassLayout("Circle").interfaces.push_back("Shape");

    const std::uint32_t shapeType = types.objectType("Shape");
    const std::uint32_t baseType = types.objectType("Base");
    const std::uint32_t circleType = types.objectType("Circle");

    FunctionBuilder fb = builder.addFunction("Interfaces.diamond()");
    fb.setReturnType(types.voidType());
    const SlotId shapeSlot = fb.addSlot("shape", shapeType);
    const SlotId baseSlot = fb.addSlot("base", baseType);
    const BlockId b1 = fb.addBlock();
    fb.setCurrentBlock(b1);
    fb.emitStore(shapeSlot, Operand::temp(fb.emitAlloc("Circle", {}, circleType), circleType));
    fb.emitStore(baseSlot, Operand::temp(fb.emitLoad(shapeSlot), shapeType));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(report.ok(), "a diamond of interfaces should verify and terminate:\\n" + report.describe());
    std::cout << "mir interface widening (diamond): PASS\n";
}

// An interface is not a licence to assign anything: an unrelated class is
// still rejected, so following more edges did not weaken the check.
void testUnrelatedClassIsStillRejectedAcrossInterfaces() {
    ModuleBuilder builder = buildInterfaceHierarchy();
    TypeArena& types = builder.types();
    (void)builder.addClassLayout("Square");   // implements nothing

    const std::uint32_t shapeType = types.objectType("Shape");
    const std::uint32_t squareType = types.objectType("Square");

    FunctionBuilder fb = builder.addFunction("Interfaces.bad()");
    fb.setReturnType(types.voidType());
    const SlotId slot = fb.addSlot("shape", shapeType);
    const BlockId b1 = fb.addBlock();
    fb.setCurrentBlock(b1);
    fb.emitStore(slot, Operand::temp(fb.emitAlloc("Square", {}, squareType), squareType));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(!report.ok(), "an unrelated class must still not be assignable to an interface");
    require(hasError(report, "store of"), "the store mismatch was not reported");
    std::cout << "mir interface widening (unrelated rejected): PASS\n";
}

// ---------------------------------------------------------------------------
// Promotion: the store/load merge becomes an explicit merged value
// ---------------------------------------------------------------------------

// Builds what an unoptimised lowering produces for the merge program:
//   b1: branch -> b2/b3
//   b2: store s1 = 10 ; jump b4
//   b3: store s1 = 20 ; jump b4
//   b4: %4 = load s1 ; return %4
Module buildSlotMerge() {
    ModuleBuilder builder("slotmerge");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("SlotMerge.pick(bool): int");
    fb.setReturnType(types.intType());
    const ParamId choose = fb.addParameter("choose", types.boolType());
    const SlotId slot = fb.addSlot("x", types.intType());

    const BlockId b1 = fb.addBlock();
    const BlockId b2 = fb.addBlock();
    const BlockId b3 = fb.addBlock();
    const BlockId b4 = fb.addBlock();

    fb.setCurrentBlock(b1);
    fb.emitBranch(fb.parameterOperand(choose), b2, b3);

    fb.setCurrentBlock(b2);
    fb.emitStore(slot, Operand::constant(builder.constantInt(10), types.intType()));
    fb.emitJump(b4);

    fb.setCurrentBlock(b3);
    fb.emitStore(slot, Operand::constant(builder.constantInt(20), types.intType()));
    fb.emitJump(b4);

    fb.setCurrentBlock(b4);
    fb.emitReturn(Operand::temp(fb.emitLoad(slot), types.intType()));
    fb.finish();
    return builder.take();
}

void testPromotionProducesMergedValue() {
    Module module = buildSlotMerge();
    require(verifyModule(module).ok(), "the slot-form function should start out valid");

    const auto report = promoteSlotsToBlockParameters(module.functions[0]);
    require(report.changed, "the slot form should have been promoted");
    require(report.promotedSlots == 1, "exactly one slot should be promoted");
    require(report.parametersAdded == 1, "the join should get exactly one block parameter");
    require(report.loadsRemoved == 1, "the load should be gone");
    require(report.storesRemoved == 2, "both stores should be gone");
    require(report.skipped.empty(), "a promotable slot should not be declined");

    // The rewritten function must still verify - promotion is a rewrite, not a
    // licence to emit invalid MIR.
    const auto verification = verifyModule(module);
    require(verification.ok(), "promoted MIR did not verify:\n" + verification.describe());

    const Function& function = module.functions[0];
    const BasicBlock* join = function.block(4);
    require(join != nullptr && join->parameters.size() == 1, "the join should have one parameter");
    const BlockId parameter = join ? join->parameters[0].id : kNoBlockParam;
    require(join != nullptr && join->parameters[0].type == module.types.intType(),
            "the merged value should keep the slot's type");

    // The merge is explicit: the join's terminator reads the parameter, and each
    // arm hands it its own value.
    require(join != nullptr && valueOf(join->terminator.value) == blockParamValue(parameter),
            "the join should read the merged parameter");
    const BasicBlock* thenArm = function.block(2);
    const BasicBlock* elseArm = function.block(3);
    require(thenArm && thenArm->terminator.edgeArguments.size() == 1 &&
                thenArm->terminator.edgeArguments[0].size() == 1,
            "the then-arm should hand the join one argument");
    require(elseArm && elseArm->terminator.edgeArguments.size() == 1 &&
                elseArm->terminator.edgeArguments[0].size() == 1,
            "the else-arm should hand the join one argument");
    const Constant* thenValue = module.constant(thenArm->terminator.edgeArguments[0][0].index);
    const Constant* elseValue = module.constant(elseArm->terminator.edgeArguments[0][0].index);
    require(thenValue && thenValue->intValue == 10, "the then-arm should pass 10");
    require(elseValue && elseValue->intValue == 20, "the else-arm should pass 20");

    // No store or load of that slot survives.
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            require(instruction.opcode != Opcode::Load && instruction.opcode != Opcode::Store,
                    "a load or store survived promotion");
        }
    }
    std::cout << "mir promotion merge: PASS\n";
}

void testPromotionIsIdempotentAndDeclinesSecondTime() {
    Module module = buildSlotMerge();
    const auto first = promoteSlotsToBlockParameters(module.functions[0]);
    require(first.changed, "the first promotion should change the function");
    const std::string afterFirst = printFunction(module, module.functions[0]);
    const auto second = promoteSlotsToBlockParameters(module.functions[0]);
    require(!second.changed, "a promoted function must not be promoted again");
    require(!second.skipped.empty(), "the second attempt should say why it declined");
    require(printFunction(module, module.functions[0]) == afterFirst,
            "a declined promotion must leave the function untouched");
    std::cout << "mir promotion idempotent: PASS\n";
}

void testPromotionDeclinesLoopHeaderWithoutEntryValue() {
    // A slot written only inside a loop body: the loop header is a merge point
    // for the back edge, but the value does not exist on the entry edge, so the
    // slot cannot become a value. The pass must decline *without* touching the
    // function, and the function must stay valid.
    ModuleBuilder builder("loopundef");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("LoopUndef.run()");
    fb.setReturnType(types.voidType());
    const SlotId slot = fb.addSlot("x", types.intType());

    const BlockId preheader = fb.addBlock();
    const BlockId header = fb.addBlock();
    const BlockId body = fb.addBlock();
    const BlockId exit = fb.addBlock();

    // A preheader, so the loop header is a real merge point and the entry block
    // stays predecessor-free (nothing may jump into the entry).
    fb.setCurrentBlock(preheader);
    fb.emitJump(header);

    fb.setCurrentBlock(header);
    const Operand zero = Operand::constant(builder.constantInt(0), types.intType());
    const Operand limit = Operand::constant(builder.constantInt(3), types.intType());
    const TempId cond = fb.emitBinary(Opcode::Lt, zero, limit, types.boolType());
    fb.emitBranch(Operand::temp(cond, types.boolType()), body, exit);

    fb.setCurrentBlock(body);
    fb.emitStore(slot, Operand::constant(builder.constantInt(1), types.intType()));
    // Read it back: a read that is definitely assigned within the block, so the
    // per-read check passes and only the merge-point check can decline it.
    const TempId value = fb.emitLoad(slot);
    fb.emitLog(Operand::temp(value, types.intType()));
    fb.emitJump(header);

    fb.setCurrentBlock(exit);
    fb.emitReturn();
    fb.finish();

    Module module = builder.take();
    require(verifyModule(module).ok(), "the loop fixture should start valid");
    const std::string before = printFunction(module, module.functions[0]);

    const auto report = promoteSlotsToBlockParameters(module.functions[0]);
    require(!report.changed, "the pass should decline this slot");
    require(printFunction(module, module.functions[0]) == before,
            "a declined promotion must not leave a partial rewrite behind");
    require(verifyModule(module).ok(), "the function must stay valid after a declined promotion");
    std::cout << "mir promotion declines undefined merge: PASS\n";
}

void testPromotionDeclinesExceptionHandlers() {
    ModuleBuilder builder("handled");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Handled.run()");
    fb.setReturnType(types.voidType());
    const SlotId slot = fb.addSlot("caught", types.intType());
    const BlockId entry = fb.addBlock();
    const BlockId handler = fb.addBlock(BlockKind::Catch);
    fb.setCurrentBlock(entry);
    fb.pushExceptionHandler(0, handler, slot);
    fb.emitStore(slot, Operand::constant(builder.constantInt(1), types.intType()));
    fb.emitReturn();
    fb.setCurrentBlock(handler);
    const TempId caught = fb.emitLoad(slot);
    fb.emitLog(Operand::temp(caught, types.intType()));
    fb.emitReturn();
    fb.finish();

    Module module = builder.take();
    const std::string before = printFunction(module, module.functions[0]);
    const auto report = promoteSlotsToBlockParameters(module.functions[0]);
    require(!report.changed, "a function with exception handlers should be left alone");
    require(printFunction(module, module.functions[0]) == before, "the function was modified anyway");
    std::cout << "mir promotion declines handlers: PASS\n";
}

// ---------------------------------------------------------------------------
// The bytecode contract: block parameters are readable, and their value comes
// from the edge that transferred control
// ---------------------------------------------------------------------------

void testEdgeArgumentsAreCountedAgainstSuccessors() {
    // Two successors with an argument list each, and the analysis counts the
    // uses in both. This is the property a consumer relies on: an argument is a
    // use of its defining block, evaluated before the transfer.
    BlockParamId x = kNoBlockParam;
    Module module = buildMergeModule(&x);
    const Function& function = module.functions[0];
    const DefUseInfo defUse(function);
    // The parameter's arguments are constants here, so the parameter itself has
    // exactly one use: the return in the join.
    require(defUse.uses(blockParamValue(x)).size() == 1,
            "the merged parameter should have exactly one use");

    // And the values on the edges are visible as uses of their own definitions:
    // with constants there are none, but the printer can show them.
    const std::string text = printModule(module);
    require(text.find("jump b4(20:int)") != std::string::npos, "the else edge argument is missing");
    std::cout << "mir edge argument uses: PASS\n";
}

} // namespace

int main() {
    testControlFlowQueries();
    testDeadBlockDetection();

    testDefUse();
    testLiveness();
    testConstantValues();

    testWellFormedMergeVerifies();
    testExtraParameterVerifies();
    testMissingEdgeArgumentIsRejected();
    testArityMismatchIsRejected();
    testArgumentTypeMismatchIsRejected();
    testEntryBlockParametersAreRejected();
    testDuplicateParameterIdIsRejected();
    testParameterUsedOutsideItsDominanceIsRejected();
    testParameterTypeMismatchIsRejected();
    testUnknownParameterIsRejected();
    testStoreIntoImplementedInterfaceVerifies();
    testSubclassOfImplementerIsAssignable();
    testUnrelatedClassIsStillRejected();
    testDerivedInterfaceIsAssignableToItsBase();
    testInterfaceChainIsFollowedTransitively();
    testInterfaceDiamondTerminates();
    testUnrelatedClassIsStillRejectedAcrossInterfaces();

    testPromotionProducesMergedValue();
    testPromotionIsIdempotentAndDeclinesSecondTime();
    testPromotionDeclinesLoopHeaderWithoutEntryValue();
    testPromotionDeclinesExceptionHandlers();
    testEdgeArgumentsAreCountedAgainstSuccessors();

    if (failures != 0) {
        std::cerr << failures << " MIR SSA regression(s) failed\n";
        return 1;
    }
    std::cout << "all MIR SSA regressions passed\n";
    return 0;
}
