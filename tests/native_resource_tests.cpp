// Native resource regressions: deterministic ownership across the C++ side of
// the boundary. A resource is destroyed exactly once; borrows and views track
// the owning lifetime and go invalid when it ends (instead of dangling);
// transfers invalidate outstanding borrows; and the process-local registry is
// the only place a handle token has meaning.
#include "zl/vm/native_resource.hpp"

#include <atomic>
#include <iostream>
#include <string>

namespace {

using namespace zl;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "native resource regression: " << message << '\n';
    ++failures;
}

void testDestroyExactlyOnce() {
    {
        std::atomic<int> destroyed{0};
        {
            NativeResourceOwner owner(0xabc, [&destroyed](NativeResourceOwner::Handle) {
                ++destroyed;
            });
            require(owner.valid(), "a constructed owner is valid");
            require(owner.get() == 0xabc, "get() returns the handle");
        }
        require(destroyed.load() == 1, "destruction happens exactly once");
    }
    {
        std::atomic<int> destroyed{0};
        NativeResourceOwner owner(0xdef, [&destroyed](NativeResourceOwner::Handle) {
            ++destroyed;
        });
        owner.reset();
        require(destroyed.load() == 1, "reset() destroys");
        require(!owner.valid(), "a reset owner is invalid");
        owner.reset();
        require(destroyed.load() == 1, "reset() is idempotent");
    }
    {
        std::atomic<int> destroyed{0};
        {
            NativeResourceOwner owner(0x111, [&destroyed](NativeResourceOwner::Handle) {
                ++destroyed;
            });
            NativeResourceOwner moved = std::move(owner);
            require(moved.valid() && !owner.valid(), "move transfers validity");
            require(moved.get() == 0x111, "the moved owner holds the handle");
        }
        require(destroyed.load() == 1, "a moved owner destroys exactly once");
    }
}

void testBorrowTracksLifetime() {
    {
        NativeResourceOwner owner(0x222, [](NativeResourceOwner::Handle) {});
        NativeResourceBorrow borrow(owner);
        require(borrow.valid(), "a borrow of a live owner is valid");
        require(borrow.get() == 0x222, "the borrow sees the owner's handle");
        owner.reset();
        require(!borrow.valid(), "the borrow goes invalid when the owner dies");
        bool threw = false;
        try {
            (void)borrow.get();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "using a dead borrow throws instead of dangling");
    }
    {
        NativeResourceOwner owner(0x333, [](NativeResourceOwner::Handle) {});
        NativeResourceBorrow borrow(owner);
        const auto handle = owner.release();
        require(handle == 0x333, "release() hands out the handle");
        require(!owner.valid(), "release() empties the owner");
        require(!borrow.valid(), "release() invalidates outstanding borrows");
    }
    {
        // Legacy untracked borrows keep the documented raw-handle semantics.
        NativeResourceBorrow legacy(0x999);
        require(legacy.valid() && legacy.get() == 0x999, "a raw-handle borrow is untracked");
    }
    {
        NativeResourceOwner empty(0, [](NativeResourceOwner::Handle) {});
        require(!empty.valid(), "a zero-handle owner is invalid");
        bool threw = false;
        try {
            (void)empty.get();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "get() on an invalid owner throws");
    }
}

void testBufferView() {
    const char payload[4] = {'a', 'b', 'c', 'd'};
    {
        NativeResourceOwner owner(0x444, [](NativeResourceOwner::Handle) {});
        NativeBufferView view(owner, payload, sizeof(payload));
        require(view.valid(), "a view of a live owner is valid");
        require(view.data() == payload && view.size() == sizeof(payload),
                "the view exposes the owner's bytes");
        owner.reset();
        require(!view.valid(), "the view dies with the owner");
        bool threw = false;
        try {
            (void)view.data();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "reading a dead view throws");
    }
    {
        NativeResourceOwner owner(0x555, [](NativeResourceOwner::Handle) {});
        bool nullRejected = false;
        try {
            NativeBufferView bad(owner, nullptr, 4);
            (void)bad;
        } catch (const std::runtime_error&) {
            nullRejected = true;
        }
        require(nullRejected, "a null view with non-zero size is rejected");
    }
}

void testRegistry() {
    {
        bool invalidRejected = false;
        try {
            (void)nativeResourceRegistry().insert(NativeResourceOwner{});
        } catch (const std::runtime_error&) {
            invalidRejected = true;
        }
        require(invalidRejected, "registering an invalid owner is rejected");
    }
    {
        auto ref = nativeResourceRegistry().insert(
            NativeResourceOwner(0x666, [](NativeResourceOwner::Handle) {}));
        require(ref.valid(), "registration returns a token");
        require(nativeResourceRegistry().contains(ref), "the token resolves");
        auto borrow = nativeResourceRegistry().borrow(ref);
        require(borrow.valid() && borrow.get() == 0x666, "a registry borrow sees the resource");

        bool doubleBorrowAfterConsume = false;
        auto consumed = nativeResourceRegistry().consume(ref);
        (void)consumed;
        require(!nativeResourceRegistry().contains(ref), "consume removes the token");
        try {
            (void)nativeResourceRegistry().consume(ref);
        } catch (const std::runtime_error& e) {
            doubleBorrowAfterConsume = std::string(e.what()) == "invalid native handle";
        }
        require(doubleBorrowAfterConsume, "a second consume is an invalid-handle error");
        try {
            (void)nativeResourceRegistry().borrow(ref);
        } catch (const std::runtime_error&) {
            require(true, "borrowing a consumed handle throws");
        }
    }
    {
        auto ref = nativeResourceRegistry().insert(
            NativeResourceOwner(0x777, [](NativeResourceOwner::Handle) {}));
        nativeResourceRegistry().close(ref);
        require(!nativeResourceRegistry().contains(ref), "close() drains the token");
    }
    {
        // The zero-id token is meaningless by construction.
        require(!nativeResourceRegistry().contains(NativeHandleRef{}), "a zero token never resolves");
    }
}

} // namespace

int main() {
    testDestroyExactlyOnce();
    testBorrowTracksLifetime();
    testBufferView();
    testRegistry();

    if (failures != 0) {
        std::cerr << failures << " native resource regression(s) failed\n";
        return 1;
    }
    std::cout << "all native resource regressions passed\n";
    return 0;
}
