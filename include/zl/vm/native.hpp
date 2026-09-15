#pragma once

#include <functional>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

#include "value.hpp"

#include "../compiler/native_catalog.hpp"

namespace zl {

// Thrown by System.exit(code). Deliberately NOT derived from std::exception
// (and definitely not std::runtime_error), so it passes straight through the
// VM's per-instruction try/catch untouched - a zl-level `try`/`catch` cannot
// intercept it, matching how System.exit works in Java/C#. main.cpp catches
// this specifically and uses it as the process's real exit code.
struct SystemExitException {
    int code;
};

// A single library func, e.g. Math.sqrt. `arity` is fixed (no varargs
// yet) so the compiler can validate call sites at COMPILE time, same as user
// functions. `fn` is a plain C++ implementation - this is the boundary where
// "the language" ends and "native code the language can call into" begins,
// same idea as JNI in Java or P/Invoke in C#, just far simpler since
// everything already shares the same Value type.
struct NativeFunction {
    NativeId id;
    std::string qualifiedName; // binding key, e.g. "Math.sqrt"
    std::function<Value(const std::vector<Value>&)> fn;
    // Declared arity for runtime-registered extension bindings
    // (NativeId::EXTENSION), which have no compile-time catalog signature to
    // derive it from. Catalog natives leave this empty and derive arity from
    // their signature as before. The VM must know how many values to pop for
    // the call, and the compiler validates call sites against it, so an
    // extension that declares no arity cannot be called at all: the residual
    // throw below is a fail-closed registration bug, not a callable path.
    std::optional<std::size_t> extensionArity;

    [[nodiscard]] std::size_t arity() const {
        if (extensionArity) return *extensionArity;
        const auto signature = findNativeSignature(qualifiedName);
        if (!signature) {
            throw std::logic_error("native binding has no catalog entry: '" + qualifiedName + "'");
        }
        return signature.value()->paramTypes.size();
    }
};

class VM;
// Returns the previous binding so re-entrant native calls can restore it.
VM* setCurrentNativeVm(VM* vm);

class NativeFunctionRegistrar {
public:
    explicit NativeFunctionRegistrar(std::vector<NativeFunction> functions);
};

// The full VM registry, built once from the canonical native catalog. Runtime
// code supplies callback implementations while the catalog supplies names and
// language-facing signatures, keeping the two concerns separate.
[[nodiscard]] const std::vector<NativeFunction>& nativeFunctionTable();

// Returns the func's index in nativeFunctionTable(), or nullopt if no
// library func has that exact "Namespace.name".
[[nodiscard]] std::optional<std::size_t> findNativeFunction(const std::string& qualifiedName);
[[nodiscard]] std::optional<std::size_t> findNativeFunction(NativeId id);
// Name-based lookup for runtime-registered extension bindings (NativeId::
// EXTENSION), which have no compile-time catalog signature. Returns the
// table index, or nullopt when no registered binding carries that name.
[[nodiscard]] std::optional<std::size_t> findNativeFunctionByName(const std::string& qualifiedName);

} // namespace zl
