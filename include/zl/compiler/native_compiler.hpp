#pragma once
#include <string>
#include <vector>
#include "zl/parser/ast.hpp"

namespace zl::native {

struct CompileResult {
    bool success{false};
    std::string source;
    std::string error;
};

[[nodiscard]] CompileResult emitSimpleCpp(const Program& program);
[[nodiscard]] CompileResult emitMirCpp(const Program& program);

}
