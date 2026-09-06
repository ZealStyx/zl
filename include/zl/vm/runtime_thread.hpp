#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <stdexcept>
#include <memory>

namespace zl {

// Runtime ownership for an explicit OS-backed ZL Thread. The VM executes one
// synchronous, zero-argument closure on this thread. Uncaught exceptions are
// deliberately fatal at the process level; join() is only a lifecycle operation.
class RuntimeThreadState {
public:
    RuntimeThreadState() = default;
    ~RuntimeThreadState();
    RuntimeThreadState(const RuntimeThreadState&) = delete;
    RuntimeThreadState& operator=(const RuntimeThreadState&) = delete;

    void join();
    [[nodiscard]] bool isAlive() const noexcept;

    template <typename Fn>
    void startWith(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) throw std::logic_error("thread has already been started");
        started_ = true;
        auto done = done_;
        done->store(false, std::memory_order_release);
        worker_ = std::thread([done, fn = std::forward<Fn>(fn)]() mutable {
            try { fn(); } catch (...) { done->store(true, std::memory_order_release); throw; }
            done->store(true, std::memory_order_release);
        });
    }

private:
    mutable std::mutex mutex_;
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> done_{std::make_shared<std::atomic<bool>>(true)};
    bool started_{false};
};

} // namespace zl
