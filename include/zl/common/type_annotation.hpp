#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "zl/parser/ast.hpp"
#include "zl/common/type_name.hpp"

namespace zl {

// Canonical source-level rendering used by runtime reflection metadata. Keeping
// the full generic/function shape here prevents compiler/runtime reflection
// boundaries from silently collapsing `list<int>` into `list`.
[[nodiscard]] inline std::string describeTypeAnnotation(const TypeAnnotation& t) {
    if (!t.unionOf.empty()) {
        std::vector<std::string> members;
        std::function<void(const TypeAnnotation&)> flatten = [&](const TypeAnnotation& member) {
            if (!member.unionOf.empty()) {
                for (const auto& child : member.unionOf) flatten(child);
            } else {
                std::string name = describeTypeAnnotation(member);
                if (member.name == "func" && member.functionHasSignature) name = "(" + name + ")";
                members.push_back(std::move(name));
            }
        };
        flatten(t);
        std::sort(members.begin(), members.end());
        members.erase(std::unique(members.begin(), members.end()), members.end());
        std::string out;
        for (const auto& name : members) { if (!out.empty()) out += "|"; out += name; }
        return out;
    }
    if (t.name == "func" && t.functionHasSignature) {
        std::string out = "func(";
        for (std::size_t i = 0; i < t.functionParamTypes.size(); ++i) {
            if (i) out += ",";
            out += describeTypeAnnotation(t.functionParamTypes[i]);
        }
        out += "):";
        out += t.functionReturnType ? describeTypeAnnotation(*t.functionReturnType) : "void";
        return out;
    }
    std::string out = t.name == "float" ? "double" : t.name;
    if (!t.typeArgs.empty()) {
        out += "<";
        for (std::size_t i = 0; i < t.typeArgs.size(); ++i) {
            if (i) out += ",";
            out += describeTypeAnnotation(t.typeArgs[i]);
        }
        out += ">";
    }
    if (t.fixedSize) {
        out = "array[" + std::to_string(*t.fixedSize) + "]<" +
              (t.typeArgs.empty() ? "unknown" : describeTypeAnnotation(t.typeArgs.front())) + ">";
    }
    return out;
}

// Expected types inferred from an earlier value still provide the same literal
// and lambda context as an explicit annotation. Decode the canonical type tree
// instead of trying to reconstruct generics/functions from coarse kind tags.
inline TypeAnnotation typeAnnotationFromName(const TypeName& name) {
    TypeAnnotation type;
    type.name = name.unionMembers.empty() ? name.name : std::string{};
    for (const auto& member : name.unionMembers) type.unionOf.push_back(typeAnnotationFromName(member));
    if (name.name == "func" && !name.args.empty()) {
        type.functionHasSignature = true;
        for (std::size_t i = 0; i + 1 < name.args.size(); ++i) type.functionParamTypes.push_back(typeAnnotationFromName(name.args[i]));
        type.functionReturnType = std::make_shared<TypeAnnotation>(typeAnnotationFromName(name.args.back()));
    } else {
        for (const auto& arg : name.args) type.typeArgs.push_back(typeAnnotationFromName(arg));
    }
    if (name.fixedSize) {
        if (*name.fixedSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) throw std::runtime_error("array type size out of range");
        type.fixedSize = static_cast<int>(*name.fixedSize);
    }
    return type;
}

// Dispatch may erase a class parameter, but runtime contracts retain it.
inline bool containsTypeParameter(const TypeAnnotation& type, const std::vector<std::string>& parameters) {
    if (std::find(parameters.begin(), parameters.end(), type.name) != parameters.end()) return true;
    for (const auto& arg : type.typeArgs) if (containsTypeParameter(arg, parameters)) return true;
    for (const auto& member : type.unionOf) if (containsTypeParameter(member, parameters)) return true;
    for (const auto& param : type.functionParamTypes) if (containsTypeParameter(param, parameters)) return true;
    return type.functionReturnType && containsTypeParameter(*type.functionReturnType, parameters);
}

} // namespace zl
