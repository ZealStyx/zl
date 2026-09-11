// MIR optimiser pipeline regressions, including the runtime differential.
//
// `mir_pass_tests.cpp` builds MIR by hand and checks each pass in isolation.
// This file drives the real thing: ZL source -> type analysis -> MIR -> verify
// -> optimise -> verify, and then asks the question the verifier cannot:
//
//   **do the two modules do the same thing?**
//
// It is asked twice, because the two answers catch different bugs.
//
//   1. Statically, with `compareModules`: the observable events (output, calls,
//      heap writes, ownership events, task and thread operations) must be the
//      same before and after.
//
//   2. At run time, by lowering both modules to bytecode and executing them in
//      a VM with its output captured. This is the check that would catch a
//      wrongly removed runtime *check* - a `refine`, a `null_check`, a
//      division by zero - which the static comparison deliberately does not
//      count, because removing a check that provably cannot fail is a correct
//      optimisation and counting it would make every correct one look broken.
//
// Programs that use no imports keep the test independent of the stdlib; the
// builtin library is lowered alongside them either way.

#include "zl/compiler/module_loader.hpp"
#include "zl/compiler/type_checker.hpp"
#include "zl/mir/differential.hpp"
#include "zl/mir/lowering.hpp"
#include "zl/mir/passes.hpp"
#include "zl/mir/printer.hpp"
#include "zl/mir/verifier.hpp"
#include "zl/mir/vm_backend.hpp"
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
    std::cerr << "mir optimiser pipeline regression: " << message << '\n';
    ++failures;
}

using namespace zl::mir;

struct Lowered {
    Module module;
    bool ok{false};
    std::string errors;
};

// Writes `source` to a scratch file and runs the front end over it.
Lowered lower(const std::string& className, const std::string& source) {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("zl_mir_opt_" + className);
    fs::create_directories(directory);
    // ZL's module system requires the primary type to be named after the file,
    // so the file is named after the class the program declares.
    const fs::path file = directory / (className + ".zl");
    {
        std::ofstream out(file);
        out << source;
    }

    Lowered result;
    try {
        zl::ModuleLoader loader(file, std::vector<fs::path>{});
        auto program = loader.load();
        zl::TypeChecker checker;
        checker.check(*program, /*requireMain=*/false);
        auto lowered = lowerProgram(*program, checker);
        const VerificationReport report = verifyModule(lowered.module);
        result.errors = report.ok() ? std::string{} : report.describe();
        result.ok = report.ok();
        result.module = std::move(lowered.module);
    } catch (const std::exception& e) {
        result.errors = std::string("exception: ") + e.what();
    }
    return result;
}

struct RunResult {
    int exitCode{0};
    std::string output;
    std::string error;
    bool ran{false};
};

// Lowers a module to bytecode and runs it, capturing everything it prints.
RunResult runModule(const Module& module) {
    RunResult result;
    const BytecodeResult backend = compileModuleToBytecode(module);
    if (!backend.ok()) {
        for (const auto& error : backend.errors) result.error += error + "\n";
        return result;
    }
    std::ostringstream captured;
    std::streambuf* previous = std::cout.rdbuf(captured.rdbuf());
    try {
        zl::VM vm;
        result.exitCode = vm.run(backend.chunk, std::vector<std::string>{});
        result.ran = true;
    } catch (const zl::SystemExitException& e) {
        result.exitCode = e.code;
        result.ran = true;
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    std::cout.rdbuf(previous);
    result.output = captured.str();
    return result;
}

// The whole argument, for one program: lower it, optimise it, and check that
// the optimised version verifies, is smaller, observes the same thing
// statically, and behaves identically when it runs.
void checkProgram(const std::string& className, const std::string& source,
                   bool expectSmaller = true) {
    // Progress on stderr: with the whole builtin library lowered alongside the
    // program, one case is a few thousand functions, and a hang has to say
    // where it hung.
    std::cerr << "[mir-opt] " << className << ": lowering...\n";
    const Lowered lowered = lower(className, source);
    require(lowered.ok, className + ": the unoptimised MIR verifies (" + lowered.errors + ")");
    if (!lowered.ok) return;

    std::cerr << "[mir-opt] " << className << ": optimising...\n";

    Module optimized = lowered.module;
    OptimizationOptions options;
    options.keepSnapshots = false;
    const OptimizationReport report = optimizeModule(optimized, options);

    require(report.verifiedAtEnd && report.finalVerification.ok(),
            className + ": the optimised MIR verifies (" + report.finalVerification.describe() + ")");
    require(report.rollbacks == 0, className + ": no pass had to be rolled back");

    std::cerr << "[mir-opt] " << className << ": comparing (static)...\n";
    const DifferentialResult differential = compareModules(lowered.module, optimized);
    require(differential.equivalent, className + ": " + differential.describe());
    if (!differential.equivalent) {
        for (const auto& mismatch : differential.mismatches)
            std::cerr << "    " << mismatch.describe() << '\n';
    }
    if (expectSmaller) {
        require(differential.instructionsAfter < differential.instructionsBefore,
                className + ": the optimiser found something to remove (" +
                    std::to_string(differential.instructionsBefore) + " -> " +
                    std::to_string(differential.instructionsAfter) + ")");
    }

    // The runtime differential: the same program, both ways.
    std::cerr << "[mir-opt] " << className << ": running both versions...\n";
    const RunResult before = runModule(lowered.module);
    const RunResult after = runModule(optimized);
    require(before.ran, className + ": the unoptimised module runs (" + before.error + ")");
    require(after.ran, className + ": the optimised module runs (" + after.error + ")");
    require(before.output == after.output,
            className + ": identical output\n--- unoptimised ---\n" + before.output +
                "--- optimised ---\n" + after.output);
    require(before.exitCode == after.exitCode,
            className + ": identical exit code (" + std::to_string(before.exitCode) + " vs " +
                std::to_string(after.exitCode) + ")");
    (void)report;
}

} // namespace

