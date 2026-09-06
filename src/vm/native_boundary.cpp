#include "zl/vm/native_boundary.hpp"
#include "zl/compiler/native_ffi_transport.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace zl {
namespace {

[[noreturn]] void throwBoundaryError(const std::string& message,
                                     const Chunk* chunk,
                                     const ExecutionState* state) {
    Value objectValue = makeEmptyObject("NativeError");
    auto object = std::get<ObjectRef>(objectValue);
    object->fields["message"] = message;
    std::string trace;
    if (state) {
        for (const auto& name : state->callStackNames()) {
            if (!trace.empty()) trace += "\n";
            trace += "at " + name;
        }
    }
    object->fields["stackTrace"] = trace.empty() ? std::string("at <native>") : trace;
    if (chunk) {
        auto it = chunk->classReflection.find("NativeError");
        if (it != chunk->classReflection.end()) object->runtimeType = it->second.runtimeType;
    }
    throw ZlThrownException(std::move(object));
}

bool matchesTag(const Value& value, ZlNativeTypeTag tag) {
    switch (tag) {
        case ZL_NATIVE_I64: return std::holds_alternative<std::int64_t>(value);
        case ZL_NATIVE_F64: return std::holds_alternative<double>(value);
        case ZL_NATIVE_BOOL: return std::holds_alternative<bool>(value);
        case ZL_NATIVE_HANDLE: return std::holds_alternative<NativeHandleRef>(value) && std::get<NativeHandleRef>(value).valid();
        case ZL_NATIVE_BUFFER_VIEW: return std::holds_alternative<NativeBufferView>(value) && std::get<NativeBufferView>(value).valid();
        case ZL_NATIVE_STRUCT_VIEW: return std::holds_alternative<NativeStructView>(value) && std::get<NativeStructView>(value).valid();
        case ZL_NATIVE_CALLBACK: return std::holds_alternative<NativeCallbackRef>(value) && std::get<NativeCallbackRef>(value).valid() && nativeCallbackRegistry().contains(std::get<NativeCallbackRef>(value));
        default: return false;
    }
}

ZlNativeValue pack(const Value& value, ZlNativeTypeTag tag) {
    ZlNativeValue packed{};
    packed.tag = tag;
    switch (tag) {
        case ZL_NATIVE_I64: packed.data.i64 = std::get<std::int64_t>(value); break;
        case ZL_NATIVE_F64: packed.data.f64 = std::get<double>(value); break;
        case ZL_NATIVE_BOOL: packed.data.boolean = std::get<bool>(value); break;
        case ZL_NATIVE_HANDLE: packed.data.handle = std::get<NativeHandleRef>(value).id; break;
        case ZL_NATIVE_BUFFER_VIEW: {
            const auto& view = std::get<NativeBufferView>(value);
            packed.data.buffer.data = view.data();
            packed.data.buffer.size = static_cast<std::uint64_t>(view.size());
            break;
        }
        case ZL_NATIVE_STRUCT_VIEW: {
            const auto& view = std::get<NativeStructView>(value);
            packed.data.structView.data = view.data();
            packed.data.structView.size = static_cast<std::uint64_t>(view.size());
            packed.data.structView.alignment = static_cast<std::uint64_t>(view.alignment());
            break;
        }
        case ZL_NATIVE_CALLBACK:
            packed.data.callback = std::get<NativeCallbackRef>(value).id;
            break;
        default: break;
    }
    return packed;
}

Value unpack(const ZlNativeValue& value, ZlNativeTypeTag expected) {
    if (value.tag != expected) throw std::runtime_error("native export returned an unexpected primitive type");
    switch (expected) {
        case ZL_NATIVE_I64: return value.data.i64;
        case ZL_NATIVE_F64: return value.data.f64;
        case ZL_NATIVE_BOOL: return value.data.boolean;
        case ZL_NATIVE_HANDLE: return unpackNativeHandle(value);
        case ZL_NATIVE_BUFFER_VIEW:
            throw std::runtime_error("native buffer views may only be borrowed into a call; they cannot be returned yet");
        case ZL_NATIVE_STRUCT_VIEW:
            throw std::runtime_error("native struct views may only be borrowed into a call; they cannot be returned yet");
        case ZL_NATIVE_CALLBACK:
            return unpackNativeCallback(value);
        default: throw std::runtime_error("native export returned an unsupported primitive type");
    }
}

} // namespace


