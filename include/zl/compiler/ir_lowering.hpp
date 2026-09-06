#pragma once
#include <optional>
#include <string>
#include "zl/compiler/ir.hpp"
#include "zl/parser/ast.hpp"

namespace zl::ir {

struct LoweringResult {
    Module module;
    std::vector<std::string> diagnostics;
    bool complete{true};
};

[[nodiscard]] LoweringResult lowerProgram(const Program& program, bool nativeOnly = false);

}
