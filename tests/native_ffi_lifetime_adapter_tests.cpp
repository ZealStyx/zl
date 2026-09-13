// FFI lifetime-adapter regressions: the host-to-ZL callback contract. A
// lifetime token is either open (leases can be taken) or closed (no new
// leases, and close() waits for the in-flight ones to drain). Leases are
// move-only RAII counters; the registry couples each callback to its lifetime
// so a native thread cannot call into an expired callback context.
#include "zl/vm/native_resource.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace zl;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "ffi lifetime regression: " << message << '\n';
    ++failures;
}

void testLifetimeOpenClose() {
    auto lifetime = makeNativeCallbackLifetime();
    require(!lifetime->closed(), "a fresh lifetime is open");
    require(lifetime->tryAcquire(), "an open lifetime accepts a lease");
    require(lifetime->tryAcquire(), "and again");
    lifetime->release();
    lifetime->release();
    require(lifetime->tryAcquire(), "releases do not close the lifetime");
    lifetime->release();

    lifetime->close();
    require(lifetime->closed(), "close() closes the lifetime");
    require(!lifetime->tryAcquire(), "a closed lifetime refuses new leases");
    lifetime->release(); // draining a zero-active lifetime is a no-op, not an underflow
}

void testCloseWaitsForInFlight() {
    auto lifetime = makeNativeCallbackLifetime();
    std::atomic<int> observers{0};
    // A worker takes a lease, "works", and holds it while a flag is set -
    // close() must block until the work is done, not race past it.
    std::thread worker([&lifetime, &observers] {
        auto lease = acquireNativeCallbackLease(lifetime);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::atomic_fetch_add(&observers, 1);
    });
    // Give the worker a head start so the lease is definitely active.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    lifetime->close(); // must block until the worker's lease releases
    worker.join();
    require(observers.load() == 1, "the in-flight lease finished before close() returned");
    require(!lifetime->tryAcquire(), "and the lifetime is closed afterwards");
}

void testLeaseMoveSemantics() {
    auto lifetime = makeNativeCallbackLifetime();
    auto first = acquireNativeCallbackLease(lifetime);
    require(first.valid(), "an acquired lease is valid");

    auto second = std::move(first);
    require(second.valid(), "the lease survives the move");
    require(lifetime->tryAcquire(), "a moved lease leaves the lifetime open");
    lifetime->release(); // release the probe
    second = NativeCallbackLease{}; // release the moved lease explicitly
    lifetime->release(); // one release too many: must not underflow the counter
    require(lifetime->tryAcquire(), "the counter did not run below zero");
    lifetime->release();

    // Assignment moves too: the assignee's old lease is released exactly once.
    auto a = acquireNativeCallbackLease(lifetime);
    auto b = acquireNativeCallbackLease(lifetime);
    b = std::move(a);
    require(b.valid(), "assignment transfers the lease");
    b = NativeCallbackLease{};
    require(!b.valid(), "assigning an empty lease drops the old one");
    require(lifetime->tryAcquire(), "both leases were released exactly once");
    lifetime->release();
    lifetime->close();
}

void testLeaseOnClosedLifetimeThrows() {
    auto lifetime = makeNativeCallbackLifetime();
    lifetime->close();
    bool threw = false;
    try {
        auto lease = acquireNativeCallbackLease(lifetime);
        (void)lease;
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "native callback lifetime is closed";
    }
    require(threw, "acquiring a lease on a closed lifetime throws");
}

void testRegistryLifetimeCoupling() {
    auto callback = nativeCallbackRegistry().insert(
        [](const ZlNativeValue*, std::uint32_t, ZlNativeValue* result, char*, std::uint32_t) {
            result->tag = ZL_NATIVE_I64;
            result->data.i64 = 1;
            return 0;
        });
    ZlNativeValue result{};
    char error[64] = {};
    const std::int32_t status =
        nativeCallbackRegistry().invoke(callback, nullptr, 0, &result, error, sizeof(error));
    require(status == 0 && result.data.i64 == 1, "an open callback invokes");

    // close() erases the entry: late invokes fail through the registry, and
    // the lifetime is closed for any holder of a shared_ptr to it.
    nativeCallbackRegistry().close(callback);
    const std::int32_t late =
        nativeCallbackRegistry().invoke(callback, nullptr, 0, &result, error, sizeof(error));
    require(late == -1, "a closed callback refuses to invoke");
    require(std::string(error) == "invalid native callback",
            "with the registry diagnostic (got: " + std::string(error) + ")");
}

void testConcurrentLeaseCounting() {
    auto lifetime = makeNativeCallbackLifetime();
    const int threads = 8;
    const int perThread = 2000;
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&lifetime, perThread] {
            for (int i = 0; i < perThread; ++i) {
                auto lease = acquireNativeCallbackLease(lifetime);
                (void)lease;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    require(lifetime->tryAcquire(), "after all leases release, the lifetime is open again");
    lifetime->release();
    lifetime->close();
    require(!lifetime->tryAcquire(), "and closed stays closed");
}

} // namespace

int main() {
    testLifetimeOpenClose();
    testCloseWaitsForInFlight();
    testLeaseMoveSemantics();
    testLeaseOnClosedLifetimeThrows();
    testRegistryLifetimeCoupling();
    testConcurrentLeaseCounting();

    if (failures != 0) {
        std::cerr << failures << " ffi lifetime regression(s) failed\n";
        return 1;
    }
    std::cout << "all ffi lifetime regressions passed\n";
    return 0;
}
