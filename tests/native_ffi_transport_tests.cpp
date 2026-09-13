// FFI transport regressions: the pack/unpack adapters are the only
// conversion points between ZL values and the C ABI. Handles, buffer views,
// struct views, and callbacks cross as tokens and copies of pointer/size
// data - never as live ZL references - and every adapter fails closed on an
// invalid input instead of fabricating an ABI value.
#include "zl/compiler/native_ffi_transport.hpp"

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>

namespace {

using namespace zl;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "ffi transport regression: " << message << '\n';
    ++failures;
}

void testHandleTransport() {
    auto ref = nativeResourceRegistry().insert(
        NativeResourceOwner(0xaaa, [](NativeResourceOwner::Handle) {}));
    const auto packed = packNativeHandle(ref);
    require(packed.tag == ZL_NATIVE_HANDLE, "a handle packs as the handle tag");
    require(packed.data.handle == ref.id, "the token carries the registry id, not a pointer");

    const auto unpacked = unpackNativeHandle(packed);
    require(unpacked == ref, "pack/unpack round-trips a handle");

    bool threw = false;
    try {
        packNativeHandle(NativeHandleRef{});
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "cannot pass an invalid native handle";
    }
    require(threw, "an invalid handle cannot be packed");

    ZlNativeValue notAHandle{};
    notAHandle.tag = ZL_NATIVE_I64;
    notAHandle.data.i64 = ref.id;
    threw = false;
    try {
        unpackNativeHandle(notAHandle);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "invalid native handle transport value";
    }
    require(threw, "an i64 is not a handle in disguise");

    nativeResourceRegistry().close(ref);
}

void testBufferViewTransport() {
    const char payload[5] = {1, 2, 3, 4, 5};
    NativeResourceOwner owner(0xbbb, [](NativeResourceOwner::Handle) {});
    NativeBufferView view(owner, payload, sizeof(payload));
    const auto packed = packNativeBufferView(view);
    require(packed.tag == ZL_NATIVE_BUFFER_VIEW, "a view packs as the buffer-view tag");
    require(packed.data.buffer.data == payload, "the pack copies the pointer");
    require(packed.data.buffer.size == sizeof(payload), "the pack copies the byte size");

    bool threw = false;
    try {
        packNativeBufferView(NativeBufferView{});
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "cannot pass an invalid native buffer view";
    }
    require(threw, "an invalid buffer view cannot be packed");
}

void testStructViewTransport() {
    alignas(8) const std::uint64_t payload[2] = {0x1000, 0x2000};
    NativeResourceOwner owner(0xccc, [](NativeResourceOwner::Handle) {});
    NativeStructView view(owner, payload, sizeof(payload), 8);
    const auto packed = packNativeStructView(view);
    require(packed.tag == ZL_NATIVE_STRUCT_VIEW, "a struct packs as the struct-view tag");
    require(packed.data.structView.data == payload, "the pointer is copied");
    require(packed.data.structView.size == sizeof(payload), "the size is copied");
    require(packed.data.structView.alignment == 8, "the alignment is copied");
}

void testCallbackTransport() {
    auto callback = nativeCallbackRegistry().insert(
        [](const ZlNativeValue*, std::uint32_t, ZlNativeValue*, char*, std::uint32_t) { return 0; });
    const auto packed = packNativeCallback(callback);
    require(packed.tag == ZL_NATIVE_CALLBACK, "a callback packs as the callback tag");
    require(packed.data.callback == callback.id, "the token carries the registry id");
    const auto unpacked = unpackNativeCallback(packed);
    require(unpacked == callback, "pack/unpack round-trips a callback");

    bool threw = false;
    try {
        packNativeCallback(NativeCallbackRef{});
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "cannot pass an invalid native callback";
    }
    require(threw, "an invalid callback cannot be packed");

    ZlNativeValue notACallback{};
    notACallback.tag = ZL_NATIVE_HANDLE;
    notACallback.data.handle = callback.id;
    threw = false;
    try {
        unpackNativeCallback(notACallback);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "invalid native callback transport value";
    }
    require(threw, "a handle is not a callback in disguise");

    nativeCallbackRegistry().close(callback);
}

void testOwnershipAdapters() {
    // adopt -> take is the round trip the C++ side of the boundary uses.
    {
        std::atomic<int> destroyed{0};
        auto packed = packOwnedNativeResource(
            NativeResourceOwner(0xddd, [&destroyed](NativeResourceOwner::Handle) { ++destroyed; }));
        require(packed.tag == ZL_NATIVE_HANDLE, "an adopted resource packs as a handle");
        auto ref = unpackNativeHandle(packed);
        require(nativeResourceRegistry().contains(ref), "adoption registered the resource");

        auto owner = takeOwnedNativeResource(ref);
        require(owner.valid() && owner.get() == 0xddd, "takeOwned recovers the owner");
        require(!nativeResourceRegistry().contains(ref), "and removes the token");
        owner.reset();
        require(destroyed.load() == 1, "the resource is destroyed exactly once at the end");
    }
    {
        // takeOwned on an unknown token is an error, not a null owner.
        bool threw = false;
        try {
            auto owner = takeOwnedNativeResource(NativeHandleRef{0x9999});
            (void)owner;
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()) == "invalid native handle";
        }
        require(threw, "taking an unknown token throws");
    }
    {
        // borrowNativeResourceBuffer hands out a view bound to the live owner.
        const char payload[2] = {'x', 'y'};
        auto ref = nativeResourceRegistry().insert(
            NativeResourceOwner(0xeee, [](NativeResourceOwner::Handle) {}));
        auto view = borrowNativeResourceBuffer(ref, payload, sizeof(payload));
        require(view.valid(), "the borrowed view is valid while the owner lives");
        require(view.data() == payload && view.size() == 2, "and it sees the bytes");
        nativeResourceRegistry().close(ref);
        require(!view.valid(), "the view dies when the registry releases the owner");
    }
}

} // namespace

int main() {
    testHandleTransport();
    testBufferViewTransport();
    testStructViewTransport();
    testCallbackTransport();
    testOwnershipAdapters();

    if (failures != 0) {
        std::cerr << failures << " ffi transport regression(s) failed\n";
        return 1;
    }
    std::cout << "all ffi transport regressions passed\n";
    return 0;
}
