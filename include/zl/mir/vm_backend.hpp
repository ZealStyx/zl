#pragma once

// MIR -> bytecode backend.
//
// This is the layer that finally makes MIR a *used* IR rather than a verified
// dead structure: it lowers a verified `zl::mir::Module` into the same
// `zl::Chunk` bytecode the existing stack VM executes. The compiler path it
// belongs to is
//
//     ZL source -> parser -> type analysis -> MIR -> verifyModule
//                 -> compileModuleToBytecode -> Chunk -> VM
//
// which coexists with the existing `Compiler` (AST -> Chunk) reference path,
// so the two backends can be differentially tested against each other.
//
// The VM and its bytecode are the behavioural reference; nothing here changes
// them. This file only translates MIR's documented semantics onto that
// bytecode. Where MIR is richer than the current bytecode can faithfully
// express (a growing, explicitly listed subset), the affected *function* is
// rejected rather than silently mis-translated: its Chunk entry is pointed at
// a shared "not translatable by this backend" stub that raises at runtime if
// it is ever reached, so a well-behaved program is unaffected and a program
// that reaches one is loudly wrong instead of subtly wrong. That is the same
// fail-closed stance `lowerProgram` takes toward constructs MIR itself cannot
// represent.

#include <cstddef>
#include <string>
#include <vector>

#include "zl/compiler/bytecode.hpp"
#include "zl/mir/function.hpp"

namespace zl::mir {

struct BytecodeResult {
    zl::Chunk chunk;
    // Number of functions that could not be translated and were stubbed.
    std::size_t stubbed{0};
    // Functions whose body was stubbed (informational).
    std::vector<std::string> stubbedFunctions;
    // Hard errors that prevent translation altogether (empty on success).
    std::vector<std::string> errors;
    bool ok() const noexcept { return errors.empty(); }
};

// Lowers a verified module to a Chunk. The module is expected to have passed
// verifyModule; behaviour is otherwise undefined. On a module whose *whole*
// translation is impossible this records an error in `result.errors` and
// leaves the chunk incomplete.
[[nodiscard]] BytecodeResult compileModuleToBytecode(const Module& module);

} // namespace zl::mir
