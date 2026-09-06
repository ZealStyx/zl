#include "zl/vm/runtime_thread.hpp"

namespace zl {

RuntimeThreadState::~RuntimeThreadState() {
    std::thread local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable()) return;
        if (worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
            return;
        }
        local = std::move(worker_);
    }
    local.join();
}

void RuntimeThreadState::join() {
    std::thread local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) throw std::logic_error("thread has not been started");
        if (!worker_.joinable()) return;
        if (worker_.get_id() == std::this_thread::get_id())
            throw std::logic_error("thread cannot join itself");
        local = std::move(worker_);
    }
    local.join();
}

bool RuntimeThreadState::isAlive() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && done_ && !done_->load(std::memory_order_acquire);
}

} // namespace zl
