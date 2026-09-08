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
    // Who owns this failure decides how its payload stays alive.
    //   Traced  - the holder is walked by the collector (a Task, a static
    //             field), so appendGCRoots supplies the edge. No pin: pinning
    //             here would permanently root any cycle back through the
    //             failed program and leak it.
    //   Untraced- the holder is invisible to the collector (a Thread's
    //             captured failure), so the payload must pin itself.
    enum class Retention { Traced, Untraced };

    StoredException() = default;
    explicit StoredException(const std::exception_ptr& error,
                         Retention retention = Retention::Traced);
    explicit operator bool() const noexcept { return managed_ || native_; }
    [[noreturn]] void rethrow() const;
    void appendGCRoots(std::vector<Value>& roots) const;
    const std::string& message() const noexcept { return message_; }
private:
    ObjectRef managed_;
    // Set only for Retention::Untraced holders, which have no other way to
    // keep the payload reachable. Shared so the class stays copyable.
    std::shared_ptr<ProtectedGCRoot> root_;
    std::exception_ptr native_;
    std::string message_;
};
} // namespace zl
