#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <memory>
#include <type_traits>
#include <mutex>
#include <optional>
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

class ProtectedGCRoot;
class ZlThrownException : public std::exception {
public:
    explicit ZlThrownException(ObjectRef value);
    ~ZlThrownException() override;
    const char* what() const noexcept override { return "ZL exception"; }
    [[nodiscard]] const ObjectRef& value() const noexcept { return value_; }
private:
    ObjectRef value_;
    std::shared_ptr<ProtectedGCRoot> root_;
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

// Hash over Value for pooled lookup (see Chunk::constantIndex). Primitive
// alternatives hash by value; every other alternative hashes to a shared
// bucket and lets the equality decide. That keeps the hash consistent (equal
// values always agree) without baking container/pointer identity into it,
// and the pool only ever holds primitives in practice, so the shared
// bucket never sits on a hot path. Doubles hash by bit pattern, matching
// ValueEqual below (-0.0 and +0.0 get distinct slots).
struct ValueHash {
    [[nodiscard]] std::size_t operator()(const Value& v) const {
        return std::visit(
            [](const auto& alt) -> std::size_t {
                using T = std::decay_t<decltype(alt)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    return 0x9e3779b9u;
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return std::hash<std::int64_t>{}(alt);
                } else if constexpr (std::is_same_v<T, double>) {
                    std::uint64_t bits = 0;
                    static_assert(sizeof(bits) == sizeof(alt));
                    std::memcpy(&bits, &alt, sizeof(bits));
                    return std::hash<std::uint64_t>{}(bits);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    return std::hash<std::string>{}(alt);
                } else if constexpr (std::is_same_v<T, bool>) {
                    return alt ? 2u : 3u;
                } else {
                    return 0;
                }
            },
            v);
    }
};

// Pool identity for Chunk::constantIndex. Variant equality would do, except
// that it treats -0.0 and +0.0 as equal while the language observes the
// difference (log prints "-0" vs "0"; division by each takes a different
// sign), so doubles compare by bit pattern here. (The language's runtime
// `==` stays IEEE; this is only slot identity.)
struct ValueEqual {
    [[nodiscard]] bool operator()(const Value& a, const Value& b) const {
        if (a.index() != b.index()) return false;
        if (const auto* x = std::get_if<double>(&a)) {
            const auto* y = std::get_if<double>(&b);
            if (y == nullptr) return false;
            std::uint64_t xBits = 0, yBits = 0;
            static_assert(sizeof(xBits) == sizeof(*x));
            std::memcpy(&xBits, x, sizeof(xBits));
            std::memcpy(&yBits, y, sizeof(yBits));
            return xBits == yBits;
        }
        return a == b;
    }
};

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

    // --- string-key lookup index ------------------------------------------
    // entries stays the source of truth and keeps its ordering contract;
    // this cache exists only to make string-key lookups O(1) instead of
    // O(n). It is sound because valuesEqual(string, non-string) is always
    // false, so a string lookup key can only ever match string entry keys
    // by exact equality - non-string keys are simply absent from the index
    // and can never be missed. Non-string lookup keys (ints, objects, ...)
    // still scan linearly, preserving valuesEqual's cross-numeric
    // semantics exactly (1 == 1.0). Any structural change (append, erase)
    // invalidates the cache; an in-place value update does not, because
    // keys never move.
    // One mutex guards `entries` and the index together, and every
    // operation below holds it across lookup AND use. A map shared across
    // threads (via Shared) may be read and written concurrently; resolving
    // a position under the lock and then using entries[pos] after unlock
    // races with another thread's append (vector reallocation) or erase
    // (entries shifting), which is use-after-free, not a stale read. Each
    // single operation is therefore atomic; multi-step sequences that must
    // be atomic together still need an explicit Mutex/Shared.withLock.
    // The methods never nest (locked helpers are private and assume the
    // mutex is held) and never run user code, so no lock ordering exists
    // to invert. Traversals snapshot: holding one map's mutex across a
    // recursive walk would invert against another thread walking nested
    // maps in the opposite order.
    static constexpr std::size_t kNoEntry = static_cast<std::size_t>(-1);
    mutable std::mutex mutex;
    mutable std::unordered_map<std::string, std::size_t> stringKeyIndex;
    mutable bool stringKeyIndexValid{false};

    // Position of the first entry whose key equals `key`, or kNoEntry.
    // Exact semantics of a linear valuesEqual scan, O(1) for string keys
    // on maps of 8+ entries. The position is only meaningful while the
    // mutex is held, so this is just the single-shot lookup for existence
    // checks; mutation and read-then-use go through the compound ops.
    [[nodiscard]] std::size_t findEntry(const Value& key) const;
    // Atomic read-then-use: copies the value out under the lock.
    [[nodiscard]] std::optional<Value> tryGetEntry(const Value& key) const;
    // Atomic lookup-then-write: updates the value in place when the key
    // exists (keys keep their positions, the index stays valid), else
    // appends at the end. Preserves the ordering contract exactly.
    void setEntry(const Value& key, Value value);
    // Atomic lookup-then-erase. True when a entry was removed.
    bool removeEntry(const Value& key);
    // Point-in-time copy of every entry, for traversals (printing, JSON
    // encoding, key/value lists, contract checks) that must not hold the
    // mutex across a recursive walk.
    [[nodiscard]] std::vector<std::pair<Value, Value>> snapshotEntries() const;
    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return entries.size();
    }

private:
    // Locked helpers: the caller holds `mutex`.
    [[nodiscard]] std::size_t findEntryLocked(const Value& key) const;
    // Drop the cache after any mutation that changes entry positions.
    void invalidateKeyIndexLocked() const {
        stringKeyIndexValid = false;
        stringKeyIndex.clear();
    }
    // Record that `key` was just appended at the end of entries (the caller
    // has already emplaced it). An index that is already valid can absorb a
    // string key in O(1); a non-string key never enters the index, and an
    // invalid index is rebuilt on the next lookup anyway.
    void noteAppendedKeyLocked(const Value& key) const {
        const auto* appended = std::get_if<std::string>(&key);
        if (!appended) return;
        if (!stringKeyIndexValid) return;
        stringKeyIndex.emplace(*appended, entries.size() - 1);
    }
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

// Maximum nesting depth any recursive value-graph traversal (printing,
// JSON encoding, structural equality, hashing) will descend. Value graphs
// are built programmatically, so without a cap a hostile or accidental
// 100k-deep structure is a stack overflow (SIGSEGV). Matches the JSON
// decoder's own limit, so anything Serialize.decode produces always stays
// printable/comparable/encodable.
inline constexpr int kMaxValueNestingDepth = 500;

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
