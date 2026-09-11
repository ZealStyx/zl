#include "zl/native/pipeline.hpp"

#include <sstream>

#include "zl/mir/verifier.hpp"

namespace zl::native {

std::string PipelineResult::describe() const {
    std::ostringstream out;
    if (!error.empty()) out << "native backend error: " << error << "\n";
    out << "native: " << nativeFunctions.size() << " function(s) compiled, "
        << vmFunctions.size() << " left to the VM\n";
    for (const auto& name : nativeFunctions) out << "  native  " << name << "\n";
    for (const auto& rejection : vmFunctions) {
        out << "  vm      " << rejection.function << " - " << rejection.reason;
        if (rejection.line != 0) out << " (line " << rejection.line << ")";
        out << "\n";
    }
    for (const auto& import : runtimeImports) out << "  runtime " << import << "\n";
    return out.str();
}

PipelineResult compileMirToNative(const zl::mir::Module& module, const TargetMachine& target,
                                  const PipelineOptions& options) {
    PipelineResult result;

    // The backend's licence to assume MIR invariants is only worth anything if
    // something checks them, so it checks them here rather than trusting the
    // caller's word.
    const auto report = zl::mir::verifyModule(module);
    if (!report.ok()) {
        result.error = "MIR did not verify; the native backend refuses unverified input:\n" + report.describe();
        return result;
    }

    auto selection = selectModule(module, target, options.selection);
    if (!selection.ok()) {
        std::ostringstream out;
        for (const auto& e : selection.errors) out << e << "\n";
        result.error = out.str();
        return result;
    }

    result.lir = std::move(selection.module);
    result.nativeFunctions = std::move(selection.lowered);
    result.vmFunctions = std::move(selection.rejected);
    result.runtimeImports = result.lir.runtimeImports;

    if (options.selectOnly) return result;

    if (result.lir.functions.empty()) return result; // nothing to encode; not an error

    auto emitted = emitModule(result.lir, target);
    if (!emitted.success) {
        result.error = emitted.error;
        return result;
    }
    result.code = std::move(emitted.functions);
    return result;
}

} // namespace zl::native
