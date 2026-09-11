#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zl::mir {

// ---------------------------------------------------------------------------
// MIR types
// ---------------------------------------------------------------------------
//
// MIR types are structural and interned. Every distinct type shape occupies
// exactly one slot in a TypeArena, so two types are the same type if and only
// if they have the same TypeId. That gives backends O(1) type equality and
// hashing without ever parsing a rendered type name back into structure - the
// mistake the pre-MIR pipeline made by carrying `std::string className` as the
// only real record of an object's identity.
//
// The kind set is deliberately the ZL type lattice, not a machine lattice:
// `list<int>` and `List<int>` are different types here because they are
// different things in the language (a builtin dynamic collection versus a
// compiled generic class).

// Handle for an interned type. 0 is reserved as "no type"; real ids start at 1.
using TypeId = std::uint32_t;

constexpr TypeId kNoType = 0;

enum class TypeKind : std::uint8_t {
    // No value at all. Only valid as a function return type or as the type of
    // an instruction with no result.
    Void,
    // The type of the `null` literal on its own. A reference type that happens
    // to hold null is still that reference type - nullability comes from the
    // kind (see isNullableKind), not from a union with Nil.
    Nil,
    Bool,
    Int,
    Double,
    String,
    // Builtin dynamic collections: the lowercase `list`/`map`/`set`/`array`
    // keywords. `arguments` holds the element / key+value / element type.
    List,
    Map,
    Set,
    // Fixed-size `array[N]<T>`; `fixedSize` is set.
    Array,
    // A class, `data` record, interface, or enum instance. `name` is the
    // declaring class name; `arguments` holds concrete generic arguments for
    // an instantiation (`List<int>` -> name "List", arguments [int]).
    Object,
    // `Task<T>`; `arguments` holds the payload type.
    Task,
    // `Shared<T>`; `arguments` holds the shared payload type.
    Shared,
    // `Option<T>` — the builtin sum of a payload and absence. `arguments`
    // holds the payload type. `Some<T>`/`None<T>` are the *class* spellings
    // (Object kinds); they are recognisably the same sum through
    // optionPayloadFor(), and `Some<int>` is assignable to `Option<int>`.
    // Keeping `Option<int>` a kind of its own (rather than an Object that
    // happens to be named "Option") is what lets a backend see "this value is
    // a payload-or-nothing" without string-matching class names.
    Option,
    // `Result<T,E>` — the builtin sum of a success value and an error value.
    // `arguments` holds [ok, error]. `Ok<T,E>`/`Err<T,E>` are the class
    // spellings (Object kinds), related through resultPartsFor() and
    // assignability the same way Some/None relate to Option.
    Result,
    // A callable value: `signature` is populated, `name` is empty.
    Function,
    // `A|B|...`; `arguments` holds the member types, kept sorted and deduped
    // by the arena so union identity is order-independent.
    Union,
    // An unsubstituted generic parameter (`T` inside `class Box<T>`). Only
    // legal inside a function that declares that parameter.
    TypeParam,
    // Statically unknown. Distinct from every real type: it means semantic
    // analysis did not resolve a type, not that the value has some type we
    // have not named yet.
    Unknown,
    // --- concurrency: threads and synchronisation ---------------------------
    // Each of these is a dedicated kind (not a plain Object) so a backend can
    // see "this value is a thread handle / channel / lock" without
    // string-matching class names. They still have a ZL class spelling
    // (`Thread`, `Channel`, `Mutex`, ...) as an Object kind carrying that
    // name; the predicates below accept both spellings the same way
    // isCollectionType accepts `list<T>` and `List<T>`. All are GC-managed
    // objects with internally-guarded native state (see ObjectBox in
    // value.hpp); sharing the handle is the intended use.
    //
    // `Thread` is an explicit OS-thread handle (`Thread.start` / `join`).
    // It is NOT itself a thread-safe capture: capturing a Thread across a
    // thread boundary is rejected, matching isThreadSafeClassName.
    Thread,
    // `Channel`: a bounded, thread-safe queue. `arguments[0]` is the element
    // type when known, `unknown` for the untyped channels the current
    // `Channel.create` produces (`Channel.send` takes `unknown`,
    // `Channel.receive` returns `unknown`).
    Channel,
    // `Mutex`, `RwLock`, `Atomic`, `Semaphore`, `Condition`: the runtime's
    // own synchronisation primitives. No type arguments.
    Mutex,
    RwLock,
    Atomic,
    Semaphore,
    Condition,
    // --- FFI: native resources ----------------------------------------------
    // Values that only exist at the native boundary. They have no ZL class
    // spelling and no GC identity: a handle is an opaque registry token
    // (NativeHandleRef), views are borrowed byte ranges tied to an owner's
    // lifetime, and a callback is a registry token with a lease. They are
    // reference-like for nullability (an invalid handle is a runtime error,
    // like null) but NOT collectable: ownership is deterministic
    // (NativeResourceOwner / Borrow / lease), never traced.
    //
    // `NativeHandle`: an opaque `NativeHandleRef` token. No arguments.
    NativeHandle,
    // `NativeBuffer`: a borrowed `NativeBufferView` (data + size). Valid only
    // while the owning resource is alive and the synchronous call is active.
    NativeBuffer,
    // `NativeStruct`: a borrowed `NativeStructView` (data + size +
    // alignment). Same lifetime as NativeBuffer. There are deliberately no
    // typed field-by-field struct schemas yet; a view is untyped bytes.
    NativeStruct,
    // `NativeCallback`: a `NativeCallbackRef` token. `signature` carries the
    // callable shape when known, exactly like Function.
    NativeCallback,
};

