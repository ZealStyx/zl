// Native boundary regressions: the ZL -> native -> ZL value transfer enforces
// invariants at the boundary, and every violation becomes a typed NativeError
// ZL exception with a message field - never a raw pointer, never a segfault.
// Ownership transfers are transactional (a failed call retains the caller's
// resource), returned owned handles must be pre-registered, views cannot be
// returned, and callbacks cross the ABI only as stable tokens with a
// lifetime lease.
#include "zl/vm/native_boundary.hpp"
#include "zl/vm/native_resource.hpp"

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace zl;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "native boundary regression: " << message << '\n';
    ++failures;
}

std::string thrownMessage(const std::function<void()>& call, bool& threw) {
    threw = false;
    try {
        call();
        return {};
    } catch (const ZlThrownException& e) {
        threw = true;
        const auto& fields = e.value()->fields;
        auto it = fields.find("message");
        if (it != fields.end() && std::holds_alternative<std::string>(it->second)) {
            return std::get<std::string>(it->second);
        }
        return {};
    }
}

// --- stub native implementations ------------------------------------------

struct CountingBox {
    int destroyed{0};
};

CountingBox* g_box = nullptr;

std::int32_t echoI64(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result,
                     char* error, std::uint32_t capacity) {
    if (!args || argc != 1) {
        if (error && capacity) std::snprintf(error, capacity, "expected one argument");
        return 1;
    }
    result->tag = ZL_NATIVE_I64;
    result->data.i64 = args[0].data.i64;
    return 0;
}

std::int32_t failWithMessage(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result,
                             char* error, std::uint32_t capacity) {
    (void)args;
    (void)argc;
    (void)result;
    if (error && capacity) std::snprintf(error, capacity, "deliberate native failure");
    return 1;
}

// Returns a handle the caller pre-registered; the boundary must verify it.
std::uint64_t g_returnHandle = 0;
std::int32_t returnPreRegistered(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result,
                                 char* error, std::uint32_t capacity) {
    (void)args;
    (void)argc;
    (void)error;
    (void)capacity;
    result->tag = ZL_NATIVE_HANDLE;
    result->data.handle = g_returnHandle;
    return 0;
}

std::int32_t returnWrongTag(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result,
                            char* error, std::uint32_t capacity) {
    (void)args;
    (void)argc;
    (void)error;
    (void)capacity;
    result->tag = ZL_NATIVE_BOOL;
    result->data.boolean = true;
    return 0;
}

std::int32_t returnBufferView(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result,
                              char* error, std::uint32_t capacity) {
    (void)args;
    (void)argc;
    (void)error;
    (void)capacity;
    result->tag = ZL_NATIVE_BUFFER_VIEW;
    result->data.buffer.data = nullptr;
    result->data.buffer.size = 0;
    return 0;
}

ZlNativeExport makeExport(const char* name, ZlNativeTypeTag returnType, ZlNativeInvokeFn invoke,
                          ZlNativeTypeTag* params = nullptr, std::uint32_t arity = 0,
                          ZlNativeOwnershipTag* ownership = nullptr,
                          ZlNativeOwnershipTag returnOwnership = ZL_NATIVE_OWNERSHIP_NONE) {
    ZlNativeExport exportInfo;
    exportInfo.name = name;
    exportInfo.arity = arity;
    exportInfo.parameterTypes = params;
    exportInfo.returnType = returnType;
    exportInfo.invoke = invoke;
    exportInfo.parameterOwnership = ownership;
    exportInfo.returnOwnership = returnOwnership;
    return exportInfo;
}

// A resource whose destruction is observable.
NativeResourceOwner makeTrackedResource(CountingBox* box) {
    return NativeResourceOwner(0x1234, [box](NativeResourceOwner::Handle) {
        if (box) ++box->destroyed;
    });
}

// ---------------------------------------------------------------------------
// Argument transfer
// ---------------------------------------------------------------------------

