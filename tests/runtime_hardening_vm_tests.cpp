// Hardening regressions for the VM execution path, written as a set of
// guard rails around the fixes Objectives 1-5 introduced:
//
//   * the compiler boundary is a check, not a claim: the include-boundary
//     lint (tools/boundary_lint.sh) must pass, and the pipeline must refuse
//     to code-generate a module that was never verified;
//   * ReachabilityReport::complete() is a removal-safety guarantee, and the
//     regression for every edge that opens the graph (unresolved function
//     values, reflection by name) is driven through the *full* pipeline -
//     real ZL source, real lowering, real MIR - not a hand-built module;
//   * an unresolved report is a lower bound, not a rejection: the VM still
//     executes such programs correctly;
//   * virtual and interface dispatch, and deep extends chains, run on the
//     VM with the right receiver, and dispatch keeps the reachability
//     report complete (a closed, over-approximated edge);
//   * a library with no main is refused explicitly (and accepted in library
//     mode), never silently skipped.
//
// Every program below is real ZL source written to a temp file, compiled by
// the real pipeline, and (where runnable) executed on the real VM with
// stdout captured.

#include "zl/compiler/pipeline.hpp"
#include "zl/mir/reachability.hpp"
#include "zl/vm/vm.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace zl::pipeline;
namespace fs = std::filesystem;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "hardening regression: " << message << '\n';
    ++failures;
}

struct RunResult {
    bool compiled{false};
    Result result{};
    bool ran{false};
    int exitCode{0};
    std::string output;
    std::string error;
};

// Writes `source` to a scratch file named after `name` (ZL requires the
// primary type to be named after the file) and runs it through the pipeline
// and the VM.
RunResult runZl(const std::string& name, const std::string& source, Options options = {}) {
    RunResult out;
    const fs::path directory = fs::temp_directory_path() / ("zl_hardening_" + name);
    fs::create_directories(directory);
    const fs::path file = directory / (name + ".zl");
    {
        std::ofstream outStream(file);
        outStream << source;
    }
    out.result = compile(file, std::vector<fs::path>{}, std::move(options));
    out.compiled = out.result.ok();
    if (!out.compiled) return out;
    if (!out.result.chunk.has_value()) {
        out.error = "pipeline succeeded without a bytecode chunk";
        return out;
    }
    std::ostringstream captured;
    std::streambuf* previous = std::cout.rdbuf(captured.rdbuf());
    try {
        // VM is enable_shared_from_this: heap-allocate so Await / async
        // resumption can take a shared owner, and so a large ExecutionState
        // does not sit on the 1 MiB Windows thread stack.
        auto vm = std::make_shared<zl::VM>();
        auto chunk = std::make_shared<zl::Chunk>(*out.result.chunk);
        out.exitCode = vm->run(chunk, std::vector<std::string>{});
        out.ran = true;
    } catch (const std::exception& error) {
        out.error = error.what();
    }
    std::cout.rdbuf(previous);
    out.output = captured.str();
    return out;
}

// Pins the documented semantics of complete(): it is the single formula a
// removal caller may rely on, and nothing else.
void requireCompleteSemantics(const zl::mir::ReachabilityReport& report, const std::string& label) {
    const bool formula = report.hasEntryPoint && !report.dynamicEntry && !report.unresolvedCalls;
    require(report.complete() == formula,
            label + ": complete() must be exactly hasEntryPoint && !dynamicEntry && !unresolvedCalls");
}

// ---------------------------------------------------------------------------
// 1. The compiler boundary
// ---------------------------------------------------------------------------

void testBoundaryLint() {
#if defined(ZL_BOUNDARY_LINT_ROOT) && defined(ZL_BOUNDARY_LINT_BASH)
    // Quote every path: Git's bash on Windows lives under "Program Files",
    // and `std::system` goes through cmd.exe, which splits on the space.
    auto quote = [](const std::string& s) {
        if (s.find_first_of(" \t") == std::string::npos) return s;
        return std::string("\"") + s + '"';
    };
    const std::string bash = quote(ZL_BOUNDARY_LINT_BASH);
    const std::string root = std::string(ZL_BOUNDARY_LINT_ROOT);
    const std::string script = quote(root + "/tools/boundary_lint.sh");
    const std::string quotedRoot = quote(root);
    const std::string command = bash + " " + script + " " + quotedRoot;
    const int status = std::system(command.c_str());
    if (status != 0) {
        // Show the lint's own report on failure - the violation list is the
        // diagnostic the developer needs.
        std::system(command.c_str());
    }
    require(status == 0, "tools/boundary_lint.sh reports boundary violations");
#else
    std::cerr << "hardening: boundary lint skipped (no bash or lint root in this build)\n";
#endif
}

