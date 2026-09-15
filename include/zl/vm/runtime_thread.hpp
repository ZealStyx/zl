#pragma once

#include "zl/vm/runtime_exception.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zl {

// Process-wide worker-thread bookkeeping.
// - gAliveWorkerThreads: runnable entities besides the calling thread that
//   might unblock a channel wait: Thread.start workers currently running
//   (counted before spawn, released at exit) plus CPU-pool tasks currently
//   queued or running (counted at enqueue, released at completion). A channel
//   operation about to block when this is 0 and the scheduler has no pending
//   frames cannot ever be unblocked - the blocking thread is the only runnable
//   entity - so it is reported as a deadlock instead of hanging forever.
// - gIsWorkerThread: true on ZL worker threads. Deadlock detection is
//   disabled there (other threads, e.g. main, may still make progress).
// - gAbandonedWorkerThreads: set when the interpreter abandoned a worker at
//   teardown instead of waiting forever for it; the host main() then exits
//   without running static destructors, because the abandoned thread may
//   still touch global state.
inline std::atomic<int> gAliveWorkerThreads{0};
inline thread_local bool gIsWorkerThread = false;
inline std::atomic<bool> gAbandonedWorkerThreads{false};

// Allocated before a thread starts, so handing off its joinable handle from a
// noexcept destructor never needs to allocate. GC references do not live here.
struct ThreadJoinNode {
    std::thread thread;
    // Set to true by the worker right before it exits, so a teardown join can
    // wait with a timeout instead of an unbounded thread::join().
    std::shared_ptr<std::atomic<bool>> done;
    std::unique_ptr<ThreadJoinNode> next;
    // The destructor is a teardown context (a drop with no active joins
    // queue): it must not hang the process on a worker blocked forever.
    ~ThreadJoinNode();
    // Mid-run wait: the worker is doing real work the program may depend on.
    void joinUnbounded();
    // Teardown join: waits up to the bound for the worker to finish; if it is
    // still running (e.g. blocked forever on a channel receive nobody will
    // serve), prints a diagnostic, detaches it and flags the abandonment.
    void joinOrAbandon();
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
    // Bounded drains are for teardown only (end of VM::run, VM destruction):
    // they abandon still-running workers instead of waiting forever. A mid-run
    // drain keeps waiting - a dropped-but-live worker is doing real work the
    // program may depend on, and only the exit path must not hang.
    void drain() noexcept;
    void drainBounded() noexcept;
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
        worker->done = done;
        // Count the worker BEFORE it is spawned, from the starting thread: a
        // channel operation on the starter can observe the alive count before
        // the OS thread has run its first instruction, and a 0 there would
        // turn a legitimate block into a false deadlock report.
        gAliveWorkerThreads.fetch_add(1, std::memory_order_release);
        try {
            worker->thread = std::thread([done, failure, failureMutex, fn = std::forward<Fn>(fn)]() mutable {
                gIsWorkerThread = true;
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
                gAliveWorkerThreads.fetch_sub(1, std::memory_order_release);
            });
        } catch (...) {
            gAliveWorkerThreads.fetch_sub(1, std::memory_order_release);
            throw;
        }
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
