#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "zl/compiler/semantic_types.hpp"

namespace zl {

// Operator identity, deliberately decoupled from the lexer. The semantics
// table is keyed by what an operation *is*, not by which token spells it in
// the source: `TokenType` is a lexer concern (front end), and keeping it out
// of this header is what lets the table - and the MIR verifier that consults
// it - stay backend-safe. The front end maps its tokens onto these names at
// the one place where both are visible (the type checker); the verifier maps
// MIR opcodes onto the same names.
enum class Operator : std::uint8_t {
    // Binary arithmetic.
    Plus, Minus, Multiply, Divide, Modulo, Power,
    // Binary bitwise.
    BitAnd, BitOr, BitXor, ShiftLeft, ShiftRight, ShiftRightZero,
    // Comparisons.
    Equal, NotEqual, Less, Greater, LessEqual, GreaterEqual,
    // Logical, non-short-circuiting (bool).
    LogicalAnd, LogicalOr,
    // Unary.
    Not, BitNot,
    // Not an operator: the caller's mapping did not recognise its input.
    Unknown,
};

// Owns the language's built-in operator typing rules. User-defined operator
// overload resolution remains a TypeChecker concern; this module answers only
// the primitive/built-in part of expression inference.
class OperatorRules {
public:
    [[nodiscard]] static std::string methodName(Operator op);
    [[nodiscard]] static std::optional<ZlType> unaryResult(Operator op, ZlType operand);
    [[nodiscard]] static std::optional<ZlType> binaryResult(Operator op, ZlType left, ZlType right);
    [[nodiscard]] static std::string unaryError(Operator op);
    [[nodiscard]] static std::string binaryError(Operator op);
};

} // namespace zl
