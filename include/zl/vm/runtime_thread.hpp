#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zl {

// Allocated before a thread starts, so handing off its joinable handle from a
// noexcept destructor never needs to allocate. GC references do not live here.
struct ThreadJoinNode {
    std::thread thread;
    std::unique_ptr<ThreadJoinNode> next;
    ~ThreadJoinNode();
};

// A VM instruction can destroy a Thread inside a map/vector update. Joining
// there would park a half-mutated graph (or deadlock a worker at a safepoint).
// Delay just the native join until the VM next reaches a stable boundary.
class DeferredThreadJoins {
public:
    ~DeferredThreadJoins();
    static DeferredThreadJoins* bind(DeferredThreadJoins* queue) noexcept;
    static void retire(std::unique_ptr<ThreadJoinNode> node) noexcept;
    bool empty() const noexcept { return !pending_; }
    void drain() noexcept;
private:
    std::unique_ptr<ThreadJoinNode> pending_;
};

// Runtime ownership for an explicit OS-backed ZL Thread. Uncaught worker
// exceptions remain process-fatal. Explicit join still waits synchronously;
// implicit joins wait at a stable VM boundary rather than inside Value teardown.
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
        auto worker = std::make_unique<ThreadJoinNode>();
        auto done = done_;
        done->store(false, std::memory_order_release);
        worker->thread = std::thread([done, fn = std::forward<Fn>(fn)]() mutable {
            try { fn(); } catch (...) { done->store(true, std::memory_order_release); throw; }
            done->store(true, std::memory_order_release);
        });
        worker_ = std::move(worker);
        started_ = true;
    }

private:
    mutable std::mutex mutex_;
    std::unique_ptr<ThreadJoinNode> worker_;
    std::shared_ptr<std::atomic<bool>> done_{std::make_shared<std::atomic<bool>>(true)};
    bool started_{false};
};
} // namespace zl