void testCodegenRefusesUnverifiedModule() {
    const char* source =
        "class HardUnverified {\n"
        "    static func add(int a, int b): int {\n"
        "        return a + b\n"
        "    }\n"
        "    func main(): void {\n"
        "        log(HardUnverified.add(1, 2))\n"
        "    }\n"
        "}\n";
    const fs::path directory = fs::temp_directory_path() / "zl_hardening_unverified";
    fs::create_directories(directory);
    const fs::path file = directory / "HardUnverified.zl";
    {
        std::ofstream outStream(file);
        outStream << source;
    }

    Options options;
    options.verify = false;
    options.verifyBeforeCodegen = false;
    Pipeline pipeline(std::move(options));
    require(pipeline.load(file, std::vector<fs::path>{}), "load");
    require(pipeline.analyze(), "analyze");
    require(pipeline.lowerToMir(), "lower");
    // Stage 4 deliberately skipped: the module was never verified.
    require(!pipeline.generate(), "stage 6 must refuse to run on an unverified module");
    const Result& result = pipeline.result();
    require(result.failure.kind == ErrorKind::Invariant,
            "the refusal is recorded as a pipeline invariant, got " +
                std::string(errorKindName(result.failure.kind)));
    require(result.failure.message.find("out of order") != std::string::npos,
            "the refusal says why (got: " + result.failure.message + ")");
    require(!result.chunk.has_value(), "no artifact exists for a refused code generation");
}

// ---------------------------------------------------------------------------
// 2. Reachability through the full pipeline: the complete() regressions
// ---------------------------------------------------------------------------

void testReachabilityClosedGraph() {
    const RunResult run = runZl("HardDirect",
        "class HardDirect {\n"
        "    static func add1(int x): int {\n"
        "        return x + 1\n"
        "    }\n"
        "    func main(): void {\n"
        "        log(HardDirect.add1(41))\n"
        "    }\n"
        "}\n");
    require(run.compiled, "the direct-call program compiles: " + run.result.failure.message);
    if (!run.compiled) return;
    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(run.result.module);
    require(report.hasEntryPoint, "a runnable program has an entry point");
    require(!report.unresolvedCalls, "direct calls never open the graph");
    require(!report.dynamicEntry, "no reflection in this program");
    require(report.complete(), "a direct-call graph is a removal guarantee");
    requireCompleteSemantics(report, "direct-call report");
    require(run.ran && run.exitCode == 0, "and it runs: " + run.error);
    require(run.output == "42\n", "and it runs correctly (got: " + run.output + ")");
}

void testReachabilityUnresolvedFunctionValue() {
    const RunResult run = runZl("HardFuncParam",
        "class HardFuncParam {\n"
        "    static func twice(func f, int x): int {\n"
        "        return f(x)\n"
        "    }\n"
        "    func main(): void {\n"
        "        log(HardFuncParam.twice(func(n) => n + 1, 41))\n"
        "    }\n"
        "}\n");
    require(run.compiled, "the function-parameter program compiles: " + run.result.failure.message);
    if (!run.compiled) return;
    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(run.result.module);
    require(report.hasEntryPoint, "a runnable program has an entry point");
    require(report.unresolvedCalls,
            "calling through a func parameter is an unresolved execute edge");
    require(report.unresolvedCallCount >= 1, "every unresolved site is counted");
    require(!report.unresolvedCallReason.empty(), "the report names the open edge");
    require(!report.complete(),
            "an unresolved call means the report is a lower bound, not a removal guarantee");
    requireCompleteSemantics(report, "function-value report");
    // A lower bound is not a rejection: the VM still executes the program,
    // correctly.
    require(run.ran && run.exitCode == 0, "an open graph still runs: " + run.error);
    require(run.output == "42\n", "an open graph still runs correctly (got: " + run.output + ")");
}

