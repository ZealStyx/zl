#include "zl/vm/runtime_thread.hpp"

namespace zl {
namespace {
thread_local DeferredThreadJoins* currentJoins = nullptr;
}

ThreadJoinNode::~ThreadJoinNode() {
    if (!thread.joinable()) return;
    if (thread.get_id() == std::this_thread::get_id()) thread.detach();
    else thread.join();
}

DeferredThreadJoins::~DeferredThreadJoins() { drain(); }

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
        node.reset();
    }
}

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
