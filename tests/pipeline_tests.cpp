// Compiler pipeline regressions: the contract between language semantics and
// code generation.
//
// Everything else in `tests/` checks a component - the verifier, the lowerer,
// a pass, a backend. This file checks the *pipeline* that composes them, and in
// particular the properties that only hold if the composition is right:
//
//   1. The stages run in the documented order, every stage records what it did,
//      and a stage driven out of turn is a compiler-invariant failure rather
//      than something that quietly works.
//   2. No backend is ever handed MIR that was not verified - including MIR that
//      was mutated after the verification stage, which the pipeline re-checks.
//   3. Selecting a backend is a code-generation decision and nothing else: the
//      bytecode backend and the native backend are handed the same module
//      (compared by digest), and the program the VM executes behaves the same
//      whatever backend generated code.
//   4. The stages that are opt-in (optimisation, slot promotion) are recorded as
//      skipped when they do not run, and are observationally invisible when they
//      do.
//
// The tests drive the real pipeline over real ZL source in a temp directory, and
// execute real bytecode in a real VM with its output captured, because the
// alternative - asserting on a mock - would test the mock.

#include "zl/compiler/compiler.hpp"
#include "zl/compiler/pipeline.hpp"
#include "zl/mir/reachability.hpp"
#include "zl/mir/printer.hpp"
#include "zl/vm/native.hpp"
#include "zl/vm/vm.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void require(bool condition, const std::string& message) {
    ++checks;
    if (condition) return;
    std::cerr << "compiler pipeline regression: " << message << '\n';
    ++failures;
}

namespace fs = std::filesystem;
using zl::pipeline::Backend;
using zl::pipeline::ErrorKind;
using zl::pipeline::Execution;
using zl::pipeline::Options;
using zl::pipeline::Pipeline;
using zl::pipeline::Result;
using zl::pipeline::Stage;

// Writes `source` to a scratch file. ZL requires the primary type to be named
// after the file, so the file is named after the class the program declares.
fs::path writeProgram(const std::string& name, const std::string& source) {
    const fs::path directory = fs::temp_directory_path() / ("zl_pipeline_" + name);
    fs::create_directories(directory);
    const fs::path file = directory / (name + ".zl");
    std::ofstream out(file);
    out << source;
    out.close();
    return file;
}

// A program with a foldable expression, a loop and a call, so that every stage
// has something to do: the verifier checks a real CFG, the optimiser has a
// constant to fold, and both backends have code to generate.
const char* kProgram =
    "class Pipe {\n"
    "    static func add(int a, int b): int {\n"
    "        return a + b\n"
    "    }\n"
    "    func main(): void {\n"
    "        var total = 0\n"
    "        for i in 0..4 {\n"
    "            total = Pipe.add(total, i * 2)\n"
    "        }\n"
    "        log(\"total=\" + total)\n"
    "    }\n"
    "}\n";

struct RunResult {
    int exitCode{0};
    std::string output;
    std::string error;
    bool ran{false};
};

// Executes a chunk on a VM with stdout captured, so a test can compare what a
// program *does* rather than only what the compiler produced.
RunResult runChunk(const zl::Chunk& chunk) {
    RunResult result;
    std::ostringstream captured;
    std::streambuf* previous = std::cout.rdbuf(captured.rdbuf());
    try {
        auto vm = std::make_shared<zl::VM>();
        result.exitCode = vm->run(chunk, std::vector<std::string>{});
        result.ran = true;
    } catch (const zl::SystemExitException& exit) {
        result.exitCode = exit.code;
        result.ran = true;
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    std::cout.rdbuf(previous);
    result.output = captured.str();
    return result;
}

Result compileWhole(const fs::path& file, Options options) {
    Pipeline pipeline(std::move(options));
    (void)pipeline.run(file, std::vector<fs::path>{});
    return std::move(pipeline.result());
}

// ---------------------------------------------------------------------------
// Stage order and the ledger
// ---------------------------------------------------------------------------

void testStageOrderAndLedger() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Options options;
    options.backend = Backend::Bytecode;
    options.optimize = true;
    Pipeline pipeline(options);
    const bool ok = pipeline.run(file, {});
    const Result& result = pipeline.result();

    require(ok, "a well-formed program should compile");
    require(result.ok(), "a successful run should record no failure");
    require(result.stages.size() == zl::pipeline::kStageCount,
            "every stage should be recorded, even the ones that were skipped");

    // Order is the contract: stage n's postcondition is stage n+1's precondition.
    for (std::size_t i = 0; i < result.stages.size(); ++i) {
        require(static_cast<std::size_t>(result.stages[i].stage) == i,
                std::string("stage ") + std::to_string(i) + " should be '" +
                    zl::pipeline::stageName(static_cast<Stage>(i)) + "'");
        require(result.stages[i].ran, "stage should have run");
        require(result.stages[i].ok, "stage should have succeeded");
        require(!result.stages[i].detail.empty(), "a stage should say what it did");
    }
    require(result.verification.ok(), "the verification stage should have accepted the module");
    require(result.complete(), "this program lowers completely");
    require(result.chunk.has_value(), "the bytecode backend should have produced a chunk");
    require(result.chunk->functions.size() > 1, "the chunk should contain the program's functions");
    require(!result.mirDigest().empty(), "the boundary should have a digest");

    const std::string ledger = result.describe();
    for (std::size_t i = 0; i < zl::pipeline::kStageCount; ++i) {
        const auto stage = static_cast<Stage>(i);
        require(ledger.find(zl::pipeline::stageName(stage)) != std::string::npos,
                std::string("the ledger should name stage '") + zl::pipeline::stageName(stage) + "'");
        require(std::string(zl::pipeline::stageInvariant(stage)).size() > 20,
                "every stage should state its invariant");
    }
    require(ledger.find("backend bytecode") != std::string::npos,
            "the ledger should name the selected backend");
    require(ledger.find(result.mirDigest()) != std::string::npos,
            "the ledger should name the MIR boundary");
}

void testSkippedStagesAreRecorded() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Options options;
    options.optimize = false;
    options.promoteSlots = false;
    const Result result = compileWhole(file, options);

    require(result.ok(), "the program should compile without the optional stages");
    const auto* optimization = result.stage(Stage::MirOptimization);
    require(optimization != nullptr, "the optimisation stage should still be recorded");
    require(!optimization->ran, "a skipped optimisation stage should be marked as not run");
    require(optimization->ok, "a skipped stage is not a failure");
    require(optimization->detail.find("skipped") != std::string::npos,
            "a skipped stage should say why it was skipped");
    require(!result.promotion.ran, "promotion should be recorded as not run");
    // And the module is still the verified boundary: skipping a stage never
    // skips the verification stage.
    require(result.verification.ok(), "verification is not optional in practice");
}

void testOutOfOrderStagesAreRefused() {
    const fs::path file = writeProgram("Pipe", kProgram);

    // Code generation first: the module does not exist yet.
    Pipeline pipeline(Options{});
    require(!pipeline.generate(), "generate() before any other stage must fail");
    require(pipeline.result().failure.kind == ErrorKind::Invariant,
            "driving the pipeline out of order is a compiler-invariant failure, not a user error");
    require(pipeline.result().failure.message.find("out of order") != std::string::npos,
            "the failure should say the pipeline was driven out of order");
    require(zl::pipeline::exitCodeFor(ErrorKind::Invariant) == 1, "invariant failures exit 1");

    // Verification before lowering.
    Pipeline second(Options{});
    require(second.load(file, {}), "load should succeed");
    require(second.analyze(), "semantic analysis should succeed");
    require(!second.verifyMir(), "verifyMir() before lowering must fail");
    require(second.result().failure.kind == ErrorKind::Invariant,
            "verifying before lowering is an ordering failure");

    // And running the stages again after they have run once.
    Pipeline third(Options{});
    require(third.load(file, {}), "load should succeed");
    require(!third.load(file, {}), "load twice must fail");
    require(third.result().failure.kind == ErrorKind::Invariant, "a repeated stage is a failure");
}

