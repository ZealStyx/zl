#pragma once

#include <cstdint>
#include <utility>
#include <stdexcept>

#include "native_abi.hpp"
#include "../vm/native_resource.hpp"

namespace zl {

inline ZlNativeValue packNativeHandle(const NativeHandleRef& handle) {
    if (!handle.valid()) throw std::runtime_error("cannot pass an invalid native handle");
    ZlNativeValue out{};
    out.tag = ZL_NATIVE_HANDLE;
    out.data.handle = handle.id;
    return out;
}

inline NativeHandleRef unpackNativeHandle(const ZlNativeValue& value) {
    if (value.tag != ZL_NATIVE_HANDLE || value.data.handle == 0)
        throw std::runtime_error("invalid native handle transport value");
    return NativeHandleRef{value.data.handle};
}

inline ZlNativeValue packNativeBufferView(const NativeBufferView& view) {
    if (!view.valid()) throw std::runtime_error("cannot pass an invalid native buffer view");
    ZlNativeValue out{};
    out.tag = ZL_NATIVE_BUFFER_VIEW;
    out.data.buffer.data = view.data();
    out.data.buffer.size = static_cast<std::uint64_t>(view.size());
    return out;
}

// Explicit ownership adapters. These are the only recommended conversion
// points between deterministic native ownership and the tokenized ZL value
// representation.
inline NativeHandleRef adoptNativeResource(NativeResourceOwner owner) {
    return nativeResourceRegistry().insert(std::move(owner));
}

inline NativeResourceOwner takeOwnedNativeResource(NativeHandleRef ref) {
    return nativeResourceRegistry().consume(ref);
}

inline NativeBufferView borrowNativeResourceBuffer(NativeHandleRef ref, const void* data, std::size_t size) {
    auto ownerBorrow = nativeResourceRegistry().borrow(ref);
    return NativeBufferView(ownerBorrow, data, size);
}

inline ZlNativeValue packOwnedNativeResource(NativeResourceOwner owner) {
    return packNativeHandle(adoptNativeResource(std::move(owner)));
}


inline ZlNativeValue packNativeStructView(const NativeStructView& view) {
    if (!view.valid()) throw std::runtime_error("cannot pass an invalid native struct view");
    ZlNativeValue out{};
    out.tag = ZL_NATIVE_STRUCT_VIEW;
    out.data.structView.data = view.data();
    out.data.structView.size = static_cast<std::uint64_t>(view.size());
    out.data.structView.alignment = static_cast<std::uint64_t>(view.alignment());
    return out;
}

inline ZlNativeValue packNativeCallback(const NativeCallbackRef& callback) {
    if (!callback.valid()) throw std::runtime_error("cannot pass an invalid native callback");
    ZlNativeValue out{};
    out.tag = ZL_NATIVE_CALLBACK;
    out.data.callback = callback.id;
    return out;
}

inline NativeCallbackRef unpackNativeCallback(const ZlNativeValue& value) {
    if (value.tag != ZL_NATIVE_CALLBACK || value.data.callback == 0)
        throw std::runtime_error("invalid native callback transport value");
    return NativeCallbackRef{value.data.callback};
}

} // namespace zl
