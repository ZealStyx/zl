#pragma once

#include <exception>
#include <string>
#include <vector>

#include "value.hpp"
#include <memory>

namespace zl { class ProtectedGCRoot; }

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
    // Keeps a managed payload alive for as long as this failure is stored.
    // Holders that the collector traces (tasks, static fields) also report it
    // through appendGCRoots; holders it does not trace - notably threads -
    // rely solely on this pin. Shared so the class stays copyable.
    std::shared_ptr<ProtectedGCRoot> root_;
    std::exception_ptr native_;
    std::string message_;
};
} // namespace zl
