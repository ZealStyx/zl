#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "zl/compiler/ir.hpp"

namespace zl::machine {

struct FunctionCode {
    std::string name;
    std::vector<std::uint8_t> bytes;
    std::size_t stackSize{0};
    std::size_t parameterCount{0};
};

struct CompileResult {
    bool success{false};
    std::vector<FunctionCode> functions;
    std::string error;
};

// Phase 19.1: a deliberately small, real x86-64 machine-code backend.
// It consumes backend-neutral MIR directly and rejects instructions outside
// the currently proven-safe straight-line integer subset.
[[nodiscard]] CompileResult emitX64(const ir::Module& module);

} // namespace zl::machine