[[nodiscard]] const char* typeName(TypeKind kind) noexcept;

// True for the value-carrying primitive types: bool, int, double, string.
[[nodiscard]] bool isPrimitiveKind(TypeKind kind) noexcept;
// True for types that hold a heap reference and are therefore nullable in ZL:
// collections, objects, tasks, shared cells, options, results, functions,
// threads, channels, synchronisation primitives, and native resources.
// Primitives and Nil are not. Note this is NOT the same question as
// isCollectableKind: native handles/views/callbacks are reference-like for
// nullability but deterministically owned, never traced.
[[nodiscard]] bool isReferenceKind(TypeKind kind) noexcept;
// True for the numeric types ZL's arithmetic and comparison rules accept.
[[nodiscard]] bool isNumericKind(TypeKind kind) noexcept;
// True when a value of this type is managed by the collector rather than by
// explicit ownership. Mirrors which ZL types `owned`/`borrow` may be applied
// to. Threads, channels and synchronisation primitives are collectable (GC
// objects with native state); native handles/views/callbacks are NOT - they
// are deterministically owned (NativeResourceOwner / Borrow / lease).
[[nodiscard]] bool isCollectableKind(TypeKind kind) noexcept;
// True when `null` is a legal value of this kind. In ZL every reference type is
// nullable, `string` is nullable, and no numeric or bool is, so nullability is a
// property of the kind rather than a per-type flag: storing it separately would
// mean storing an invariant that every constructor had to remember to keep true.
// Note this is not the same question as isReferenceKind - a union's
// nullability depends on its members, so use TypeArena::isNullable for a type
// id rather than reading its kind here.
[[nodiscard]] bool isNullableKind(TypeKind kind) noexcept;

// A resolved callable shape. Kept as its own struct because function types are
// the one MIR type whose identity is a signature rather than a name plus
// arguments.
struct FunctionSignature {
    std::vector<std::uint32_t> parameterTypes; // TypeIds
    std::uint32_t returnType{0};               // TypeId
    bool isAsync{false};
    // False for a bare `func` with no declared signature. ZL allows an
    // unparameterised callable type, and inventing `func():void` for it would
    // be a lie a backend could act on: an unparameterised callable is one whose
    // arity and result are checked at the call boundary, not here.
    bool hasSignature{true};

