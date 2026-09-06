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

    [[nodiscard]] long scanAnnotation(std::size_t pos) const;

private:
    [[nodiscard]] long scanTerm(std::size_t pos) const;

    const std::vector<Token>& tokens_;
};

} // namespace zl
