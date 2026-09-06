#include "zl/vm/runtime_task.hpp"

#include <stdexcept>
#include <iostream>
#include <string>

namespace zl {

namespace {
bool isTerminalStatus(TaskStatus status) {
    return status == TaskStatus::Succeeded ||
           status == TaskStatus::Failed ||
           status == TaskStatus::Cancelled;
}
}


RuntimeTaskState::~RuntimeTaskState() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ != TaskStatus::Failed || failureObserved_ || !error_) return;
        std::string message;
        try {
            std::rethrow_exception(error_);
        } catch (const ZlThrownException& e) {
            if (e.value() && e.value()->fields.count("message")) {
                message = valueToString(e.value()->fields.at("message"));
            } else {
                message = e.value() ? e.value()->className : std::string("ZL exception");
            }
        } catch (const std::exception& e) {
            message = e.what();
        } catch (...) {
            message = "unknown exception";
        }
        std::cerr << "unobserved task failure: " << message << "\n";
    } catch (...) {
        // Reporting must never turn task destruction into a new runtime failure.
    }
}

TaskStatus RuntimeTaskState::status() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool RuntimeTaskState::cancellationRequested() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancellationRequested_;
}

bool RuntimeTaskState::failureObserved() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return failureObserved_;
}

std::string RuntimeTaskState::valueTypeName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return valueTypeName_;
}

void RuntimeTaskState::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != TaskStatus::Pending) {
        throw std::logic_error("task can only start from Pending state");
    }
    status_ = TaskStatus::Running;
}

void RuntimeTaskState::succeed(Value value) {
    std::vector<std::function<void()>> continuations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_)) {
            throw std::logic_error("task is already terminal");
        }
        result_ = std::move(value);
        status_ = TaskStatus::Succeeded;
        continuations.swap(continuations_);
        cancellationContinuations_.clear();
    }
    condition_.notify_all();
    std::exception_ptr firstError;
    for (auto& continuation : continuations) {
        try {
            continuation();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    if (firstError) std::rethrow_exception(firstError);
}

void RuntimeTaskState::fail(std::exception_ptr error) {
    if (!error) {
        throw std::invalid_argument("failed task requires an exception");
    }
    std::vector<std::function<void()>> continuations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_)) {
            throw std::logic_error("task is already terminal");
        }
        error_ = std::move(error);
        status_ = TaskStatus::Failed;
        continuations.swap(continuations_);
        cancellationContinuations_.clear();
    }
    condition_.notify_all();
    std::exception_ptr firstError;
    for (auto& continuation : continuations) {
        try {
            continuation();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    if (firstError) std::rethrow_exception(firstError);
}

void RuntimeTaskState::cancel() {
    std::vector<std::function<void()>> continuations;
    std::vector<std::function<void()>> cancellationContinuations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_)) {
            throw std::logic_error("task is already terminal");
        }
        status_ = TaskStatus::Cancelled;
        cancellationRequested_ = true;
        continuations.swap(continuations_);
        cancellationContinuations.swap(cancellationContinuations_);
    }
    condition_.notify_all();
    std::exception_ptr firstError;
    for (auto& continuation : continuations) {
        try {
            continuation();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    for (auto& continuation : cancellationContinuations) {
        try {
            continuation();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    if (firstError) std::rethrow_exception(firstError);
}

void RuntimeTaskState::requestCancellation() noexcept {
    std::vector<std::function<void()>> continuations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_) || cancellationRequested_) return;
        cancellationRequested_ = true;
        continuations.swap(cancellationContinuations_);
    }
    condition_.notify_all();
    for (auto& continuation : continuations) {
        try {
            continuation();
        } catch (...) {
            // Cancellation requests are noexcept by contract. One failing
            // observer must not prevent remaining observers from running.
        }
    }
}

void RuntimeTaskState::ignore() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    failureObserved_ = true;
}

void RuntimeTaskState::appendGCRoots(std::vector<Value>& roots) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (result_) roots.push_back(*result_);
    if (error_) {
        try {
            std::rethrow_exception(error_);
        } catch (const ZlThrownException& e) {
            if (e.value()) roots.push_back(e.value());
        } catch (...) {
            // Native exceptions do not retain ZL managed values.
        }
    }
}


void RuntimeTaskState::onCancellation(std::function<void()> continuation) {
    if (!continuation) return;
    bool runNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ == TaskStatus::Cancelled || cancellationRequested_) runNow = true;
        else if (!isTerminalStatus(status_)) cancellationContinuations_.push_back(std::move(continuation));
    }
    if (runNow) continuation();
}
void RuntimeTaskState::then(std::function<void()> continuation) {
    if (!continuation) return;
    bool runNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_)) runNow = true;
        else continuations_.push_back(std::move(continuation));
    }
    if (runNow) continuation();
}

bool RuntimeTaskState::isTerminal() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return isTerminalStatus(status_);
}

Value RuntimeTaskState::observe() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return isTerminalStatus(status_); });

    switch (status_) {
        case TaskStatus::Succeeded:
            return *result_;
        case TaskStatus::Failed:
            failureObserved_ = true;
            std::rethrow_exception(error_);
        case TaskStatus::Cancelled: {
            failureObserved_ = true;
            Value object = makeEmptyObject("CancellationException");
            auto ex = std::get<ObjectRef>(object);
            ex->fields["message"] = std::string("task was cancelled");
            throw ZlThrownException(std::move(ex));
        }
        case TaskStatus::Pending:
        case TaskStatus::Running:
            break;
    }

    throw std::logic_error("invalid task state");
}

} // namespace zl
