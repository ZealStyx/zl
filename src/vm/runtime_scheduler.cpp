#include "zl/vm/runtime_scheduler.hpp"

#include <stdexcept>
#include <utility>

namespace zl {

AsyncFrame::AsyncFrame(ResumeFn resume)
    : resume_(std::move(resume)) {
    if (!resume_) {
        throw std::invalid_argument("async frame requires a resume continuation");
    }
}

void AsyncFrame::resume() {
    ResumeFn resume = std::move(resume_);
    if (!resume) {
        throw std::logic_error("async frame has already been resumed");
    }
    resume();
}

void RuntimeScheduler::enqueue(AsyncFrameRef frame) {
    if (!frame) {
        throw std::invalid_argument("scheduler cannot enqueue a null async frame");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ready_.push_back(std::move(frame));
}

bool RuntimeScheduler::runOne() {
    AsyncFrameRef frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_.empty()) return false;
        frame = std::move(ready_.front());
        ready_.pop_front();
    }
    frame->resume();
    return true;
}

std::size_t RuntimeScheduler::runUntilIdle() {
    std::size_t executed = 0;
    while (runOne()) ++executed;
    return executed;
}

std::size_t RuntimeScheduler::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_.size();
}

} // namespace zl
