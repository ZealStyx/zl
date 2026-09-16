#pragma once

#include <memory>
#include "zl/parser/ast.hpp"

namespace zl {

class Parser;

// Owns the expression grammar and precedence ladder while sharing the parser
// cursor/error machinery and declaration-level helpers through Parser.
class ExpressionParser {
public:
    explicit ExpressionParser(Parser& parser) : parser_(parser) {}

    NodePtr parseExpression();
    NodePtr parseConditionExpression();

private:
    NodePtr parsePratt(int minBindingPower);
    NodePtr parseCall();
    NodePtr parsePrimary();
    // Parses `<T, U>` when the next tokens are a generic call-site argument
    // list followed by '('. Returns empty when the lookahead does not match.
    std::vector<TypeAnnotation> parseOptionalCallTypeArgs();
    NodePtr parseCollectionLiteral();
    NodePtr parseDataLiteral(const Token& nameTok);
    NodePtr parseLambdaExpr();
    NodePtr parseMatchExpr();

    Parser& parser_;
};

} // namespace zl
