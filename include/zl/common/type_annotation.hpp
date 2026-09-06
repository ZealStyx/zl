#pragma once

#include <string>

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

} // namespace zl
