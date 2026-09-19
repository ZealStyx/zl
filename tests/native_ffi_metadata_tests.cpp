// FFI metadata regressions: the export table is validated at registration
// time (malformed rows, duplicates, null tables) and the per-export type and
// ownership metadata is enforced at invocation time - ownership descriptors
// only make sense on resource tags, buffer/struct views are BORROWED only,
// and return ownership is only the handle-OWNED form the runtime can verify.
// A legacy export with no ownership metadata at all keeps working.
#include "zl/vm/native_boundary.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace zl;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "ffi metadata regression: " << message << '\n';
    ++failures;
}

std::string thrownMessage(const std::function<void()>& call, bool& threw) {
    threw = false;
    try {
        call();
        return {};
    } catch (const ZlThrownException& e) {
        threw = true;
        const Value* it = objectFieldLookup(*e.value(), "message");
        if (it != nullptr && std::holds_alternative<std::string>(*it)) {
            return std::get<std::string>(*it);
        }
        return {};
    }
}

// A native export that adds its two i64 arguments.
std::int32_t addInvoke(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result,
                       char* error, std::uint32_t capacity) {
    (void)args;
    (void)argc;
    (void)error;
    (void)capacity;
    result->tag = ZL_NATIVE_I64;
    result->data.i64 = 42;
    return 0;
}

ZlNativeExport makeAddExport() {
    static const ZlNativeTypeTag params[] = {ZL_NATIVE_I64, ZL_NATIVE_I64};
    ZlNativeExport exportInfo;
    exportInfo.name = "add";
    exportInfo.arity = 2;
    exportInfo.parameterTypes = params;
    exportInfo.returnType = ZL_NATIVE_I64;
    exportInfo.invoke = addInvoke;
    exportInfo.parameterOwnership = nullptr;
    exportInfo.returnOwnership = ZL_NATIVE_OWNERSHIP_NONE;
    return exportInfo;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void testRegistrationValidation() {
    {
        NativeExportRegistry registry;
        bool threw = false;
        try {
            registry.registerModule(nullptr, 1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "a null export table with a non-zero count is rejected");
    }
    {
        NativeExportRegistry registry;
        const ZlNativeExport bad = makeAddExport();
        ZlNativeExport unnamed{};
        unnamed.name = nullptr;
        unnamed.arity = 0;
        unnamed.invoke = addInvoke;
        bool threw = false;
        try {
            registry.registerModule(&unnamed, 1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "an export without a name is rejected");
        (void)bad;

        ZlNativeExport noInvoke = makeAddExport();
        noInvoke.invoke = nullptr;
        threw = false;
        try {
            registry.registerModule(&noInvoke, 1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "an export without an invoke entry point is rejected");
    }
    {
        NativeExportRegistry registry;
        ZlNativeExport first = makeAddExport();
        ZlNativeExport second = makeAddExport();
        registry.registerModule(&first, 1);
        bool threw = false;
        try {
            registry.registerModule(&second, 1);
        } catch (const std::invalid_argument& e) {
            threw = true;
            require(std::string(e.what()).find("duplicate native export: add") != std::string::npos,
                    "the duplicate is diagnosed by name (got: " + std::string(e.what()) + ")");
        }
        require(threw, "a duplicate export name is rejected");
    }
    {
        NativeExportRegistry registry;
        ZlNativeExport a = makeAddExport();
        a.name = "first";
        ZlNativeExport b = makeAddExport();
        b.name = "second";
        const ZlNativeExport pair[2] = {a, b};
        registry.registerModule(pair, 2);
        require(registry.find("first") != nullptr, "the first export resolves by name");
        require(registry.find("second") != nullptr, "the second export resolves by name");
        require(registry.find("third") == nullptr, "an unknown name resolves to nothing");
        registry.registerModule(nullptr, 0); // zero rows is a no-op, not an error
    }
}

// ---------------------------------------------------------------------------
// Type metadata at the boundary
// ---------------------------------------------------------------------------

void testParameterTypeMetadata() {
    {
        // A two-argument export that declares no parameter types at all.
        ZlNativeExport exportInfo = makeAddExport();
        exportInfo.parameterTypes = nullptr;
        bool threw = false;
        const auto message = thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {1, 2})); }, threw);
        require(threw, "missing parameter type metadata is rejected");
        require(message.find("no parameter type metadata") != std::string::npos,
                "the rejection says which metadata is missing (got: " + message + ")");
    }
    {
        const auto exportInfo = makeAddExport();
        bool threw = false;
        const auto message =
            thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {1.5, 2})); }, threw);
        require(threw, "a double where an i64 is declared is rejected");
        require(message.find("wrong primitive type") != std::string::npos,
                "the rejection names the type mismatch (got: " + message + ")");
    }
    {
        const auto exportInfo = makeAddExport();
        bool threw = false;
        const auto message =
            thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {1, 2, 3})); }, threw);
        require(threw, "too many arguments are rejected");
        require(message.find("expected 2 arguments") != std::string::npos,
                "the rejection states the arity (got: " + message + ")");
        threw = false;
        const auto shortMessage = thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {1})); }, threw);
        require(threw, "too few arguments are rejected");
        require(shortMessage.find("expected 2 arguments") != std::string::npos,
                "the same diagnostic (got: " + shortMessage + ")");
    }
    {
        const auto exportInfo = makeAddExport();
        const auto result = invokeNativeExport(exportInfo, {40, 2});
        require(std::holds_alternative<std::int64_t>(result) && std::get<std::int64_t>(result) == 42,
                "a well-typed call reaches the native function");
    }
}

