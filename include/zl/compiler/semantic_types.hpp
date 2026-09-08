#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zl {

enum class ZlType {
    INT, DOUBLE, STRING, BOOL, VOID_TYPE, NIL, LIST, MAP, SET, ARRAY,
    OBJECT, FUNCTION, TASK, UNKNOWN, UNION,
};

[[nodiscard]] std::string zlTypeName(ZlType t);

// Inverse of zlTypeName for a single *base* type name as it appears in a
// rendered type name (e.g. the "int" inside "list<int>" or the "Task" inside
// "Task<int>"). Lowercase keywords map to their builtin kinds (`int`, `list`,
// ...); the lone capitalised builtin with its own kind is `Task`. Capitalised
// generic classes (`List`, `Map`, `Set`, `Box`, ...) are real object classes
// and so map to OBJECT - only the lowercase `list`/`map`/... keywords get the
// collection kinds. Anything else unrecognised is a user/generic class.
[[nodiscard]] inline ZlType zlTypeFromBaseName(const std::string& name) {
    if (name == "int") return ZlType::INT;
    if (name == "double" || name == "float") return ZlType::DOUBLE;
    if (name == "string") return ZlType::STRING;
    if (name == "bool") return ZlType::BOOL;
    if (name == "void") return ZlType::VOID_TYPE;
    if (name == "nil" || name == "null") return ZlType::NIL;
    if (name == "func") return ZlType::FUNCTION;
    if (name == "unknown") return ZlType::UNKNOWN;
    if (name == "Task") return ZlType::TASK;
    if (name == "list") return ZlType::LIST;
    if (name == "map") return ZlType::MAP;
    if (name == "set") return ZlType::SET;
    if (name == "array") return ZlType::ARRAY;
    return ZlType::OBJECT;
}

// Split a rendered generic name into (base name, top-level argument string).
// `List<int>`      -> ("List", "int")
// `Map<string,int>`-> ("Map", "string,int")
// `Widget`         -> ("Widget", "")
// Nested generics are kept whole in the argument string (depth-aware), so a
// caller that wants per-argument splitting should call splitGenericArgs on it.
[[nodiscard]] inline std::pair<std::string, std::string>
splitGenericName(const std::string& name) {
    const std::size_t open = name.find('<');
    if (open == std::string::npos || name.empty() || name.back() != '>')
        return {name, std::string{}};
    return {name.substr(0, open), name.substr(open + 1, name.size() - open - 2)};
}

// Split a top-level generic argument string on depth-0 commas, so nested
// arguments stay intact: "string,List<int>" -> ["string", "List<int>"].
[[nodiscard]] inline std::vector<std::string>
splitGenericArgs(const std::string& argString) {
    std::vector<std::string> out;
    std::size_t start = 0, depth = 0;
    for (std::size_t i = 0; i <= argString.size(); ++i) {
        if (i == argString.size() || (argString[i] == ',' && depth == 0)) {
            out.push_back(argString.substr(start, i - start));
            start = i + 1;
            continue;
        }
        if (argString[i] == '<') ++depth;
        else if (argString[i] == '>') { if (depth) --depth; }
    }
    return out;
}

class TypeCheckError : public std::runtime_error {
public:
    explicit TypeCheckError(const std::string& message) : std::runtime_error(message) {}
};

} // namespace zl
