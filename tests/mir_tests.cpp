// MIR data-model, CFG analysis, and verifier regressions.
//
// These tests build MIR directly rather than lowering it, because the point is
// to prove the verifier rejects each specific class of malformed MIR. A
// lowering-only test could never reach them: the builder refuses to produce
// most of these shapes.
#include "zl/mir/analysis.hpp"
#include "zl/mir/builder.hpp"
#include "zl/mir/printer.hpp"
#include "zl/mir/verifier.hpp"

#include <cstdlib>
#include <optional>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "mir regression: " << message << '\n';
    ++failures;
}

// True when the report contains an error whose message includes `needle`.
bool hasError(const zl::mir::VerificationReport& report, const std::string& needle) {
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.severity != zl::mir::DiagnosticSeverity::Error) continue;
        if (diagnostic.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string reportText(const zl::mir::VerificationReport& report) {
    return report.describe();
}

using namespace zl::mir;

// Constants are built field-by-field rather than by aggregate initialisation,
// so adding a payload member to `Constant` does not silently shift the meaning
// of every positional literal in this file.
Constant intConstant(std::int64_t value) {
    Constant c;
    c.kind = ConstKind::Int;
    c.intValue = value;
    return c;
}

Constant boolConstant(bool value) {
    Constant c;
    c.kind = ConstKind::Bool;
    c.boolValue = value;
    return c;
}

Constant stringConstant(std::string value) {
    Constant c;
    c.kind = ConstKind::String;
    c.stringValue = std::move(value);
    return c;
}

Constant nilConstant() {
    Constant c;
    c.kind = ConstKind::Nil;
    return c;
}

// ---------------------------------------------------------------------------
// Type arena
// ---------------------------------------------------------------------------

void testTypeArena() {
    TypeArena arena;
    const TypeId a = arena.listType(arena.intType());
    const TypeId b = arena.listType(arena.intType());
    require(a == b, "interning did not unify identical list types");
    require(a != arena.listType(arena.doubleType()), "distinct element types shared a type id");

    // Union identity must not depend on the order members were written in.
    const TypeId unionAB = arena.unionType({arena.intType(), arena.stringType()});
    const TypeId unionBA = arena.unionType({arena.stringType(), arena.intType()});
    require(unionAB == unionBA, "union type identity depended on member order");

    // A one-member union is that member.
    require(arena.unionType({arena.intType()}) == arena.intType(), "single-member union was not collapsed");

    require(arena.render(arena.mapType(arena.stringType(), arena.intType())) == "map<string,int>",
            "map rendering was wrong: " + arena.render(arena.mapType(arena.stringType(), arena.intType())));
    require(arena.render(arena.objectType("Box", {arena.intType()})) == "Box<int>",
            "generic object rendering was wrong");
    require(arena.render(arena.arrayType(arena.intType(), 10)) == "array[10]<int>",
            "fixed array rendering was wrong");

    FunctionSignature signature;
    signature.parameterTypes = {arena.intType(), arena.stringType()};
    signature.returnType = arena.boolType();
    require(arena.render(arena.functionType(signature)) == "func(int,string):bool",
            "function type rendering was wrong: " + arena.render(arena.functionType(signature)));

    require(arena.find(0) == nullptr, "type id 0 must not resolve");
    require(arena.find(9999) == nullptr, "out-of-range type id resolved");
    std::cout << "mir type arena: PASS\n";
}

// ---------------------------------------------------------------------------
// A small well-formed module reused by several tests
// ---------------------------------------------------------------------------

// Builds:  func Calc.sum(int,int): int { return a + b }
//          func Calc.main(): void { log(sum(1,2)) }
Module buildValidModule() {
    ModuleBuilder builder("test");
    TypeArena& types = builder.types();

    {
        FunctionBuilder fb = builder.addFunction("Calc.sum(int,int)");
        fb.setReturnType(types.intType());
        const ParamId a = fb.addParameter("a", types.intType());
        const ParamId b = fb.addParameter("b", types.intType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        const TempId sum = fb.emitBinary(Opcode::Add, fb.parameterOperand(a), fb.parameterOperand(b),
                                         types.intType());
        fb.emitReturn(Operand::temp(sum, types.intType()));
        fb.finish();
    }

    FunctionId mainId = kNoFunction;
    {
        FunctionBuilder fb = builder.addFunction("Calc.main()");
        fb.setReturnType(types.voidType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        const FunctionId sum = builder.findFunction("Calc.sum(int,int)");
        const Operand one = Operand::constant(builder.constantInt(1), types.intType());
        const Operand two = Operand::constant(builder.constantInt(2), types.intType());
        const TempId result = fb.emitCall(sum, {one, two}, types.intType());
        fb.emitLog(Operand::temp(result, types.intType()));
        fb.emitReturn();
        fb.finish();
        mainId = fb.function().id;
    }
    builder.setEntryPoint(mainId);
    return builder.take();
}

void testValidModuleVerifies() {
    Module module = buildValidModule();
    const auto report = verifyModule(module);
    require(report.ok(), "a well-formed module was rejected:\n" + reportText(report));

    // The entry point and function identity must survive the round trip.
    require(module.entryPoint != kNoFunction, "entry point was lost");
    require(module.functions.size() == 2, "expected two lowered functions");

    // Edges were materialised by finish(); a single-block function has none.
    require(module.functions[0].edges.empty(), "a straight-line function reported control-flow edges");

    const std::string text = printModule(module);
    require(text.find("func Calc.sum(int,int)") != std::string::npos, "printer dropped the function header");
    require(text.find("add") != std::string::npos, "printer dropped the add instruction");
    std::cout << "mir valid module: PASS\n";
}

// ---------------------------------------------------------------------------
// Control-flow graph analysis
// ---------------------------------------------------------------------------

// Builds a diamond with a loop back-edge so dominance is non-trivial:
//   b1 -> b2/b3 ; b2 -> b4 ; b3 -> b4 ; b4 -> b2 (loop) and b4 -> b5
Module buildCfgModule() {
    ModuleBuilder builder("cfg");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Cfg.walk(int)");
    fb.setReturnType(types.voidType());
    const ParamId n = fb.addParameter("n", types.intType());

    const BlockId b1 = fb.addBlock();
    const BlockId b2 = fb.addBlock();
    const BlockId b3 = fb.addBlock();
    const BlockId b4 = fb.addBlock();
    const BlockId b5 = fb.addBlock();

    fb.setCurrentBlock(b1);
    const Operand zero = Operand::constant(builder.constantInt(0), types.intType());
    const Operand limit = Operand::constant(builder.constantInt(10), types.intType());
    const TempId counter = fb.emitBinary(Opcode::Add, fb.parameterOperand(n), zero, types.intType());
    const SlotId slot = fb.addSlot("i", types.intType());
    fb.emitStore(slot, Operand::temp(counter, types.intType()));
    fb.emitBranch(Operand::constant(builder.constantBool(true), types.boolType()), b2, b3);

    fb.setCurrentBlock(b2);
    // A value defined only on this arm, so a use of it from b3 is a genuine
    // dominance violation rather than a legal one.
    const TempId armOnly = fb.emitBinary(Opcode::Add, fb.parameterOperand(n), zero, types.intType());
    const SlotId armSlot = fb.addSlot("arm", types.intType());
    fb.emitStore(armSlot, Operand::temp(armOnly, types.intType()));
    fb.emitJump(b4);

    fb.setCurrentBlock(b3);
    fb.emitJump(b4);

    fb.setCurrentBlock(b4);
    const Operand current = Operand::temp(fb.emitLoad(slot), types.intType());
    const TempId keepGoing = fb.emitBinary(Opcode::Lt, current, limit, types.boolType());
    fb.emitBranch(Operand::temp(keepGoing, types.boolType()), b2, b5);

    fb.setCurrentBlock(b5);
    fb.emitReturn();
    fb.finish();
    return builder.take();
}

void testControlFlowAnalysis() {
    Module module = buildCfgModule();
    const auto report = verifyModule(module);
    require(report.ok(), "the CFG fixture was rejected:\n" + reportText(report));

    const Function& function = module.functions.front();
    ControlFlowGraph cfg(function);
    require(cfg.blockCount() == 5, "expected five blocks");
    require(cfg.successors(1).size() == 2, "the entry block did not have two successors");
    require(cfg.predecessors(4).size() == 2, "the join block did not have two predecessors");
    require(cfg.isReachable(5), "the exit block was reported unreachable");

    // Reverse post-order starts at the entry and visits every reachable block.
    require(cfg.reversePostOrder().size() == 5, "reverse post-order did not cover the function");
    require(cfg.reversePostOrder().front() == 1, "reverse post-order did not start at the entry");

    // b1 dominates everything; b2 does not dominate b3 (they are siblings).
    require(cfg.dominates(1, 4), "the entry block did not dominate the join");
    require(cfg.dominates(1, 5), "the entry block did not dominate the exit");
    require(!cfg.dominates(2, 3), "a branch arm was reported to dominate its sibling");
    require(!cfg.dominates(4, 1), "a loop body was reported to dominate the entry");
    require(cfg.dominates(4, 4), "a block must dominate itself");

    // The materialised edge list must match the terminators.
    require(function.edges.size() == 6, "unexpected edge count");
    std::cout << "mir control-flow analysis: PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: malformed control flow
// ---------------------------------------------------------------------------

void testRejectsMissingBlockReference() {
    Module module = buildValidModule();
    // Point the call's block at a block that does not exist by replacing the
    // terminator with a jump to a nonexistent id.
    module.functions[0].blocks[0].terminator = Terminator{};
    module.functions[0].blocks[0].terminator.kind = TerminatorKind::Jump;
    module.functions[0].blocks[0].terminator.target = 99;
    const auto report = verifyModule(module);
    require(!report.ok(), "a jump to a nonexistent block was accepted");
    require(hasError(report, "does not exist"), "the missing-block error was not reported:\n" + reportText(report));
    std::cout << "mir verifier missing block: PASS\n";
}

void testRejectsMissingTerminator() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].terminator = Terminator{};
    const auto report = verifyModule(module);
    require(!report.ok(), "a block with no terminator was accepted");
    require(hasError(report, "no terminator"), "the missing-terminator error was not reported:\n" +
                                                    reportText(report));
    std::cout << "mir verifier missing terminator: PASS\n";
}

void testRejectsUnreachableBlock() {
    Module module = buildCfgModule();
    // Detach b3 so nothing reaches it.
    module.functions[0].blocks[0].terminator.elseBlock = 2;
    module.functions[0].rebuildEdges();
    const auto report = verifyModule(module);
    require(!report.ok(), "an unreachable block was accepted");
    require(hasError(report, "unreachable"), "the unreachable-block error was not reported:\n" +
                                                 reportText(report));

    // The same module is only a warning when the option is relaxed.
    VerifierOptions options;
    options.unreachableBlocksAreErrors = false;
    const auto relaxed = verifyModule(module, options);
    require(relaxed.ok(), "an unreachable block was still an error with the option relaxed:\n" +
                              reportText(relaxed));
    require(relaxed.warningCount() > 0, "the relaxed run recorded no warning");
    std::cout << "mir verifier unreachable block: PASS\n";
}

void testRejectsInconsistentPredecessors() {
    Module module = buildCfgModule();
    // Drop an edge without changing any terminator.
    module.functions[0].edges.pop_back();
    const auto report = verifyModule(module);
    require(!report.ok(), "a stale control-flow edge list was accepted");
    require(hasError(report, "edge list does not match"),
            "the edge-consistency error was not reported:\n" + reportText(report));
    std::cout << "mir verifier edge consistency: PASS\n";
}

void testRejectsDominanceViolation() {
    Module module = buildCfgModule();
    Function& function = module.functions[0];
    // b2 and b3 are the two arms of the entry branch: neither dominates the
    // other. A temp defined in b2 and used in b3 is therefore invalid.
    TempId armOnly = kNoTemp;
    TypeId armType = 0;
    for (const auto& instruction : function.blocks[1].instructions) {
        if (instruction.opcode == Opcode::Add) { armOnly = instruction.result; armType = instruction.resultType; }
    }
    require(armOnly != kNoTemp, "the fixture did not define an arm-local value");

    Instruction bad;
    bad.opcode = Opcode::Log;
    bad.operands = {Operand::temp(armOnly, armType)};
    function.blocks[2].instructions.insert(function.blocks[2].instructions.begin(), bad);
    function.rebuildEdges();

    const auto report = verifyModule(module);
    require(!report.ok(), "a use outside the definition's dominator tree was accepted");
    require(hasError(report, "does not dominate"),
            "the dominance error was not reported:\n" + reportText(report));

    // Sanity check on the same fixture: a value defined in b1 IS available in
    // b3, because the entry dominates every reachable block.
    Module okModule = buildCfgModule();
    TempId entryValue = kNoTemp;
    TypeId entryType = 0;
    for (const auto& instruction : okModule.functions[0].blocks[0].instructions) {
        if (instruction.opcode == Opcode::Add) { entryValue = instruction.result; entryType = instruction.resultType; }
    }
    Instruction legal;
    legal.opcode = Opcode::Log;
    legal.operands = {Operand::temp(entryValue, entryType)};
    okModule.functions[0].blocks[2].instructions.insert(okModule.functions[0].blocks[2].instructions.begin(), legal);
    okModule.functions[0].rebuildEdges();
    const auto okReport = verifyModule(okModule);
    require(okReport.ok(), "a dominating use was rejected:\n" + reportText(okReport));
    std::cout << "mir verifier dominance: PASS\n";
}

void testRejectsForwardUse() {
    Module module = buildValidModule();
    Function& function = module.functions[0];
    // Move the add after the return by appending a second use before it.
    Instruction& add = function.blocks[0].instructions[0];
    const TempId result = add.result;
    const TypeId type = add.resultType;
    function.blocks[0].instructions.insert(function.blocks[0].instructions.begin(), add);
    // Now the first copy defines it and the original (second) redefines it.
    const auto report = verifyModule(module);
    require(!report.ok(), "a duplicated temp definition was accepted");
    require(hasError(report, "defined more than once"),
            "the duplicate-definition error was not reported:\n" + reportText(report));
    (void)result;
    (void)type;
    std::cout << "mir verifier ssa uniqueness: PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: operands and types
// ---------------------------------------------------------------------------

void testRejectsUndefinedTemp() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].instructions[0].operands[0] = Operand::temp(77, module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a reference to an undefined temp was accepted");
    require(hasError(report, "never defined"), "the undefined-temp error was not reported:\n" +
                                                   reportText(report));
    std::cout << "mir verifier undefined temp: PASS\n";
}

void testRejectsWrongOperandType() {
    Module module = buildValidModule();
    // `int - string` is not a ZL operation. (`int + string` is: concatenation.)
    const TypeId stringType = module.types.stringType();
    const ConstId text = module.internConstant(stringConstant("x"));
    module.functions[0].blocks[0].instructions[0].opcode = Opcode::Sub;
    module.functions[0].blocks[0].instructions[0].operands[1] = Operand::constant(text, stringType);
    const auto report = verifyModule(module);
    require(!report.ok(), "int - string was accepted");
    require(hasError(report, "sub on int and string"),
            "the operator typing error was not reported:\n" + reportText(report));
    std::cout << "mir verifier operand types: PASS\n";
}

void testRejectsWrongResultType() {
    Module module = buildValidModule();
    // Claim the add produces a bool.
    module.functions[0].blocks[0].instructions[0].resultType = module.types.boolType();
    const auto report = verifyModule(module);
    require(!report.ok(), "an add with a bool result was accepted");
    require(hasError(report, "must produce int"), "the result-type error was not reported:\n" +
                                                      reportText(report));
    std::cout << "mir verifier result type: PASS\n";
}

void testRejectsBadConstant() {
    Module module = buildValidModule();
    // An int operand pointing at a string constant.
    const ConstId text = module.internConstant(stringConstant("x"));
    module.functions[0].blocks[0].instructions[0].operands[0] =
        Operand::constant(text, module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a string constant typed as int was accepted");
    require(hasError(report, "constant"), "the constant-kind error was not reported:\n" + reportText(report));
    std::cout << "mir verifier constant kind: PASS\n";
}

void testRejectsBadParameterReference() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].instructions[0].operands[0] =
        Operand::param(9, module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "an out-of-range parameter reference was accepted");
    require(hasError(report, "unknown parameter"),
            "the parameter error was not reported:\n" + reportText(report));
    std::cout << "mir verifier parameter reference: PASS\n";
}

void testRejectsWrongOperandCount() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].instructions[0].operands.pop_back();
    const auto report = verifyModule(module);
    require(!report.ok(), "a binary operator with one operand was accepted");
    require(hasError(report, "requires 2 operand"),
            "the operand-count error was not reported:\n" + reportText(report));
    std::cout << "mir verifier operand count: PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: terminators, returns, signatures
