#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include "semantic_types.hpp"

namespace zl {

// Compile-time description of a native func. This module owns the
// language-facing metadata; the VM remains responsible for C++ callback
// bindings. Keeping the catalog separate gives the type checker a focused
// test surface instead of embedding 100+ native definitions in semantic
// analysis.
enum class NativeId : std::uint16_t {
    TYPE_NAME,
    TYPE_FIELDS,
    TYPE_METHODS,
    TYPE_BASE,
    TYPE_CALLABLE,
    TYPE_KIND,
    TYPE_IS_DATA,
    TYPE_IS_ENUM,
    TYPE_ENUM_MEMBERS,
    MATH_SQRT,
    MATH_ABS,
    MATH_POW,
    MATH_FLOOR,
    MATH_CEIL,
    MATH_ROUND,
    MATH_MIN,
    MATH_MAX,
    MATH_CBRT,
    MATH_TRUNC,
    MATH_SIN,
    MATH_COS,
    MATH_TAN,
    MATH_ASIN,
    MATH_ACOS,
    MATH_ATAN,
    MATH_ATAN2,
    MATH_LOG,
    MATH_LOG10,
    MATH_EXP,
    MATH_RANDOM,
    MATH_RANDOMINT,
    MATH_RANDOMFLOAT,
    IO_PRINT,
    IO_PRINTLN,
    IO_READLINE,
    IO_CLEAR,
    COLLECTION_NEWLIST,
    COLLECTION_PUSH,
    COLLECTION_POP,
    COLLECTION_GET,
    COLLECTION_SET,
    COLLECTION_LENGTH,
    COLLECTION_NEWMAP,
    COLLECTION_MAPSET,
    COLLECTION_MAPGET,
    COLLECTION_MAPHAS,
    COLLECTION_MAPREMOVE,
    COLLECTION_MAPKEYS,
    COLLECTION_MAPVALUES,
    COLLECTION_NEWSET,
    COLLECTION_SETADD,
    COLLECTION_SETHAS,
    COLLECTION_SETREMOVE,
    COLLECTION_SETITEMS,
    QUEUE_NEWQUEUE,
    QUEUE_ENQUEUE,
    QUEUE_DEQUEUE,
    QUEUE_PEEK,
    QUEUE_ISEMPTY,
    QUEUE_SIZE,
    STACK_NEWSTACK,
    STACK_PUSH,
    STACK_POP,
    STACK_PEEK,
    STACK_ISEMPTY,
    STACK_SIZE,
    STRING_LENGTH,
    STRING_UPPER,
    STRING_LOWER,
    STRING_TRIM,
    STRING_CONTAINS,
    STRING_INDEXOF,
    STRING_CHARAT,
    STRING_SUBSTRING,
    STRING_REPLACE,
    STRING_SPLIT,
    STRING_TOINT,
    STRING_TOFLOAT,
    STRING_REGEXMATCHES,
    STRING_REGEXFULLMATCHES,
    STRING_REGEXFINDALL,
    STRING_REGEXREPLACE,
    STRING_REGEXFIND,
    STRING_REGEXFINDMATCHES,
    FILESYSTEM_READFILE,
    FILESYSTEM_WRITEFILE,
    FILESYSTEM_APPENDFILE,
    FILESYSTEM_EXISTS,
    FILESYSTEM_DELETEFILE,
    FILESYSTEM_LISTDIR,
    NETWORK_RESOLVE,
    TIME_NOW,
    TIME_NOWMILLIS,
    TIME_SLEEP,
    TIME_SLEEP_ASYNC,
    TIME_YEAR,
    TIME_MONTH,
    TIME_DAY,
    TIME_HOUR,
    TIME_MINUTE,
    TIME_SECOND,
    TIME_FORMAT,
    THREAD_START,
    THREAD_JOIN,
    THREAD_ISALIVE,
    TASK_SPAWN,
    MUTEX_WITHLOCK,
    RWLOCK_WITHREAD,
    RWLOCK_WITHWRITE,
    ATOMIC_LOAD,
    ATOMIC_STORE,
    ATOMIC_ADD,
    ATOMIC_LOAD_BOOL,
    ATOMIC_STORE_BOOL,
    ATOMIC_LOAD_DOUBLE,
    ATOMIC_STORE_DOUBLE,
    ATOMIC_LOAD_REF,
    ATOMIC_STORE_REF,
    SEMAPHORE_ACQUIRE,
    SEMAPHORE_RELEASE,
    SEMAPHORE_AVAILABLE,
    CONDITION_WAIT,
    CONDITION_NOTIFYONE,
    CONDITION_NOTIFYALL,
    CHANNEL_CREATE,
    CHANNEL_SEND,
    CHANNEL_RECEIVE,
    CHANNEL_SIZE,
    CHANNEL_SEND_ASYNC,
    CHANNEL_RECEIVE_ASYNC,
    TEST_FAIL,
    TEST_ASSERTTRUE,
    TEST_ASSERTFALSE,
    TEST_ASSERTEQUAL,
    TEST_ASSERTNOTEQUAL,
    TEST_ASSERTNEAR,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    TEXT_FORMAT,
    TEXT_REGEXMATCHES,
    TEXT_REGEXFULLMATCHES,
    TEXT_REGEXFINDALL,
    TEXT_REGEXREPLACE,
    SERIALIZE_ENCODE,
    SERIALIZE_DECODE,
    SERIALIZE_ASSTRING,
    SERIALIZE_ASINT,
    SERIALIZE_ASDOUBLE,
    SERIALIZE_ASBOOL,
    CRYPTO_CRC32,
    CRYPTO_SHA256,
    HASH_CODE,
    SYSTEM_EXIT,
    SYSTEM_GETENV,
    SYSTEM_EXEC,
    INT_PARSE,
    DOUBLE_PARSE,
    BOOL_PARSE,
    TYPE_OF,
    SHARED_SHARE,
    SHARED_GET,
    SHARED_SET,
    SHARED_WITHLOCK,
    REFLECTION_NAME,
    REFLECTION_KIND,
    REFLECTION_IS_DATA,
    REFLECTION_IS_ENUM,
    REFLECTION_ENUM_MEMBERS,
    REFLECTION_FIELDS,
    REFLECTION_METHODS,
    REFLECTION_BASE,
    REFLECTION_INTERFACES,
    REFLECTION_TYPE_PARAMETERS,
    REFLECTION_FIELD_NAME,
    REFLECTION_FIELD_TYPE,
    REFLECTION_FIELD_ACCESS,
    REFLECTION_METHOD_NAME,
    REFLECTION_METHOD_RETURN_TYPE,
    REFLECTION_METHOD_ACCESS,
    REFLECTION_METHOD_STATIC,
    REFLECTION_METHOD_ASYNC,
    REFLECTION_METHOD_PARAMETERS,
    REFLECTION_FUNCTION,
    REFLECTION_FUNCTION_NAME,
    REFLECTION_FUNCTION_PARAMETERS,
    REFLECTION_FUNCTION_RETURN_TYPE,
    REFLECTION_FUNCTION_ASYNC,
    REFLECTION_FUNCTION_IS_NATIVE,
    REFLECTION_FUNCTION_INVOKE,
    REFLECTION_TYPE_CONSTRUCTORS,
    REFLECTION_CONSTRUCTOR_PARAMETERS,
    REFLECTION_FIELD,
    REFLECTION_METHOD,
    REFLECTION_CONSTRUCTOR,
    REFLECTION_METHOD_INVOKE,
    REFLECTION_CONSTRUCTOR_INVOKE,
};

enum class NativeReturnTypeRule {
    NONE,
    SAME_NUMERIC_KIND,
    FIRST_ARGUMENT_TYPES,
};

enum class NativeOwnershipKind : std::uint8_t { NONE, BORROWED, OWNED, CONSUMED };

struct NativeSignature {
    NativeId id;
    std::string qualifiedName;
    std::vector<ZlType> paramTypes;
    ZlType returnType;
    std::string homePackage{"zl.lang"};
    // Optional richer parameter constraints. An empty entry falls back to
    // the corresponding paramTypes entry, preserving the simple catalog form.
    std::vector<std::vector<ZlType>> acceptedParamTypes;
    NativeReturnTypeRule returnTypeRule{NativeReturnTypeRule::NONE};
    // When set, the native call returns Task<taskValueType> rather than the plain returnType.
    ZlType taskValueType{ZlType::UNKNOWN};
    // Optional object class name for object-valued natives.
    std::string returnClassName;
    std::vector<NativeOwnershipKind> paramOwnership;
    NativeOwnershipKind returnOwnership{NativeOwnershipKind::NONE};
    // Generic native signatures bind the names in the first parameter's
    // type arguments, e.g. map<K,V>. Later parameters/results reuse K and V.
    std::vector<std::string> parameterTypeNames;
};

[[nodiscard]] std::unordered_map<std::string, std::string> nativeTypeBindings(
    const NativeSignature& signature, const std::string& firstArgumentType);
[[nodiscard]] const std::vector<NativeSignature>& nativeSignatureTable();
[[nodiscard]] std::optional<const NativeSignature*> findNativeSignature(const std::string& qualifiedName);

} // namespace zl
