#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ZlNativeTypeTag : std::uint32_t {
    ZL_NATIVE_I64 = 1,
    ZL_NATIVE_F64 = 2,
    ZL_NATIVE_BOOL = 3,
    // Opaque native handle transported as an integer token. The runtime owns
    // the underlying resource; ZL never receives a raw pointer.
    ZL_NATIVE_HANDLE = 4,
    // Borrowed native memory view. The pointer is valid only for the duration
    // of the synchronous native call; length is expressed in bytes.
    ZL_NATIVE_BUFFER_VIEW = 5,
    // Borrowed ABI struct bytes. The pointer is valid only for the duration
    // of the synchronous native call.
    ZL_NATIVE_STRUCT_VIEW = 6,
    // Stable callback token. The callback target is process-local and never
    // crosses the ABI as a raw function pointer.
    ZL_NATIVE_CALLBACK = 7
} ZlNativeTypeTag;

// Ownership semantics for an FFI boundary. NONE is used for legacy primitive
// exports; BORROWED never transfers ownership; OWNED transfers ownership to
// the callee/consumer; CONSUMED transfers ownership into the callee exactly
// once. These descriptors are enforced by the runtime transport; ownership
// transfer of a resource is still explicit through NativeResourceOwner.
typedef enum ZlNativeOwnershipTag : std::uint32_t {
    ZL_NATIVE_OWNERSHIP_NONE = 0,
    ZL_NATIVE_OWNERSHIP_BORROWED = 1,
    ZL_NATIVE_OWNERSHIP_OWNED = 2,
    ZL_NATIVE_OWNERSHIP_CONSUMED = 3
} ZlNativeOwnershipTag;

typedef struct ZlNativeValue {
    ZlNativeTypeTag tag;
    union {
        std::int64_t i64;
        double f64;
        bool boolean;
        std::uint64_t handle;
        std::uint64_t callback;
        struct {
            const void* data;
            std::uint64_t size;
        } buffer;
        struct {
            const void* data;
            std::uint64_t size;
            std::uint64_t alignment;
        } structView;
    } data;
} ZlNativeValue;

typedef std::int32_t (*ZlNativeInvokeFn)(
    const ZlNativeValue* args,
    std::uint32_t argc,
    ZlNativeValue* result,
    char* errorMessage,
    std::uint32_t errorMessageCapacity);

typedef struct ZlNativeExport {
    const char* name;
    std::uint32_t arity;
    const ZlNativeTypeTag* parameterTypes;
    ZlNativeTypeTag returnType;
    ZlNativeInvokeFn invoke;
    // Optional ownership metadata. nullptr preserves legacy ownership-neutral exports.
    const ZlNativeOwnershipTag* parameterOwnership;
    ZlNativeOwnershipTag returnOwnership;
} ZlNativeExport;

#ifdef __cplusplus
}
#endif