    friend bool operator==(const FunctionSignature& a, const FunctionSignature& b) noexcept {
        return a.isAsync == b.isAsync && a.returnType == b.returnType &&
               a.hasSignature == b.hasSignature && a.parameterTypes == b.parameterTypes;
    }
};

// One interned type. Construct through TypeArena, never by hand: the arena is
// what makes `id` a stable identity and keeps the rendering cache consistent.
struct Type {
    TypeKind kind{TypeKind::Unknown};
    // Declaring/base name for Object/Task/Shared/Thread/Channel/Mutex/RwLock/
    // Atomic/Semaphore/Condition/NativeHandle/NativeBuffer/NativeStruct/
    // NativeCallback/TypeParam; empty otherwise.
    std::string name;
    // Generic arguments (Object/Task/Shared), element or key/value types
    // (List/Map/Set/Array), or union members (Union). TypeIds.
    std::vector<std::uint32_t> arguments;
    // Fixed array length for Array.
    std::optional<int> fixedSize;
    // Callable shape for Function.
    FunctionSignature signature;

    friend bool operator==(const Type& a, const Type& b) noexcept {
        return a.kind == b.kind && a.name == b.name && a.arguments == b.arguments &&
               a.fixedSize == b.fixedSize && a.signature == b.signature;
    }
};

// True for ZL's collection types.
//
// ZL spells a collection two ways. The lowercase keyword forms - `list<T>`,
// `map<K,V>`, `set<T>` - arrive as the List/Map/Set kinds. The capitalised
// class forms - `List<T>`, `Map<K,V>`, `Set<T>` - are real generic classes, so
// they arrive as an Object carrying that name with the same arguments. Both
// denote the same runtime collection, and a generic class can only be spelled
// the second way, since `list<T>` cannot be parameterised by a type variable
// the way `List<T>` can.
//
// Every rule about "is this a collection" therefore has to accept both
// spellings, or the two forms quietly diverge - indexing may accept `List<T>`
// while construction rejects it, which is what this predicate exists to stop.
[[nodiscard]] bool isCollectionType(const Type& type) noexcept;

// ---------------------------------------------------------------------------
// Concurrency and native-resource types: dual spellings
// ---------------------------------------------------------------------------
//
// The runtime synchronisation primitives (`Thread`, `Channel`, `Mutex`,
// `RwLock`, `Atomic`, `Semaphore`, `Condition`) each have a dedicated kind
// AND a ZL class spelling (Object carrying that name), because lowering may
// meet either: a `Thread.start` result typed by the checker as OBJECT
// "Thread", or a `thread_start` result typed directly as the Thread kind.
// Every rule about "is this a mutex" must accept both, or the two spellings
// quietly diverge the way collections once did.
//
// Native resources (`NativeHandle`, `NativeBuffer`, `NativeStruct`,
// `NativeCallback`) have no ZL class spelling: they only appear as their own
// kinds, produced and consumed at the FFI boundary.

// True for `Thread` (the kind) and for an Object named "Thread".
[[nodiscard]] bool isThreadType(const Type& type) noexcept;
// True for `Channel` (the kind, with element type in arguments[0]) and for an
// Object named "Channel".
[[nodiscard]] bool isChannelType(const Type& type) noexcept;
// True for `Mutex` and for an Object named "Mutex".
[[nodiscard]] bool isMutexType(const Type& type) noexcept;
// True for `RwLock` and for an Object named "RwLock".
[[nodiscard]] bool isRwLockType(const Type& type) noexcept;
// True for `Atomic` and for an Object named "Atomic".
[[nodiscard]] bool isAtomicType(const Type& type) noexcept;
// True for `Semaphore` and for an Object named "Semaphore".
[[nodiscard]] bool isSemaphoreType(const Type& type) noexcept;
// True for `Condition` and for an Object named "Condition".
[[nodiscard]] bool isConditionType(const Type& type) noexcept;
// True for `Shared<T>` (the kind) and for `Shared`/`Shared<T>` objects.
[[nodiscard]] bool isSharedType(const Type& type) noexcept;
// True for `Task<T>` (the kind) and for `Task`/`Task<T>` objects.
[[nodiscard]] bool isTaskType(const Type& type) noexcept;

