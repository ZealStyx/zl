#pragma once

#include <cstddef>
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
    std::vector<std::string> callStackNames() const;
    CallFrame& currentFrame();
    const CallFrame& currentFrame() const;

    void pushHandler(std::size_t catchIp, std::string catchClassName = {}, bool rethrowAfterHandler = false, std::size_t groupId = 0);
    bool hasHandler() const noexcept { return !handlers_.empty(); }
    Handler popHandler();
    void removeHandlerGroup(std::size_t groupId);
    void discardHandlersForFinishedFrames();
    void unwindTo(const Handler& handler);

private:
    std::vector<Value> stack_;
    std::unordered_map<std::string, Value> globals_;
    std::vector<CallFrame> callStack_;
    std::size_t maxCallDepth_{100000};
    std::vector<Handler> handlers_;
};

} // namespace zl
