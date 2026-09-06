#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace zl {

// A suspended async VM computation is represented by a heap object. The frame owns
// the resume continuation; the scheduler owns only ready-to-resume frames.
class AsyncFrame {
public:
    using ResumeFn = std::function<void()>;

    explicit AsyncFrame(ResumeFn resume);

    void resume();

private:
    ResumeFn resume_;
};

using AsyncFrameRef = std::shared_ptr<AsyncFrame>;

// VM-owned cooperative scheduler. It is intentionally single-policy and thread-safe:
// future CPU executors/workers can feed the same ready queue instead of creating a
// second scheduler implementation.
class RuntimeScheduler {
public:
    RuntimeScheduler() = default;
    RuntimeScheduler(const RuntimeScheduler&) = delete;
    RuntimeScheduler& operator=(const RuntimeScheduler&) = delete;

    void enqueue(AsyncFrameRef frame);
    [[nodiscard]] bool runOne();
    std::size_t runUntilIdle();
    [[nodiscard]] std::size_t pendingCount() const;

private:
    mutable std::mutex mutex_;
    std::deque<AsyncFrameRef> ready_;
};

} // namespace zl
