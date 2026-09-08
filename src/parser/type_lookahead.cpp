#include "zl/parser/type_lookahead.hpp"

namespace zl {

bool TypeLookahead::matches(const Cursor& cursor, TokenType type) const {
    if (!cursor.atTokenBoundary()) return false;
    return cursor.pos < tokens_.size() && tokens_[cursor.pos].type == type;
}

bool TypeLookahead::consume(Cursor& cursor, TokenType type) const {
    if (!matches(cursor, type)) return false;
    ++cursor.pos;
    return true;
}

// `List<List<int>>` lexes the closing angles as one SHR token (and `>>>` as
// USHR). The recognizer therefore tracks how many '>' remain inside the token
// under the cursor instead of demanding a standalone GT.
bool TypeLookahead::consumeAngleClose(Cursor& cursor) const {
    if (cursor.pending > 0) {
        if (--cursor.pending == 0) ++cursor.pos;
        return true;
    }
    if (cursor.pos >= tokens_.size()) return false;
    switch (tokens_[cursor.pos].type) {
        case TokenType::GT:
            ++cursor.pos;
            return true;
        case TokenType::SHR:
            cursor.pending = 1; // one '>' consumed here, one left in the token
            return true;
        case TokenType::USHR:
            cursor.pending = 2;
            return true;
        default:
            return false;
    }
}

bool TypeLookahead::scanTermCursor(Cursor& cursor) const {
    if (!cursor.atTokenBoundary() || cursor.pos >= tokens_.size()) return false;
    const TokenType type = tokens_[cursor.pos].type;

    if (type == TokenType::KW_ARRAY || type == TokenType::KW_LIST ||
        type == TokenType::KW_SET || type == TokenType::KW_MAP) {
        ++cursor.pos;
        if (type == TokenType::KW_ARRAY && matches(cursor, TokenType::LBRACKET)) {
            ++cursor.pos;
            if (!consume(cursor, TokenType::INT_LITERAL)) return false;
            if (!consume(cursor, TokenType::RBRACKET)) return false;
        }
        if (!consume(cursor, TokenType::LT)) return false;
        if (!scanAnnotationCursor(cursor)) return false;
        if (type == TokenType::KW_MAP) {
            if (!consume(cursor, TokenType::COMMA)) return false;
            if (!scanAnnotationCursor(cursor)) return false;
        }
        return consumeAngleClose(cursor);
    }

    if (type == TokenType::KW_VOID) {
        ++cursor.pos;
        return true;
    }

    if (type == TokenType::KW_FUNC) {
        ++cursor.pos;
        // Bare `func` is an untyped function slot. `func(...) : T` is a
        // complete function type; scan its nested type annotations so a
        // declaration like `func(int, int): int add = ...` is recognized as
        // a typed variable declaration.
        if (!matches(cursor, TokenType::LPAREN)) return true;
        ++cursor.pos;
        if (!matches(cursor, TokenType::RPAREN)) {
            if (!scanAnnotationCursor(cursor)) return false;
            while (consume(cursor, TokenType::COMMA)) {
                if (!scanAnnotationCursor(cursor)) return false;
            }
        }
        if (!consume(cursor, TokenType::RPAREN)) return false;
        if (consume(cursor, TokenType::COLON)) {
            if (!scanAnnotationCursor(cursor)) return false;
        }
        return true;
    }

    if (type != TokenType::IDENTIFIER) return false;
    ++cursor.pos;
    if (matches(cursor, TokenType::LT)) {
        ++cursor.pos;
        if (!scanAnnotationCursor(cursor)) return false;
        while (consume(cursor, TokenType::COMMA)) {
            if (!scanAnnotationCursor(cursor)) return false;
        }
        return consumeAngleClose(cursor);
    }
    return true;
}

bool TypeLookahead::scanAnnotationCursor(Cursor& cursor) const {
    if (!scanTermCursor(cursor)) return false;
    while (consume(cursor, TokenType::BIT_OR)) {
        if (!scanTermCursor(cursor)) return false;
    }
    return true;
}

long TypeLookahead::scanAnnotation(std::size_t pos) const {
    Cursor cursor{pos, 0};
    if (!scanAnnotationCursor(cursor)) return -1;
    // A boundary inside a fused shift token is not a usable token index; the
    // parser splits such tokens itself when it really parses the annotation.
    if (!cursor.atTokenBoundary()) return -1;
    return static_cast<long>(cursor.pos);
}

} // namespace zl
