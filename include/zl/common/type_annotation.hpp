#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "zl/parser/ast.hpp"

namespace zl {

// Canonical source-level rendering used by runtime reflection metadata. Keeping
// the full generic/function shape here prevents compiler/runtime reflection
// boundaries from silently collapsing `list<int>` into `list`.
[[nodiscard]] inline std::string describeTypeAnnotation(const TypeAnnotation& t) {
    if (!t.unionOf.empty()) {
        std::string out;
        for (std::size_t i = 0; i < t.unionOf.size(); ++i) {
            if (i) out += "|";
            out += describeTypeAnnotation(t.unionOf[i]);
        }
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
    std::string out = t.name;
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

// Whether a type annotation names one of `typeParams`. Unlike matching on the
// rendered string, this inspects the annotation tree, so a concrete class whose
// name happens to equal a parameter token elsewhere is not mistaken for one.
[[nodiscard]] inline bool typeAnnotationMentionsTypeParam(
        const TypeAnnotation& t, const std::vector<std::string>& typeParams) {
    const auto mentions = [&](const std::string& name) {
        return std::find(typeParams.begin(), typeParams.end(), name) != typeParams.end();
    };
    if (mentions(t.name)) return true;
    for (const auto& a : t.typeArgs)
        if (typeAnnotationMentionsTypeParam(a, typeParams)) return true;
    for (const auto& m : t.unionOf)
        if (typeAnnotationMentionsTypeParam(m, typeParams)) return true;
    for (const auto& p : t.functionParamTypes)
        if (typeAnnotationMentionsTypeParam(p, typeParams)) return true;
    if (t.functionReturnType && typeAnnotationMentionsTypeParam(*t.functionReturnType, typeParams))
        return true;
    return false;
}

// Runtime type assertions cannot check an unsubstituted generic type
// parameter (there is no concrete type to compare against). Render a type
// annotation for a runtime signature, erasing any part that mentions one of
// the owner class's type parameters to the "unknown" wildcard (which the VM
// treats as "no assertion"). Working from the AST - rather than scanning the
// rendered string for identifier tokens - avoids false erasure of a concrete
// type whose spelling merely coincides with a parameter name.
[[nodiscard]] inline std::string describeTypeForRuntime(
        const TypeAnnotation& t, const std::vector<std::string>& typeParams) {
    if (typeAnnotationMentionsTypeParam(t, typeParams)) return "unknown";
    return describeTypeAnnotation(t);
}

} // namespace zl
