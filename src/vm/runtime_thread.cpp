#include "zl/vm/runtime_thread.hpp"

#include <chrono>
#include <iostream>

namespace zl {
namespace {
thread_local DeferredThreadJoins* currentJoins = nullptr;

// How long a teardown join waits for a worker before abandoning it.
constexpr auto kTeardownJoinBound = std::chrono::seconds(2);
constexpr auto kTeardownJoinPoll = std::chrono::milliseconds(2);
} // namespace

ThreadJoinNode::~ThreadJoinNode() {
    // A node destroyed directly (rather than through a joins-queue drain) is
    // in a teardown context - e.g. the VM is gone and its state teardown
    // dropped a still-running worker. Never wait forever there.
    joinOrAbandon();
}

void ThreadJoinNode::joinUnbounded() {
    if (!thread.joinable()) return;
    if (thread.get_id() == std::this_thread::get_id()) {
        thread.detach();
        return;
    }
    thread.join();
}

void ThreadJoinNode::joinOrAbandon() {
    if (!thread.joinable()) return;
    if (thread.get_id() == std::this_thread::get_id()) {
        thread.detach();
        return;
    }
    if (!done) {
        // No completion flag (should not happen - startWith always sets it):
        // fall back to a plain join rather than risk an early abandon.
        thread.join();
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + kTeardownJoinBound;
    while (!done->load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "zl: worker thread still running after "
                      << std::chrono::duration_cast<std::chrono::seconds>(kTeardownJoinBound).count()
                      << "s at interpreter shutdown (it is likely blocked forever on a channel"
                         " receive or similar); abandoning it and exiting\n";
            gAbandonedWorkerThreads.store(true, std::memory_order_release);
            thread.detach();
            return;
        }
        std::this_thread::sleep_for(kTeardownJoinPoll);
    }
    thread.join();
}

DeferredThreadJoins::~DeferredThreadJoins() { drainBounded(); }

DeferredThreadJoins* DeferredThreadJoins::bind(DeferredThreadJoins* queue) noexcept {
    return std::exchange(currentJoins, queue);
}

void DeferredThreadJoins::retire(std::unique_ptr<ThreadJoinNode> node) noexcept {
    if (!node || !node->thread.joinable()) return;
    if (!currentJoins || node->thread.get_id() == std::this_thread::get_id()) return;
    node->next = std::move(currentJoins->pending_);
    currentJoins->pending_ = std::move(node);
}

void DeferredThreadJoins::drain() noexcept {
    while (pending_) {
        auto node = std::move(pending_);
        pending_ = std::move(node->next);
        // Bounded everywhere: a retired worker's ZL handle is gone, so no one
        // can ever join it or observe its completion - waiting longer than the
        // bound only risks hanging the process on a worker blocked forever.
        // Workers that finish within the bound are joined normally, so the
        // implicit-join contract holds for every well-behaved program.
        node->joinOrAbandon();
    }
}

void DeferredThreadJoins::drainBounded() noexcept { drain(); }

RuntimeThreadState::~RuntimeThreadState() {
    std::unique_ptr<ThreadJoinNode> local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local = std::move(worker_);
    }
    DeferredThreadJoins::retire(std::move(local));
}

void RuntimeThreadState::join() {
    std::thread local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) throw std::logic_error("thread has not been started");
        if (!worker_ || !worker_->thread.joinable()) return;
        if (worker_->thread.get_id() == std::this_thread::get_id())
            throw std::logic_error("thread cannot join itself");
        local = std::move(worker_->thread);
    }
    local.join();

    // The worker captured any uncaught exception rather than letting it escape
    // its entry point (which would call std::terminate). Rethrow it here so
    // the joining thread sees a normal, catchable failure.
    std::lock_guard<std::mutex> failureLock(*failureMutex_);
    if (*failure_) failure_->rethrow();
}

std::shared_ptr<StoredException> RuntimeThreadState::failure() const {
    std::lock_guard<std::mutex> lock(*failureMutex_);
    return failure_;
}

bool RuntimeThreadState::isAlive() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && done_ && !done_->load(std::memory_order_acquire);
}
} // namespace zl