void testPartialPipelineStopsWhereAsked() {
    const fs::path file = writeProgram("Pipe", kProgram);

    // This is how `--check` is implemented: stop after the front end.
    Pipeline pipeline(Options{});
    require(pipeline.load(file, {}), "load should succeed");
    require(pipeline.analyze(), "semantic analysis should succeed");
    require(pipeline.result().stages.size() == 2,
            "only the stages that ran should be recorded");
    require(pipeline.result().stage(Stage::TypedLowering) == nullptr,
            "a stage that did not run should not appear in the ledger");
    require(pipeline.result().module.functions.empty(), "no MIR should exist yet");
}

// ---------------------------------------------------------------------------
// No backend ever sees unverified MIR
// ---------------------------------------------------------------------------

void testCodeGenerationRequiresVerification() {
    const fs::path file = writeProgram("Pipe", kProgram);

    // Verification disabled and no pre-codegen re-check: code generation must
    // refuse rather than consume a module nobody checked.
    Options unchecked;
    unchecked.verify = false;
    unchecked.verifyBeforeCodegen = false;
    Pipeline pipeline(unchecked);
    require(pipeline.load(file, {}), "load should succeed");
    require(pipeline.analyze(), "semantic analysis should succeed");
    require(pipeline.lowerToMir(), "lowering should succeed");
    require(pipeline.verifyMir(), "a disabled verification stage is not a failure");
    require(!pipeline.result().stage(Stage::MirVerification)->ran,
            "a disabled verification stage should be recorded as not run");
    require(!pipeline.generate(), "code generation must refuse an unverified module");
    require(pipeline.result().failure.kind == ErrorKind::Invariant,
            "the refusal is a compiler invariant, not a backend error");

    // Same configuration, but with the pre-codegen re-check left on: the module
    // is verified, just at the last possible moment.
    Options recheck;
    recheck.verify = false;
    recheck.verifyBeforeCodegen = true;
    Pipeline second(recheck);
    require(second.run(file, {}), "the pre-codegen re-check should stand in for stage 4");
    require(second.result().verification.ok(), "the re-check should have accepted the module");
    require(second.result().chunk.has_value(), "the backend should have run");
}

void testMutationAfterVerificationIsCaught() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Pipeline pipeline(Options{});
    require(pipeline.load(file, {}), "load should succeed");
    require(pipeline.analyze(), "semantic analysis should succeed");
    require(pipeline.lowerToMir(), "lowering should succeed");
    require(pipeline.verifyMir(), "the module should verify");
    require(pipeline.optimizeMir(), "the (skipped) optimisation stage is still a stage");
    require(!pipeline.result().stage(Stage::MirOptimization)->ran, "and it was skipped");

    // Simulate a compiler bug: something rewrites the module between the
    // verification stage and the backend. The entry block is given a jump to a
    // block that does not exist, which the verifier rejects.
    Result& result = pipeline.result();
    require(!result.module.functions.empty(), "the module should have functions");
    auto& block = result.module.functions.front().blocks.front();
    block.terminator.kind = zl::mir::TerminatorKind::Jump;
    block.terminator.target = 4242;
    block.terminator.elseBlock = zl::mir::kNoBlock;
    block.terminator.cases.clear();
    block.terminator.edgeArguments.clear();
    block.terminator.value = zl::mir::Operand::none();

    require(!pipeline.generate(), "code generation must re-check the module it was handed");
    require(pipeline.result().failure.kind == ErrorKind::Verification,
            "a module mutated after verification is a verification failure");
    require(pipeline.result().chunk.has_value() == false,
            "no artifact may be produced from an invalid module");
}

// ---------------------------------------------------------------------------
// Backend selection is a code-generation decision
// ---------------------------------------------------------------------------

void testBackendSelectionKeepsMirIdentical() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Options bytecodeOptions;
    bytecodeOptions.backend = Backend::Bytecode;
    const Result bytecode = compileWhole(file, bytecodeOptions);

    Options nativeOptions;
    nativeOptions.backend = Backend::Native;
    const Result native = compileWhole(file, nativeOptions);

    require(bytecode.ok() && native.ok(), "both backends should compile this program");
    require(bytecode.mirDigest() == native.mirDigest(),
            "the two backends must be handed the same MIR module (equal digests)");
    require(bytecode.module.functions.size() == native.module.functions.size(),
            "the boundary module should have the same shape whichever backend is selected");
    require(bytecode.chunk.has_value(), "the bytecode backend should produce a chunk");
    require(native.native.has_value(), "the native backend should produce a native result");
    require(!native.nativeTargetTriple.empty(), "the native result should name its target");
    // The execution driver: the VM runs bytecode translated from the same MIR.
    require(native.chunk.has_value(), "a run with the native backend still needs VM bytecode");
}

void testBackendChoiceDoesNotChangeBehaviour() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Options mirror;
    mirror.backend = Backend::Bytecode;
    const Result mirrored = compileWhole(file, mirror);
    require(mirrored.ok() && mirrored.chunk.has_value(), "the MIR path should run");

    Options native;
    native.backend = Backend::Native;
    const Result natively = compileWhole(file, native);
    require(natively.ok() && natively.chunk.has_value(), "the native-backed run should be runnable");

    // The reference path: the original AST -> bytecode compiler, unchanged.
    std::unique_ptr<zl::Program> program;
    {
        zl::ModuleLoader loader(file, std::vector<fs::path>{});
        program = loader.load();
        zl::TypeChecker checker;
        checker.check(*program, /*requireMain=*/true);
    }
    zl::Compiler referenceCompiler;
    const zl::Chunk referenceChunk = referenceCompiler.compile(*program);

    const RunResult reference = runChunk(referenceChunk);
    const RunResult mir = runChunk(*mirrored.chunk);
    const RunResult viaNative = runChunk(*natively.chunk);

    require(reference.ran && mir.ran && viaNative.ran, "all three paths should execute");
    require(reference.output == mir.output,
            "the MIR pipeline must reproduce the reference compiler's output");
    require(reference.exitCode == mir.exitCode, "and its exit code");
    require(viaNative.output == reference.output,
            "selecting the native backend must not change what the program prints");
    require(viaNative.exitCode == reference.exitCode, "or what it exits with");
    require(!reference.output.empty(), "the comparison should not be vacuous");
}

// ---------------------------------------------------------------------------
// The native tier reports a partial result, or refuses to have one
// ---------------------------------------------------------------------------

