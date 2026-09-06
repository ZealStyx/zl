#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "zl/parser/ast.hpp"
#include "zl/compiler/semantic_types.hpp"

namespace zl {

// Program-wide discovery used by bytecode compilation. Keeping this plan
// separate from Compiler makes declaration inventory and reflection planning
// independently testable and keeps Compiler focused on bytecode emission.
struct CompilationPlan {
    std::vector<const FunctionDecl*> allFunctions;
    const FunctionDecl* mainFunction{nullptr};
    std::unordered_map<std::string, std::vector<std::string>> classTypeParams;
    std::unordered_map<std::string, std::string> classParents;
    std::unordered_map<std::string, ClassReflectionInfo> classReflection;
};

[[nodiscard]] CompilationPlan buildCompilationPlan(const Program& program);

} // namespace zl