// True for the values that may cross a thread boundary: `Shared<T>` plus the
// runtime's own synchronisation primitives (`Atomic`, `Mutex`, `RwLock`,
// `Semaphore`, `Channel`, `Condition`), each in either spelling. This is the
// MIR spelling of isThreadSafeClassName / capturedValueCrossesThreadBoundary:
// `Thread` itself is NOT included, and neither are primitives, plain objects,
// or func values. A closure that carries anything else across `Thread.start`
// or `Task.spawn` is a compile error at the source level and a verifier error
// here; the runtime gate (capturesAreExplicitlyShared) is the backstop.
[[nodiscard]] bool isThreadSafeCaptureType(const Type& type) noexcept;

// True for any synchronisation primitive: Shared, Atomic, Mutex, RwLock,
// Semaphore, Channel, Condition (either spelling). Thread is NOT included:
// it is a handle to a thread, not a guard for shared state.
[[nodiscard]] bool isSyncPrimitiveType(const Type& type) noexcept;

// True for the FFI resource kinds. These have no Object spelling.
[[nodiscard]] bool isNativeHandleType(const Type& type) noexcept;
[[nodiscard]] bool isNativeBufferType(const Type& type) noexcept;
[[nodiscard]] bool isNativeStructType(const Type& type) noexcept;
[[nodiscard]] bool isNativeCallbackType(const Type& type) noexcept;
// True for any of the four FFI resource kinds.
[[nodiscard]] bool isNativeResourceType(const Type& type) noexcept;

// The element type of a Channel-shaped type, or 0 for anything else / a
// malformed shape. Untyped channels (the current `Channel.create`) carry
// `unknown` здесь.
[[nodiscard]] std::uint32_t channelElementFor(const Type& type) noexcept;
// The payload type of a Task-shaped type, or 0 for anything else.
[[nodiscard]] std::uint32_t taskPayloadFor(const Type& type) noexcept;
// The payload type of a Shared-shaped type, or 0 for anything else.
[[nodiscard]] std::uint32_t sharedPayloadFor(const Type& type) noexcept;

// ---------------------------------------------------------------------------
// Sum types: Option / Result and their class spellings
// ---------------------------------------------------------------------------
//
// ZL spells a sum two ways. `Option<int>` in source is the sum type itself;
// `new Some<int>(42)` and `new None<int>()` construct the *classes* that
// implement it (`Some<T> extends Option<T>`, `None<T> extends Option<T>`, and
// likewise Ok/Err for Result). A MIR type coming straight out of a
// constructor call is therefore an Object named "Some", while the same value
// passed to a parameter declared `Option<int>` is the Option kind - and both
// must be readable as the same sum or every unwrap-shaped optimisation would
// need a special case per spelling.
//
// These predicates are that single answer, for both questions. They accept
// the kind forms (Option/Result) and the class spellings (Some/None/Ok/Err)
// by name; anything else is an ordinary class and reports "not a sum".

// True for `Option<T>` (the kind) and for `Some<T>`/`None<T>` (Object kinds
// carrying those names).
[[nodiscard]] bool isOptionType(const Type& type) noexcept;
// True for `Result<T,E>` (the kind) and for `Ok<T,E>`/`Err<T,E>`.
[[nodiscard]] bool isResultType(const Type& type) noexcept;
// True for either sum family, in either spelling.
[[nodiscard]] bool isSumType(const Type& type) noexcept;