void testNativeLedgerAndStrictMode() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Options native;
    native.backend = Backend::Native;
    const Result ledger = compileWhole(file, native);
    require(ledger.ok(), "the native tier compiles the subset it can prove");
    require(ledger.native.has_value(), "and hands the ledger back with the result");

    // The ledger's contract: every function in the module is accounted for,
    // exactly once, as native or as left to the VM. That is what makes a future
    // mixed-mode execution a matter of wiring rather than of rediscovery.
    const std::size_t accounted = ledger.native->nativeFunctions.size() +
                                  ledger.native->vmFunctions.size();
    require(accounted == ledger.module.functions.size(),
            "the native ledger accounts for every function in the module exactly once");
    require(!ledger.native->nativeFunctions.empty(),
            "the fixture has functions the native tier compiles");
    require(!ledger.native->vmFunctions.empty(),
            "and the rest of the module (the stdlib) is left to the VM");
    // One check, not one per function: the count should move when behaviour
    // changes, not when the standard library grows.
    std::size_t unexplained = 0;
    for (const auto& rejection : ledger.native->vmFunctions) {
        if (rejection.reason.empty()) ++unexplained;
    }
    require(unexplained == 0, "every function left to the VM carries the reason it was left");

    Options strict = native;
    strict.strictNative = true;
    const Result refused = compileWhole(file, strict);
    require(!refused.ok(), "strict native mode refuses a partial result");
    require(refused.failure.kind == ErrorKind::Backend,
            "reported as a backend failure, not as a MIR one");
    require(refused.failure.stage == Stage::CodeGeneration, "at the code-generation stage");
    require(refused.failure.message.find("could not be compiled natively") != std::string::npos,
            "naming what it refused");
    require(refused.failure.detail.find("left to the VM") != std::string::npos,
            "and giving a reason for the functions it refused");
    require(zl::pipeline::exitCodeFor(refused.failure.kind) == 4, "which exits 4");
    require(!refused.chunk.has_value(), "and no artifact is produced");

    // Strict mode is a switch on the native tier and nothing else: the bytecode
    // backend has no subset to be strict about.
    Options bytecode;
    bytecode.strictNative = true;
    const Result tolerant = compileWhole(file, bytecode);
    require(tolerant.ok(), "strict native mode must not affect the bytecode backend");
    require(!tolerant.native.has_value(), "which does not run the native tier at all");
}

// ---------------------------------------------------------------------------
// Reachability: what a program can actually run
// ---------------------------------------------------------------------------

// A dispatch site keeps every override in the hierarchy, and nothing outside it:
// `Animal.speak` and `Dog.speak` both run depending on the receiver, `Loner`
// shares the method name but is not an `Animal`.
const char* kDispatchProgram =
    "class Zoo {\n"
    "    func main(): void {\n"
    "        Animal a = new Dog()\n"
    "        log(a.speak())\n"
    "    }\n"
    "}\n"
    "class Animal {\n"
    "    public func speak(): string { return \"...\" }\n"
    "}\n"
    "class Dog extends Animal {\n"
    "    public func speak(): string { return \"woof\" }\n"
    "}\n"
    "class Loner {\n"
    "    public func speak(): string { return \"quiet\" }\n"
    "}\n";

// Reflection invokes whatever a `Method` value describes, so a program that
// calls it has no closed call graph.
const char* kReflectionInvokeProgram =
    "class Reflector {\n"
    "    func helper(): int { return 7 }\n"
    "    func main(): void {\n"
    "        var r = new Reflector()\n"
    "        Method m = Type.of(r).method(\"helper\")\n"
    "        object result = m.invoke(r, [])\n"
    "        log(\"result \" + result)\n"
    "    }\n"
    "}\n";

// Functions are identified by their rendered name, e.g. "Animal.speak()".
zl::mir::FunctionId functionIdOf(const zl::mir::Module& module, const std::string& name) {
    for (const auto& function : module.functions) {
        if (function.name == name) return function.id;
    }
    return zl::mir::kNoFunction;
}

void testReachabilityIsSoundAndUseful() {
    const fs::path file = writeProgram("Zoo", kDispatchProgram);
    const Result compiled = compileWhole(file, Options{});
    require(compiled.ok(), "the dispatch program should compile");
    require(compiled.module.entryPoint != zl::mir::kNoFunction, "and have an entry point");

    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(compiled.module);
    require(report.hasEntryPoint, "the report should start from the entry point");
    require(report.complete(), "a program without reflection has a closed call graph");
    require(report.size() < compiled.module.functions.size(),
            "most of a module is stdlib the program never calls");

    require(report.contains(functionIdOf(compiled.module, "Zoo.main()")),
            "the entry point is reachable");
    require(report.contains(functionIdOf(compiled.module, "Animal.speak()")),
            "the declared implementation of a called method is reachable");
    require(report.contains(functionIdOf(compiled.module, "Dog.speak()")),
            "and so is an override the receiver's runtime class can select");
    require(!report.contains(functionIdOf(compiled.module, "Loner.speak()")),
            "a same-named method outside the receiver's hierarchy is not reachable");
    require(!report.contains(functionIdOf(compiled.module, "Loner.Loner()")),
            "and neither is a constructor nothing constructs");
    const std::string described = report.describe(compiled.module);
    require(described.find("can run") != std::string::npos,
            "the report says how many functions can run");
}

void testReachabilityRefusesToCloseAGraphWithReflection() {
    const fs::path file = writeProgram("Reflector", kReflectionInvokeProgram);
    const Result compiled = compileWhole(file, Options{});
    require(compiled.ok(), "the reflection program should compile");

    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(compiled.module);
    require(report.hasEntryPoint, "it has an entry point");
    require(report.dynamicEntry,
            "a reachable Reflection.methodInvoke means the call graph is not closed");
    require(!report.complete(), "so the analysis does not claim to be exact");
    require(!report.dynamicEntryReason.empty(), "and says which native opens the graph");
    require(report.describe(compiled.module).find("reflection") != std::string::npos,
            "and the report warns that the answer is a lower bound");
}

void testReachabilityOnALibrary() {
    const fs::path libraryFile =
        writeProgram("Shelf", "class Shelf {\n    func size(): int { return 0 }\n}\n");
    Options noMain;
    noMain.requireMain = false;
    noMain.codeGeneration = false;
    const Result library = compileWhole(libraryFile, noMain);
    require(library.ok(), "a library module should lower and verify");

    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(library.module);
    require(!report.hasEntryPoint, "a library has no entry point to reach anything from");
    require(report.functions.empty(), "so nothing is claimed to run");
    require(!report.complete(), "and the analysis does not pretend to be complete");
    require(report.describe(library.module).find("library") != std::string::npos,
            "it says why it has no answer");
}

// ---------------------------------------------------------------------------
// Function values: the execution edges a call through a value creates
// ---------------------------------------------------------------------------

// A plain direct call is a closed edge: the target is recorded on the
// instruction, and the report may be used as a removal guarantee.
const char* kDirectCallProgram =
    "class FnDirect {\n"
    "    static func add(int a, int b): int { return a + b }\n"
    "    func main(): void {\n"
    "        log(\"sum \" + FnDirect.add(2, 3))\n"
    "    }\n"
    "}\n";

// A closure that is provably one body: `let` keeps it as an SSA value, `var`
// stores it to a single slot and reads it back. Both are pinned by SSA
// structure, so both are closed edges.
const char* kKnownClosureProgram =
    "class FnClosure {\n"
    "    static func body(int x): int { return x * 3 }\n"
    "    static func viaLet(): int {\n"
    "        let f = FnClosure.body\n"
    "        return f(10)\n"
    "    }\n"
    "    static func viaSlot(): int {\n"
    "        var f = FnClosure.body\n"
    "        return f(20)\n"
    "    }\n"
    "    func main(): void {\n"
    "        log(\"let \" + FnClosure.viaLet())\n"
    "        log(\"slot \" + FnClosure.viaSlot())\n"
    "    }\n"
    "}\n";

