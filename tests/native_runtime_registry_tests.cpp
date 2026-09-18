// Runtime registry regressions: the process-local registries are the runtime
// side of the native boundary - the export registry routes name-based calls,
// the resource registry owns opaque handles, and the callback registry owns
// host-to-ZL callbacks. These tests pin their service contracts: resolution
// is exact, duplicates are rejected across registration batches, name-based
// failures surface as NativeError objects carrying a message and a stack
// trace from the execution state, and every registry drains deterministically.
#include "zl/vm/native_boundary.hpp"
#include "zl/vm/native_resource.hpp"

#include <atomic>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace zl;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "runtime registry regression: " << message << '\n';
    ++failures;
}

std::string thrownField(const std::function<void()>& call, bool& threw, const std::string& field) {
    threw = false;
    try {
        call();
        return {};
    } catch (const ZlThrownException& e) {
        threw = true;
        const Value* it = objectFieldLookup(*e.value(), field);
        if (it != nullptr && std::holds_alternative<std::string>(*it)) {
            return std::get<std::string>(*it);
        }
        return {};
    }
}

std::int32_t mulInvoke(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result,
                       char* error, std::uint32_t capacity) {
    if (argc != 2) {
        if (error && capacity) std::snprintf(error, capacity, "arity");
        return 1;
    }
    result->tag = ZL_NATIVE_I64;
    result->data.i64 = args[0].data.i64 * args[1].data.i64;
    return 0;
}

std::int32_t sumInvoke(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result,
                       char* error, std::uint32_t capacity) {
    if (argc != 2) {
        if (error && capacity) std::snprintf(error, capacity, "arity");
        return 1;
    }
    result->tag = ZL_NATIVE_I64;
    result->data.i64 = args[0].data.i64 + args[1].data.i64;
    return 0;
}

static const ZlNativeTypeTag twoI64[] = {ZL_NATIVE_I64, ZL_NATIVE_I64};

ZlNativeExport makeExport(const char* name, ZlNativeInvokeFn invoke) {
    ZlNativeExport exportInfo;
    exportInfo.name = name;
    exportInfo.arity = 2;
    exportInfo.parameterTypes = twoI64;
    exportInfo.returnType = ZL_NATIVE_I64;
    exportInfo.invoke = invoke;
    exportInfo.parameterOwnership = nullptr;
    exportInfo.returnOwnership = ZL_NATIVE_OWNERSHIP_NONE;
    return exportInfo;
}

// ---------------------------------------------------------------------------
// Export registry
// ---------------------------------------------------------------------------

void testExportRegistryResolution() {
    ZlNativeExport mul = makeExport("reg_multiply", mulInvoke);
    ZlNativeExport sum = makeExport("reg_add", sumInvoke);
    registerNativeModule(&mul, 1);
    registerNativeModule(&sum, 1);

    const auto product = invokeNativeExportByName("reg_multiply", {6, 7});
    require(std::holds_alternative<std::int64_t>(product) && std::get<std::int64_t>(product) == 42,
            "a registered name routes to the right export");
    const auto total = invokeNativeExportByName("reg_add", {40, 2});
    require(std::holds_alternative<std::int64_t>(total) && std::get<std::int64_t>(total) == 42,
            "and so does the other one");
}

void testExportRegistryDuplicatesAcrossBatches() {
    ZlNativeExport first = makeExport("reg_once", mulInvoke);
    registerNativeModule(&first, 1);
    ZlNativeExport second = makeExport("reg_once", sumInvoke); // same name, different table
    bool threw = false;
    try {
        registerNativeModule(&second, 1);
    } catch (const std::invalid_argument& e) {
        threw = std::string(e.what()).find("duplicate native export: reg_once") != std::string::npos;
    }
    require(threw, "a duplicate name is rejected even from a different batch");
    // The first registration still wins the name.
    const auto result = invokeNativeExportByName("reg_once", {3, 4});
    require(std::get<std::int64_t>(result) == 12, "the original export still resolves");
}

void testNameRoutingErrorsCarryContext() {
    bool threw = false;
    const auto message = thrownField([&] { static_cast<void>(invokeNativeExportByName("reg_missing", {1, 2})); }, threw,
                                     "message");
    require(threw, "an unregistered name fails");
    require(message.find("reg_missing") != std::string::npos &&
              message.find("is not registered") != std::string::npos,
            "the message names the missing export (got: " + message + ")");
    const auto trace = thrownField([&] { static_cast<void>(invokeNativeExportByName("reg_missing", {1, 2})); }, threw,
                                   "stackTrace");
    require(trace == "at <native>", "without an execution state the trace says so (got: " + trace + ")");

    // With an execution state, the trace carries the live call stack - the
    // whole point of routing through NativeError instead of a C error code.
    ExecutionState state;
    ExecutionState::CallFrame frame;
    frame.functionName = "step";
    frame.ownerClassName = "Driver";
    state.enterFrame(std::move(frame));
    const auto withState =
        thrownField([&] { static_cast<void>(invokeNativeExportByName("reg_missing", {1, 2}, nullptr, &state)); }, threw,
                    "stackTrace");
    require(threw, "still a failure with state");
    require(withState == "at step",
            "the trace names the calling function (got: " + withState + ")");
}

