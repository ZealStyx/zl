#pragma once

#include <string>
#include <vector>

#include "zl/mir/function.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// MIR verifier
// ---------------------------------------------------------------------------
//
// The MIR is a contract, and this is what makes it one. Every backend stage is
// entitled to assume the invariants in docs/mir.md hold without re-checking
// them; the verifier is the single place they are checked, run once at the
// lowering boundary and again after any pass that rewrites MIR.
//
// Design decisions worth stating plainly:
//
//   * It is a *checker*, not a repairer. Nothing here mutates the module.
//   * It collects every violation it finds rather than stopping at the first,
//     because a lowering bug almost always produces several and the first one
//     is rarely the most informative. A cap keeps a badly broken module from
//     producing unbounded output.
//   * Diagnostics carry a location whenever the offending construct has one, so
//     a MIR error points at the ZL source line that produced it.

enum class DiagnosticSeverity : std::uint8_t {
    // The MIR is invalid. A backend must not consume it.
    Error,
    // The MIR is valid but suspicious. Recorded, never fatal.
    Warning,
};

[[nodiscard]] const char* severityName(DiagnosticSeverity severity) noexcept;

// Stable property identifiers for tooling/research; never classify English messages.
enum class SafetyProperty : std::uint8_t {
    Structure, TypeFlow, Definition, Move, Ownership, Borrow, Reachability,
    ControlFlow, TypeAssumption, DynamicBoundary, Return, NativeCall,
    ResourceLifetime, Concurrency,
};
[[nodiscard]] const char* propertyName(SafetyProperty property) noexcept;

struct Diagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::Error};
    // Empty for a module-level problem, otherwise the function being verified.
    std::string function;
    // kNoBlock for a function-level problem.
    BlockId block{kNoBlock};
    // Position within the block, or -1 when the problem is not tied to one
    // instruction.
    long instructionIndex{-1};
    std::string message;
    SourceLocation location;
    SafetyProperty property{SafetyProperty::Structure};

    [[nodiscard]] std::string describe() const;
};

struct VerificationReport {
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const {
        for (const auto& d : diagnostics) {
            if (d.severity == DiagnosticSeverity::Error) return false;
        }
        return true;
    }
    [[nodiscard]] std::size_t errorCount() const;
    [[nodiscard]] std::size_t warningCount() const;
    // All messages, one per line, in the order they were found.
    [[nodiscard]] std::string describe() const;
    [[nodiscard]] std::string toJson() const;
};

struct VerifierOptions {
    // Run the flow-sensitive ownership analysis (use-after-move, borrow of a
    // moved value, dropping a borrow). On by default: ownership is part of ZL's
    // semantics, so a MIR that violates it is invalid, merely harder to see.
    bool checkOwnership{true};
    // Report blocks unreachable from the entry as errors. On by default; a
    // pass that intentionally leaves dead blocks behind can relax this.
    bool unreachableBlocksAreErrors{true};
    // Stop after this many errors. 0 means no limit.
    std::size_t maxErrors{0};
    // Audit warnings identify runtime obligations, not static rejections.
    bool auditBoundaries{false};
};

// Verifies a whole module: types, statics, class layouts, function signatures,
// and every function body.
[[nodiscard]] VerificationReport verifyModule(const Module& module, const VerifierOptions& options = {});

// Verifies one function against the module it belongs to. Exposed separately so
// a pass that rewrites a single function can re-check just that function.
[[nodiscard]] VerificationReport verifyFunction(const Module& module, const Function& function,
                                                const VerifierOptions& options = {});

} // namespace zl::mir