// The callee is a parameter: whatever the caller chose can run. The analysis
// must not pretend the graph is closed, and must not invent a target.
const char* kFunctionParameterProgram =
    "class FnParam {\n"
    "    static func apply(func(int): int f, int v): int { return f(v) }\n"
    "    func main(): void {\n"
    "        log(\"applied \" + FnParam.apply(func(int x) => x + 1, 40))\n"
    "    }\n"
    "}\n";

// A function value stored in a slot and invoked later: provable when the slot
// holds one known closure, open when it holds a parameter the caller chose.
const char* kStoredKnownFunctionProgram =
    "class FnStored {\n"
    "    static func body(int x): int { return x + 7 }\n"
    "    static func use(): int {\n"
    "        var g = FnStored.body\n"
    "        return g(6)\n"
    "    }\n"
    "    func main(): void {\n"
    "        log(\"stored \" + FnStored.use())\n"
    "    }\n"
    "}\n";

const char* kStoredUnknownFunctionProgram =
    "class FnStoredUnknown {\n"
    "    static func use(func(int): int f, int v): int {\n"
    "        var g = f\n"
    "        return g(v)\n"
    "    }\n"
    "    func main(): void {\n"
    "        log(\"stored \" + FnStoredUnknown.use(func(int x) => x * 2, 21))\n"
    "    }\n"
    "}\n";

// One proven target and one open edge in the same module: the known body is
// still accounted for, and the unknown edge still makes the report
// incomplete.
const char* kMixedTargetsProgram =
    "class FnMixed {\n"
    "    static func body(int x): int { return x + 7 }\n"
    "    static func known(int x): int {\n"
    "        var f = FnMixed.body\n"
    "        return f(x)\n"
    "    }\n"
    "    static func unknown(func(int): int f, int x): int { return f(x) }\n"
    "    func main(): void {\n"
    "        log(\"known \" + FnMixed.known(1))\n"
    "        log(\"unknown \" + FnMixed.unknown(func(int x) => x, 2))\n"
    "    }\n"
    "}\n";

void testDirectCallsKeepTheGraphClosed() {
    const fs::path file = writeProgram("FnDirect", kDirectCallProgram);
    const Result compiled = compileWhole(file, Options{});
    require(compiled.ok(), "the direct-call program should compile");

    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(compiled.module);
    require(report.hasEntryPoint, "it has an entry point");
    require(report.complete(), "direct calls are recorded targets, so the graph is closed");
    require(!report.unresolvedCalls, "and no function-value edge was reported");
    require(report.contains(functionIdOf(compiled.module, "FnDirect.main()")),
            "the entry point is reachable");
    require(report.contains(functionIdOf(compiled.module, "FnDirect.add(int,int)")),
            "the directly called function is reachable");
}

void testKnownClosureCallsAreAccounted() {
    const fs::path file = writeProgram("FnClosure", kKnownClosureProgram);
    const Result compiled = compileWhole(file, Options{});
    require(compiled.ok(), "the known-closure program should compile");

    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(compiled.module);
    require(report.complete(),
            "a closure pinned to one body (directly or through a single-store slot) is a closed edge");
    require(!report.unresolvedCalls, "so no edge was left unresolved");
    require(report.contains(functionIdOf(compiled.module, "FnClosure.body(int)")),
            "the closure body is reachable through both the SSA value and the stored value");
}

void testUnresolvedIndirectCallsLeaveTheGraphOpen() {
    const fs::path file = writeProgram("FnParam", kFunctionParameterProgram);
    const Result compiled = compileWhole(file, Options{});
    require(compiled.ok(), "the function-parameter program should compile");

    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(compiled.module);
    require(report.hasEntryPoint, "it has an entry point");
    require(report.unresolvedCalls,
            "a call through a function-valued parameter is an execution edge with no static target");
    require(report.unresolvedCallCount >= 1, "the unresolved edge is counted");
    require(!report.unresolvedCallReason.empty(),
            "and the report says which function and construct opened the graph");
    require(report.unresolvedCallReason.find("FnParam.apply") != std::string::npos,
            "the reason names the function that executes the value");
    require(!report.complete(),
            "so the caller cannot treat the reachable set as a removal guarantee");
    require(report.describe(compiled.module).find("not pinned") != std::string::npos,
            "and the one-line report carries the caveat");
}

void testStoredFunctionValuesFollowTheSameRule() {
    const fs::path known = writeProgram("FnStored", kStoredKnownFunctionProgram);
    const Result knownCompiled = compileWhole(known, Options{});
    require(knownCompiled.ok(), "the stored-known program should compile");
    const zl::mir::ReachabilityReport knownReport = zl::mir::reachableFunctions(knownCompiled.module);
    require(knownReport.complete(),
            "a slot holding one known closure is provable, so the stored call is a closed edge");
    require(knownReport.contains(functionIdOf(knownCompiled.module, "FnStored.body(int)")),
            "the stored closure's body is reachable");

    const fs::path unknown = writeProgram("FnStoredUnknown", kStoredUnknownFunctionProgram);
    const Result unknownCompiled = compileWhole(unknown, Options{});
    require(unknownCompiled.ok(), "the stored-unknown program should compile");
    const zl::mir::ReachabilityReport unknownReport =
        zl::mir::reachableFunctions(unknownCompiled.module);
    require(unknownReport.unresolvedCalls,
            "a slot holding a parameter can hold any function the caller chose");
    require(!unknownReport.complete(),
            "so the reachable set is a lower bound and not removal-safe");
}

void testMixedKnownAndUnknownIndirectTargets() {
    const fs::path file = writeProgram("FnMixed", kMixedTargetsProgram);
    const Result compiled = compileWhole(file, Options{});
    require(compiled.ok(), "the mixed-targets program should compile");

    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(compiled.module);
    require(report.unresolvedCalls, "the parameter call opens the graph");
    require(!report.complete(), "one open edge is enough to make the report incomplete");
    require(report.contains(functionIdOf(compiled.module, "FnMixed.body(int)")),
            "but the proven target is still accounted for in the reachable set");
    require(report.unresolvedCallCount >= 1, "and the open edge is recorded, not dropped");
}

// ---------------------------------------------------------------------------
// The pipeline-report contract: a runnable program, or a precise refusal
// ---------------------------------------------------------------------------
//
// `--pipeline-report` describes a runnable program's pipeline, code
// generation included, so it runs with the same program contract a run does:
// requireMain, enforced at the semantic stage with the entry-point
// diagnostics. A library is refused there - never reported as if a
// code-generation stage had run for a module nothing can enter.

