#pragma once

#include <optional>
#include <string>
#include "zl/parser/ast.hpp"

namespace zl::native {

struct FfiDeclaration {
    std::string functionName;
    std::string library;
    std::string symbol;
};

// Parse the source-level @ffi("library", "symbol") declaration. One argument
// selects a symbol in the default process; two arguments select an explicit
// shared library plus symbol. Any other shape is rejected.
[[nodiscard]] std::optional<FfiDeclaration> parseFfiDeclaration(const FunctionDecl& fn, std::string& error);

}