// ---------------------------------------------------------------------------

void testRejectsInvalidReturn() {
    Module module = buildValidModule();
    // A void function that returns a value.
    Function& main = module.functions[1];
    main.blocks[0].terminator.value = Operand::constant(module.internConstant(
        intConstant(7)), module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a value returned from a void function was accepted");
    require(hasError(report, "returns a value from a void function"),
            "the void-return error was not reported:\n" + reportText(report));
    std::cout << "mir verifier void return: PASS\n";
}

void testRejectsMissingReturnValue() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].terminator.value = Operand::none();
    const auto report = verifyModule(module);
    require(!report.ok(), "a bare return from an int function was accepted");
    require(hasError(report, "returns no value"), "the missing-return-value error was not reported:\n" +
                                                      reportText(report));
    std::cout << "mir verifier missing return value: PASS\n";
}

void testRejectsReturnTypeMismatch() {
    Module module = buildValidModule();
    // Return a bool where the signature says int.
    module.functions[0].blocks[0].terminator.value = Operand::constant(
        module.internConstant(boolConstant(true)), module.types.boolType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a bool returned from an int function was accepted");
    require(hasError(report, "but the function returns int"),
            "the return-type error was not reported:\n" + reportText(report));
    std::cout << "mir verifier return type mismatch: PASS\n";
}

void testRejectsMalformedSignature() {
    Module module = buildValidModule();
    // A void parameter is not a thing.
    module.functions[0].parameters[0].type = module.types.voidType();
    const auto report = verifyModule(module);
    require(!report.ok(), "a void parameter was accepted");
    require(hasError(report, "cannot have type void"),
            "the malformed-signature error was not reported:\n" + reportText(report));
    std::cout << "mir verifier malformed signature: PASS\n";
}

void testRejectsNonBoolBranch() {
    Module module = buildCfgModule();
    module.functions[0].blocks[0].terminator.value =
        Operand::constant(module.internConstant(intConstant(1)),
                          module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a branch on an int was accepted");
    require(hasError(report, "must be bool"), "the branch-condition error was not reported:\n" +
                                                  reportText(report));
    std::cout << "mir verifier branch condition: PASS\n";
}

void testRejectsEntryWithPredecessor() {
    Module module = buildCfgModule();
    // Make the exit block jump back to the entry.
    module.functions[0].blocks[4].terminator = Terminator{};
    module.functions[0].blocks[4].terminator.kind = TerminatorKind::Jump;
    module.functions[0].blocks[4].terminator.target = 1;
    module.functions[0].rebuildEdges();
    const auto report = verifyModule(module);
    require(!report.ok(), "an entry block with a predecessor was accepted");
    require(hasError(report, "entry block"), "the entry-predecessor error was not reported:\n" +
                                                 reportText(report));
    std::cout << "mir verifier entry predecessor: PASS\n";
}

void testRejectsCallArityMismatch() {
    Module module = buildValidModule();
    // Call the two-parameter sum with one argument.
    Instruction& call = module.functions[1].blocks[0].instructions[0];
    call.operands.pop_back();
    call.target.argumentTypes.pop_back();
    const auto report = verifyModule(module);
    require(!report.ok(), "a call with the wrong arity was accepted");
    require(hasError(report, "but it takes 2"), "the call-arity error was not reported:\n" +
                                                    reportText(report));
    std::cout << "mir verifier call arity: PASS\n";
}

void testRejectsCallArgumentType() {
    Module module = buildValidModule();
    Instruction& call = module.functions[1].blocks[0].instructions[0];
    const ConstId text = module.internConstant(stringConstant("x"));
    call.operands[1] = Operand::constant(text, module.types.stringType());
    call.target.argumentTypes[1] = module.types.stringType();
    const auto report = verifyModule(module);
    require(!report.ok(), "a string passed to an int parameter was accepted");
    require(hasError(report, "but the parameter is int"),
            "the call-argument-type error was not reported:\n" + reportText(report));
    std::cout << "mir verifier call argument type: PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: impossible instruction combinations and ownership
// ---------------------------------------------------------------------------

void testRejectsAwaitOutsideAsync() {
    Module module = buildValidModule();
    Function& function = module.functions[0];
    Instruction await;
    await.opcode = Opcode::Await;
    await.result = 50;
    await.resultType = module.types.intType();
    await.operands = {Operand::constant(module.internConstant(nilConstant()),
                                        module.types.taskType(module.types.intType()))};
    function.blocks[0].instructions.insert(function.blocks[0].instructions.begin(), await);
    const auto report = verifyModule(module);
    require(!report.ok(), "an await in a non-async function was accepted");
    require(hasError(report, "await in a non-async function"),
            "the await error was not reported:\n" + reportText(report));
    std::cout << "mir verifier await outside async: PASS\n";
}

void testRejectsStoreToImmutableSlot() {
    ModuleBuilder builder("immutable");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Immut.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId slot = fb.addSlot("constant", types.intType(), /*isMutable=*/false);
    fb.emitStore(slot, Operand::constant(builder.constantInt(1), types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a store into an immutable slot was accepted");
    require(hasError(report, "immutable"), "the immutability error was not reported:\n" + reportText(report));
    std::cout << "mir verifier immutable slot: PASS\n";
}

void testRejectsMoveOfNonOwnedSlot() {
    ModuleBuilder builder("move");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Move.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // A GC slot cannot be moved; only an owned one can.
    const SlotId slot = fb.addSlot("value", types.listType(types.intType()), true, zl::OwnershipKind::GC);
    (void)fb.emitMove(slot);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a move from a GC slot was accepted");
    require(hasError(report, "only an owned slot can be moved"),
            "the move-ownership error was not reported:\n" + reportText(report));
    std::cout << "mir verifier move of non-owned: PASS\n";
}

void testRejectsUseAfterMove() {
    ModuleBuilder builder("aftermove");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("AfterMove.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId slot = fb.addSlot("buffer", types.listType(types.intType()), true, zl::OwnershipKind::OWNED);
    fb.emitStore(slot, Operand::temp(fb.emitNewCollection(types.listType(types.intType())),
                                     types.listType(types.intType())));
    const TempId moved = fb.emitMove(slot);
    fb.emitDrop(Operand::temp(moved, types.listType(types.intType())));
    // Reading the slot after the move is the error.
    (void)fb.emitLoad(slot);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a read after a move was accepted");
    require(hasError(report, "after it was moved"),
            "the use-after-move error was not reported:\n" + reportText(report));
    std::cout << "mir verifier use after move: PASS\n";
}

void testRejectsMoveWhileBorrowed() {
    ModuleBuilder builder("borrowed");
    TypeArena& types = builder.types();
    const TypeId listType = types.listType(types.intType());
    FunctionBuilder fb = builder.addFunction("Borrowed.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId owner = fb.addSlot("owner", listType, true, zl::OwnershipKind::OWNED);
    const SlotId borrow = fb.addSlot("view", listType, true, zl::OwnershipKind::BORROW);
    fb.emitStore(owner, Operand::temp(fb.emitNewCollection(listType), listType));
    const TempId ownerValue = fb.emitLoad(owner);
    fb.emitBorrow(borrow, Operand::temp(ownerValue, listType));
    // Moving the owner while the borrow is live is the error.
    (void)fb.emitMove(owner);
    fb.emitEndBorrow(borrow);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a move of a borrowed local was accepted");
    require(hasError(report, "while it is borrowed"),
            "the borrow-conflict error was not reported:\n" + reportText(report));
    std::cout << "mir verifier move while borrowed: PASS\n";
}

void testRejectsDropOfBorrow() {
    ModuleBuilder builder("dropborrow");
    TypeArena& types = builder.types();
    const TypeId listType = types.listType(types.intType());
    FunctionBuilder fb = builder.addFunction("DropBorrow.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId owner = fb.addSlot("owner", listType, true, zl::OwnershipKind::OWNED);
    const SlotId borrow = fb.addSlot("view", listType, true, zl::OwnershipKind::BORROW);
    fb.emitStore(owner, Operand::temp(fb.emitNewCollection(listType), listType));
    const TempId ownerValue = fb.emitLoad(owner);
    fb.emitBorrow(borrow, Operand::temp(ownerValue, listType));
    const TempId borrowed = fb.emitLoad(borrow);
    // Dropping a borrowed value instead of ending the borrow is the error.
    fb.emitDrop(Operand::temp(borrowed, listType));
    fb.emitEndBorrow(borrow);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "dropping a borrowed value was accepted");
    require(hasError(report, "end the borrow instead"),
            "the drop-of-borrow error was not reported:\n" + reportText(report));
    std::cout << "mir verifier drop of borrow: PASS\n";
}

void testRejectsEndBorrowWithoutBorrow() {
    ModuleBuilder builder("endborrow");
    TypeArena& types = builder.types();
    const TypeId listType = types.listType(types.intType());
    FunctionBuilder fb = builder.addFunction("EndBorrow.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId borrow = fb.addSlot("view", listType, true, zl::OwnershipKind::BORROW);
    fb.emitEndBorrow(borrow);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "an end_borrow with no active borrow was accepted");
    require(hasError(report, "does not hold an active borrow"),
            "the end_borrow error was not reported:\n" + reportText(report));
    std::cout << "mir verifier end borrow: PASS\n";
}

void testRejectsNullCheckOnNonNullable() {
    Module module = buildValidModule();
    Function& function = module.functions[0];
    Instruction check;
    check.opcode = Opcode::NullCheck;
    check.result = 60;
    check.resultType = module.types.intType();
    check.operands = {Operand::constant(module.internConstant(intConstant(1)),
                                        module.types.intType())};
    function.blocks[0].instructions.insert(function.blocks[0].instructions.begin(), check);
    const auto report = verifyModule(module);
    require(!report.ok(), "a null check on an int was accepted");
    require(hasError(report, "cannot be nil"), "the null-check error was not reported:\n" + reportText(report));
    std::cout << "mir verifier null check: PASS\n";
}

void testRejectsUndeclaredTypeParamUse() {
    ModuleBuilder builder("typeparam");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Generic.id(T)");
    // Not marked as a generic template, so T is out of scope.
    const TypeId param = types.typeParam("T");
    fb.setReturnType(param);
    const ParamId value = fb.addParameter("value", param);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitReturn(fb.parameterOperand(value));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a generic parameter outside a template was accepted");
    require(hasError(report, "not a generic template"),
            "the type-parameter scope error was not reported:\n" + reportText(report));
    std::cout << "mir verifier type parameter scope: PASS\n";
}

void testAcceptsDeclaredTypeParam() {
    ModuleBuilder builder("typeparam");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Generic.id(T)");
    fb.setGenericTemplate({"T"});
    const TypeId param = types.typeParam("T");
    fb.setReturnType(param);
    const ParamId value = fb.addParameter("value", param);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitReturn(fb.parameterOperand(value));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "a declared generic parameter was rejected:\n" + reportText(report));
    std::cout << "mir verifier type parameter declared: PASS\n";
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

void testExceptionEdges() {
    ModuleBuilder builder("exceptions");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Throws.run()");
    fb.setReturnType(types.voidType());

    const BlockId entry = fb.addBlock();
    const BlockId catchBlock = fb.addBlock(BlockKind::Catch);
    const BlockId after = fb.addBlock();
    const SlotId binding = fb.addSlot("e", types.stringType(), false);
    fb.function().slots[0].isCatchBinding = true;

    fb.setCurrentBlock(entry);
    fb.pushExceptionHandler(0, catchBlock, binding);
    fb.emitJump(after);

    fb.setCurrentBlock(catchBlock);
    fb.emitLog(Operand::temp(fb.emitLoad(binding), types.stringType()));
    fb.emitJump(after);

    fb.setCurrentBlock(after);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "a valid try/catch shape was rejected:\n" + reportText(report));

    const Function& function = module.functions.front();
    ControlFlowGraph cfg(function);
    require(cfg.predecessors(catchBlock).empty(), "a catch block had a normal predecessor");
    require(cfg.unwindPredecessors(catchBlock).size() == 1, "the catch block had no unwind predecessor");
    require(cfg.reachableWithUnwind().count(catchBlock) != 0, "the catch block was not unwind-reachable");

    // A catch block that is also reachable by falling through is invalid.
    Module broken = module;
    broken.functions[0].blocks[0].terminator.kind = TerminatorKind::Jump;
    broken.functions[0].blocks[0].terminator.target = catchBlock;
    broken.functions[0].rebuildEdges();
    const auto brokenReport = verifyModule(broken);
    require(!brokenReport.ok(), "a catch block reachable by normal flow was accepted");
    require(hasError(brokenReport, "normal control flow"),
            "the catch-reachability error was not reported:\n" + reportText(brokenReport));
    std::cout << "mir verifier exception edges: PASS\n";
}

// ---------------------------------------------------------------------------
// Regressions from lowering real ZL programs
//
// Each of these is a shape the example corpus produced and an earlier verifier
// or type layer got wrong. They are kept as MIR-level tests so the fix stays
// pinned even when the lowerer changes.
// ---------------------------------------------------------------------------

void testInheritedFieldAccess() {
    ModuleBuilder builder("inherit");
    TypeArena& types = builder.types();

    ClassLayout& animal = builder.addClassLayout("Animal");
    FieldLayout name;
    name.name = "name";
    name.type = types.stringType();
    animal.fields.push_back(name);

    // `Dog extends Animal` declares no fields of its own.
    ClassLayout& dog = builder.addClassLayout("Dog");
    dog.parent = "Animal";

    FunctionBuilder fb = builder.addFunction("Dog.fetch()");
    fb.setReturnType(types.stringType());
    const ParamId self = fb.addParameter("this", types.objectType("Dog"));
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // `this.name` - declared on the parent, not on Dog.
    const TempId loaded = fb.emitFieldLoad(fb.parameterOperand(self), "name", types.stringType());
    fb.emitReturn(Operand::temp(loaded, types.stringType()));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "reading an inherited field was rejected:\n" + reportText(report));

    // A field nothing in the chain declares is still an error.
    module.functions[0].blocks[0].instructions[0].name = "collar";
    const auto missing = verifyModule(module);
    require(!missing.ok(), "a field no class declares was accepted");
    require(hasError(missing, "do not declare"),
            "the inherited-field error was not reported:\n" + reportText(missing));
    std::cout << "mir verifier inherited fields: PASS\n";
}

void testListObjectIndexing() {
    ModuleBuilder builder("index");
    TypeArena& types = builder.types();

    // `List<T>` is a real class, so an indexable base arrives as an Object
    // named "List" rather than the `list` keyword kind.
    const TypeId listOfInt = types.objectType("List", {types.intType()});

    FunctionBuilder fb = builder.addFunction("Indexer.first(List<int>)");
    fb.setReturnType(types.intType());
    const ParamId xs = fb.addParameter("xs", listOfInt);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const Operand zero = Operand::constant(builder.constantInt(0), types.intType());
    const TempId element = fb.emitIndexLoad(fb.parameterOperand(xs), zero, types.intType());
    fb.emitReturn(Operand::temp(element, types.intType()));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "indexing a List<int> was rejected:\n" + reportText(report));

    // Only a List is indexable in ZL - `Box<int>[0]` is not a thing. New types
    // come from the module's arena: `builder.take()` moved the builder's one out.
    const TypeId boxOfInt = module.types.objectType("Box", {module.types.intType()});
    module.functions[0].parameters[0].type = boxOfInt;
    module.functions[0].blocks[0].instructions[0].operands[0] = Operand::param(0, boxOfInt);
    const auto notAList = verifyModule(module);
    require(!notAList.ok(), "indexing a non-List object was accepted");
    require(hasError(notAList, "requires a List"),
            "the indexability error was not reported:\n" + reportText(notAList));
    std::cout << "mir verifier list indexing: PASS\n";
}

void testEnumMemberConstant() {
    ModuleBuilder builder("enums");
    TypeArena& types = builder.types();
    const TypeId color = types.objectType("Color");

    ClassLayout& enumeration = builder.addClassLayout("Color");
    enumeration.isEnum = true;
    enumeration.enumMembers = {"RED", "GREEN"};

    FunctionBuilder fb = builder.addFunction("Enums.isRed(Color)");
    fb.setReturnType(types.boolType());
    const ParamId c = fb.addParameter("c", color);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // `Color.RED`: a string at runtime, but statically a Color.
    const ConstId redConst = builder.constantEnumMember("Color", "RED");
    const Operand red = Operand::constant(redConst, color);
    const TempId same = fb.emitBinary(Opcode::Eq, fb.parameterOperand(c), red, types.boolType());
    fb.emitReturn(Operand::temp(same, types.boolType()));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "comparing an enum member to its own enum was rejected:\n" + reportText(report));

    // Typing the same constant as string is what used to happen, and it must not
    // come back: the comparison would then be Color against string.
    module.functions[0].blocks[0].instructions[0].operands[1] = Operand::constant(redConst, types.stringType());
    const auto asString = verifyModule(module);
    require(!asString.ok(), "an enum member typed as string was accepted");
    std::cout << "mir verifier enum members: PASS\n";
}

void testGenericCallInstantiation() {
    ModuleBuilder builder("generic");
    TypeArena& types = builder.types();
    const TypeId payload = types.typeParam("T");
    const TypeId sharedT = types.sharedType(payload);

    // A generic template: `Shared<T>.set(T)`. MIR never duplicates it per
    // instantiation, so the call site records which T it selected.
    FunctionId setter = kNoFunction;
    {
        FunctionBuilder fb = builder.addFunction("Shared.set(T)");
        fb.function().typeParameters = {"T"};
        fb.setReturnType(types.voidType());
        const ParamId self = fb.addParameter("this", sharedT);
        (void)fb.addParameter("value", payload);
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        fb.emitFieldStore(fb.parameterOperand(self), "__value", Operand::param(1, payload));
        fb.emitReturn();
        fb.finish();
        setter = fb.function().id;
    }

    const TypeId sharedInt = types.sharedType(types.intType());
    FunctionBuilder fb = builder.addFunction("Use.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId instance = fb.emitAlloc("Shared", {types.intType()}, sharedInt);
    const Operand value = Operand::constant(builder.constantInt(1), types.intType());
    const TempId callTemp = fb.emitCall(setter, {Operand::temp(instance, sharedInt), value}, 0, {},
                                        {types.intType()});
    (void)callTemp;
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(),
            "a call into a generic template with its instantiation recorded was rejected:\n" +
                reportText(report));

    // What the recorded instantiation buys is precision. With T bound to int, a
    // string argument is a real mismatch and must be caught.
    Module wrong = module;
    const ConstId text = wrong.internConstant(stringConstant("x"));
    wrong.functions[1].blocks[0].instructions[1].operands[1] =
        Operand::constant(text, wrong.types.stringType());
    wrong.functions[1].blocks[0].instructions[1].target.argumentTypes = {sharedInt,
                                                                         wrong.types.stringType()};
    const auto mismatch = verifyModule(wrong);
    require(!mismatch.ok(), "a string passed for T:=int was accepted");
    require(hasError(mismatch, "but the parameter is int"),
            "the substituted-parameter error was not reported:\n" + reportText(mismatch));

    // Without the instantiation the parameter stays Shared<T>/T, and an
    // unsubstituted generic parameter is deliberately compatible with anything -
    // the verifier cannot know what T will be. So the same call is accepted, and
    // only less precisely checked.
    Module uninstantiated = module;
    const ConstId uninstantiatedText = uninstantiated.internConstant(stringConstant("x"));
    uninstantiated.functions[1].blocks[0].instructions[1].typeArguments.clear();
    uninstantiated.functions[1].blocks[0].instructions[1].operands[1] =
        Operand::constant(uninstantiatedText, uninstantiated.types.stringType());
    uninstantiated.functions[1].blocks[0].instructions[1].target.argumentTypes = {
        sharedInt, uninstantiated.types.stringType()};
    const auto permissive = verifyModule(uninstantiated);
    require(permissive.ok(),
            "an uninstantiated generic call was rejected, but T is unknowable there:\n" +
                reportText(permissive));
    std::cout << "mir verifier generic instantiation: PASS\n";
}

void testSharedIsReferenceType() {
    ModuleBuilder builder("shared");
    TypeArena& types = builder.types();
    const TypeId sharedInt = types.sharedType(types.intType());

    ClassLayout& layout = builder.addClassLayout("Shared");
    layout.typeParameters = {"T"};
    FieldLayout value;
    value.name = "__value";
    value.type = types.typeParam("T");
    layout.fields.push_back(value);

    FunctionBuilder fb = builder.addFunction("Shared.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // Construction of a Shared<T> allocates a reference, not a plain object.
    const TempId instance = fb.emitAlloc("Shared", {types.intType()}, sharedInt);
    const Operand receiver = Operand::temp(instance, sharedInt);
    fb.emitFieldStore(receiver, "__value", Operand::constant(builder.constantInt(0), types.intType()));
    // Shared<T> has methods of its own, so it is a legal dispatch receiver.
    (void)fb.emitInvokeMethod(receiver, "Shared", "withLock", {}, 0);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "Shared<T> as alloc result, field base, and receiver was rejected:\n" +
                             reportText(report));
    std::cout << "mir verifier shared reference type: PASS\n";
}

void testAwaitOfVoidTaskDefinesNoTemp() {
    ModuleBuilder builder("awaitvoid");
    TypeArena& types = builder.types();

    FunctionBuilder fb = builder.addFunction("Tasks.run()");
    fb.setAsync(true);
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // `TaskCreate` from nil gives a Task<void>; awaiting it yields nothing, so
    // there is no temp - even though `await` is a value-producing opcode.
    const TypeId voidTask = types.taskType(types.voidType());
    const TempId task = fb.emitTaskCreate(Operand::constant(builder.constantNil(), types.nilType()), voidTask);
    (void)fb.emitAwait(Operand::temp(task, voidTask), 0);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    require(opcodeShape(Opcode::Await).optionalResult,
            "await is not marked as having an optional result");
    const auto report = verifyModule(module);
    require(report.ok(), "awaiting a Task<void> was rejected:\n" + reportText(report));

    // Awaiting a Task<int> still has to define a temp.
    const TypeId intTask = types.taskType(types.intType());
    module.functions[0].blocks[0].instructions[0].resultType = intTask;
    module.functions[0].blocks[0].instructions[1].operands[0] = Operand::temp(1, intTask);
    const auto noTemp = verifyModule(module);
    require(!noTemp.ok(), "awaiting a Task<int> with no temp was accepted");
    std::cout << "mir verifier await result: PASS\n";
}

void testMapIndexAccess() {
    ModuleBuilder builder("mapindex");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Maps.put()");
    const TypeId mapType = types.mapType(types.stringType(), types.intType());
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId map = fb.emitNewCollection(mapType);
    const Operand collection = Operand::temp(map, mapType);
    fb.emitIndexStore(collection,
                      Operand::constant(builder.constantString("a"), types.stringType()),
                      Operand::constant(builder.constantInt(1), types.intType()));
    const TempId read = fb.emitIndexLoad(collection,
                                         Operand::constant(builder.constantString("a"), types.stringType()),
                                         types.intType());
    (void)read;
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "indexing a map by its key was rejected:\n" + reportText(report));

    // A map is keyed, not positioned, so an int where the key type is string is
    // a genuine mismatch - the same rule a list applies to its index.
    Module wrongKey = module;
    wrongKey.functions[0].blocks[0].instructions[1].operands[1] =
        Operand::constant(wrongKey.internConstant(intConstant(0)), wrongKey.types.intType());
    const auto badKey = verifyModule(wrongKey);
    require(!badKey.ok(), "indexing a map<string,int> with an int key was accepted");
    require(hasError(badKey, "the key must be string"),
            "the key-type error was not reported:\n" + reportText(badKey));

    // And the value read back has to be the map's value type.
    Module wrongValue = module;
    wrongValue.functions[0].blocks[0].instructions[2].resultType = wrongValue.types.stringType();
    const auto badValue = verifyModule(wrongValue);
    require(!badValue.ok(), "reading a string out of a map<string,int> was accepted");
    std::cout << "mir verifier map indexing: PASS\n";
}

void testSetIndexing() {
    // A set literal is built by storing each element in turn, so `set<T>` has to
    // accept positional index access even though the language has no positional
    // read for one. This check once covered List/Array/Map and fell through to
    // the "requires a List<T>" error for a set, so `set<int> s = [1, 2, 3]` -
    // which the VM runs fine - produced MIR that failed to verify.
    ModuleBuilder builder("setindex");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Sets.build()");
    const TypeId setOfInt = types.setType(types.intType());
    fb.setReturnType(setOfInt);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId set = fb.emitNewCollection(setOfInt);
    const Operand collection = Operand::temp(set, setOfInt);
    fb.emitIndexStore(collection,
                      Operand::constant(builder.constantInt(0), types.intType()),
                      Operand::constant(builder.constantInt(7), types.intType()));
    fb.emitReturn(collection);
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "building a set<int> by position was rejected:\n" + reportText(report));

    // Positional, not keyed: a string where an index belongs is a mismatch, the
    // same rule a list applies.
    Module wrongIndex = module;
    wrongIndex.functions[0].blocks[0].instructions[1].operands[1] = Operand::constant(
        wrongIndex.internConstant(stringConstant("a")), wrongIndex.types.stringType());
    const auto badIndex = verifyModule(wrongIndex);
    require(!badIndex.ok(), "storing into a set<int> with a string index was accepted");
    require(hasError(badIndex, "the index must be int"),
            "the index-type error was not reported:\n" + reportText(badIndex));

    // And the element written has to be the set's element type.
    Module wrongValue = module;
    wrongValue.functions[0].blocks[0].instructions[1].operands[2] = Operand::constant(
        wrongValue.internConstant(stringConstant("a")), wrongValue.types.stringType());
    const auto badValue = verifyModule(wrongValue);
    require(!badValue.ok(), "storing a string into a set<int> was accepted");
    std::cout << "mir verifier set indexing: PASS\n";
}

// `List<T>` and `Map<K,V>` spelled as classes are the same collections as the
// lowercase keyword forms - and inside a generic class the class spelling is the
// only one available. Construction has to accept both, or a spelling can be
// indexable but not constructible.
void testCollectionClassFormsAreConstructible() {
    ModuleBuilder builder("collections");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Generic.singleton(T)");
    const TypeId typeParam = types.typeParam("T");
    const TypeId listOfT = types.objectType("List", {typeParam});
    fb.setGenericTemplate({"T"});
    fb.setReturnType(listOfT);
    const ParamId item = fb.addParameter("item", typeParam);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId list = fb.emitNewCollection(listOfT);
    fb.emitIndexStore(Operand::temp(list, listOfT),
                      Operand::constant(builder.constantInt(0), types.intType()),
                      fb.parameterOperand(item));
    fb.emitReturn(Operand::temp(list, listOfT));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "building a List<T> was rejected:\n" + reportText(report));

    // A non-collection class is still not constructible as one.
    Module notACollection = module;
    const TypeId boxOfT = notACollection.types.objectType("Box", {notACollection.types.typeParam("T")});
    notACollection.functions[0].blocks[0].instructions[0].resultType = boxOfT;
    const auto bad = verifyModule(notACollection);
    require(!bad.ok(), "new_collection of a Box<T> was accepted");
    require(hasError(bad, "must produce list/set/map"),
            "the construction error was not reported:\n" + reportText(bad));
    std::cout << "mir verifier collection class forms: PASS\n";
}