// The payload type of an Option-shaped type: the payload for `Option<T>`,
// `Some<T>`, `None<T>`. Returns 0 for anything else, and for a sum whose
// argument is missing or unresolved.
[[nodiscard]] std::uint32_t optionPayloadFor(const Type& type) noexcept;

// The two halves of a Result-shaped type: [ok, error] for `Result<T,E>`,
// `Ok<T,E>`, `Err<T,E>`. Returns {0, 0} for anything else.
struct ResultParts {
    std::uint32_t ok{0};
    std::uint32_t error{0};
};
[[nodiscard]] ResultParts resultPartsFor(const Type& type) noexcept;

// True when `from` (an Option/Result-shaped type in either spelling) can be
// used where `to` is expected, by the sum relation alone: the two must be the
// same family, with assignable payloads. Nominal subclass edges beyond the
// sum relation (e.g. a user class extending Option) are the caller's question,
// because they need the module's class layouts, which live above the type
// table. `assignableArg` decides payload compatibility; pass the verifier's
// assignable (or your own) so nesting composes - this is what makes
// `Some<List<int>>` assignable to `Option<list<int>>` and
// `Ok<Option<int>, string>` assignable to `Result<Option<int>,string>`.
template <typename Assignable>
[[nodiscard]] bool sumAssignable(const Type& from, const Type& to, const Assignable& assignableArg) noexcept {
    if (isOptionType(from) && isOptionType(to)) {
        const std::uint32_t fromPayload = optionPayloadFor(from);
        const std::uint32_t toPayload = optionPayloadFor(to);
        return fromPayload != 0 && toPayload != 0 && assignableArg(fromPayload, toPayload);
    }
    if (isResultType(from) && isResultType(to)) {
        const ResultParts fromParts = resultPartsFor(from);
        const ResultParts toParts = resultPartsFor(to);
        return fromParts.ok != 0 && toParts.ok != 0 && fromParts.error != 0 && toParts.error != 0 &&
               assignableArg(fromParts.ok, toParts.ok) && assignableArg(fromParts.error, toParts.error);
    }
    return false;
}

struct TypeHash {
    [[nodiscard]] std::size_t operator()(const Type& type) const noexcept;
};

// Owns every type in one MIR module and hands out TypeIds. Ids are dense and
// start at 1, so 0 is a usable "no type" sentinel.
//
// Interning is a memoising lookup: asking for a type that already exists
// changes nothing observable. The mutating methods are therefore `const` and
// the storage is `mutable`, so a pass that holds a `const Module&` can still
// build a type to compare against without copying the arena or casting away
// constness. The consequence is that a TypeArena is not safe to intern into
// from two threads at once, which matches how MIR is used - one compiler
// thread owns a module.
class TypeArena {
public:
    TypeArena();

    // Interns `type`, returning the id of the existing identical type when
    // there is one. Canonicalises unions (sorted, deduplicated, single-member
    // unions collapse to that member) so `int|string` and `string|int` share
    // an id.
    [[nodiscard]] std::uint32_t intern(Type type) const;

    // The interned type for `id`, or nullptr when `id` is not in this arena.
    [[nodiscard]] const Type* find(std::uint32_t id) const;

    [[nodiscard]] std::size_t size() const { return types_.size(); }

    // Pre-interned common types. Stable across arenas.
    [[nodiscard]] std::uint32_t voidType() const { return void_; }
    [[nodiscard]] std::uint32_t nilType() const { return nil_; }
    [[nodiscard]] std::uint32_t boolType() const { return bool_; }
    [[nodiscard]] std::uint32_t intType() const { return int_; }
    [[nodiscard]] std::uint32_t doubleType() const { return double_; }
    [[nodiscard]] std::uint32_t stringType() const { return string_; }
    [[nodiscard]] std::uint32_t unknownType() const { return unknown_; }