void testLocalRegistryIsIsolated() {
    ZlNativeExport exportInfo = makeExport("reg_local_only", mulInvoke);
    NativeExportRegistry local;
    local.registerModule(&exportInfo, 1);
    require(local.find("reg_local_only") != nullptr, "the local registry resolves its export");
    bool threw = false;
    thrownField([&] { static_cast<void>(invokeNativeExportByName("reg_local_only", {1, 2})); }, threw, "message");
    require(threw, "a local registration is invisible to global name routing");
}

// ---------------------------------------------------------------------------
// Resource registry as a service
// ---------------------------------------------------------------------------

void testResourceRegistryService() {
    std::atomic<int> destroyed{0};
    auto ref = nativeResourceRegistry().insert(
        NativeResourceOwner(0xE000, [&destroyed](NativeResourceOwner::Handle) { ++destroyed; }));
    require(ref.valid() && nativeResourceRegistry().contains(ref), "insert returns a live token");

    auto borrow = nativeResourceRegistry().borrow(ref);
    require(borrow.valid() && borrow.get() == 0xE000, "borrow resolves through the service");

    nativeResourceRegistry().close(ref);
    require(!nativeResourceRegistry().contains(ref), "close drains the token");
    require(destroyed.load() == 1, "and destroys the resource exactly once");

    bool doubleClose = false;
    try {
        nativeResourceRegistry().close(ref);
    } catch (const std::runtime_error&) {
        doubleClose = true;
    }
    require(doubleClose, "closing an already-drained token is an error");

    // Distinct resources get distinct tokens.
    auto a = nativeResourceRegistry().insert(
        NativeResourceOwner(0xE001, [](NativeResourceOwner::Handle) {}));
    auto b = nativeResourceRegistry().insert(
        NativeResourceOwner(0xE002, [](NativeResourceOwner::Handle) {}));
    require(a != b, "tokens are distinct");
    nativeResourceRegistry().close(a);
    nativeResourceRegistry().close(b);
}

// ---------------------------------------------------------------------------
// Callback registry as a service
// ---------------------------------------------------------------------------

void testCallbackRegistryService() {
    auto callback = nativeCallbackRegistry().insert(
        [](const ZlNativeValue*, std::uint32_t, ZlNativeValue* result, char*, std::uint32_t) {
            result->tag = ZL_NATIVE_I64;
            result->data.i64 = 5;
            return 0;
        });
    require(callback.valid() && nativeCallbackRegistry().contains(callback),
            "insert returns a live callback token");

    ZlNativeValue result{};
    char error[64] = {};
    const std::int32_t status =
        nativeCallbackRegistry().invoke(callback, nullptr, 0, &result, error, sizeof(error));
    require(status == 0 && result.data.i64 == 5, "invoke routes through the service");

    // A callback that throws still ends in a clean -1 plus a message.
    auto throwing = nativeCallbackRegistry().insert(
        [](const ZlNativeValue*, std::uint32_t, ZlNativeValue*, char*, std::uint32_t) -> std::int32_t {
            throw std::runtime_error("callback exploded");
        });
    const std::int32_t failed =
        nativeCallbackRegistry().invoke(throwing, nullptr, 0, &result, error, sizeof(error));
    require(failed == -1, "a throwing callback is contained");
    require(std::string(error) == "callback exploded", "with its exception text (got: " +
                                                           std::string(error) + ")");
    nativeCallbackRegistry().close(throwing);

    // close() drains: late invokes reject, and the token stops resolving.
    nativeCallbackRegistry().close(callback);
    require(!nativeCallbackRegistry().contains(callback), "close drains the token");
    const std::int32_t late =
        nativeCallbackRegistry().invoke(callback, nullptr, 0, &result, error, sizeof(error));
    require(late == -1, "a late invoke is refused");
}

void testEmptyCallbackRegistrationRejected() {
    bool threw = false;
    try {
        auto callback = nativeCallbackRegistry().insert(NativeCallbackRegistry::InvokeFn{});
        (void)callback;
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "cannot register an empty native callback";
    }
    require(threw, "an empty callback is not registrable");
}

} // namespace

int main() {
    testExportRegistryResolution();
    testExportRegistryDuplicatesAcrossBatches();
    testNameRoutingErrorsCarryContext();
    testLocalRegistryIsIsolated();
    testResourceRegistryService();
    testCallbackRegistryService();
    testEmptyCallbackRegistrationRejected();

    if (failures != 0) {
        std::cerr << failures << " runtime registry regression(s) failed\n";
        return 1;
    }
    std::cout << "all runtime registry regressions passed\n";
    return 0;
}
