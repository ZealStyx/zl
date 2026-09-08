#pragma once

#include "zl/vm/runtime_exception.hpp"
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

    // Joins the thread. If its entry func terminated with an uncaught
    // exception, that exception is rethrown here in the joining thread, so a
    // worker failure surfaces as a normal catchable ZL exception instead of
    // killing the process.
    void join();
    [[nodiscard]] bool isAlive() const noexcept;

    // The failure captured from the worker, if any. Valid once the thread has
    // finished; used to keep the stored exception's payload GC-reachable.
    [[nodiscard]] std::shared_ptr<StoredException> failure() const;

    template <typename Fn>
    void startWith(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) throw std::logic_error("thread has already been started");
        auto worker = std::make_unique<ThreadJoinNode>();
        auto done = done_;
        auto failure = failure_;
        auto failureMutex = failureMutex_;
        done->store(false, std::memory_order_release);
        worker->thread = std::thread([done, failure, failureMutex, fn = std::forward<Fn>(fn)]() mutable {
            try {
                fn();
            } catch (...) {
                // Capture rather than propagate: an exception escaping a
                // std::thread's entry point calls std::terminate. join()
                // rethrows this in the joining thread instead.
                // The collector does not trace threads, so this failure pins
                // its own payload until join() consumes it.
                std::lock_guard<std::mutex> lock(*failureMutex);
                *failure = StoredException(std::current_exception(),
                                           StoredException::Retention::Untraced);
            }
            done->store(true, std::memory_order_release);
        });
        worker_ = std::move(worker);
        started_ = true;
    }

private:
    mutable std::mutex mutex_;
    std::unique_ptr<ThreadJoinNode> worker_;
    std::shared_ptr<std::atomic<bool>> done_{std::make_shared<std::atomic<bool>>(true)};
    std::shared_ptr<std::mutex> failureMutex_{std::make_shared<std::mutex>()};
    std::shared_ptr<StoredException> failure_{std::make_shared<StoredException>()};
    bool started_{false};
};
} // namespace zl
