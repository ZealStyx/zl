#include "zl/compiler/operator_rules.hpp"

namespace zl {

std::string OperatorRules::methodName(Operator op) {
    switch (op) {
        case Operator::Plus: return "operator+";
        case Operator::Minus: return "operator-";
        case Operator::Multiply: return "operator*";
        case Operator::Divide: return "operator/";
        case Operator::Modulo: return "operator%";
        case Operator::Power: return "operator^^";
        case Operator::BitAnd: return "operator&";
        case Operator::BitOr: return "operator|";
        case Operator::BitXor: return "operator^";
        case Operator::ShiftLeft: return "operator<<";
        case Operator::ShiftRight: return "operator>>";
        case Operator::ShiftRightZero: return "operator>>>";
        case Operator::Equal: return "operator==";
        case Operator::NotEqual: return "operator!=";
        case Operator::Less: return "operator<";
        case Operator::Greater: return "operator>";
        case Operator::LessEqual: return "operator<=";
        case Operator::GreaterEqual: return "operator>=";
        case Operator::Not: return "operator!";
        case Operator::BitNot: return "operator~";
        default: return {};
    }
}

std::optional<ZlType> OperatorRules::unaryResult(Operator op, ZlType operand) {
    switch (op) {
        case Operator::Minus:
        case Operator::Plus:
            if (operand == ZlType::INT) return ZlType::INT;
            if (operand == ZlType::DOUBLE) return ZlType::DOUBLE;
            return std::nullopt;
        case Operator::Not:
            return ZlType::BOOL;
        case Operator::BitNot:
            if (operand == ZlType::INT) return ZlType::INT;
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

std::optional<ZlType> OperatorRules::binaryResult(Operator op, ZlType left, ZlType right) {
    switch (op) {
        case Operator::Plus:
            if (left == ZlType::STRING || right == ZlType::STRING) return ZlType::STRING;
            if (left == ZlType::INT && right == ZlType::INT) return ZlType::INT;
            if (left == ZlType::DOUBLE || right == ZlType::DOUBLE) return ZlType::DOUBLE;
            return std::nullopt;
        case Operator::Minus:
        case Operator::Multiply:
        case Operator::Divide:
        case Operator::Modulo:
            if (left == ZlType::INT && right == ZlType::INT) return ZlType::INT;
            if (left == ZlType::DOUBLE || right == ZlType::DOUBLE) return ZlType::DOUBLE;
            return std::nullopt;
        case Operator::Power:
            if (left == ZlType::INT && right == ZlType::INT) return ZlType::INT;
            if ((left == ZlType::INT || left == ZlType::DOUBLE) &&
                (right == ZlType::INT || right == ZlType::DOUBLE)) return ZlType::DOUBLE;
            return std::nullopt;
        case Operator::BitAnd:
        case Operator::BitOr:
        case Operator::BitXor:
        case Operator::ShiftLeft:
        case Operator::ShiftRight:
        case Operator::ShiftRightZero:
            if (left == ZlType::INT && right == ZlType::INT) return ZlType::INT;
            return std::nullopt;
        case Operator::Equal:
        case Operator::NotEqual:
            return ZlType::BOOL;
        case Operator::Less:
        case Operator::Greater:
        case Operator::LessEqual:
        case Operator::GreaterEqual:
            if ((left == ZlType::INT || left == ZlType::DOUBLE) &&
                (right == ZlType::INT || right == ZlType::DOUBLE)) return ZlType::BOOL;
            return std::nullopt;
        case Operator::LogicalAnd:
        case Operator::LogicalOr:
            return ZlType::BOOL;
        default:
            return std::nullopt;
    }
}

std::string OperatorRules::unaryError(Operator op) {
    switch (op) {
        case Operator::Minus: return "unary '-' requires a numeric operand";
        case Operator::Plus: return "unary '+' requires a numeric operand";
        case Operator::BitNot: return "unary '~' requires an integer operand";
        default: return "invalid unary operator";
    }
}

std::string OperatorRules::binaryError(Operator op) {
    switch (op) {
        case Operator::Plus: return "operator '+' requires numeric operands or string concatenation";
        case Operator::Minus:
        case Operator::Multiply:
        case Operator::Divide:
        case Operator::Modulo:
        case Operator::Power: return "arithmetic operators require numeric operands";
        case Operator::BitAnd:
        case Operator::BitOr:
        case Operator::BitXor:
        case Operator::ShiftLeft:
        case Operator::ShiftRight:
        case Operator::ShiftRightZero: return "bitwise operators require integer operands";
        case Operator::Less:
        case Operator::Greater:
        case Operator::LessEqual:
        case Operator::GreaterEqual: return "ordered comparison operators require numeric operands";
        default: return "invalid binary operator";
    }
}

} // namespace zl
