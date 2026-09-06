#include "zl/compiler/generic_instantiation.hpp"

#include <utility>

namespace zl {
namespace {

std::size_t hashCombine(std::size_t seed, std::size_t value) noexcept {
    seed ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6) + (seed >> 2);
    return seed;
}

std::string describeArg(const ResolvedTypeArg& arg) {
    if (arg.type == ZlType::FUNCTION && arg.functionHasSignature) {
        std::string result = "func(";
        for (std::size_t i = 0; i < arg.functionParamTypes.size(); ++i) {
            if (i) result += ",";
            result += describeArg(arg.functionParamTypes[i]);
        }
        result += ")";
        result += ":";
        result += arg.functionReturnType ? describeArg(*arg.functionReturnType) : "void";
        return result;
    }
    if (arg.type == ZlType::OBJECT && !arg.className.empty()) return arg.className;
    switch (arg.type) {
        case ZlType::INT: return "int";
        case ZlType::DOUBLE: return "double";
        case ZlType::STRING: return "string";
        case ZlType::BOOL: return "bool";
        case ZlType::VOID_TYPE: return "void";
        case ZlType::NIL: return "nil";
        case ZlType::LIST: return "list";
        case ZlType::MAP: return "map";
        case ZlType::SET: return "set";
        case ZlType::ARRAY: return "array";
        case ZlType::FUNCTION: return "func";
        case ZlType::UNKNOWN: return "unknown";
        case ZlType::TASK: return "Task";
        case ZlType::OBJECT: return "object";
    }
    return "unknown";
}

} // namespace

std::string GenericInstantiation::describe() const {
    std::string result = base + "<";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) result += ",";
        result += describeArg(args[i]);
    }
    result += ">";
    return result;
}

std::size_t hashArg(const ResolvedTypeArg& arg) noexcept {
    std::size_t seed = std::hash<int>{}(static_cast<int>(arg.type));
    seed = hashCombine(seed, std::hash<std::string>{}(arg.className));
    seed = hashCombine(seed, std::hash<bool>{}(arg.functionHasSignature));
    for (const auto& param : arg.functionParamTypes) seed = hashCombine(seed, hashArg(param));
    if (arg.functionReturnType) seed = hashCombine(seed, hashArg(*arg.functionReturnType));
    return seed;
}

std::size_t GenericInstantiationHash::operator()(const GenericInstantiation& value) const noexcept {
    std::size_t seed = std::hash<std::string>{}(value.base);
    for (const auto& arg : value.args) seed = hashCombine(seed, hashArg(arg));
    return seed;
}

} // namespace zl