void testReachabilityReflectionOpensGraph() {
    const RunResult run = runZl("HardReflect",
        "class HardReflect {\n"
        "    public func shout(): string {\n"
        "        return \"shout\"\n"
        "    }\n"
        "    func main(): void {\n"
        "        var self = new HardReflect()\n"
        "        var t = Type.of(self)\n"
        "        log(t.method(\"shout\").invoke(self, []))\n"
        "    }\n"
        "}\n");
    require(run.compiled, "the reflection program compiles: " + run.result.failure.message);
    if (!run.compiled) return;
    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(run.result.module);
    require(report.dynamicEntry,
            "a reflective invoke enters code by name, which opens the graph");
    require(!report.dynamicEntryReason.empty(), "the report names the reflective native");
    require(!report.complete(), "reflection means the report is a lower bound");
    requireCompleteSemantics(report, "reflection report");
    require(run.ran && run.exitCode == 0, "a reflective program still runs: " + run.error);
    require(run.output == "shout\n", "and it runs correctly (got: " + run.output + ")");
}

// ---------------------------------------------------------------------------
// 3. Libraries and entry points
// ---------------------------------------------------------------------------

void testLibraryWithoutMain() {
    const std::string library =
        "class HardLibrary {\n"
        "    static func add(int a, int b): int {\n"
        "        return a + b\n"
        "    }\n"
        "}\n";
    // Default options: a run needs an entry point, so this is refused -
    // explicitly, with a type-check diagnostic, not a silent skip.
    const RunResult refused = runZl("HardLibrary", library);
    require(!refused.compiled, "a program with no main() is refused under default options");
    require(refused.result.failure.kind == ErrorKind::TypeCheck,
            "the refusal is a type-check failure, got " +
                std::string(errorKindName(refused.result.failure.kind)));
    require(refused.result.failure.message.find("no main") != std::string::npos,
            "the refusal names the missing entry point (got: " +
                refused.result.failure.message + ")");
    require(exitCodeFor(refused.result.failure.kind) == 1, "the CLI maps the refusal to exit 1");

    // Library mode: the same declarations (named for their own file) are a
    // module, and a module is the result.
    const std::string libraryRenamed =
        "class HardLibraryLib {\n"
        "    static func add(int a, int b): int {\n"
        "        return a + b\n"
        "    }\n"
        "}\n";
    Options libraryOptions;
    libraryOptions.requireMain = false;
    libraryOptions.codeGeneration = false;
    const RunResult accepted = runZl("HardLibraryLib", libraryRenamed, std::move(libraryOptions));
    require(accepted.compiled, "the same declarations compile in library mode: " +
                                   accepted.result.failure.message);
    require(!accepted.result.chunk.has_value(),
            "a library has no executable artifact");
    require(!accepted.result.module.functions.empty(),
            "the library's functions are the module");

    // A static main is not an entry point either.
    const RunResult staticMain = runZl("HardStaticMain",
        "class HardStaticMain {\n"
        "    static func main(): void {\n"
        "    }\n"
        "}\n");
    require(!staticMain.compiled, "a static main() is refused");
    require(staticMain.result.failure.message.find("instance method") != std::string::npos,
            "the refusal says main must be an instance method (got: " +
                staticMain.result.failure.message + ")");
}

// ---------------------------------------------------------------------------
// 4. Dispatch: virtual, interface, and deep hierarchies
// ---------------------------------------------------------------------------

void testVirtualDispatch() {
    const RunResult run = runZl("HardVirtual",
        "class Bird {\n"
        "    public func sound(): string {\n"
        "        return \"bird\"\n"
        "    }\n"
        "}\n"
        "class Duck extends Bird {\n"
        "    @Override\n"
        "    public func sound(): string {\n"
        "        return \"quack\"\n"
        "    }\n"
        "}\n"
        "class HardVirtual {\n"
        "    public static func announce(Bird b): string {\n"
        "        return b.sound()\n"
        "    }\n"
        "    func main(): void {\n"
        "        Bird b = new Duck()\n"
        "        log(HardVirtual.announce(b))\n"
        "        Bird plain = new Bird()\n"
        "        log(plain.sound())\n"
        "    }\n"
        "}\n");
    require(run.compiled, "the virtual-dispatch program compiles: " + run.result.failure.message);
    if (!run.compiled) return;
    require(run.ran && run.exitCode == 0, "the virtual-dispatch program runs: " + run.error);
    require(run.output == "quack\nbird\n",
            "dispatch goes to the dynamic type (got: " + run.output + ")");
    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(run.result.module);
    require(report.complete(), "virtual dispatch is a closed (over-approximated) edge");
    requireCompleteSemantics(report, "virtual-dispatch report");
}

