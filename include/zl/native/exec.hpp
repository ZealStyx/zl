#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zl/native/emit.hpp"
#include "zl/native/lir.hpp"

// ---------------------------------------------------------------------------
// Native execution driver
// ---------------------------------------------------------------------------
//
// The last piece of "native" that was missing is not code generation - the
// emitter has produced correct bytes since the arithmetic-contract work - but
// something to put those bytes in memory a CPU will execute, bind their call
// sites, and hand back a result. This file is that driver: it maps an emitted
// module executable, resolves direct calls among the module's own functions,
// and offers typed entry for the one calling shape it can honestly speak today
// (integer in, integer out, SysV). Everything else is refused *before* any
// code runs, by name and with a reason:
//
//   * a call relocation that is not bound (a runtime import, or a callee the
//     selection left out) - binding would point at nothing;
//   * a function whose parameter or return class is not Integer - a float
//     arrives in an XMM register and a reference carries GC obligations the
//     driver does not model;
//   * a host this code does not target: execution is x86-64 Linux (the same
//     guard the executable-native tests use); every other build constructs a
//     NativeExecutable that reports `unsupported platform` rather than
//     pretending.
//
// That refusal list is the current subset boundary made concrete. Growing it -
// refs and objects with GC maps - is P1-6's remaining half; the driver is the
// half that makes the bytes reachable at all.

namespace zl::native {

class NativeExecutable {
public:
    // Takes the emitted functions plus their LIR signatures (parameter
    // classes); binds inter-function calls; never binds runtime calls.
    NativeExecutable(const std::vector<EmittedFunction>& functions, const LirModule& lir);
    ~NativeExecutable();
    NativeExecutable(const NativeExecutable&) = delete;
    NativeExecutable& operator=(const NativeExecutable&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // Emitted names, in module order.
    [[nodiscard]] const std::vector<std::string>& names() const noexcept { return names_; }

    // Resolve a call target by name. Accepts the exact LIR function name, or
    // a bare method token (the part after the last '.' and before '('), when
    // that identifies exactly one function. On ambiguity or absence, fills
    // `error` and returns kNoEntry.
    static constexpr std::size_t kNoEntry = static_cast<std::size_t>(-1);
    std::size_t resolve(const std::string& spec, std::string& error) const;

    // Call entry e with integer arguments (up to six, SysV integer ABI).
    // Returns false with a reason when the signature is not driver-compatible.
    bool callInt64(std::size_t e, const std::vector<std::int64_t>& args,
                   std::int64_t& result, std::string& error) const;

private:
    bool ok_{false};
    std::string error_;
    void* base_{nullptr};
    std::size_t size_{0};
    std::vector<std::string> names_;
    std::vector<std::uintptr_t> entries_;
    std::vector<std::vector<ValueClass>> parameterClasses_;
    std::vector<ValueClass> returnClasses_;
};

} // namespace zl::native
