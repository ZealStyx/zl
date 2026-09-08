#pragma once

#include <cstdint>
#include <deque>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <memory>
#include <type_traits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "runtime_type.hpp"
#include "native_resource.hpp"

namespace zl {

// Forward declarations to break a circular dependency: Value needs to hold a
// reference to a list of Values, but a list of Values needs Value to already
// be a complete type. The fix is one more level of indirection: ListRef/MapRef/
// GcRef carries a small non-owning pointer to a collector-owned allocation.
// The concrete boxes can still be defined after Value because the handle only
// needs an incomplete pointee type.
struct ListBox;
struct MapBox;
struct ObjectBox;
struct ClosureBox;
struct Chunk;
struct AtomicRefState;
class RuntimeTaskState;
using TaskRef = std::shared_ptr<RuntimeTaskState>;
class RuntimeThreadState;
using ThreadRef = std::shared_ptr<RuntimeThreadState>;

template <typename T>
class GcRef {
public:
    constexpr GcRef() noexcept = default;
    constexpr GcRef(std::nullptr_t) noexcept : ptr_(nullptr) {}
    [[nodiscard]] constexpr T* get() const noexcept { return ptr_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] constexpr T& operator*() const noexcept { return *ptr_; }
    [[nodiscard]] constexpr T* operator->() const noexcept { return ptr_; }
    friend constexpr bool operator==(GcRef a, GcRef b) noexcept { return a.ptr_ == b.ptr_; }
    friend constexpr bool operator!=(GcRef a, GcRef b) noexcept { return a.ptr_ != b.ptr_; }

public:
    // Constructed by the GC allocation gateway; Values copy only this non-owning handle.
    explicit constexpr GcRef(T* ptr) noexcept : ptr_(ptr) {}

private:
    T* ptr_{nullptr};
    template <typename U> friend class GcRef;
    friend class TracingGC;
};

using ListRef = GcRef<ListBox>;
using MapRef = GcRef<MapBox>;
using ObjectRef = GcRef<ObjectBox>;
using ClosureRef = GcRef<ClosureBox>;

class ZlThrownException : public std::exception {
public:
    explicit ZlThrownException(ObjectRef value) : value_(std::move(value)) {}
    const char* what() const noexcept override { return "ZL exception"; }
    [[nodiscard]] const ObjectRef& value() const noexcept { return value_; }
private:
    ObjectRef value_;
};

// std::variant<A, B, C, ...> is a type-safe union: a Value IS EXACTLY ONE of
// these types at any moment, and it knows which one.
//
// Lists/arrays/sets share ListRef; maps use MapRef; objects use ObjectRef.
// These are non-owning GC handles with REFERENCE semantics: two Values holding
// the same handle point at the same underlying data while the collector keeps
// that allocation alive. This is different from int/double/string/bool, which
// are plain values and get copied on assignment.
using Value = std::variant<std::monostate, std::int64_t, double, std::string,
                           bool, ListRef, MapRef, ObjectRef, ClosureRef, TaskRef, ThreadRef,
                           NativeHandleRef, NativeBufferView, NativeStructView, NativeCallbackRef>;

// Value is complete now, so these can finally hold real containers of it.
struct ListBox {
    NativeContainerTypeRef storageType;
    std::vector<Value> items;
    // Queue operations use this logical front offset so dequeue is O(1)
    // amortized instead of erasing from the front of a vector.
    std::size_t frontIndex{0};
};
struct MapBox {
    NativeContainerTypeRef storageType;
    // A simple association list (linear scan on lookup) rather than a hash
    // map - this avoids needing a std::hash<Value> specialization, and is
    // more than fast enough for the sizes a language this young will see.
    //
    // This also gives `map` a real, guaranteed ordering contract for free
    // (see ROADMAP.md's Phase 4 "HashMap/ordered-map distinction"
    // item): insertion order is preserved. Collection.mapSet on a NEW key
    // appends it at the end; on an EXISTING key it updates the value
    // in-place without moving the entry. Collection.mapRemove simply
    // erases, leaving every other entry's relative order untouched. No
    // separate "ordered map" type is needed - this behavior isn't
    // incidental, it's a direct consequence of `entries` being a plain
    // vector, and every Collection.map* native above works entry-by-entry
    // in the vector's own order.
    std::vector<std::pair<Value, Value>> entries;
};

// An instance of a user-defined class. `className` identifies the class for
// error messages and type checking; `fields` holds the instance's field values,
// keyed by field name. Methods are NOT stored per-instance (they live in the
// Chunk's func table, same as free functions) - only data fields go here.
struct ObjectBox {
    // Concrete generic identity for reflection/type validation. The dispatch
    // className remains the erased runtime class (e.g. Box).
    std::string genericTypeName;
    // Guards lazy initialization of synchronization/runtime backing state.
    // Object values can cross threads explicitly via Shared<T>, so first-use
    // initialization itself must be data-race-free.
    mutable std::mutex stateInitMutex;
    std::shared_ptr<std::mutex> mutexState;
    std::shared_ptr<std::shared_mutex> rwLockState;
    std::shared_ptr<std::atomic<std::int64_t>> atomicState;
    std::shared_ptr<std::atomic<bool>> atomicBoolState;
    std::shared_ptr<std::atomic<double>> atomicDoubleState;
    std::shared_ptr<std::mutex> atomicRefMutex;
    // Shared<T> exposes a synchronized cell. The class payload itself remains
    // GC-managed; this mutex protects concurrent get/set of the cell value.
    std::shared_ptr<std::recursive_mutex> sharedValueMutex;
    std::shared_ptr<AtomicRefState> atomicRefState;
    struct SemaphoreState {
        std::mutex mutex;
        std::condition_variable cv;
        std::int64_t permits{0};
    };
    struct ConditionState {
        std::mutex mutex;
        std::condition_variable cv;
    };
    std::shared_ptr<SemaphoreState> semaphoreState;
    std::shared_ptr<ConditionState> conditionState;
    struct ChannelState {
        struct PendingSend {
            Value value;
            TaskRef task;
        };
        struct PendingReceive {
            TaskRef task;
        };
        std::mutex mutex;
        std::condition_variable cvNotEmpty;
        std::condition_variable cvNotFull;
        std::deque<Value> items;
        std::deque<PendingSend> pendingSends;
        std::deque<PendingReceive> pendingReceives;
        std::size_t capacity{1};
    };
    std::shared_ptr<ChannelState> channelState;
    std::string className;
    RuntimeTypeRef runtimeType;
    std::unordered_map<std::string, Value> fields;
};

struct AtomicRefState {
    std::mutex mutex;
    Value value{};
};

// A lambda VALUE at runtime - a compiled func body (paramNames +
// entryAddress, same shape as FunctionInfo in bytecode.hpp) plus a snapshot
// of the enclosing scope it was created in. `captured` is a COPY (by-value
// capture - see ROADMAP.md Phase 7's capture-semantics decision), taken
// once at the moment the OpCode::MakeClosure instruction runs; mutating the
// original variable afterward is never visible through `captured`, and vice
// versa. See Compiler::compileLambdaExpr / OpCode::MakeClosure /
// OpCode::CallValue for where this gets built and consumed.
struct ClosureBox {
    std::string functionName;
    std::vector<std::string> paramNames;
    std::vector<std::string> parameterTypeNames;
    std::string returnTypeName;
    bool isAsync{false};
    bool isNative{false};
    std::size_t entryAddress{0};
    std::size_t functionIndex{0};
    std::shared_ptr<const Chunk> chunk;
    std::unordered_map<std::string, Value> captured;
    RuntimeTypeBindings typeBindings;
};

[[nodiscard]] Value makeEmptyList();
[[nodiscard]] Value makeEmptyMap();
[[nodiscard]] Value makeEmptyObject(const std::string& className);

// Converts any Value to its printable text form.
[[nodiscard]] std::string valueToString(const Value &v);
[[nodiscard]] std::int64_t valueHashCode(const Value &v);

// Truthiness rule for `if`, `&&`, `||`: bool uses itself, numbers are false only
// at zero, strings/lists/maps are false only when empty, nil is always false.
// Objects are always truthy (non-nil reference).
[[nodiscard]] bool isTruthy(const Value &v);

// Shared numeric helpers - used by the VM's arithmetic/bitwise ops AND by
// native library functions (Math.sqrt, etc.), so they live here instead of
// being duplicated in both places.
[[nodiscard]] bool isNumericValue(const Value &v);
[[nodiscard]] double toDouble(const Value &v);
[[nodiscard]] std::int64_t toInt64Strict(const Value &v);

// Value equality with numeric cross-type promotion (so `1 == 1.0` is true).
// Used by both the VM's == operator AND by Map/Set native functions doing key
// lookups - one shared definition of "equal" for the whole language.
[[nodiscard]] bool valuesEqual(const Value &a, const Value &b);
// Numeric comparison: returns -1, 0, or 1. Returns 2 for unordered values
// such as NaN, allowing relational operators to implement IEEE semantics.
[[nodiscard]] int compareNumericValues(const Value &a, const Value &b);

} // namespace zl
