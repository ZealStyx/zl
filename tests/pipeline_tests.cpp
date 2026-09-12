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
        zl::VM vm;
        result.exitCode = vm.run(chunk, std::vector<std::string>{});
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

    if (failures == 0) {
        std::cout << "compiler pipeline: " << checks << " checks passed\n";
        return 0;
    }
    std::cerr << "compiler pipeline: " << failures << " of " << checks << " checks failed\n";
    return 1;
}