void testArgumentInvariants() {
    static ZlNativeTypeTag i64[] = {ZL_NATIVE_I64};
    const auto exportInfo = makeExport("echo", ZL_NATIVE_I64, echoI64, i64, 1);

    bool threw = false;
    const auto noArgs = thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {})); }, threw);
    require(threw, "a zero-argument call to a one-argument export fails");
    require(noArgs.find("expected 1 arguments") != std::string::npos,
            "with an arity diagnostic (got: " + noArgs + ")");

    threw = false;
    const auto wrongType =
        thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {std::string("one")})); }, threw);
    require(threw, "a string where an i64 is declared fails");
    require(wrongType.find("wrong primitive type") != std::string::npos,
            "with a type diagnostic (got: " + wrongType + ")");

    threw = false;
    const auto nil = thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {Value{}})); }, threw);
    require(threw, "nil where an i64 is declared fails");
    require(nil.find("wrong primitive type") != std::string::npos,
            "(got: " + nil + ")");

    const auto ok = invokeNativeExport(exportInfo, {77});
    require(std::holds_alternative<std::int64_t>(ok) && std::get<std::int64_t>(ok) == 77,
            "a matching argument crosses the boundary untouched");

    ZlNativeExport emptyInvoke{};
    emptyInvoke.name = "ghost";
    emptyInvoke.arity = 0;
    emptyInvoke.invoke = nullptr;
    threw = false;
    const auto ghost = thrownMessage([&] { static_cast<void>(invokeNativeExport(emptyInvoke, {})); }, threw);
    require(threw, "an export with no invoke entry point fails");
    require(ghost.find("no invoke entry point") != std::string::npos,
            "with a diagnostic (got: " + ghost + ")");
}

// ---------------------------------------------------------------------------
// Ownership transfer: transactional
// ---------------------------------------------------------------------------

void testOwnedHandleTransfer() {
    static ZlNativeTypeTag handle[] = {ZL_NATIVE_HANDLE};
    static ZlNativeOwnershipTag consumed[] = {ZL_NATIVE_OWNERSHIP_CONSUMED};
    const auto exportInfo = makeExport("consume", ZL_NATIVE_I64, echoI64, handle, 1, consumed);

    // An OWNED/CONSUMED handle must already be in the resource registry.
    {
        const NativeHandleRef stray{0x777};
        bool threw = false;
        const auto message =
            thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {stray})); }, threw);
        require(threw, "an unregistered owned handle is rejected before the call");
        require(message.find("invalid owned native handle") != std::string::npos,
                "with an ownership diagnostic (got: " + message + ")");
    }
    {
        // Static storage: the global registry outlives the test, so anything
        // it still owns at exit must be addressable.
        static CountingBox box;
        box.destroyed = 0;
        g_box = &box;
        auto ref = nativeResourceRegistry().insert(makeTrackedResource(&box));
        require(nativeResourceRegistry().contains(ref), "the resource is registered");

        const auto result = invokeNativeExport(exportInfo, {ref});
        require(std::holds_alternative<std::int64_t>(result), "the call succeeds");
        require(!nativeResourceRegistry().contains(ref),
                "a CONSUMED handle is consumed from the registry exactly once");
        require(box.destroyed == 1, "consumption destroyed the resource exactly once");

        bool gone = false;
        try {
            nativeResourceRegistry().consume(ref);
        } catch (const std::runtime_error&) {
            gone = true;
        }
        require(gone, "the consumed handle cannot be consumed again");
    }
    {
        // The transactional part: when the native call fails, the caller
        // keeps its resource.
        const auto failing = makeExport("fail", ZL_NATIVE_I64, failWithMessage, handle, 1, consumed);
        static CountingBox box;
        box.destroyed = 0;
        g_box = &box;
        auto ref = nativeResourceRegistry().insert(makeTrackedResource(&box));
        bool threw = false;
        const auto message = thrownMessage([&] { static_cast<void>(invokeNativeExport(failing, {ref})); }, threw);
        require(threw, "the failing native call surfaces as a ZL exception");
        require(message.find("deliberate native failure") != std::string::npos,
                "carrying the native's error text (got: " + message + ")");
        require(nativeResourceRegistry().contains(ref),
                "a failed call retains the caller's owned handle");
        require(box.destroyed == 0, "and the resource is still alive");
    }
    g_box = nullptr;
}

// ---------------------------------------------------------------------------
// Return value invariants
// ---------------------------------------------------------------------------

