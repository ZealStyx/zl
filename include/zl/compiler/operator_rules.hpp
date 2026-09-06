#pragma once

#include <optional>
#include <string>

#include "zl/compiler/semantic_types.hpp"
#include "zl/lexer/token.hpp"

namespace zl {

// Owns the language's built-in operator typing rules. User-defined operator
// overload resolution remains a TypeChecker concern; this module answers only
// the primitive/built-in part of expression inference.
class OperatorRules {
public:
    [[nodiscard]] static std::string methodName(TokenType op);
    [[nodiscard]] static std::optional<ZlType> unaryResult(TokenType op, ZlType operand);
    [[nodiscard]] static std::optional<ZlType> binaryResult(TokenType op, ZlType left, ZlType right);
    [[nodiscard]] static std::string unaryError(TokenType op);
    [[nodiscard]] static std::string binaryError(TokenType op);
};

} // namespace zl
