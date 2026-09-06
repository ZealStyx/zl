#include "zl/vm/execution_state.hpp"

#include <algorithm>

#include <stdexcept>

namespace zl {

Value ExecutionState::pop() {
    if (stack_.empty()) {
        throw std::runtime_error("VM stack underflow (this is a compiler bug, not a program bug)");
    }
    Value value = stack_.back();
    stack_.pop_back();
    return value;
}

void ExecutionState::push(const Value& value) {
    stack_.push_back(value);
}

const Value& ExecutionState::top() const {
    if (stack_.empty()) throw std::runtime_error("VM stack underflow (this is a compiler bug, not a program bug)");
    return stack_.back();
}

Value& ExecutionState::top() {
    if (stack_.empty()) throw std::runtime_error("VM stack underflow (this is a compiler bug, not a program bug)");
    return stack_.back();
}

bool ExecutionState::hasLocal(const std::string& name) const {
    return !callStack_.empty() && callStack_.back().locals.find(name) != callStack_.back().locals.end();
}

Value* ExecutionState::findLocal(const std::string& name) {
    if (callStack_.empty()) return nullptr;
    auto it = callStack_.back().locals.find(name);
    return it == callStack_.back().locals.end() ? nullptr : &it->second;
}

const Value* ExecutionState::findLocal(const std::string& name) const {
    if (callStack_.empty()) return nullptr;
    auto it = callStack_.back().locals.find(name);
    return it == callStack_.back().locals.end() ? nullptr : &it->second;
}

void ExecutionState::setVariable(const std::string& name, const Value& value) {
    if (callStack_.empty()) globals_[name] = value;
    else callStack_.back().locals[name] = value;
}

const Value& ExecutionState::global(const std::string& name) const {
    auto it = globals_.find(name);
    if (it == globals_.end()) throw std::runtime_error("undefined variable '" + name + "'");
    return it->second;
}

bool ExecutionState::hasGlobal(const std::string& name) const {
    return globals_.find(name) != globals_.end();
}

Value ExecutionState::loadVariable(const std::string& name) const {
    if (const Value* local = findLocal(name)) return *local;
    return global(name);
}

void ExecutionState::dropLocal(const std::string& name) {
    if (callStack_.empty()) return;
    callStack_.back().locals.erase(name);
}

std::unordered_map<std::string, Value> ExecutionState::snapshotScope() const {
    return callStack_.empty() ? globals_ : callStack_.back().locals;
}

void ExecutionState::appendGCRoots(std::vector<Value>& roots) const {
    roots.reserve(roots.size() + stack_.size() + globals_.size());
    roots.insert(roots.end(), stack_.begin(), stack_.end());
    for (const auto& [name, value] : globals_) roots.push_back(value);
    for (const auto& frame : callStack_) {
        if (frame.activeClosure) roots.emplace_back(frame.activeClosure);
        for (const auto& [name, value] : frame.locals) roots.push_back(value);
    }
}

void ExecutionState::enterFrame(CallFrame frame) {
    callStack_.push_back(std::move(frame));
}

ExecutionState::CallFrame ExecutionState::leaveFrame() {
    if (callStack_.empty()) throw std::runtime_error("VM: attempted to leave an empty call stack");
    CallFrame finished = std::move(callStack_.back());
    callStack_.pop_back();
    return finished;
}

ExecutionState::CallFrame& ExecutionState::currentFrame() {
    if (callStack_.empty()) throw std::runtime_error("VM: no active call frame");
    return callStack_.back();
}

const ExecutionState::CallFrame& ExecutionState::currentFrame() const {
    if (callStack_.empty()) throw std::runtime_error("VM: no active call frame");
    return callStack_.back();
}

std::vector<std::string> ExecutionState::callStackNames() const {
    std::vector<std::string> names;
    names.reserve(callStack_.size());
    for (auto it = callStack_.rbegin(); it != callStack_.rend(); ++it) {
        names.push_back(it->functionName.empty() ? std::string("<anonymous>") : it->functionName);
    }
    return names;
}

void ExecutionState::pushHandler(std::size_t catchIp, std::string catchClassName, bool rethrowAfterHandler, std::size_t groupId) {
    handlers_.push_back(Handler{catchIp, stack_.size(), callStack_.size(), std::move(catchClassName), rethrowAfterHandler, groupId});
}

void ExecutionState::removeHandlerGroup(std::size_t groupId) {
    if (groupId == 0) return;
    handlers_.erase(std::remove_if(handlers_.begin(), handlers_.end(),
                                   [groupId](const Handler& handler) { return handler.groupId == groupId; }),
                    handlers_.end());
}

ExecutionState::Handler ExecutionState::popHandler() {
    if (handlers_.empty()) throw std::runtime_error("VM: no active exception handler");
    Handler handler = handlers_.back();
    handlers_.pop_back();
    return handler;
}

void ExecutionState::discardHandlersForFinishedFrames() {
    while (!handlers_.empty() && handlers_.back().callStackSize > callStack_.size()) {
        handlers_.pop_back();
    }
}

void ExecutionState::unwindTo(const Handler& handler) {
    stack_.resize(handler.stackSize);
    callStack_.resize(handler.callStackSize);
}

} // namespace zl
