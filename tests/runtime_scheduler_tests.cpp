// Cooperative scheduler regressions: frame resume semantics, FIFO scheduling
// order, rescheduling (frames that enqueue their successors), idle behavior,
// starved-queue exhaustion at scale, and thread-safe concurrent enqueue.
#include "zl/vm/runtime_exception.hpp"
#include "zl/vm/runtime_scheduler.hpp"
#include "zl/vm/runtime_task.hpp"
#include "zl/vm/value.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "scheduler regression: " << message << '\n';
    ++failures;
}

using namespace std::chrono_literals;

void testAsyncFrameResume() {
    int runs = 0;
    auto frame = std::make_shared<zl::AsyncFrame>([&runs] { ++runs; });
    frame->resume();
    require(runs == 1, "resume runs the continuation");
    bool doubleResumeRejected = false;
    try {
        frame->resume();
    } catch (const std::logic_error&) {
        doubleResumeRejected = true;
    }
    require(doubleResumeRejected, "a frame cannot resume twice");
    bool emptyRejected = false;
    try {
        std::shared_ptr<zl::AsyncFrame> empty(new zl::AsyncFrame(zl::AsyncFrame::ResumeFn{}));
        (void)empty;
    } catch (const std::invalid_argument&) {
        emptyRejected = true;
    }
    require(emptyRejected, "a frame without a continuation is rejected");
}

void testFifoOrder() {
    zl::RuntimeScheduler scheduler;
    std::vector<int> order;
    for (int i = 0; i < 16; ++i) {
        scheduler.enqueue(std::make_shared<zl::AsyncFrame>([&order, i] { order.push_back(i); }));
    }
    require(scheduler.pendingCount() == 16, "the queue reports its depth");
    while (scheduler.runOne()) {
    }
    bool fifo = order.size() == 16;
    for (int i = 0; fifo && i < 16; ++i) fifo = order[i] == i;
    require(fifo, "frames run in FIFO order");
    require(!scheduler.runOne(), "an idle scheduler reports no frame");
    require(scheduler.runUntilIdle() == 0, "runUntilIdle on an empty queue runs nothing");
}

void testRescheduling() {
    zl::RuntimeScheduler scheduler;
    int completed = 0;
    // Each frame enqueues its successor: the scheduler must keep running until
    // the chain settles, including work enqueued from inside runOne.
    scheduler.enqueue(std::make_shared<zl::AsyncFrame>([&scheduler, &completed] {
        scheduler.enqueue(std::make_shared<zl::AsyncFrame>([&completed] { ++completed; }));
    }));
    const std::size_t executed = scheduler.runUntilIdle();
    require(executed == 2, "a frame enqueued during runOne is drained");
    require(completed == 1, "the rescheduled frame ran");
    require(scheduler.pendingCount() == 0, "the scheduler settles empty");
}

void testEnqueueValidationAndConcurrentFeed() {
    zl::RuntimeScheduler scheduler;
    bool nullRejected = false;
    try {
        scheduler.enqueue({});
    } catch (const std::invalid_argument&) {
        nullRejected = true;
    }
    require(nullRejected, "a null frame is rejected");

    std::atomic<int> ran{0};
    const int threads = 4;
    const int perThread = 500;
    std::vector<std::thread> feeders;
    for (int t = 0; t < threads; ++t) {
        // perThread is captured explicitly. Reading a const int with a
        // constant initializer is not an odr-use, so the standard needs no
        // capture at all - but MSVC rejects that with C3493 ("cannot be
        // implicitly captured") unless the variable is named here.
        feeders.emplace_back([&scheduler, &ran, perThread] {
            for (int i = 0; i < perThread; ++i) {
                scheduler.enqueue(std::make_shared<zl::AsyncFrame>([&ran] { ++ran; }));
            }
        });
    }
    for (auto& feeder : feeders) feeder.join();
    const std::size_t executed = scheduler.runUntilIdle();
    require(executed == threads * perThread, "concurrent enqueues all settle");
    require(ran.load() == threads * perThread, "every frame ran exactly once");
}

void testNoStarvationAtScale() {
    zl::RuntimeScheduler scheduler;
    const int frames = 10000;
    std::atomic<int> ran{0};
    for (int i = 0; i < frames; ++i) {
        scheduler.enqueue(std::make_shared<zl::AsyncFrame>([&ran] { ++ran; }));
    }
    const auto start = std::chrono::steady_clock::now();
    const std::size_t executed = scheduler.runUntilIdle();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(executed == frames, "no frame in a long queue is dropped");
    require(ran.load() == frames, "every frame in a long queue runs");
    require(elapsed < std::chrono::seconds(30), "draining 10k frames is not pathologically slow");
}

void testTaskIntegration() {
    zl::RuntimeScheduler scheduler;
    auto task = std::make_shared<zl::RuntimeTaskState>("int");
    scheduler.enqueue(std::make_shared<zl::AsyncFrame>([task] {
        task->start();
        task->succeed(zl::Value(99));
    }));
    scheduler.runUntilIdle();
    require(task->status() == zl::TaskStatus::Succeeded, "a frame can settle a task");
    require(task->observe() == zl::Value(99), "the task carries its result");
}

} // namespace

int main() {
    testAsyncFrameResume();
    testFifoOrder();
    testRescheduling();
    testEnqueueValidationAndConcurrentFeed();
    testNoStarvationAtScale();
    testTaskIntegration();

    if (failures != 0) {
        std::cerr << failures << " scheduler regression(s) failed\n";
        return 1;
    }
    std::cout << "all scheduler regressions passed\n";
    return 0;
}