int main() {
    // --- constant folding: arithmetic the source spelled out ---------------
    checkProgram("Fold", R"(
class Fold {
    func main(): void {
        var a = 2 + 3
        var b = a * 4
        var c = 100 / 7
        log("a=" + a)
        log("b=" + b)
        log("c=" + c)
    }
}
)");

    // --- a branch decided by constants -------------------------------------
    checkProgram("Decided", R"(
class Decided {
    func main(): void {
        if (1 < 2) {
            log("always")
        } else {
            log("never")
        }
        var n = 0
        if (n > 10) {
            log("unreachable")
        }
        log("done")
    }
}
)");

    // --- algebraic identities on a value that is not constant --------------
    checkProgram("Identities", R"(
class Identities {
    static func twice(int n): int {
        return n + n
    }
    func main(): void {
        var n = 21
        var a = n * 1
        var b = n + 0
        var c = n - n
        var d = n / 1
        log("a=" + a)
        log("b=" + b)
        log("c=" + c)
        log("d=" + d)
        log("t=" + Identities.twice(4))
    }
}
)");

    // --- a loop: dead blocks, dead values, and a live counter --------------
    checkProgram("Loop", R"(
class Loop {
    func main(): void {
        var total = 0
        var i = 0
        while (i < 5) {
            total = total + i
            i = i + 1
        }
        log("total=" + total)
    }
}
)");

    // --- strings, concatenation and a call --------------------------------
    checkProgram("Strings", R"(
class Strings {
    static func greet(string name): string {
        return "hello, " + name
    }
    func main(): void {
        log(Strings.greet("world"))
        log("literal " + "concat")
    }
}
)");

    // --- ownership: a drop must survive optimisation untouched ------------
    checkProgram("Ownership", R"(
class Ownership {
    func main(): void {
        var message = "kept"
        log(message)
    }
}
)");

    // --- an exception that must still be raised ---------------------------
    // A division by zero is not folded, because folding it would replace a
    // thrown error with a value. The runtime differential is what proves the
    // optimiser kept it.
    checkProgram("StillRaises", R"(
class StillRaises {
    func main(): void {
        var zero = 0
        var value = 10
        try {
            var result = value / zero
            log("not reached " + result)
        } catch Exception e {
            log("caught")
        }
        log("after")
    }
}
)", /*expectSmaller=*/false);

    // --- a closure that mutates what it captured --------------------------
    // ZL captures by value, and a captured `var` a closure mutates persists
    // between calls - so the store that updates it looks dead ("nothing reads
    // this slot again") and is not. This program is the one that caught that
    // bug in the runtime differential; it stays here so it cannot come back.
    checkProgram("Closure", R"(
class Closure {
    static func makeCounter(): func {
        var count = 0
        var inc = func() {
            count = count + 1
            return count
        }
        return inc
    }
    func main(): void {
        var c = Closure.makeCounter()
        log(c())
        log(c())
        log(c())
        var other = Closure.makeCounter()
        log(other())
    }
}
)");

    // --- a program the optimiser has nothing to say about -----------------
    // Included so the differential is exercised on the identity case too: no
    // change must be reported as no change, and the two runs must still agree.
    checkProgram("Nothing", R"(
class Nothing {
    func main(): void {
        var n = 0
        var i = 0
        while (i < 3) {
            n = n + i * i
            i = i + 1
        }
        log("n=" + n)
    }
}
)", /*expectSmaller=*/false);

    std::cout << "mir optimiser pipeline regressions: " << checks << " checks, " << failures
              << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
