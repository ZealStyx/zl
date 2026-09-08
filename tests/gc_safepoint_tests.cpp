// Coordinator regression with a controlled collector, not a timing-dependent
// heap stress test. Link only gc_safepoint.cpp; the tracer is replaced below so
// a collection can be held open while a native waiter tries to resume.
#include "zl/vm/gc.hpp"
#include "zl/vm/gc_safepoint.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
std::mutex mutex;
std::condition_variable cv;
bool pressure = false;
bool sampled = false;
bool collecting = false;
bool holdCollection = true;
bool finished = false;
bool failCollection = false;
std::vector<zl::Value> collectedRoots;
std::atomic<const char*> stage{"starting"};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "safepoint regression: " << message << '\n';
        std::abort();
    }
}

void requestCollection() {
    std::lock_guard<std::mutex> lock(mutex);
    pressure = true;
    sampled = false;
}

bool containsRoot(std::int64_t value) {
    return std::any_of(collectedRoots.begin(), collectedRoots.end(), [&](const zl::Value& root) {
        const auto* number = std::get_if<std::int64_t>(&root);
        return number && *number == value;
    });
}
} // namespace

namespace zl {
TracingGC& TracingGC::instance() {
    static TracingGC gc;
    return gc;
}
bool TracingGC::shouldCollect() const {
    std::lock_guard<std::mutex> lock(mutex);
    sampled = true;
    cv.notify_all();
    return pressure;
}
TracingGC::Collection TracingGC::collect(const GCRoots& roots) {
    std::unique_lock<std::mutex> lock(mutex);
    require(!collecting, "two collectors elected for one rendezvous");
    collecting = true;
    collectedRoots = roots.values;
    cv.notify_all();
    cv.wait(lock, [] { return !holdCollection; });
    collecting = false;
    pressure = false;
    if (failCollection) {
        failCollection = false;
        throw std::runtime_error("controlled collection failure");
    }
    return {};
}
} // namespace zl

int main() {
    // On a liveness failure, report the exact transition being tested instead
    // of interpreting an undiagnosed process timeout as a pass/fail signal.
    std::thread watchdog([] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, 10s, [] { return finished; })) {
            std::cerr << "safepoint transition stalled: " << stage.load() << '\n';
            std::abort();
        }
    });
    auto& coordinator = zl::GCSafepointCoordinator::instance();
    const auto runner = coordinator.registerParticipant();
    const auto waiter = coordinator.registerParticipant();
    const auto idle = coordinator.registerParticipant();
    coordinator.beginBlockingNative(waiter, {std::int64_t{11}});
    coordinator.beginBlockingNative(idle, {std::int64_t{33}});

    requestCollection();
    std::thread poller([&] { coordinator.poll(runner, {std::int64_t{22}}); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [] { return collecting; });
    }
    stage = "native reactivation must wait for the running collector";
    std::promise<void> attemptingResume;
    auto resumed = std::async(std::launch::async, [&] {
        attemptingResume.set_value();
        coordinator.endBlockingNative(waiter);
        std::lock_guard<std::mutex> lock(mutex);
        return !collecting;
    });
    attemptingResume.get_future().wait();
    require(resumed.wait_for(30ms) == std::future_status::timeout,
            "native waiter resumed while the collector was held open");
    {
        std::lock_guard<std::mutex> lock(mutex);
        require(containsRoot(11) && containsRoot(22) && containsRoot(33),
                "collector did not receive every parked root snapshot");
        holdCollection = false;
        cv.notify_all();
    }
    require(resumed.get(), "native waiter raced tracing");
    poller.join();

    // A parent may create a child during a pending rendezvous, then finish
    // without polling. Registration must not wait for the request to finish;
    // unregistration must wake a poller to take over collection.
    stage = "registration during a request / last active participant exiting";
    requestCollection();
    std::thread nextPoller([&] { coordinator.poll(runner, {std::int64_t{44}}); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [] { return sampled; });
    }
    const auto child = coordinator.registerParticipant();
    coordinator.unregisterParticipant(child);
    coordinator.unregisterParticipant(waiter);
    nextPoller.join();
    require(containsRoot(33) && containsRoot(44),
            "idle participant lost its roots across collections");
    stage = "collection failure must release the rendezvous";
    {
        std::lock_guard<std::mutex> lock(mutex);
        failCollection = true;
    }
    requestCollection();
    bool caught = false;
    try { coordinator.poll(runner, {std::int64_t{55}}); }
    catch (const std::runtime_error&) { caught = true; }
    require(caught, "controlled collection failure was swallowed");
    requestCollection();
    coordinator.poll(runner, {std::int64_t{66}});
    require(containsRoot(33) && containsRoot(66), "failure damaged the next rendezvous/root snapshot");
    coordinator.endBlockingNative(idle);
    coordinator.unregisterParticipant(idle);
    coordinator.unregisterParticipant(runner);
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
        cv.notify_all();
    }
    watchdog.join();
    std::cout << "safepoint transitions: PASS\n";
}