void testPipelineReportRequiresAProgram() {
    // A library: the program contract refuses it at the semantic stage, and
    // the diagnostic names what is missing.
    const fs::path libraryFile =
        writeProgram("ShelfReport", "class ShelfReport {\n    func size(): int { return 0 }\n}\n");
    Options programContract;
    programContract.requireMain = true; // what --pipeline-report runs with
    const Result library = compileWhole(libraryFile, programContract);
    require(!library.ok(), "a library has no entry point, so the program pipeline refuses it");
    require(library.failure.kind == ErrorKind::TypeCheck,
            "the refusal is a semantic-stage diagnostic, not a later surprise");
    require(library.failure.stage == Stage::SemanticAnalysis,
            "the entry point is validated where language semantics are decided");
    require(library.failure.message.find("main") != std::string::npos,
            "and the diagnostic names what is missing: " + library.failure.message);
    require(library.stage(Stage::CodeGeneration) == nullptr ||
                !library.stage(Stage::CodeGeneration)->ran,
            "code generation never ran for a module nothing can enter");

    // A program: the same contract accepts it, records the entry point, and
    // runs the code-generation stage the report describes.
    const fs::path programFile = writeProgram("Pipe", kProgram);
    Options okOptions;
    okOptions.requireMain = true;
    const Result program = compileWhole(programFile, okOptions);
    require(program.ok(), "a program with a valid main satisfies the report contract");
    require(program.module.entryPoint != zl::mir::kNoFunction,
            "and the entry point the report starts from is recorded");
    require(program.stage(Stage::CodeGeneration) != nullptr &&
                program.stage(Stage::CodeGeneration)->ran,
            "the code-generation stage the report describes actually ran");

    // An invalid entry point: the same stage names the exact violation.
    const fs::path wrongReturn =
        writeProgram("BadReturn", "class BadReturn {\n    func main(): int { return 0 }\n}\n");
    const Result wrongReturnCompiled = compileWhole(wrongReturn, okOptions);
    require(!wrongReturnCompiled.ok(), "a main that returns a value is not a valid entry point");
    require(wrongReturnCompiled.failure.message.find("void") != std::string::npos,
            "and the diagnostic says what main must return: " + wrongReturnCompiled.failure.message);

    const fs::path staticMain =
        writeProgram("BadStatic", "class BadStatic {\n    static func main(): void {}\n}\n");
    const Result staticMainCompiled = compileWhole(staticMain, okOptions);
    require(!staticMainCompiled.ok(), "a static main is not a valid entry point");
    require(staticMainCompiled.failure.message.find("instance") != std::string::npos,
            "and the diagnostic says main must be an instance method: " +
                staticMainCompiled.failure.message);
}

// ---------------------------------------------------------------------------
// The opt-in stages are invisible, and say so when skipped
// ---------------------------------------------------------------------------

void testOptimizationIsObservational() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Options off;
    off.optimize = false;
    const Result unoptimized = compileWhole(file, off);

    Options on;
    on.optimize = true;
    const Result optimized = compileWhole(file, on);

    require(unoptimized.ok() && optimized.ok(), "both configurations should compile");
    require(optimized.stage(Stage::MirOptimization)->ran, "the stage should have run");
    require(!unoptimized.stage(Stage::MirOptimization)->ran, "and should have been skipped");
    require(optimized.mirDigest() != unoptimized.mirDigest(),
            "the optimiser should have changed this program's MIR");
    require(optimized.verification.ok(), "the optimised module must still verify");

    const RunResult before = runChunk(*unoptimized.chunk);
    const RunResult after = runChunk(*optimized.chunk);
    require(before.ran && after.ran, "both modules should execute");
    require(before.output == after.output, "optimisation must not change what the program prints");
    require(before.exitCode == after.exitCode, "or its exit code");
}

void testOptimizationDifferentialCheckIsStageOption() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Options options;
    options.optimize = true;
    options.checkOptimization = true;
    const Result result = compileWhole(file, options);

    require(result.ok(), "the differential check should accept a correct optimisation");
    // The check compares the pre- and post-optimisation modules; the stage that
    // produced them is still the same stage.
    require(result.stage(Stage::MirOptimization)->ran, "the optimisation stage ran");
}

void testPromotionIsAFormChangeOnly() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Options stored;
    stored.optimize = false;
    const Result memory = compileWhole(file, stored);

    Options promoted;
    promoted.optimize = false;
    promoted.promoteSlots = true;
    const Result value = compileWhole(file, promoted);

    require(memory.ok() && value.ok(), "both MIR forms should compile");
    require(value.promotion.ran, "the promotion should be recorded as run");
    require(!memory.promotion.ran, "and as not run when it was not asked for");
    require(value.promotion.promotedSlots > 0 || value.promotion.changedFunctions == 0,
            "promotion reports what it changed");
    require(value.verification.ok(), "stage 4 verifies the *promoted* module, not the original");
    require(value.mirDigest() != memory.mirDigest(), "the two forms are genuinely different MIR");

    const RunResult storedRun = runChunk(*memory.chunk);
    const RunResult promotedRun = runChunk(*value.chunk);
    require(storedRun.ran && promotedRun.ran, "both forms should execute");
    require(storedRun.output == promotedRun.output,
            "the memory form and the value form must behave identically");
}

// ---------------------------------------------------------------------------
// Failures: kind, message, exit code
// ---------------------------------------------------------------------------

void testFrontendFailuresAreClassified() {
    const fs::path syntax = writeProgram("Broken", "class Broken {\n    func main(): void {\n        var x = \n    }\n}\n");
    const Result syntaxResult = compileWhole(syntax, Options{});
    require(!syntaxResult.ok(), "a syntax error should fail the pipeline");
    require(syntaxResult.failure.kind == ErrorKind::Syntax, "and be classified as a syntax error");
    require(syntaxResult.failure.stage == Stage::Load, "in the load stage");
    require(zl::pipeline::exitCodeFor(syntaxResult.failure.kind) == 1, "which exits 1");
    // The ledger must say where the pipeline stopped: a stage that failed is a
    // stage that ran, and its line carries the reason.
    const auto* loadStage = syntaxResult.stage(Stage::Load);
    require(loadStage != nullptr && loadStage->ran && !loadStage->ok,
            "a failing stage still appears in the ledger, marked failed");
    require(loadStage != nullptr && loadStage->detail.find("Expected expression") != std::string::npos,
            "and its line carries the failure message");

    const fs::path noMain = writeProgram("NoMain", "class NoMain {\n    func other(): int { return 1 }\n}\n");
    Options requireMain;
    requireMain.requireMain = true;
    const Result noMainResult = compileWhole(noMain, requireMain);
    require(!noMainResult.ok(), "a program with no main should fail when main is required");
    require(noMainResult.failure.kind == ErrorKind::TypeCheck,
            "which is a semantic-analysis failure, not a MIR one");
    require(noMainResult.failure.stage == Stage::SemanticAnalysis, "in the semantic stage");

    Options optionalMain = requireMain;
    optionalMain.requireMain = false;
    optionalMain.codeGeneration = false;  // this is how `--emit-mir` inspects a library
    const Result tolerant = compileWhole(noMain, optionalMain);
    require(tolerant.ok(), "a library module should lower and verify when main is not required");
    require(!tolerant.stage(Stage::CodeGeneration)->ran,
            "with code generation deliberately skipped, not silently attempted");

    const fs::path missing = fs::temp_directory_path() / "zl_pipeline_missing" / "Absent.zl";
    const Result missingResult = compileWhole(missing, Options{});
    require(!missingResult.ok(), "a missing entry file should fail");
    require(missingResult.failure.kind == ErrorKind::Module ||
                missingResult.failure.kind == ErrorKind::Environment,
            "and be classified as a module or environment failure");
    require(zl::pipeline::exitCodeFor(missingResult.failure.kind) == 1,
            "a missing file is an ordinary failure, not a MIR one");
}

