#pragma once

#include <stdexcept>
#include <string>

namespace zl {

enum class ZlType {
    INT, DOUBLE, STRING, BOOL, VOID_TYPE, NIL, LIST, MAP, SET, ARRAY,
    OBJECT, FUNCTION, TASK, UNKNOWN,
};

[[nodiscard]] std::string zlTypeName(ZlType t);

class TypeCheckError : public std::runtime_error {
public:
    explicit TypeCheckError(const std::string& message) : std::runtime_error(message) {}
};

} // namespace zl
