#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zl {
// Canonical runtime/semantic type-name grammar. It contains no VM values or AST
// nodes: compiler substitutions and runtime checks interpret exactly one shape.
struct TypeName {
    std::string name;
    std::vector<TypeName> args;
    std::vector<TypeName> unionMembers;
    std::optional<std::size_t> fixedSize;
};
TypeName parseTypeName(const std::string& text);
std::string describeTypeName(const TypeName& type);
bool typeNamesEqual(const TypeName& a, const TypeName& b);
std::string substituteTypeParams(const std::string& typeName,
                                const std::unordered_map<std::string, std::string>& bindings);
} // namespace zl
