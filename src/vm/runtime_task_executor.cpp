#include "zl/vm/runtime_task_executor.hpp"

#include <algorithm>
#include <stdexcept>

namespace zl {

RuntimeTaskExecutor& RuntimeTaskExecutor::instance() {
    static RuntimeTaskExecutor executor;
    return executor;
}

RuntimeTaskExecutor::RuntimeTaskExecutor() {
    const auto count = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

RuntimeTaskExecutor::~RuntimeTaskExecutor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

void RuntimeTaskExecutor::enqueue(std::function<void()> work) {
    if (!work) throw std::invalid_argument("CPU task executor cannot enqueue empty work");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) throw std::runtime_error("CPU task executor is shutting down");
        queue_.push(std::move(work));
    }
    condition_.notify_one();
}

void RuntimeTaskExecutor::workerLoop() {
    while (true) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            work = std::move(queue_.front());
            queue_.pop();
        }
        try {
            work();
        } catch (...) {
            // Individual task work is responsible for translating failures into
            // Task state. Never let a worker exception escape the OS thread.
        }
    }
}

} // namespace zl