void testReturnInvariants() {
    // An owned handle returned from native must be pre-registered by the
    // runtime - a native module cannot mint a resource the runtime does not
    // own.
    {
        const auto exportInfo = makeExport("mint", ZL_NATIVE_HANDLE, returnPreRegistered, nullptr, 0,
                                           nullptr, ZL_NATIVE_OWNERSHIP_OWNED);
        g_returnHandle = 0x999; // not registered
        bool threw = false;
        const auto message = thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {})); }, threw);
        require(threw, "an unregistered owned return handle is rejected");
        require(message.find("unregistered owned native handle") != std::string::npos,
                "with a diagnostic (got: " + message + ")");

        static CountingBox box;
        box.destroyed = 0;
        g_box = &box;
        auto ref = nativeResourceRegistry().insert(makeTrackedResource(&box));
        g_returnHandle = ref.id;
        const auto result = invokeNativeExport(exportInfo, {});
        require(std::holds_alternative<NativeHandleRef>(result) &&
                  std::get<NativeHandleRef>(result).id == ref.id,
                "a registered owned handle comes back as a stable token");
        require(box.destroyed == 0, "returning a handle borrows it from the registry, not from memory");
    }
    // The declared return type is part of the contract.
    {
        const auto exportInfo = makeExport("liar", ZL_NATIVE_I64, returnWrongTag);
        bool threw = false;
        const auto message = thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {})); }, threw);
        require(threw, "a return tag that does not match the declared type is rejected");
        require(message.find("unexpected primitive type") != std::string::npos,
                "with a diagnostic (got: " + message + ")");
    }
    // Views are call-duration: they may be borrowed in, never returned out.
    {
        const auto exportInfo = makeExport("viewOut", ZL_NATIVE_BUFFER_VIEW, returnBufferView);
        bool threw = false;
        const auto message = thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {})); }, threw);
        require(threw, "a buffer view return is rejected");
        require(message.find("cannot be returned") != std::string::npos,
                "with a diagnostic (got: " + message + ")");
    }
    g_box = nullptr;
}

// ---------------------------------------------------------------------------
// Callbacks: token only, lifetime-leased
// ---------------------------------------------------------------------------

void testCallbackBoundary() {
    const NativeCallbackRef missing{0x42};
    bool threw = false;
    const auto message =
        thrownMessage([&] {
            ZlNativeValue result{};
            char error[64] = {};
            static_cast<void>(invokeNativeCallback(missing, {1}, &result, error, sizeof(error)));
        }, threw);
    require(threw, "a callback token nobody registered is rejected");
    require(message.find("native callback is invalid") != std::string::npos,
            "with a diagnostic (got: " + message + ")");

    auto callback = nativeCallbackRegistry().insert(
        [](const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result, char*, std::uint32_t) {
            result->tag = ZL_NATIVE_I64;
            result->data.i64 = args[0].data.i64 * 2;
            return 0;
        });
    ZlNativeValue result{};
    char error[64] = {};
    const int32_t status = invokeNativeCallback(callback, {21}, &result, error, sizeof(error));
    require(status == 0, "a registered callback invokes through its token");
    require(result.tag == ZL_NATIVE_I64 && result.data.i64 == 42, "its result crosses back");

    // Unsupported ZL values cannot cross into a callback either.
    threw = false;
    const auto badValue =
        thrownMessage([&] { static_cast<void>(invokeNativeCallback(callback, {std::string("nope")}, &result, error, sizeof(error))); },
                      threw);
    require(threw, "a string argument to a callback is rejected");
    require(badValue.find("unsupported ZL value") != std::string::npos,
            "with a diagnostic (got: " + badValue + ")");

    // A closed callback rejects late calls instead of invoking stale state.
    nativeCallbackRegistry().close(callback);
    ZlNativeValue lateResult{};
    char lateError[64] = {};
    const int32_t late = nativeCallbackRegistry().invoke(callback, nullptr, 0, &lateResult, lateError,
                                                         sizeof(lateError));
    require(late == -1, "a closed callback refuses to run");
    require(std::string(lateError) == "invalid native callback",
            "with the registry's diagnostic (got: " + std::string(lateError) + ")");
}

} // namespace

int main() {
    testArgumentInvariants();
    testOwnedHandleTransfer();
    testReturnInvariants();
    testCallbackBoundary();

    if (failures != 0) {
        std::cerr << failures << " native boundary regression(s) failed\n";
        return 1;
    }
    std::cout << "all native boundary regressions passed\n";
    return 0;
}