NativeExportRegistry& nativeExportRegistry() {
    static NativeExportRegistry registry;
    return registry;
}

void NativeExportRegistry::registerModule(const ZlNativeExport* exports, std::uint32_t count) {
    if (count == 0) return;
    if (!exports) throw std::invalid_argument("native module has a null export table");
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!exports[i].name || !*exports[i].name || !exports[i].invoke)
            throw std::invalid_argument("native module contains an invalid export");
        const std::string name(exports[i].name);
        if (exports_.count(name)) throw std::invalid_argument("duplicate native export: " + name);
        exports_.emplace(name, &exports[i]);
    }
}

const ZlNativeExport* NativeExportRegistry::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = exports_.find(name);
    return it == exports_.end() ? nullptr : it->second;
}

void registerNativeModule(const ZlNativeExport* exports, std::uint32_t count) {
    nativeExportRegistry().registerModule(exports, count);
}

Value invokeNativeExportByName(const std::string& name, const std::vector<Value>& args, const Chunk* chunk, const ExecutionState* state) {
    const auto* exportInfo = nativeExportRegistry().find(name);
    if (!exportInfo) throwBoundaryError("native export '" + name + "' is not registered", chunk, state);
    return invokeNativeExport(*exportInfo, args, chunk, state);
}

Value invokeNativeExport(const ZlNativeExport& exportInfo,
                         const std::vector<Value>& args,
                         const Chunk* chunk,
                         const ExecutionState* state) {
    if (!exportInfo.invoke) throwBoundaryError("native export '" + std::string(exportInfo.name ? exportInfo.name : "<unnamed>") + "' has no invoke entry point", chunk, state);
    if (args.size() != exportInfo.arity) {
        throwBoundaryError("native export '" + std::string(exportInfo.name ? exportInfo.name : "<unnamed>") + "' expected " + std::to_string(exportInfo.arity) + " arguments", chunk, state);
    }
    if (exportInfo.arity > 0 && !exportInfo.parameterTypes) {
        throwBoundaryError("native export '" + std::string(exportInfo.name ? exportInfo.name : "<unnamed>") + "' has no parameter type metadata", chunk, state);
    }
    if (exportInfo.parameterOwnership && exportInfo.arity > 0) {
        for (std::size_t i = 0; i < exportInfo.arity; ++i) {
            const auto ownership = exportInfo.parameterOwnership[i];
            const auto tag = exportInfo.parameterTypes[i];
            if (ownership == ZL_NATIVE_OWNERSHIP_NONE) continue;
            if (tag != ZL_NATIVE_HANDLE && tag != ZL_NATIVE_BUFFER_VIEW && tag != ZL_NATIVE_STRUCT_VIEW) {
                throwBoundaryError("native export '" + std::string(exportInfo.name ? exportInfo.name : "<unnamed>") + " declares ownership metadata for a non-resource value", chunk, state);
            }
            if ((tag == ZL_NATIVE_BUFFER_VIEW || tag == ZL_NATIVE_STRUCT_VIEW) && ownership != ZL_NATIVE_OWNERSHIP_BORROWED) {
                throwBoundaryError("native buffer views may only use BORROWED ownership", chunk, state);
            }
        }
    }
    std::vector<ZlNativeValue> packedArgs;
    packedArgs.reserve(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!exportInfo.parameterTypes || !matchesTag(args[i], exportInfo.parameterTypes[i])) {
            throwBoundaryError("native export '" + std::string(exportInfo.name ? exportInfo.name : "<unnamed>") + " received an argument with the wrong primitive type", chunk, state);
        }
        if (exportInfo.parameterOwnership) {
            const auto ownership = exportInfo.parameterOwnership[i];
            if (exportInfo.parameterTypes[i] == ZL_NATIVE_HANDLE && ownership != ZL_NATIVE_OWNERSHIP_BORROWED) {
                // Transfer is transactional: if the native call fails, the caller retains the owner.
                if (ownership == ZL_NATIVE_OWNERSHIP_OWNED || ownership == ZL_NATIVE_OWNERSHIP_CONSUMED) {
                    if (!std::holds_alternative<NativeHandleRef>(args[i]) || !nativeResourceRegistry().contains(std::get<NativeHandleRef>(args[i]))) {
                        throwBoundaryError("native export '" + std::string(exportInfo.name ? exportInfo.name : "<unnamed>") + "' received an invalid owned native handle", chunk, state);
                    }
                }
            }
        }
        packedArgs.push_back(pack(args[i], exportInfo.parameterTypes[i]));
    }
    if (exportInfo.returnOwnership != ZL_NATIVE_OWNERSHIP_NONE) {
        if (exportInfo.returnType != ZL_NATIVE_HANDLE || exportInfo.returnOwnership != ZL_NATIVE_OWNERSHIP_OWNED) {
            throwBoundaryError("native export '" + std::string(exportInfo.name ? exportInfo.name : "<unnamed>") + " has unsupported return ownership metadata", chunk, state);
        }
    }

    ZlNativeValue result{};
    char error[512] = {};
    const auto status = exportInfo.invoke(packedArgs.data(), exportInfo.arity, &result, error, sizeof(error));
    if (status != 0) {
        throwBoundaryError(error[0] ? std::string(error) : "native export failed", chunk, state);
    }
    if (exportInfo.parameterOwnership) {
        for (std::size_t i = 0; i < exportInfo.arity; ++i) {
            const auto ownership = exportInfo.parameterOwnership[i];
            if ((ownership == ZL_NATIVE_OWNERSHIP_OWNED || ownership == ZL_NATIVE_OWNERSHIP_CONSUMED) &&
                exportInfo.parameterTypes[i] == ZL_NATIVE_HANDLE) {
                (void)nativeResourceRegistry().consume(std::get<NativeHandleRef>(args[i]));
            }
        }
    }
    try {
        if (exportInfo.returnType == ZL_NATIVE_HANDLE && exportInfo.returnOwnership == ZL_NATIVE_OWNERSHIP_OWNED) {
            if (result.tag != ZL_NATIVE_HANDLE || result.data.handle == 0 || !nativeResourceRegistry().contains(NativeHandleRef{result.data.handle})) {
                throwBoundaryError("native export returned an unregistered owned native handle", chunk, state);
            }
        }
        return unpack(result, exportInfo.returnType);
    } catch (const std::exception& e) {
        throwBoundaryError(e.what(), chunk, state);
    }
}


