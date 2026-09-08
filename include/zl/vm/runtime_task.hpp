#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <functional>
#include <vector>
#include <string>

#include "value.hpp"

namespace zl {

// Runtime representation shared by the future language-level Task<T> abstraction.
// It deliberately owns only lifecycle/result state; scheduling policy lives in Scheduler.
enum class TaskStatus {
    Pending,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

class RuntimeTaskState {
public:
    explicit RuntimeTaskState(std::string valueTypeName = {})
        : valueTypeName_(std::move(valueTypeName)) {}
    ~RuntimeTaskState() noexcept;
    RuntimeTaskState(const RuntimeTaskState&) = delete;
    RuntimeTaskState& operator=(const RuntimeTaskState&) = delete;

    [[nodiscard]] TaskStatus status() const noexcept;
    [[nodiscard]] bool cancellationRequested() const noexcept;
    [[nodiscard]] bool failureObserved() const noexcept;
    [[nodiscard]] std::string valueTypeName() const;

    // State transitions are idempotent only for cancellation requests. Terminal
    // completion/failure/cancellation are single-owner transitions and reject misuse.
    void start();
    void succeed(Value value);
    void fail(std::exception_ptr error);
    void cancel();
    void requestCancellation() noexcept;

    // Observing a task marks a failure as observed. Calling this before
    // completion waits until the task reaches a terminal state.
    Value observe();
    [[nodiscard]] bool isTerminal() const noexcept;
    void ignore() noexcept;
    void then(std::function<void()> continuation);
    void onCancellation(std::function<void()> continuation);

    // Append managed values retained by the task itself (currently its result
    // and the value carried by a ZL exception) to an active GC mark worklist.
    void appendGCRoots(std::vector<Value>& roots) const;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    TaskStatus status_{TaskStatus::Pending};
    std::optional<Value> result_;
    std::exception_ptr error_;
    bool cancellationRequested_{false};
    bool failureObserved_{false};
    const std::string valueTypeName_;
    std::vector<std::function<void()>> continuations_;
    std::vector<std::function<void()>> cancellationContinuations_;
};

} // namespace zl