void testExitCodeMapping() {
    require(zl::pipeline::exitCodeFor(ErrorKind::None) == 0, "success exits 0");
    require(zl::pipeline::exitCodeFor(ErrorKind::Syntax) == 1, "syntax errors exit 1");
    require(zl::pipeline::exitCodeFor(ErrorKind::Module) == 1, "module errors exit 1");
    require(zl::pipeline::exitCodeFor(ErrorKind::TypeCheck) == 1, "type errors exit 1");
    require(zl::pipeline::exitCodeFor(ErrorKind::Usage) == 2, "usage errors exit 2");
    require(zl::pipeline::exitCodeFor(ErrorKind::Verification) == 4, "invalid MIR exits 4");
    require(zl::pipeline::exitCodeFor(ErrorKind::Optimization) == 4, "a broken pass exits 4");
    require(zl::pipeline::exitCodeFor(ErrorKind::Backend) == 4, "a refusing backend exits 4");
    require(zl::pipeline::exitCodeFor(ErrorKind::Unsupported) == 6, "unsupported lowering exits 6");
    require(zl::pipeline::exitCodeFor(ErrorKind::Environment) == 1, "environment failures exit 1");
    for (int i = 0; i <= static_cast<int>(ErrorKind::Usage); ++i) {
        const auto kind = static_cast<ErrorKind>(i);
        require(zl::pipeline::errorKindName(kind) != std::string("unknown"),
                "every error kind should have a name");
        if (kind != ErrorKind::None) {
            require(std::string(zl::pipeline::errorKindLabel(kind)).size() > 0,
                    "every failure should have a message label");
        }
    }
}

void testBackendParsing() {
    Backend backend = Backend::Native;
    require(zl::pipeline::parseBackend("bytecode", backend) && backend == Backend::Bytecode,
            "'bytecode' should parse");
    require(zl::pipeline::parseBackend("NATIVE", backend) && backend == Backend::Native,
            "backend names should be case-insensitive");
    require(!zl::pipeline::parseBackend("gpu", backend),
            "an unknown backend must be refused, not defaulted");
    require(!zl::pipeline::parseBackend("", backend), "an empty backend name is not a backend");
    require(std::string(zl::pipeline::backendName(Backend::Bytecode)) == "bytecode",
            "the bytecode backend has a stable name");
    require(std::string(zl::pipeline::backendName(Backend::Native)) == "native",
            "the native backend has a stable name");
}

// ---------------------------------------------------------------------------
// The boundary digest
// ---------------------------------------------------------------------------

void testDigestIsStableAndSensitive() {
    const fs::path file = writeProgram("Pipe", kProgram);

    const Result first = compileWhole(file, Options{});
    const Result second = compileWhole(file, Options{});
    require(first.ok() && second.ok(), "both runs should compile");
    require(first.mirDigest() == second.mirDigest(),
            "lowering the same source twice must produce the same module");

    const std::string other =
        "class Other {\n"
        "    func main(): void {\n"
        "        log(\"other\")\n"
        "    }\n"
        "}\n";
    const Result different = compileWhole(writeProgram("Other", other), Options{});
    require(different.ok(), "the second program should compile");
    require(different.mirDigest() != first.mirDigest(),
            "different programs must produce different digests");

    require(zl::pipeline::moduleDigest(first.module) == first.mirDigest(),
            "the digest is a pure function of the module");

    // The digest must be sensitive to program structure, not only to names: two
    // programs with the same shape but a different constant must not collide.
    const std::string tweaked =
        "class Pipe2 {\n"
        "    static func add(int a, int b): int {\n"
        "        return a + b\n"
        "    }\n"
        "    func main(): void {\n"
        "        var total = 7\n"
        "        for i in 0..4 {\n"
        "            total = Pipe2.add(total, i * 2)\n"
        "        }\n"
        "        log(\"total=\" + total)\n"
        "    }\n"
        "}\n";
    const Result tweakedResult = compileWhole(writeProgram("Pipe2", tweaked), Options{});
    require(tweakedResult.ok(), "the tweaked program should compile");
    require(tweakedResult.mirDigest() != first.mirDigest(),
            "an initial constant is part of the digest");

    // Callee identity: two modules whose only difference is *which* of two
    // identical functions a call names - same signatures, same operands, same
    // shape - must not collide.
    const std::string callFirst =
        "class Calls {\n"
        "    static func first(int v): int { return v }\n"
        "    static func second(int v): int { return v }\n"
        "    func main(): void { log(Calls.first(3)) }\n"
        "}\n";
    const std::string callSecond =
        "class Calls {\n"
        "    static func first(int v): int { return v }\n"
        "    static func second(int v): int { return v }\n"
        "    func main(): void { log(Calls.second(3)) }\n"
        "}\n";
    const Result firstCall = compileWhole(writeProgram("Calls", callFirst), Options{});
    const Result secondCall = compileWhole(writeProgram("Calls", callSecond), Options{});
    require(firstCall.ok() && secondCall.ok(), "both call programs should compile");
    require(firstCall.mirDigest() != secondCall.mirDigest(),
            "which function a call names is part of the digest");
}

// ---------------------------------------------------------------------------
// Errors and diagnostics surface through the result, not through exceptions
// ---------------------------------------------------------------------------

void testDiagnosticsAreData() {
    const fs::path file = writeProgram("Pipe", kProgram);

    Pipeline pipeline(Options{});
    (void)pipeline.run(file, {});

    // The result carries everything the CLI needs to report a failure or a
    // success without re-deriving it from an exception.
    require(pipeline.result().program != nullptr, "the program should be kept for diagnostics");
    require(pipeline.result().checker != nullptr, "and the checker that analysed it");
    require(!pipeline.result().module.functions.empty(), "the module is the boundary value");
    require(pipeline.result().failure.ok(), "no failure was recorded");
    require(pipeline.result().failure.message.empty(), "and none is described");

    // Printing the module is a pure read used by --emit-mir.
    const std::string text = zl::mir::printModule(pipeline.result().module);
    require(text.find("func Pipe.main()") != std::string::npos,
            "the module should contain the program's functions");
}

// ---------------------------------------------------------------------------
// The reference compiler emits no bytes the VM cannot reach
// ---------------------------------------------------------------------------

// Every function body used to be followed by an implicit `push nil; return`,
// including a body that already ended in `return` or `throw` - a Return does
// not fall through, so those instructions were unreachable. The tail is now
// emitted only where control flow can actually reach the end of the body, and
// this pins both directions: a body that cannot fall through carries exactly
// the returns its source has, and a body that can still carries the implicit
// one (without it the VM would run off the end of the chunk).
const char* kTailProgram =
    "class Tail {\n"
    "    static func doubled(int x): int {\n"
    "        return x * 2\n"
    "    }\n"
    "    static func branchy(int x): int {\n"
    "        if (x > 0) { return 1 } else { return 0 }\n"
    "    }\n"
    "    static func guarded(int x): int {\n"
    "        if (x < 0) { throw new Exception(\"negative\") }\n"
    "        return x\n"
    "    }\n"
    "    static func quiet(): void {\n"
    "        log(\"quiet\")\n"
    "    }\n"
    "    static func partial(int x): void {\n"
    "        if (x > 0) { return }\n"
    "        log(\"fell through\")\n"
    "    }\n"
    "    func main(): void {\n"
    "        var addOne = func(int y): int { return y + 1 }\n"
    "        log(Tail.doubled(2))\n"
    "        log(Tail.branchy(-1))\n"
    "        log(Tail.guarded(3))\n"
    "        Tail.quiet()\n"
    "        Tail.partial(0)\n"
    "        log(addOne(4))\n"
    "    }\n"
    "}\n";

