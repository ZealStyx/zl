#pragma once

#include <stdexcept>
#include <string>

namespace zl {

// A runtime failure raised by the VM or a native primitive that already knows
// which ZL exception class describes it. The VM turns this into a real ZL
// exception object, so programs can catch `IndexError`, `KeyError`,
// `ArithmeticError`, ... by type instead of relying on an untyped catch-all.
//
// Plain std::runtime_error remains supported and is reported as `RuntimeError`.
class ZlRuntimeFault : public std::runtime_error {
public:
    ZlRuntimeFault(std::string className, const std::string& message)
        : std::runtime_error(message), className_(std::move(className)) {}
    [[nodiscard]] const std::string& className() const noexcept { return className_; }

private:
    std::string className_;
};

[[noreturn]] inline void throwTypeError(const std::string& message) {
    throw ZlRuntimeFault("TypeError", message);
}
[[noreturn]] inline void throwIndexError(const std::string& message) {
    throw ZlRuntimeFault("IndexError", message);
}
[[noreturn]] inline void throwKeyError(const std::string& message) {
    throw ZlRuntimeFault("KeyError", message);
}
[[noreturn]] inline void throwArithmeticError(const std::string& message) {
    throw ZlRuntimeFault("ArithmeticError", message);
}
[[noreturn]] inline void throwIOError(const std::string& message) {
    throw ZlRuntimeFault("IOError", message);
}
[[noreturn]] inline void throwStackOverflowError(const std::string& message) {
    throw ZlRuntimeFault("StackOverflowError", message);
}

} // namespace zl