void testTypeTest() {
    ModuleBuilder builder("typetest");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Narrow.check(int|string)");
    const TypeId unionType = types.unionType({types.intType(), types.stringType()});
    fb.setReturnType(types.boolType());
    const ParamId value = fb.addParameter("value", unionType);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId isInt = fb.emitTypeTest(fb.parameterOperand(value), types.intType());
    fb.emitReturn(Operand::temp(isInt, types.boolType()));
    fb.finish();
    Module module = builder.take();

    require(opcodeShape(Opcode::TypeTest).producesResult, "type_test is not marked as producing a result");
    require(!opcodeShape(Opcode::TypeTest).mayThrow,
            "type_test is marked as raising; a match arm has to survive a 'no'");
    const auto report = verifyModule(module);
    require(report.ok(), "a runtime type test was rejected:\n" + reportText(report));

    // A test that answers anything but bool cannot drive a branch.
    Module notBool = module;
    notBool.functions[0].blocks[0].instructions[0].resultType = notBool.types.intType();
    const auto wrongResult = verifyModule(notBool);
    require(!wrongResult.ok(), "a type_test producing int was accepted");
    require(hasError(wrongResult, "must produce bool"),
            "the result-type error was not reported:\n" + reportText(wrongResult));

    // The tested type lives in its own field, so clearing it must be caught
    // rather than read as "test against nothing".
    Module untested = module;
    untested.functions[0].blocks[0].instructions[0].testedType = kNoType;
    const auto noType = verifyModule(untested);
    require(!noType.ok(), "a type_test with no tested type was accepted");
    require(hasError(noType, "no tested type"),
            "the missing-type error was not reported:\n" + reportText(noType));

    // Nothing can be tested against void.
    Module againstVoid = module;
    againstVoid.functions[0].blocks[0].instructions[0].testedType = againstVoid.types.voidType();
    const auto voidTest = verifyModule(againstVoid);
    require(!voidTest.ok(), "a type_test against void was accepted");
    require(hasError(voidTest, "cannot test against void"),
            "the void error was not reported:\n" + reportText(voidTest));

    // Testing against an unresolved type parameter proves nothing at all.
    Module againstParam = module;
    againstParam.functions[0].blocks[0].instructions[0].testedType =
        againstParam.types.typeParam("T");
    const auto paramTest = verifyModule(againstParam);
    require(!paramTest.ok(), "a type_test against a type parameter was accepted");
    require(hasError(paramTest, "unresolved type parameter"),
            "the type-parameter error was not reported:\n" + reportText(paramTest));

    // A test the static types already rule out is not malformed - it is dead -
    // so it warns rather than failing.
    Module unrelated = module;
    unrelated.functions[0].blocks[0].instructions[0].testedType = unrelated.types.boolType();
    const auto unrelatedReport = verifyModule(unrelated);
    require(unrelatedReport.ok(), "a type_test against an unrelated type was rejected:\n" +
                                      reportText(unrelatedReport));
    bool warned = false;
    for (const auto& diagnostic : unrelatedReport.diagnostics)
        if (diagnostic.severity == DiagnosticSeverity::Warning &&
            diagnostic.message.find("can never match") != std::string::npos)
            warned = true;
    require(warned, "a type_test that can never match produced no warning");
    std::cout << "mir verifier type test: PASS\n";
}

