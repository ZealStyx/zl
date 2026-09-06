#include "zl/parser/type_lookahead.hpp"

namespace zl {

long TypeLookahead::scanTerm(std::size_t pos) const {
    if (pos >= tokens_.size()) return -1;
    const TokenType type = tokens_[pos].type;

    if (type == TokenType::KW_ARRAY || type == TokenType::KW_LIST ||
        type == TokenType::KW_SET || type == TokenType::KW_MAP) {
        ++pos;
        if (type == TokenType::KW_ARRAY && pos < tokens_.size() &&
            tokens_[pos].type == TokenType::LBRACKET) {
            ++pos;
            if (pos >= tokens_.size() || tokens_[pos].type != TokenType::INT_LITERAL) return -1;
            ++pos;
            if (pos >= tokens_.size() || tokens_[pos].type != TokenType::RBRACKET) return -1;
            ++pos;
        }
        if (pos >= tokens_.size() || tokens_[pos].type != TokenType::LT) return -1;
        ++pos;

        long afterFirst = scanAnnotation(pos);
        if (afterFirst < 0) return -1;
        pos = static_cast<std::size_t>(afterFirst);

        if (type == TokenType::KW_MAP) {
            if (pos >= tokens_.size() || tokens_[pos].type != TokenType::COMMA) return -1;
            ++pos;
            long afterSecond = scanAnnotation(pos);
            if (afterSecond < 0) return -1;
            pos = static_cast<std::size_t>(afterSecond);
        }

        if (pos >= tokens_.size() || tokens_[pos].type != TokenType::GT) return -1;
        return static_cast<long>(pos + 1);
    }

    if (type == TokenType::KW_VOID) {
        return static_cast<long>(pos + 1);
    }

    if (type == TokenType::KW_FUNC) {
        ++pos;
        // Bare `func` is an untyped function slot. `func(...) : T` is a
        // complete function type; scan its nested type annotations so a
        // declaration like `func(int, int): int add = ...` is recognized as
        // a typed variable declaration.
        if (pos >= tokens_.size() || tokens_[pos].type != TokenType::LPAREN) {
            return static_cast<long>(pos);
        }
        ++pos;
        if (pos < tokens_.size() && tokens_[pos].type != TokenType::RPAREN) {
            long after = scanAnnotation(pos);
            if (after < 0) return -1;
            pos = static_cast<std::size_t>(after);
            while (pos < tokens_.size() && tokens_[pos].type == TokenType::COMMA) {
                ++pos;
                after = scanAnnotation(pos);
                if (after < 0) return -1;
                pos = static_cast<std::size_t>(after);
            }
        }
        if (pos >= tokens_.size() || tokens_[pos].type != TokenType::RPAREN) return -1;
        ++pos;
        if (pos < tokens_.size() && tokens_[pos].type == TokenType::COLON) {
            ++pos;
            pos = static_cast<std::size_t>(scanAnnotation(pos));
            if (static_cast<long>(pos) < 0) return -1;
        }
        return static_cast<long>(pos);
    }

    if (type != TokenType::IDENTIFIER) return -1;

    ++pos;
    if (pos < tokens_.size() && tokens_[pos].type == TokenType::LT) {
        ++pos;
        long afterFirst = scanAnnotation(pos);
        if (afterFirst < 0) return -1;
        pos = static_cast<std::size_t>(afterFirst);
        while (pos < tokens_.size() && tokens_[pos].type == TokenType::COMMA) {
            ++pos;
            long afterNext = scanAnnotation(pos);
            if (afterNext < 0) return -1;
            pos = static_cast<std::size_t>(afterNext);
        }
        if (pos >= tokens_.size() || tokens_[pos].type != TokenType::GT) return -1;
        ++pos;
    }

    return static_cast<long>(pos);
}

long TypeLookahead::scanAnnotation(std::size_t pos) const {
    long end = scanTerm(pos);
    if (end < 0) return -1;

    while (static_cast<std::size_t>(end) < tokens_.size() &&
           tokens_[static_cast<std::size_t>(end)].type == TokenType::BIT_OR) {
        long next = scanTerm(static_cast<std::size_t>(end) + 1);
        if (next < 0) return -1;
        end = next;
    }

    return end;
}

} // namespace zl