std::size_t countReturns(const zl::Chunk& chunk, std::size_t begin, std::size_t end) {
    std::size_t returns = 0;
    for (std::size_t i = begin; i < end && i < chunk.code.size(); ++i) {
        if (chunk.code[i].op == zl::OpCode::Return) ++returns;
    }
    return returns;
}

// A named function's body is [its entry, the next entry after it): pass 2 hands
// out entry addresses in emission order. The functions measured here contain no
// lambdas, whose entries sit inside their enclosing body.
std::pair<std::size_t, std::size_t> namedFunctionRange(const zl::Chunk& chunk,
                                                       const std::string& namePrefix) {
    const zl::FunctionInfo* target = nullptr;
    for (const auto& fn : chunk.functions) {
        if (fn.name.rfind(namePrefix, 0) == 0) { target = &fn; break; }
    }
    if (target == nullptr) return {0, 0};
    std::size_t end = chunk.code.size();
    for (const auto& fn : chunk.functions) {
        if (fn.entryAddress > target->entryAddress && fn.entryAddress < end) end = fn.entryAddress;
    }
    return {target->entryAddress, end};
}

// A lambda's body ends at the MakeClosure that closes over it, which the
// compiler emits immediately after the body's last instruction.
std::pair<std::size_t, std::size_t> lambdaRange(const zl::Chunk& chunk, const std::string& namePrefix) {
    std::size_t index = chunk.functions.size();
    std::size_t entry = 0;
    for (std::size_t i = 0; i < chunk.functions.size(); ++i) {
        if (chunk.functions[i].name.rfind(namePrefix, 0) == 0) {
            index = i;
            entry = chunk.functions[i].entryAddress;
            break;
        }
    }
    if (index == chunk.functions.size()) return {0, 0};
    for (std::size_t i = entry; i < chunk.code.size(); ++i) {
        if (chunk.code[i].op == zl::OpCode::MakeClosure && chunk.code[i].operand == index) {
            return {entry, i};
        }
    }
    return {entry, chunk.code.size()};
}

void testUnreachableReturnTailIsNotEmitted() {
    const fs::path file = writeProgram("Tail", kTailProgram);

    std::unique_ptr<zl::Program> program;
    {
        zl::ModuleLoader loader(file, std::vector<fs::path>{});
        program = loader.load();
        zl::TypeChecker checker;
        checker.check(*program, /*requireMain=*/true);
    }
    zl::Compiler compiler;
    const zl::Chunk chunk = compiler.compile(*program);

    // `return x * 2` is the whole body: one return, and no tail after it.
    const auto doubled = namedFunctionRange(chunk, "Tail.doubled");
    require(doubled.first != doubled.second, "the fixture's doubled() should be in the chunk");
    require(countReturns(chunk, doubled.first, doubled.second) == 1,
            "a body that ends in `return expr` emits that return and nothing after it");

    // Both arms return, so neither path reaches the end of the body.
    const auto branchy = namedFunctionRange(chunk, "Tail.branchy");
    require(countReturns(chunk, branchy.first, branchy.second) == 2,
            "an if/else whose arms both return emits one return per arm and no tail");

    // A `throw` before the only return: the return is still the last exit.
    const auto guarded = namedFunctionRange(chunk, "Tail.guarded");
    require(countReturns(chunk, guarded.first, guarded.second) == 1,
            "a body ending in `return` after a conditional throw emits no tail");

    // The conservative direction: these bodies do fall through, so the
    // implicit `return nil` is load-bearing and must still be there.
    const auto quiet = namedFunctionRange(chunk, "Tail.quiet");
    require(countReturns(chunk, quiet.first, quiet.second) == 1,
            "a body with no return still gets the implicit one");

    const auto partial = namedFunctionRange(chunk, "Tail.partial");
    require(countReturns(chunk, partial.first, partial.second) == 2,
            "an `if` without an `else` can fall through, so the implicit return stays");

    // The same rule applies to a block-body lambda: its explicit return is the
    // last instruction before MakeClosure.
    const auto lambda = lambdaRange(chunk, "$lambda");
    require(lambda.first != lambda.second, "the fixture's block-body lambda should be in the chunk");
    require(countReturns(chunk, lambda.first, lambda.second) == 1,
            "a block-body lambda ending in `return expr` emits no unreachable tail");

    // None of this may change what the program does.
    const RunResult run = runChunk(chunk);
    require(run.ran, "the fixture should still execute");
    require(run.output == "4\n0\n3\nquiet\nfell through\n5\n",
            "the fixture prints the same values as before the tail was dropped: " + run.output);
}

// ---------------------------------------------------------------------------
// String methods are the String.* natives, not a second implementation
// ---------------------------------------------------------------------------

// `s.length()` resolves to the catalog entry `String.length` with the receiver
// bound as that native's first argument, so the method spelling is not a
// different code path from the qualified one and must not become one. The two
// programs below write the same values through the two spellings; they are
// compiled and run independently and their output must be identical. The
// bytecode is then checked directly - the method spelling emits exactly a
// CallNative of the native it maps to - and a typo has to come back as a type
// error naming the surface, which is where a user meets the surface first.
const char* kStringMethodCalls =
    "class StringMethodCalls {\n"
    "    func main(): void {\n"
    "        var s = \"Hello, World\"\n"
    "        log(s.length())\n"
    "        log(s.upper())\n"
    "        log(s.contains(\"World\"))\n"
    "        log(s.startsWith(\"Hello\"))\n"
    "        log(s.endsWith(\"World\"))\n"
    "        log(s.indexOf(\"World\"))\n"
    "        log(s.substring(0, 5))\n"
    "        log(s.replace(\"World\", \"ZL\"))\n"
    "        log(Collection.get(s.split(\", \"), 1))\n"
    "        log(\"42\".toInt() + 1)\n"
    "        log(\"  pad  \".trimStart() + s.trimEnd())\n"
    "    }\n"
    "}\n";

const char* kStringNativeCalls =
    "class StringNativeCalls {\n"
    "    func main(): void {\n"
    "        var s = \"Hello, World\"\n"
    "        log(String.length(s))\n"
    "        log(String.upper(s))\n"
    "        log(String.contains(s, \"World\"))\n"
    "        log(String.startsWith(s, \"Hello\"))\n"
    "        log(String.endsWith(s, \"World\"))\n"
    "        log(String.indexOf(s, \"World\"))\n"
    "        log(String.substring(s, 0, 5))\n"
    "        log(String.replace(s, \"World\", \"ZL\"))\n"
    "        log(Collection.get(String.split(s, \", \"), 1))\n"
    "        log(String.toInt(\"42\") + 1)\n"
    "        log(String.trimStart(\"  pad  \") + String.trimEnd(s))\n"
    "    }\n"
    "}\n";

const char* kStringMethodTypo =
    "class StringMethodTypo {\n"
    "    func main(): void {\n"
    "        var s = \"abc\"\n"
    "        log(s.trimLeft())\n"
    "    }\n"
    "}\n";

const char* kStringMethodArity =
    "class StringMethodArity {\n"
    "    func main(): void {\n"
    "        var s = \"abc\"\n"
    "        log(s.length(1))\n"
    "    }\n"
    "}\n";