void testUnionNullabilityFollowsItsMembers() {
    TypeArena arena;
    // `string` is a value type at runtime but the language lets it be null, so
    // it has to count as nullable - and a union is nullable when any member is.
    // Testing only the top-level kind would call `int|string` non-nullable and
    // reject every `null` arm of a match over it.
    require(arena.isNullable(arena.stringType()), "string was not treated as nullable");
    require(!arena.isNullable(arena.intType()), "int was treated as nullable");
    require(!arena.isNullable(arena.boolType()), "bool was treated as nullable");
    require(arena.isNullable(arena.unionType({arena.intType(), arena.stringType()})),
            "int|string was not treated as nullable");
    require(!arena.isNullable(arena.unionType({arena.intType(), arena.doubleType()})),
            "int|double was treated as nullable");
    std::cout << "mir union nullability: PASS\n";
}

void testNominalTypesCarryTheirName() {
    TypeArena arena;
    // `name` is the declaring name of a nominal type. Rendering never needed it
    // (the kind supplies "Task"/"Shared"), but every consumer asking "which
    // class is this?" does - a method call on such a receiver came back with no
    // class name at all when it was missing.
    require(arena.find(arena.taskType(arena.intType()))->name == "Task", "Task<T> has no name");
    require(arena.find(arena.sharedType(arena.intType()))->name == "Shared", "Shared<T> has no name");
    require(arena.render(arena.taskType(arena.intType())) == "Task<int>", "Task<int> rendered wrong");
    require(arena.render(arena.sharedType(arena.intType())) == "Shared<int>", "Shared<int> rendered wrong");
    std::cout << "mir nominal type names: PASS\n";
}

