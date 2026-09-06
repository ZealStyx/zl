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
                    result += p.className.empty() ? "object" : p.className;
                    break;
                case DispatchTypeKind::GENERIC_OBJECT: result += "object"; break;
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
