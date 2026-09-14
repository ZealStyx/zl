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

    // True when tokens starting at `from` are a generic call-site argument
    // list that is immediately followed by '(': `firstOf<int>(...)`,
    // `items.map<string>(...)`. Used to intercept '<' before Pratt treats it
    // as less-than. The scan is non-mutating.
    [[nodiscard]] bool looksLikeGenericCallArgs(std::size_t from) const;

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

    // The recognizer recurses once per nesting level (`Box<Box<...>>`), so
    // hostile input without a cap is a stack overflow (SIGSEGV) rather than
    // a failed scan. The budget counts nested scanTermCursor activations;
    // exceeding it fails the scan, exactly like any other non-type input.
    // Kept in step with Parser::kMaxTypeDepth: anything deeper would be
    // rejected by the real type parser anyway.
    static constexpr int kMaxScanDepth = 500;

    [[nodiscard]] bool matches(const Cursor& cursor, TokenType type) const;
    [[nodiscard]] bool consume(Cursor& cursor, TokenType type) const;
    // Consumes one '>' , splitting a fused shift token when needed.
    [[nodiscard]] bool consumeAngleClose(Cursor& cursor) const;
    [[nodiscard]] bool scanTermCursor(Cursor& cursor, int depth) const;
    [[nodiscard]] bool scanAnnotationCursor(Cursor& cursor, int depth) const;

    const std::vector<Token>& tokens_;
};

} // namespace zl
