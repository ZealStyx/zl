#pragma once

#include <exception>
#include <string>
#include <vector>

#include "value.hpp"

namespace zl {
// In-flight ZL exceptions pin their object while native C++ unwinds. A cached
// failure instead owns a traceable edge, not that pin: keeping the pin inside
// a Task/static store would permanently root cycles through the failed program.
class StoredException {
public:
    StoredException() = default;
    explicit StoredException(const std::exception_ptr& error);
    explicit operator bool() const noexcept { return managed_ || native_; }
    [[noreturn]] void rethrow() const;
    void appendGCRoots(std::vector<Value>& roots) const;
    const std::string& message() const noexcept { return message_; }
private:
    ObjectRef managed_;
    std::exception_ptr native_;
    std::string message_;
};
} // namespace zl
