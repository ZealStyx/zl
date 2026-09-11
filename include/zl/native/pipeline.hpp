#pragma once

#include <string>
#include <vector>

#include "zl/mir/function.hpp"
#include "zl/native/emit.hpp"
#include "zl/native/select.hpp"
#include "zl/native/target.hpp"

// ---------------------------------------------------------------------------
// The native pipeline
// ---------------------------------------------------------------------------
//
//     ZL source -> parser -> type analysis -> MIR -> verifyModule
//                -> selectModule  (native IR)
//                -> emitModule    (machine code)
//
// This header is only the second half - MIR onwards - because the first half
// already exists and is shared with the bytecode backend. That is the point:
// the native backend is a *consumer of verified MIR*, on equal footing with
// `compileModuleToBytecode`, and it never re-derives semantics from the AST.
//
// The result is deliberately partial. A native tier that must compile
// everything before it compiles anything never ships; this one compiles the
// subset it can prove it handles and reports, per function and by name, what
// it left to the VM. `nativeFunctions` and `vmFunctions` together account for
// every function in the module, which is the property that makes a mixed-mode
// runtime possible at all.

namespace zl::native {

struct PipelineResult {
    // The native IR, for inspection and testing.
    LirModule lir;
    // Machine code for the functions in `lir`.
    std::vector<EmittedFunction> code;

    // Functions compiled natively.
    std::vector<std::string> nativeFunctions;
    // Functions left to the VM, with the reason each was left.
    std::vector<SelectionRejection> vmFunctions;
    // Runtime helpers the native code calls.
    std::vector<std::string> runtimeImports;

    // Hard failure (verification, or an encoder error). Empty on success; a
    // module with zero native functions but no error is a normal outcome.
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    // A one-screen summary of what went native and what did not.
    [[nodiscard]] std::string describe() const;
};

struct PipelineOptions {
    SelectionOptions selection;
    // Stop after selection. Useful on a host with no encoder, and for tests
    // that assert on the native IR rather than on bytes.
    bool selectOnly{false};
};

// Runs the native backend over a verified MIR module.
//
// `module` must have passed `zl::mir::verifyModule`: this function re-verifies
// it and refuses to run if it has not, because "the backend may assume the
// invariants hold" is only safe if something checked them.
[[nodiscard]] PipelineResult compileMirToNative(const zl::mir::Module& module,
                                                const TargetMachine& target,
                                                const PipelineOptions& options = {});

} // namespace zl::native
