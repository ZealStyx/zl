#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace zl {

enum class DispatchTypeKind {
    INT,
    DOUBLE,
    STRING,
    BOOL,
    VOID_TYPE,
    LIST,
    ARRAY,
    SET,
    MAP,
    FUNCTION,
    OBJECT,
    GENERIC_OBJECT,
};

// One dispatched parameter type. `className` is the erased runtime identity:
// the declaring name for a plain class, and - for a concrete generic
// instantiation such as `Option<int>` - the generic class's base name
// ("Option"), because the runtime erases instantiations of one generic class
// into a single body with per-storage contracts. It is empty only where the
// type is genuinely not a single class at runtime: a class's own type
// parameter, `unknown`, `nil`, or a union-of-erased shapes.
//
// Keeping the base name is what keeps distinct declarations distinct:
// `label(Option<int>)` and `label(List<int>)` used to describe to the same
// `label(object)` and overwrite each other in every function table keyed by
// that name, binding calls to whichever declaration registered last. With the
// base name they are `label(Option)` and `label(List)` - still one body per
// declaration (never per instantiation), but never one declaration silently
// shadowing another.
struct DispatchType {
    DispatchTypeKind kind{DispatchTypeKind::OBJECT};
    std::string className;

    friend bool operator==(const DispatchType& a, const DispatchType& b) {
        return a.kind == b.kind && a.className == b.className;
    }
};

struct DispatchSignature {
    std::string name;
    std::vector<DispatchType> parameters;

    friend bool operator==(const DispatchSignature& a, const DispatchSignature& b) {
        return a.name == b.name && a.parameters == b.parameters;
    }

    [[nodiscard]] std::string describe() const {
        std::string result = name + "(";
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            if (i) result += ",";
            const auto& p = parameters[i];
            switch (p.kind) {
                case DispatchTypeKind::INT: result += "int"; break;
                case DispatchTypeKind::DOUBLE: result += "double"; break;
                case DispatchTypeKind::STRING: result += "string"; break;
                case DispatchTypeKind::BOOL: result += "bool"; break;
                case DispatchTypeKind::VOID_TYPE: result += "void"; break;
                case DispatchTypeKind::LIST: result += "list"; break;
                case DispatchTypeKind::ARRAY: result += "array"; break;
                case DispatchTypeKind::SET: result += "set"; break;
                case DispatchTypeKind::MAP: result += "map"; break;
                case DispatchTypeKind::FUNCTION: result += "func"; break;
                case DispatchTypeKind::OBJECT:
                case DispatchTypeKind::GENERIC_OBJECT:
                    result += p.className.empty() ? "object" : p.className;
                    break;
            }
        }
        result += ")";
        return result;
    }
};

struct DispatchSignatureHash {
    std::size_t operator()(const DispatchSignature& signature) const noexcept {
        std::size_t seed = std::hash<std::string>{}(signature.name);
        for (const auto& param : signature.parameters) {
            seed ^= static_cast<std::size_t>(param.kind) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
            seed ^= std::hash<std::string>{}(param.className) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        }
        return seed;
    }
};

} // namespace zl