// ---------------------------------------------------------------------------
// Ownership metadata
// ---------------------------------------------------------------------------

void testOwnershipMetadataValidation() {
    {
        // OWNED on a primitive parameter: ownership only exists for resources.
        static const ZlNativeTypeTag params[] = {ZL_NATIVE_I64};
        static const ZlNativeOwnershipTag ownership[] = {ZL_NATIVE_OWNERSHIP_OWNED};
        ZlNativeExport exportInfo;
        exportInfo.name = "ownInt";
        exportInfo.arity = 1;
        exportInfo.parameterTypes = params;
        exportInfo.returnType = ZL_NATIVE_I64;
        exportInfo.invoke = addInvoke;
        exportInfo.parameterOwnership = ownership;
        exportInfo.returnOwnership = ZL_NATIVE_OWNERSHIP_NONE;
        bool threw = false;
        const auto message = thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {1})); }, threw);
        require(threw, "ownership metadata on a primitive parameter is rejected");
        require(message.find("non-resource value") != std::string::npos,
                "the rejection says ownership needs a resource (got: " + message + ")");
    }
    {
        // A buffer view that is not BORROWED.
        static const ZlNativeTypeTag params[] = {ZL_NATIVE_BUFFER_VIEW};
        static const ZlNativeOwnershipTag ownership[] = {ZL_NATIVE_OWNERSHIP_OWNED};
        ZlNativeExport exportInfo;
        exportInfo.name = "ownBuffer";
        exportInfo.arity = 1;
        exportInfo.parameterTypes = params;
        exportInfo.returnType = ZL_NATIVE_I64;
        exportInfo.invoke = addInvoke;
        exportInfo.parameterOwnership = ownership;
        exportInfo.returnOwnership = ZL_NATIVE_OWNERSHIP_NONE;
        bool threw = false;
        const auto message =
            thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {NativeBufferView{}})); }, threw);
        require(threw, "an OWNED buffer view is rejected");
        require(message.find("may only use BORROWED ownership") != std::string::npos,
                "views are BORROWED-only (got: " + message + ")");
    }
    {
        // Return ownership that is not the verified handle-OWNED form.
        static const ZlNativeTypeTag params[] = {ZL_NATIVE_I64};
        ZlNativeExport exportInfo;
        exportInfo.name = "weirdReturn";
        exportInfo.arity = 1;
        exportInfo.parameterTypes = params;
        exportInfo.returnType = ZL_NATIVE_I64;
        exportInfo.invoke = addInvoke;
        exportInfo.parameterOwnership = nullptr;
        exportInfo.returnOwnership = ZL_NATIVE_OWNERSHIP_CONSUMED;
        bool threw = false;
        const auto message = thrownMessage([&] { static_cast<void>(invokeNativeExport(exportInfo, {1})); }, threw);
        require(threw, "CONSUMED return ownership is unsupported");
        require(message.find("unsupported return ownership") != std::string::npos,
                "the rejection names the return metadata (got: " + message + ")");
    }
    {
        // The legacy shape: no ownership metadata anywhere. Already exercised
        // by makeAddExport above, but pin it explicitly - a null ownership
        // array must mean "ownership-neutral", not "missing metadata".
        const auto exportInfo = makeAddExport();
        require(exportInfo.parameterOwnership == nullptr,
                "the legacy export carries no ownership array");
        const auto result = invokeNativeExport(exportInfo, {1, 2});
        require(std::get<std::int64_t>(result) == 42, "the legacy export still invokes");
    }
}

// ---------------------------------------------------------------------------
// Name-based routing
// ---------------------------------------------------------------------------

void testNameRouting() {
    ZlNativeExport exportInfo = makeAddExport();
    NativeExportRegistry registry;
    registry.registerModule(&exportInfo, 1);

    bool threw = false;
    const auto message =
        thrownMessage([&] { static_cast<void>(invokeNativeExportByName("add", {40, 2}, nullptr, nullptr)); }, threw);
    // invokeNativeExportByName routes through the GLOBAL registry, so a local
    // registration is invisible to it: the call must fail as unregistered.
    require(threw, "a name not in the global registry is refused");
    require(message.find("is not registered") != std::string::npos,
            "the refusal names the export (got: " + message + ")");

    registerNativeModule(&exportInfo, 1);
    const auto result = invokeNativeExportByName("add", {40, 2});
    require(std::holds_alternative<std::int64_t>(result) && std::get<std::int64_t>(result) == 42,
            "a registered name routes to its export");
}

} // namespace

int main() {
    testRegistrationValidation();
    testParameterTypeMetadata();
    testOwnershipMetadataValidation();
    testNameRouting();

    if (failures != 0) {
        std::cerr << failures << " ffi metadata regression(s) failed\n";
        return 1;
    }
    std::cout << "all ffi metadata regressions passed\n";
    return 0;
}
