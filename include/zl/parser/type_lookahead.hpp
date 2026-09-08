#pragma once

#include <cstddef>
#include <vector>

#include "zl/lexer/token.hpp"

namespace zl {

// Non-mutating type grammar recognizer used only when the parser must
// disambiguate a declaration from an expression. It deliberately returns a
// token boundary instead of building AST nodes, keeping speculative parsing
// cheap and side-effect free.
class TypeLookahead {
public:
    explicit TypeLookahead(const std::vector<Token>& tokens) : tokens_(tokens) {}

    // Returns the index of the first token *after* a complete type annotation
    // starting at `pos`, or -1 when the tokens are not a type annotation.
    //
    // A nested annotation such as `List<List<int>>` ends on a fused `>>`
    // token. Scanning stops only on a *fully consumed* closer, so the result
    // stays a plain token boundary the parser can compare against.
    [[nodiscard]] long scanAnnotation(std::size_t pos) const;

private:
    // A scan position plus the number of '>' characters still unconsumed
    // inside a fused `>>` / `>>>` token at `pos`. `pending > 0` means the
    // cursor sits *inside* tokens_[pos], so no other token may be matched
    // there.
    struct Cursor {
        std::size_t pos{0};
        int pending{0};
        [[nodiscard]] bool atTokenBoundary() const { return pending == 0; }
    };

    [[nodiscard]] bool matches(const Cursor& cursor, TokenType type) const;
    [[nodiscard]] bool consume(Cursor& cursor, TokenType type) const;
    // Consumes one '>' , splitting a fused shift token when needed.
    [[nodiscard]] bool consumeAngleClose(Cursor& cursor) const;
    [[nodiscard]] bool scanTermCursor(Cursor& cursor) const;
    [[nodiscard]] bool scanAnnotationCursor(Cursor& cursor) const;

    const std::vector<Token>& tokens_;
};

} // namespace zl
