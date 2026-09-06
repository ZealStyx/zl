#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "zl/compiler/bytecode.hpp"
#include "zl/common/dispatch_signature.hpp"
#include "zl/parser/ast.hpp"

namespace zl {

struct DispatchTableSet {
    std::unordered_map<DispatchSignature, std::size_t, DispatchSignatureHash> methodSlots;
    std::unordered_map<std::string, std::vector<std::size_t>> classVTables;
};

[[nodiscard]] DispatchSignature dispatchSignatureForFunction(
    const FunctionDecl& func, const std::vector<std::string>& genericTypeParams);

[[nodiscard]] DispatchTableSet buildDispatchTables(
    const std::vector<const FunctionDecl*>& functions,
    const std::unordered_map<std::string, std::vector<std::string>>& classTypeParams,
    const std::unordered_map<std::string, std::string>& classParents);

} // namespace zl
