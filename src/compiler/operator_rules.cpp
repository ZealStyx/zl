#include "zl/compiler/operator_rules.hpp"

namespace zl {

std::string OperatorRules::methodName(TokenType op) {
    switch (op) {
        case TokenType::PLUS: return "operator+";
        case TokenType::MINUS: return "operator-";
        case TokenType::STAR: return "operator*";
        case TokenType::SLASH: return "operator/";
        case TokenType::PERCENT: return "operator%";
        case TokenType::POW: return "operator^^";
        case TokenType::BIT_AND: return "operator&";
        case TokenType::BIT_OR: return "operator|";
        case TokenType::BIT_XOR: return "operator^";
        case TokenType::SHL: return "operator<<";
        case TokenType::SHR: return "operator>>";
        case TokenType::USHR: return "operator>>>";
        case TokenType::EQ: return "operator==";
        case TokenType::NEQ: return "operator!=";
        case TokenType::LT: return "operator<";
        case TokenType::GT: return "operator>";
        case TokenType::LTE: return "operator<=";
        case TokenType::GTE: return "operator>=";
        case TokenType::NOT: return "operator!";
        case TokenType::BIT_NOT: return "operator~";
        default: return {};
    }
}

std::optional<ZlType> OperatorRules::unaryResult(TokenType op, ZlType operand) {
    switch (op) {
        case TokenType::MINUS:
        case TokenType::PLUS:
            if (operand == ZlType::INT) return ZlType::INT;
            if (operand == ZlType::DOUBLE) return ZlType::DOUBLE;
            return std::nullopt;
        case TokenType::NOT:
            return ZlType::BOOL;
        case TokenType::BIT_NOT:
            if (operand == ZlType::INT) return ZlType::INT;
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

std::optional<ZlType> OperatorRules::binaryResult(TokenType op, ZlType left, ZlType right) {
    switch (op) {
        case TokenType::PLUS:
            if (left == ZlType::STRING || right == ZlType::STRING) return ZlType::STRING;
            if (left == ZlType::INT && right == ZlType::INT) return ZlType::INT;
            if (left == ZlType::DOUBLE || right == ZlType::DOUBLE) return ZlType::DOUBLE;
            return std::nullopt;
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT:
            if (left == ZlType::INT && right == ZlType::INT) return ZlType::INT;
            if (left == ZlType::DOUBLE || right == ZlType::DOUBLE) return ZlType::DOUBLE;
            return std::nullopt;
        case TokenType::POW:
            if (left == ZlType::INT && right == ZlType::INT) return ZlType::INT;
            if ((left == ZlType::INT || left == ZlType::DOUBLE) &&
                (right == ZlType::INT || right == ZlType::DOUBLE)) return ZlType::DOUBLE;
            return std::nullopt;
        case TokenType::BIT_AND:
        case TokenType::BIT_OR:
        case TokenType::BIT_XOR:
        case TokenType::SHL:
        case TokenType::SHR:
        case TokenType::USHR:
            if (left == ZlType::INT && right == ZlType::INT) return ZlType::INT;
            return std::nullopt;
        case TokenType::EQ:
        case TokenType::NEQ:
            return ZlType::BOOL;
        case TokenType::LT:
        case TokenType::GT:
        case TokenType::LTE:
        case TokenType::GTE:
            if ((left == ZlType::INT || left == ZlType::DOUBLE) &&
                (right == ZlType::INT || right == ZlType::DOUBLE)) return ZlType::BOOL;
            return std::nullopt;
        case TokenType::AND:
        case TokenType::OR:
            return ZlType::BOOL;
        default:
            return std::nullopt;
    }
}

std::string OperatorRules::unaryError(TokenType op) {
    switch (op) {
        case TokenType::MINUS: return "unary '-' requires a numeric operand";
        case TokenType::PLUS: return "unary '+' requires a numeric operand";
        case TokenType::BIT_NOT: return "unary '~' requires an integer operand";
        default: return "invalid unary operator";
    }
}

std::string OperatorRules::binaryError(TokenType op) {
    switch (op) {
        case TokenType::PLUS: return "operator '+' requires numeric operands or string concatenation";
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT:
        case TokenType::POW: return "arithmetic operators require numeric operands";
        case TokenType::BIT_AND:
        case TokenType::BIT_OR:
        case TokenType::BIT_XOR:
        case TokenType::SHL:
        case TokenType::SHR:
        case TokenType::USHR: return "bitwise operators require integer operands";
        case TokenType::LT:
        case TokenType::GT:
        case TokenType::LTE:
        case TokenType::GTE: return "ordered comparison operators require numeric operands";
        default: return "invalid binary operator";
    }
}

} // namespace zl