void testArraySizeIsOptional() {
    TypeArena arena;
    // A dynamic `array<int>` must not become `array[0]<int>`: the source never
    // stated a length, and inventing one changes the type.
    require(arena.render(arena.arrayType(arena.intType(), std::nullopt)) == "array<int>",
            "a dynamic array rendered as a fixed-size one");
    require(arena.render(arena.arrayType(arena.intType(), 10)) == "array[10]<int>",
            "a fixed-size array rendered wrong");
    require(arena.arrayType(arena.intType(), std::nullopt) != arena.arrayType(arena.intType(), 10),
            "a dynamic array and array[10] interned to the same type");
    std::cout << "mir array sizes: PASS\n";
}

} // namespace

int main() {
    testTypeArena();
    testValidModuleVerifies();
    testControlFlowAnalysis();

    testRejectsMissingBlockReference();
    testRejectsMissingTerminator();
    testRejectsUnreachableBlock();
    testRejectsInconsistentPredecessors();
    testRejectsDominanceViolation();
    testRejectsForwardUse();

    testRejectsUndefinedTemp();
    testRejectsWrongOperandType();
    testRejectsWrongResultType();
    testRejectsBadConstant();
    testRejectsBadParameterReference();
    testRejectsWrongOperandCount();

    testRejectsInvalidReturn();
    testRejectsMissingReturnValue();
    testRejectsReturnTypeMismatch();
    testRejectsMalformedSignature();
    testRejectsNonBoolBranch();
    testRejectsEntryWithPredecessor();
    testRejectsCallArityMismatch();
    testRejectsCallArgumentType();

    testRejectsAwaitOutsideAsync();
    testRejectsStoreToImmutableSlot();
    testRejectsMoveOfNonOwnedSlot();
    testRejectsUseAfterMove();
    testRejectsMoveWhileBorrowed();
    testRejectsDropOfBorrow();
    testRejectsEndBorrowWithoutBorrow();
    testRejectsNullCheckOnNonNullable();
    testRejectsUndeclaredTypeParamUse();
    testAcceptsDeclaredTypeParam();

    testExceptionEdges();

    testInheritedFieldAccess();
    testListObjectIndexing();
    testEnumMemberConstant();
    testGenericCallInstantiation();
    testSharedIsReferenceType();
    testAwaitOfVoidTaskDefinesNoTemp();
    testMapIndexAccess();
    testSetIndexing();
    testCollectionClassFormsAreConstructible();
    testTypeTest();
    testUnionNullabilityFollowsItsMembers();
    testNominalTypesCarryTheirName();
    testArraySizeIsOptional();

    if (failures != 0) {
        std::cerr << failures << " MIR regression(s) failed\n";
        return 1;
    }
    std::cout << "all MIR regressions passed\n";
    return 0;
}
