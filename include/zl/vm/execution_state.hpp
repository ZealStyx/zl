#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "value.hpp"

namespace zl {

class ExecutionState {
public:
    struct CallFrame {
        std::size_t returnIp{0};
        std::unordered_map<std::string, Value> locals;
        ClosureRef activeClosure;
        std::string functionName;
        std::vector<std::string> ownedLocalNames;

        void dropOwnedLocals() {
            for (const auto& name : ownedLocalNames) locals.erase(name);
            ownedLocalNames.clear();
        }
    };

    struct Handler {
        std::size_t catchIp{0};
        std::size_t stackSize{0};
        std::size_t callStackSize{0};
        std::string catchClassName; // empty = catch-all
        bool rethrowAfterHandler{false};
        std::size_t groupId{0};
    };

    explicit ExecutionState(std::size_t maxCallDepth = 100000);

    Value pop();
    void push(const Value& value);
    const Value& top() const;
    Value& top();

    bool hasLocal(const std::string& name) const;
    Value* findLocal(const std::string& name);
    const Value* findLocal(const std::string& name) const;
    void setVariable(const std::string& name, const Value& value);
    Value loadVariable(const std::string& name) const;
    void dropLocal(const std::string& name);
    const Value& global(const std::string& name) const;
    bool hasGlobal(const std::string& name) const;

    std::unordered_map<std::string, Value> snapshotScope() const;
    void appendGCRoots(std::vector<Value>& roots) const;
    void enterFrame(CallFrame frame);
    CallFrame leaveFrame();
    bool inFunction() const noexcept { return !callStack_.empty(); }
    std::size_t callDepth() const noexcept { return callStack_.size(); }
    std::size_t valueStackSize() const noexcept { return stack_.size(); }
    std::vector<std::string> callStackNames() const;
    CallFrame& currentFrame();
    const CallFrame& currentFrame() const;

    void pushHandler(std::size_t catchIp, std::string catchClassName = {}, bool rethrowAfterHandler = false, std::size_t groupId = 0);
    bool hasHandler() const noexcept { return !handlers_.empty(); }
    // Call-stack depth at which the most recently pushed handler was
    // installed (UINT_MAX when none). Used to scope exception handling to the
    // execute() run that owns the handler (nested runs must not catch on the
    // caller's handlers).
    std::size_t topHandlerCallDepth() const noexcept {
        return handlers_.empty() ? std::numeric_limits<std::size_t>::max()
                                 : handlers_.back().callStackSize;
    }
    Handler popHandler();
    void removeHandlerGroup(std::size_t groupId);
    void discardHandlersForFinishedFrames();
    void unwindTo(const Handler& handler);
    // Drop any handlers installed strictly deeper than the given call-stack
    // depth. Used when a nested VM-driven closure (Mutex.withLock, reflection
    // invoke, a thread entry) exits via an uncaught exception: the handlers it
    // pushed must not leak back into the caller's run, while the caller's own
    // handlers (installed at exactly its frame depth) are preserved.
    void discardHandlersDeeperThan(std::size_t callStackSize);
    // Restore the value/frame stacks to the recorded depths, discarding any
    // leftover frames a nested execution failed to pop. The value stack is only
    // shrunk (values the caller pushed are preserved).
    void restoreToDepth(std::size_t valueStackSize, std::size_t callStackSize);

private:
    std::vector<Value> stack_;
    std::unordered_map<std::string, Value> globals_;
    std::vector<CallFrame> callStack_;
    std::size_t maxCallDepth_{100000};
    std::vector<Handler> handlers_;
};

} // namespace zl