    // Convenience constructors for the composite types.
    [[nodiscard]] std::uint32_t objectType(const std::string& className,
                                           std::vector<std::uint32_t> arguments = {}) const;
    [[nodiscard]] std::uint32_t listType(std::uint32_t element) const;
    [[nodiscard]] std::uint32_t setType(std::uint32_t element) const;
    [[nodiscard]] std::uint32_t mapType(std::uint32_t key, std::uint32_t value) const;
    // `size` is empty for a dynamic `array<T>` and set for `array[N]<T>`. Taking
    // a plain int here would force every caller to invent a length for the
    // dynamic form, which then renders as a fixed-size array the source never
    // wrote.
    [[nodiscard]] std::uint32_t arrayType(std::uint32_t element, std::optional<int> size) const;
    [[nodiscard]] std::uint32_t taskType(std::uint32_t payload) const;
    [[nodiscard]] std::uint32_t sharedType(std::uint32_t payload) const;
    // `Option<T>`: the builtin payload-or-nothing sum. `Option<int>` and a
    // hypothetical Object named "Option" with argument `int` are different
    // ids: the kind is semantic, and a backend may rely on it.
    [[nodiscard]] std::uint32_t optionType(std::uint32_t payload) const;
    // `Result<T,E>`: [ok, error].
    [[nodiscard]] std::uint32_t resultType(std::uint32_t ok, std::uint32_t error) const;
    [[nodiscard]] std::uint32_t functionType(FunctionSignature signature) const;
    [[nodiscard]] std::uint32_t unionType(std::vector<std::uint32_t> members) const;
    [[nodiscard]] std::uint32_t typeParam(const std::string& name) const;
    // Concurrency and FFI types. Each sets the kind's declaring `name` (see
    // taskType) so consumers asking "which class is this?" get an answer.
    [[nodiscard]] std::uint32_t threadType() const;
    [[nodiscard]] std::uint32_t channelType(std::uint32_t element) const;
    [[nodiscard]] std::uint32_t mutexType() const;
    [[nodiscard]] std::uint32_t rwLockType() const;
    [[nodiscard]] std::uint32_t atomicType() const;
    [[nodiscard]] std::uint32_t semaphoreType() const;
    [[nodiscard]] std::uint32_t conditionType() const;
    [[nodiscard]] std::uint32_t nativeHandleType() const;
    [[nodiscard]] std::uint32_t nativeBufferType() const;
    [[nodiscard]] std::uint32_t nativeStructType() const;
    [[nodiscard]] std::uint32_t nativeCallbackType(FunctionSignature signature) const;
    [[nodiscard]] std::uint32_t nativeCallbackType() const;

    // Canonical rendering, e.g. "int", "List<int>", "map<string,int>",
    // "func(int,string):bool", "int|string", "T". Cached on first use. The
    // rendering is exactly the spelling ZL source uses, so a rendered MIR type
    // can be read against the program that produced it.
    [[nodiscard]] std::string render(std::uint32_t id) const;

    // True when `null` is a legal value of this type. A union is nullable when
    // any member is, matching how the checker reads `int|string` - `string`
    // admits null, so the union does too. Testing only the top-level kind would
    // call every union non-nullable.
    [[nodiscard]] bool isNullable(std::uint32_t id) const {
        const Type* type = find(id);
        if (!type) return false;
        if (type->kind == TypeKind::Union) {
            for (std::uint32_t member : type->arguments)
                if (isNullable(member)) return true;
            return false;
        }
        return isNullableKind(type->kind);
    }

private:
    // Mutable because interning is memoisation, not a change to the arena's
    // meaning. See the class comment.
    mutable std::vector<Type> types_;
    // Rendering cache: filled lazily by the const render() accessor.
    mutable std::vector<std::string> renderings_;
    mutable std::unordered_map<Type, std::uint32_t, TypeHash> index_;
    std::uint32_t void_{0};
    std::uint32_t nil_{0};
    std::uint32_t bool_{0};
    std::uint32_t int_{0};
    std::uint32_t double_{0};
    std::uint32_t string_{0};
    std::uint32_t unknown_{0};
};

} // namespace zl::mir