const char* kStringMethodArgument =
    "class StringMethodArgument {\n"
    "    func main(): void {\n"
    "        var s = \"abc\"\n"
    "        log(s.contains(3))\n"
    "    }\n"
    "}\n";

// Counts CallNative instructions targeting a catalog entry inside a function
// body, so "the method spelling is this native" is checked against the emitted
// code rather than against the source's intent.
std::size_t countNativeCalls(const zl::Chunk& chunk, std::size_t begin, std::size_t end,
                             const std::string& qualifiedName) {
    const auto target = zl::findNativeFunction(qualifiedName);
    if (!target) return 0;
    std::size_t calls = 0;
    for (std::size_t i = begin; i < end && i < chunk.code.size(); ++i) {
        if (chunk.code[i].op == zl::OpCode::CallNative && chunk.code[i].operand == *target) ++calls;
    }
    return calls;
}

// The MIR text of one function, from its header to the next one - enough to say
// what a single body lowered to without reading the builtin library's own.
std::string mirFunctionBody(const std::string& module, const std::string& header) {
    const auto start = module.find(header);
    if (start == std::string::npos) return {};
    const auto end = module.find("\nfunc ", start + header.size());
    return module.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

void testStringMethodsAreTheStringNatives() {
    const fs::path calls = writeProgram("StringMethodCalls", kStringMethodCalls);
    const fs::path natives = writeProgram("StringNativeCalls", kStringNativeCalls);

    Options both;
    both.backend = Backend::Bytecode;
    const Result viaMethods = compileWhole(calls, both);
    const Result viaNatives = compileWhole(natives, both);
    require(viaMethods.ok() && viaMethods.chunk.has_value(),
            "a program using string methods should compile");
    require(viaNatives.ok() && viaNatives.chunk.has_value(),
            "the same values through String.* should compile");

    const RunResult methodRun = runChunk(*viaMethods.chunk);
    const RunResult nativeRun = runChunk(*viaNatives.chunk);
    require(methodRun.ran && nativeRun.ran, "both spellings should execute");
    require(!methodRun.output.empty(), "the comparison should not be vacuous");
    require(methodRun.output == nativeRun.output,
            "a string method must produce exactly what the native it maps to produces: "
            "methods gave '" + methodRun.output + "', natives gave '" + nativeRun.output + "'");

    // The method spelling is that native call in the emitted bytecode: one
    // CallNative per method, and no method dispatch anywhere in the body.
    const auto body = namedFunctionRange(*viaMethods.chunk, "StringMethodCalls.main");
    require(body.first != body.second, "the fixture's main() should be in the chunk");
    require(countNativeCalls(*viaMethods.chunk, body.first, body.second, "String.length") == 1,
            "`s.length()` emits a call to the String.length native");
    require(countNativeCalls(*viaMethods.chunk, body.first, body.second, "String.startsWith") == 1,
            "`s.startsWith(p)` emits a call to the String.startsWith native");
    require(countNativeCalls(*viaMethods.chunk, body.first, body.second, "String.trimEnd") == 1,
            "`s.trimEnd()` emits a call to the String.trimEnd native");
    require(countNativeCalls(*viaMethods.chunk, body.first, body.second, "String.split") == 1,
            "`s.split(sep)` emits a call to the String.split native");

    // And the MIR names the same entry, with no invoke_method in the body.
    const std::string mirBody = mirFunctionBody(zl::mir::printModule(viaMethods.module),
                                               "func StringMethodCalls.main()");
    require(!mirBody.empty(), "the method body should be printable");
    require(mirBody.find("native 'String.length'") != std::string::npos,
            "the MIR should name String.length for the method spelling");
    require(mirBody.find("invoke_method") == std::string::npos,
            "a string method should not lower to a method dispatch");

    // The reference compiler runs the surface identically, so it is not a
    // MIR-only path.
    std::unique_ptr<zl::Program> program;
    {
        zl::ModuleLoader loader(calls, std::vector<fs::path>{});
        program = loader.load();
        zl::TypeChecker checker;
        checker.check(*program, /*requireMain=*/true);
    }
    zl::Compiler referenceCompiler;
    const RunResult reference = runChunk(referenceCompiler.compile(*program));
    require(reference.ran && reference.output == methodRun.output,
            "the reference compiler must run the surface identically");

    // A method outside the surface names the surface in the diagnostic, rather
    // than reporting a method call on a type that has none.
    const Result typo = compileWhole(writeProgram("StringMethodTypo", kStringMethodTypo), Options{});
    require(!typo.ok(), "an unknown string method should not compile");
    require(typo.failure.kind == ErrorKind::TypeCheck, "and should be classified as a type error");
    require(typo.failure.message.find("has no method 'trimLeft'") != std::string::npos &&
                typo.failure.message.find("startsWith") != std::string::npos &&
                typo.failure.message.find("utf8Reverse") != std::string::npos,
            "the diagnostic should name the unknown method and list the surface: " +
                typo.failure.message);

    // Arity and argument types are checked against the catalog entry at
    // compile time, so a bad call never reaches the native's own contract.
    const Result arity = compileWhole(writeProgram("StringMethodArity", kStringMethodArity), Options{});
    require(!arity.ok() && arity.failure.kind == ErrorKind::TypeCheck,
            "a string method arity mismatch should be a type error");
    require(arity.failure.message.find("string method 'length' expects 0 argument(s)") != std::string::npos,
            "the arity diagnostic should name the method and the count: " + arity.failure.message);
    const Result argument = compileWhole(writeProgram("StringMethodArgument", kStringMethodArgument), Options{});
    require(!argument.ok() && argument.failure.kind == ErrorKind::TypeCheck,
            "a string method argument type mismatch should be a type error");
    require(argument.failure.message.find("argument 1 to string method 'contains'") != std::string::npos &&
                argument.failure.message.find("expected string") != std::string::npos,
            "the argument diagnostic should name the method and the expected type: " +
                argument.failure.message);
}

} // namespace

int main() {
    testStageOrderAndLedger();
    testSkippedStagesAreRecorded();
    testOutOfOrderStagesAreRefused();
    testPartialPipelineStopsWhereAsked();
    testCodeGenerationRequiresVerification();
    testMutationAfterVerificationIsCaught();
    testNativeLedgerAndStrictMode();
    testReachabilityIsSoundAndUseful();
    testReachabilityRefusesToCloseAGraphWithReflection();
    testReachabilityOnALibrary();
    testDirectCallsKeepTheGraphClosed();
    testKnownClosureCallsAreAccounted();
    testUnresolvedIndirectCallsLeaveTheGraphOpen();
    testStoredFunctionValuesFollowTheSameRule();
    testMixedKnownAndUnknownIndirectTargets();
    testPipelineReportRequiresAProgram();
    testBackendSelectionKeepsMirIdentical();
    testBackendChoiceDoesNotChangeBehaviour();
    testOptimizationIsObservational();
    testOptimizationDifferentialCheckIsStageOption();
    testPromotionIsAFormChangeOnly();
    testFrontendFailuresAreClassified();
    testExitCodeMapping();
    testBackendParsing();
    testDigestIsStableAndSensitive();
    testDiagnosticsAreData();
    testUnreachableReturnTailIsNotEmitted();
    testStringMethodsAreTheStringNatives();

    if (failures == 0) {
        std::cout << "compiler pipeline: " << checks << " checks passed\n";
        return 0;
    }
    std::cerr << "compiler pipeline: " << failures << " of " << checks << " checks failed\n";
    return 1;
}
