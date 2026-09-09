#pragma once

#include <cstdint>
#include <string>

namespace zl::mir {

// Source position carried by MIR functions, blocks, and instructions.
//
// ZL's AST records a line per node and the lexer records a column per token,
// but the AST does not propagate columns. `column` is therefore 0 ("unknown")
// for everything lowered from an AST node and is kept in the model so a future
// AST that carries columns - or a machine-generated MIR - can populate it
// without changing the MIR.
struct SourceLocation {
    // Optional source file identity, empty when a module has a single implied
    // file (the common case for a ZL translation unit).
    std::string file;
    std::uint32_t line{0};
    std::uint32_t column{0};

    [[nodiscard]] bool valid() const noexcept { return line != 0; }
    [[nodiscard]] std::string describe() const;

    friend bool operator==(const SourceLocation& a, const SourceLocation& b) noexcept {
        return a.line == b.line && a.column == b.column && a.file == b.file;
    }
};

} // namespace zl::mir
