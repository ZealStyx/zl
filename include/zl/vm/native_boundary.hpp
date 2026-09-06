#pragma once

#include <vector>
#include <mutex>
#include <unordered_map>
#include <string>
#include <cstdint>

#include "zl/compiler/native_abi.hpp"
#include "zl/vm/value.hpp"
#include "zl/vm/native_resource.hpp"
#include "zl/compiler/bytecode.hpp"
#include "zl/vm/execution_state.hpp"

namespace zl {

class NativeExportRegistry {
public:
    NativeExportRegistry() = default;
    NativeExportRegistry(const NativeExportRegistry&) = delete;
    NativeExportRegistry& operator=(const NativeExportRegistry&) = delete;

    void registerModule(const ZlNativeExport* exports, std::uint32_t count);
    [[nodiscard]] const ZlNativeExport* find(const std::string& name) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, const ZlNativeExport*> exports_;
};

NativeExportRegistry& nativeExportRegistry();
void registerNativeModule(const ZlNativeExport* exports, std::uint32_t count);
[[nodiscard]] Value invokeNativeExportByName(const std::string& name, const std::vector<Value>& args, const Chunk* chunk = nullptr, const ExecutionState* state = nullptr);

// Invokes a restricted Phase 10 native export using the stable primitive ABI.
// Unsupported ZL values/types are rejected before the native call. Native
// failures become the normal typed NativeError ZL exception when runtime
// metadata is available in the supplied chunk.
[[nodiscard]] Value invokeNativeExport(const ZlNativeExport& exportInfo,
                                       const std::vector<Value>& args,
                                       const Chunk* chunk = nullptr,
                                       const ExecutionState* state = nullptr);

// Invokes a registered native callback through its lifetime lease. The callback
// target is never exposed as a raw function pointer to ZL code.
[[nodiscard]] std::int32_t invokeNativeCallback(NativeCallbackRef callback,
                                                 const std::vector<Value>& args,
                                                 ZlNativeValue* result,
                                                 char* errorMessage,
                                                 std::uint32_t errorMessageCapacity,
                                                 const Chunk* chunk = nullptr,
                                                 const ExecutionState* state = nullptr);

} // namespace zl