void testInterfaceDispatch() {
    const RunResult run = runZl("HardInterface",
        "interface Greeter {\n"
        "    greet(): string\n"
        "}\n"
        "class FrenchGreeter implements Greeter {\n"
        "    @Override\n"
        "    public func greet(): string {\n"
        "        return \"salut\"\n"
        "    }\n"
        "}\n"
        "class GermanGreeter implements Greeter {\n"
        "    @Override\n"
        "    public func greet(): string {\n"
        "        return \"hallo\"\n"
        "    }\n"
        "}\n"
        "class HardInterface {\n"
        "    public static func ask(Greeter g): string {\n"
        "        return g.greet()\n"
        "    }\n"
        "    func main(): void {\n"
        "        Greeter g = new GermanGreeter()\n"
        "        log(HardInterface.ask(g))\n"
        "        log(new FrenchGreeter().greet())\n"
        "    }\n"
        "}\n");
    require(run.compiled, "the interface-dispatch program compiles: " + run.result.failure.message);
    if (!run.compiled) return;
    require(run.ran && run.exitCode == 0, "the interface-dispatch program runs: " + run.error);
    require(run.output == "hallo\nsalut\n",
            "interface-typed calls reach the implementor (got: " + run.output + ")");
    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(run.result.module);
    require(report.complete(), "interface dispatch is a closed edge");
}

void testDeepHierarchyDispatch() {
    const RunResult run = runZl("HardDeep",
        "class L1 {\n"
        "    public func name(): string {\n"
        "        return \"L1\"\n"
        "    }\n"
        "}\n"
        "class L2 extends L1 {\n"
        "    @Override\n"
        "    public func name(): string {\n"
        "        return \"L2\"\n"
        "    }\n"
        "}\n"
        "class L3 extends L2 {\n"
        "    @Override\n"
        "    public func name(): string {\n"
        "        return \"L3\"\n"
        "    }\n"
        "}\n"
        "class L4 extends L3 {\n"
        "    @Override\n"
        "    public func name(): string {\n"
        "        return \"L4\"\n"
        "    }\n"
        "}\n"
        "class L5 extends L4 {\n"
        "    @Override\n"
        "    public func name(): string {\n"
        "        return \"L5\"\n"
        "    }\n"
        "}\n"
        "class HardDeep {\n"
        "    func main(): void {\n"
        "        L1 top = new L5()\n"
        "        log(top.name())\n"
        "        L1 mid = new L3()\n"
        "        log(mid.name())\n"
        "    }\n"
        "}\n");
    require(run.compiled, "the deep-hierarchy program compiles: " + run.result.failure.message);
    if (!run.compiled) return;
    require(run.ran && run.exitCode == 0, "the deep-hierarchy program runs: " + run.error);
    require(run.output == "L5\nL3\n",
            "a top-typed variable dispatches to the deepest override (got: " + run.output + ")");
    const zl::mir::ReachabilityReport report = zl::mir::reachableFunctions(run.result.module);
    require(report.complete(), "a deep hierarchy does not open the graph");
    requireCompleteSemantics(report, "deep-hierarchy report");
}

} // namespace

int main() {
    testBoundaryLint();
    testCodegenRefusesUnverifiedModule();
    testReachabilityClosedGraph();
    testReachabilityUnresolvedFunctionValue();
    testReachabilityReflectionOpensGraph();
    testLibraryWithoutMain();
    testVirtualDispatch();
    testInterfaceDispatch();
    testDeepHierarchyDispatch();

    if (failures != 0) {
        std::cerr << failures << " hardening regression(s) failed\n";
        return 1;
    }
    std::cout << "all hardening regressions passed\n";
    return 0;
}
