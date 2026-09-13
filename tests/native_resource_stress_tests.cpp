// Native resource stress regressions: the FFI ownership machinery under
// real concurrent load. The guarantees that must hold at scale - destroy
// exactly once per resource, no lost or double frees, a consumed token
// rejects every later use, and close() drains in-flight callbacks without
// ever invoking expired state - are the ones a single-threaded unit test can
// only assert by construction. Here they are exercised by threads that race.
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
    std::cerr << "native resource stress regression: " << message << '\n';
    ++failures;
}

// Every resource is created in one thread, its token published to a pool of
// consumers, and consumed back exactly once. The destroy counter is the leak
// and double-free detector: it must land exactly on the total.
void testConcurrentConsumeIsExactlyOnce() {
    const int resources = 20000;
    const int consumers = 8;
    std::atomic<int> destroyed{0};
    std::vector<NativeHandleRef> tokens;
    tokens.reserve(resources);
    for (int i = 0; i < resources; ++i) {
        tokens.push_back(nativeResourceRegistry().insert(
            NativeResourceOwner(static_cast<std::uintptr_t>(0xA000 + i),
                                [&destroyed](NativeResourceOwner::Handle) { ++destroyed; })));
    }

    std::atomic<int> claims{0};
    std::atomic<int> taken{0};
    std::atomic<int> rejected{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < consumers; ++t) {
        workers.emplace_back([&] {
            // Each worker races for work; whoever takes a token owns it. The
            // claim counter runs past the total: the extra claims are what
            // tell a worker the pool is empty.
            for (;;) {
                const int index = claims.fetch_add(1);
                if (index >= resources) break;
                try {
                    auto owner = nativeResourceRegistry().consume(tokens[static_cast<std::size_t>(index)]);
                    (void)owner;
                    ++taken;
                } catch (const std::runtime_error&) {
                    ++rejected;
                }
            }
            // Now hammer the already-consumed pool: every use must reject.
            for (int i = 0; i < 200; ++i) {
                try {
                    static_cast<void>(nativeResourceRegistry().consume(tokens[static_cast<std::size_t>(i)]));
                } catch (const std::runtime_error&) {
                    ++rejected;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();

    require(taken.load() == resources, "every token was consumed exactly once");
    require(claims.load() >= resources, "workers kept claiming until the pool was empty");
    require(destroyed.load() == resources,
            "the destroy counter lands exactly on the total (no leak, no double free)");
    require(rejected.load() == consumers * 200,
            "every post-consume use was rejected as an invalid handle");
}

// Borrows race against consumption: each borrow either sees a valid token or
// a clean invalid-handle error - never a dangling handle. The accounting must
// be exact (no lost outcomes), and once everything is consumed every borrow
// must reject.
void testBorrowVsConsumeRace() {
    const int rounds = 20000;
    std::atomic<bool> sawDangling{false};
    std::atomic<int> validBorrows{0};
    std::atomic<int> cleanRejections{0};

    std::vector<NativeHandleRef> tokens(rounds);
    for (int i = 0; i < rounds; ++i) {
        tokens[static_cast<std::size_t>(i)] = nativeResourceRegistry().insert(
            NativeResourceOwner(static_cast<std::uintptr_t>(0xD000 + i),
                                [](NativeResourceOwner::Handle) {}));
    }

    std::thread borrower([&] {
        for (int i = 0; i < rounds; ++i) {
            try {
                auto borrow = nativeResourceRegistry().borrow(tokens[static_cast<std::size_t>(i)]);
                if (borrow.valid() &&
                    borrow.get() == static_cast<std::uintptr_t>(0xD000 + i)) {
                    ++validBorrows;
                } else {
                    sawDangling = true; // a valid borrow with the wrong handle is a lie
                }
            } catch (const std::runtime_error&) {
                ++cleanRejections;
            }
        }
    });
    // Let the borrower get moving, then consume every token while it walks
    // the same list: a genuine interleaving, whatever the scheduler chooses.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (int i = 0; i < rounds; ++i) {
        try {
            auto owner = nativeResourceRegistry().consume(tokens[static_cast<std::size_t>(i)]);
            (void)owner;
        } catch (const std::runtime_error&) {
            // unreachable: the borrower only borrows, it never consumes
        }
    }
    borrower.join();

    require(!sawDangling.load(), "a borrow never reports a handle the owner no longer controls");
    require(validBorrows.load() + cleanRejections.load() == rounds,
            "every borrow resolved to exactly one outcome (valid or clean rejection)");

    // After the drain, the rejection must be universal and clean.
    int rejectedAfter = 0;
    for (int i = 0; i < rounds; i += 97) {
        try {
            static_cast<void>(nativeResourceRegistry().borrow(tokens[static_cast<std::size_t>(i)]));
        } catch (const std::runtime_error&) {
            ++rejectedAfter;
        }
    }
    require(rejectedAfter > 0, "every post-drain borrow is rejected");
}

// Callback lifetime under load: invokers hammer a registered callback while
// close() races in from another thread. After close(), every invocation must
// refuse; before it, every one must run. No torn state in between.
void testCallbackCloseUnderLoad() {
    auto callback = nativeCallbackRegistry().insert(
        [](const ZlNativeValue*, std::uint32_t, ZlNativeValue* result, char*, std::uint32_t) {
            result->tag = ZL_NATIVE_I64;
            result->data.i64 = 1;
            return 0;
        });
    const int invokers = 4;
    std::atomic<int> ran{0};
    std::atomic<int> refused{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < invokers; ++t) {
        workers.emplace_back([&] {
            while (!stop.load()) {
                ZlNativeValue result{};
                char error[64] = {};
                const std::int32_t status =
                    nativeCallbackRegistry().invoke(callback, nullptr, 0, &result, error, sizeof(error));
                if (status == 0) {
                    ++ran;
                } else {
                    ++refused;
                }
            }
        });
    }
    // Close while the invokers are mid-flight, then tell them to stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    nativeCallbackRegistry().close(callback);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    stop = true;
    for (auto& worker : workers) worker.join();

    require(ran.load() > 0, "invocations ran before close()");
    require(refused.load() > 0, "invocations after close() were refused");

    // Late invokes after everyone has joined must keep refusing.
    ZlNativeValue result{};
    char error[64] = {};
    require(nativeCallbackRegistry().invoke(callback, nullptr, 0, &result, error, sizeof(error)) == -1,
            "post-drain invokes keep refusing");
}

} // namespace

int main() {
    testConcurrentConsumeIsExactlyOnce();
    testBorrowVsConsumeRace();
    testCallbackCloseUnderLoad();

    if (failures != 0) {
        std::cerr << failures << " native resource stress regression(s) failed\n";
        return 1;
    }
    std::cout << "all native resource stress regressions passed\n";
    return 0;
}