std::int32_t invokeNativeCallback(NativeCallbackRef callback,
                                  const std::vector<Value>& args,
                                  ZlNativeValue* result,
                                  char* errorMessage,
                                  std::uint32_t errorMessageCapacity,
                                  const Chunk* chunk,
                                  const ExecutionState* state) {
    if (!callback.valid() || !nativeCallbackRegistry().contains(callback)) {
        throwBoundaryError("native callback is invalid", chunk, state);
    }
    std::vector<ZlNativeValue> packed;
    packed.reserve(args.size());
    for (const auto& value : args) {
        switch (value.index()) {
            case 1: packed.push_back(pack(value, ZL_NATIVE_I64)); break;
            case 2: packed.push_back(pack(value, ZL_NATIVE_F64)); break;
            case 4: packed.push_back(pack(value, ZL_NATIVE_BOOL)); break;
            case 11: packed.push_back(pack(value, ZL_NATIVE_HANDLE)); break;
            case 12: packed.push_back(pack(value, ZL_NATIVE_BUFFER_VIEW)); break;
            case 13: packed.push_back(pack(value, ZL_NATIVE_STRUCT_VIEW)); break;
            case 14: packed.push_back(pack(value, ZL_NATIVE_CALLBACK)); break;
            default: throwBoundaryError("native callback received an unsupported ZL value", chunk, state);
        }
    }
    return nativeCallbackRegistry().invoke(callback, packed.data(), static_cast<std::uint32_t>(packed.size()), result, errorMessage, errorMessageCapacity);
}

} // namespace zl
