#include "zl/mir/type.hpp"

#include <algorithm>
#include <functional>
#include <sstream>

namespace zl::mir {
namespace {

std::size_t hashCombine(std::size_t seed, std::size_t value) noexcept {
    return seed ^ (value + 0x9e3779b9u + (seed << 6) + (seed >> 2));
}

} // namespace

const char* typeName(TypeKind kind) noexcept {
    switch (kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Nil: return "nil";
        case TypeKind::Bool: return "bool";
        case TypeKind::Int: return "int";
        case TypeKind::Double: return "double";
        case TypeKind::String: return "string";
        case TypeKind::List: return "list";
        case TypeKind::Map: return "map";
        case TypeKind::Set: return "set";
        case TypeKind::Array: return "array";
        case TypeKind::Object: return "object";
        case TypeKind::Task: return "Task";
        case TypeKind::Shared: return "Shared";
        case TypeKind::Option: return "Option";
        case TypeKind::Result: return "Result";
        case TypeKind::Function: return "func";
        case TypeKind::Union: return "union";
        case TypeKind::TypeParam: return "typeparam";
        case TypeKind::Unknown: return "unknown";
        case TypeKind::Thread: return "Thread";
        case TypeKind::Channel: return "Channel";
        case TypeKind::Mutex: return "Mutex";
        case TypeKind::RwLock: return "RwLock";
        case TypeKind::Atomic: return "Atomic";
        case TypeKind::Semaphore: return "Semaphore";
        case TypeKind::Condition: return "Condition";
        case TypeKind::NativeHandle: return "NativeHandle";
        case TypeKind::NativeBuffer: return "NativeBuffer";
        case TypeKind::NativeStruct: return "NativeStruct";
        case TypeKind::NativeCallback: return "NativeCallback";
    }
    return "unknown";
}

bool isPrimitiveKind(TypeKind kind) noexcept {
    switch (kind) {
        case TypeKind::Bool:
        case TypeKind::Int:
        case TypeKind::Double:
        case TypeKind::String:
            return true;
        default:
            return false;
    }
}

bool isReferenceKind(TypeKind kind) noexcept {
    switch (kind) {
        case TypeKind::List:
        case TypeKind::Map:
        case TypeKind::Set:
        case TypeKind::Array:
        case TypeKind::Object:
        case TypeKind::Task:
        case TypeKind::Shared:
        case TypeKind::Option:
        case TypeKind::Result:
        case TypeKind::Function:
        case TypeKind::Thread:
        case TypeKind::Channel:
        case TypeKind::Mutex:
        case TypeKind::RwLock:
        case TypeKind::Atomic:
        case TypeKind::Semaphore:
        case TypeKind::Condition:
        case TypeKind::NativeHandle:
        case TypeKind::NativeBuffer:
        case TypeKind::NativeStruct:
        case TypeKind::NativeCallback:
            return true;
        default:
            return false;
    }
}

bool isNumericKind(TypeKind kind) noexcept {
    return kind == TypeKind::Int || kind == TypeKind::Double;
}

bool isCollectableKind(TypeKind kind) noexcept {
    // Native resources are reference-like for nullability but deterministically
    // owned, never traced. Everything else reference-like is collector-managed.
    switch (kind) {
        case TypeKind::NativeHandle:
        case TypeKind::NativeBuffer:
        case TypeKind::NativeStruct:
        case TypeKind::NativeCallback:
            return false;
        default:
            return isReferenceKind(kind);
    }
}

bool isNullableKind(TypeKind kind) noexcept {
    // `string` is a value type at runtime - the VM holds it inline in the Value
    // variant, not behind a collector handle - but the language still lets it
    // be null, so nullability cannot simply be "is a reference". Keeping the
    // two questions separate is what lets isCollectableKind stay a pure
    // reference test.
    switch (kind) {
        case TypeKind::String:
        case TypeKind::Nil:
            return true;
        default:
            return isReferenceKind(kind);
    }
}

bool isCollectionType(const Type& type) noexcept {
    switch (type.kind) {
        case TypeKind::List:
        case TypeKind::Map:
        case TypeKind::Set:
        // A fixed-size array is a list at runtime (see the array branch of
        // runtimeValueTypeName), and its literals are built by the same
        // new_collection + index_store shape. Excluding it here made the
        // lowerer downgrade an `array[N]<T>` literal to list<unknown>, a
        // store the verifier then refused on the array-typed slot.
        case TypeKind::Array:
            return true;
        // The capitalised class forms. Only the three collection classes count:
        // ZL has plenty of other generic classes, and none of them is a
        // collection just because it takes a type argument.
        case TypeKind::Object:
            return type.name == "List" || type.name == "Map" || type.name == "Set";
        default:
            return false;
    }
}

bool isOptionType(const Type& type) noexcept {
    if (type.kind == TypeKind::Option) return true;
    // The class spellings. Only the exact builtin names count: a user class
    // called "Some" is still an ordinary class, but the builtin library owns
    // these names (they are declared in the always-present builtin source, not
    // importable user code), so matching by name cannot collide with a
    // program's own types.
    return type.kind == TypeKind::Object && (type.name == "Some" || type.name == "None");
}

bool isResultType(const Type& type) noexcept {
    if (type.kind == TypeKind::Result) return true;
    return type.kind == TypeKind::Object && (type.name == "Ok" || type.name == "Err");
}

bool isSumType(const Type& type) noexcept {
    return isOptionType(type) || isResultType(type);
}

namespace {

bool isObjectNamed(const Type& type, const char* name) noexcept {
    if (type.kind != TypeKind::Object) return false;
    if (type.name == name) return true;
    // A generic instantiation keeps its full name (`Shared<int>`); the base
    // name is what identifies the class.
    const std::string prefix = std::string(name) + "<";
    return type.name.rfind(prefix, 0) == 0;
}

} // namespace

bool isThreadType(const Type& type) noexcept {
    if (type.kind == TypeKind::Thread) return true;
    return isObjectNamed(type, "Thread");
}

bool isChannelType(const Type& type) noexcept {
    if (type.kind == TypeKind::Channel) return true;
    return isObjectNamed(type, "Channel");
}

bool isMutexType(const Type& type) noexcept {
    if (type.kind == TypeKind::Mutex) return true;
    return isObjectNamed(type, "Mutex");
}

bool isRwLockType(const Type& type) noexcept {
    if (type.kind == TypeKind::RwLock) return true;
    return isObjectNamed(type, "RwLock");
}

bool isAtomicType(const Type& type) noexcept {
    if (type.kind == TypeKind::Atomic) return true;
    return isObjectNamed(type, "Atomic");
}

bool isSemaphoreType(const Type& type) noexcept {
    if (type.kind == TypeKind::Semaphore) return true;
    return isObjectNamed(type, "Semaphore");
}

bool isConditionType(const Type& type) noexcept {
    if (type.kind == TypeKind::Condition) return true;
    return isObjectNamed(type, "Condition");
}

bool isSharedType(const Type& type) noexcept {
    if (type.kind == TypeKind::Shared) return true;
    return isObjectNamed(type, "Shared");
}

bool isTaskType(const Type& type) noexcept {
    if (type.kind == TypeKind::Task) return true;
    return isObjectNamed(type, "Task");
}

bool isSyncPrimitiveType(const Type& type) noexcept {
    return isSharedType(type) || isAtomicType(type) || isMutexType(type) || isRwLockType(type) ||
           isSemaphoreType(type) || isChannelType(type) || isConditionType(type);
}

bool isThreadSafeCaptureType(const Type& type) noexcept {
    // Keep this list in step with isThreadSafeClassName in src/vm/native.cpp
    // and capturedValueCrossesThreadBoundary in thread_capture.cpp. `Thread`
    // is deliberately absent: a thread handle may not cross.
    return isSyncPrimitiveType(type);
}

bool isNativeHandleType(const Type& type) noexcept {
    if (type.kind == TypeKind::NativeHandle) return true;
    return type.kind == TypeKind::Object && type.name == "NativeHandle";
}

bool isNativeBufferType(const Type& type) noexcept {
    return type.kind == TypeKind::NativeBuffer;
}

bool isNativeStructType(const Type& type) noexcept {
    return type.kind == TypeKind::NativeStruct;
}

bool isNativeCallbackType(const Type& type) noexcept {
    return type.kind == TypeKind::NativeCallback;
}

bool isNativeResourceType(const Type& type) noexcept {
    return isNativeHandleType(type) || isNativeBufferType(type) || isNativeStructType(type) ||
           isNativeCallbackType(type);
}

std::uint32_t channelElementFor(const Type& type) noexcept {
    if (type.kind == TypeKind::Channel) {
        return type.arguments.size() == 1 ? type.arguments.front() : 0;
    }
    if (type.kind == TypeKind::Object && (type.name == "Channel" || type.name.rfind("Channel<", 0) == 0)) {
        return type.arguments.size() == 1 ? type.arguments.front() : 0;
    }
    return 0;
}

std::uint32_t taskPayloadFor(const Type& type) noexcept {
    if (!isTaskType(type)) return 0;
    return type.arguments.size() == 1 ? type.arguments.front() : 0;
}

std::uint32_t sharedPayloadFor(const Type& type) noexcept {
    if (!isSharedType(type)) return 0;
    return type.arguments.size() == 1 ? type.arguments.front() : 0;
}

std::uint32_t optionPayloadFor(const Type& type) noexcept {
    if (!isOptionType(type)) return 0;
    // Option<T> always has exactly the payload argument; the class spellings
    // carry it too (`Some<T>` has one parameter because `None<T>` still needs
    // the payload type to satisfy `Option<T>`). A malformed shape yields 0
    // rather than guessing.
    if (type.arguments.size() != 1) return 0;
    return type.arguments.front();
}

ResultParts resultPartsFor(const Type& type) noexcept {
    if (!isResultType(type)) return {};
    if (type.arguments.size() != 2) return {};
    return ResultParts{type.arguments[0], type.arguments[1]};
}

std::size_t TypeHash::operator()(const Type& type) const noexcept {
    std::size_t seed = static_cast<std::size_t>(type.kind);
    seed = hashCombine(seed, std::hash<std::string>{}(type.name));
    for (std::uint32_t argument : type.arguments) seed = hashCombine(seed, static_cast<std::size_t>(argument));
    if (type.fixedSize) seed = hashCombine(seed, static_cast<std::size_t>(*type.fixedSize));
    for (std::uint32_t parameter : type.signature.parameterTypes)
        seed = hashCombine(seed, static_cast<std::size_t>(parameter));
    seed = hashCombine(seed, static_cast<std::size_t>(type.signature.returnType));
    if (type.signature.isAsync) seed = hashCombine(seed, 3u);
    return seed;
}

TypeArena::TypeArena() {
    // Slot 0 is reserved as the "no type" sentinel so that a zero-initialised
    // TypeId can never be mistaken for a real type.
    types_.push_back(Type{});
    renderings_.push_back("<none>");

    auto internPrimitive = [&](TypeKind kind) {
        Type t;
        t.kind = kind;
        types_.push_back(t);
        renderings_.push_back(typeName(kind));
        index_.emplace(types_.back(), static_cast<std::uint32_t>(types_.size() - 1));
        return static_cast<std::uint32_t>(types_.size() - 1);
    };

    void_ = internPrimitive(TypeKind::Void);
    nil_ = internPrimitive(TypeKind::Nil);
    bool_ = internPrimitive(TypeKind::Bool);
    int_ = internPrimitive(TypeKind::Int);
    double_ = internPrimitive(TypeKind::Double);
    string_ = internPrimitive(TypeKind::String);
    unknown_ = internPrimitive(TypeKind::Unknown);
}

std::uint32_t TypeArena::intern(Type type) const {
    if (type.kind == TypeKind::Union) {
        std::sort(type.arguments.begin(), type.arguments.end());
        type.arguments.erase(std::unique(type.arguments.begin(), type.arguments.end()), type.arguments.end());
        // A one-member union is that member. This keeps `int|nil` style
        // degenerate unions from creating a distinct identity.
        if (type.arguments.size() == 1) return type.arguments.front();
    }
    // `fixedSize` is deliberately NOT normalised here. It used to be a plain int
    // where 0 meant "no size given", so interning filled it in; it is now an
    // optional, and writing 0 into it would turn a dynamic `array<int>` into
    // `array[0]<int>` - a fixed-size array the source never wrote.
    if (type.kind == TypeKind::Function) type.name.clear();

    const auto existing = index_.find(type);
    if (existing != index_.end()) return existing->second;

    const std::uint32_t id = static_cast<std::uint32_t>(types_.size());
    types_.push_back(std::move(type));
    renderings_.push_back({});
    index_.emplace(types_.back(), id);
    return id;
}

const Type* TypeArena::find(std::uint32_t id) const {
    if (id == kNoType || id >= types_.size()) return nullptr;
    return &types_[id];
}

std::uint32_t TypeArena::objectType(const std::string& className, std::vector<std::uint32_t> arguments) const {
    Type t;
    t.kind = TypeKind::Object;
    t.name = className;
    t.arguments = std::move(arguments);
    return intern(std::move(t));
}

std::uint32_t TypeArena::listType(std::uint32_t element) const {
    Type t;
    t.kind = TypeKind::List;
    t.arguments = {element};
    return intern(std::move(t));
}

std::uint32_t TypeArena::setType(std::uint32_t element) const {
    Type t;
    t.kind = TypeKind::Set;
    t.arguments = {element};
    return intern(std::move(t));
}

std::uint32_t TypeArena::mapType(std::uint32_t key, std::uint32_t value) const {
    Type t;
    t.kind = TypeKind::Map;
    t.arguments = {key, value};
    return intern(std::move(t));
}

std::uint32_t TypeArena::arrayType(std::uint32_t element, std::optional<int> size) const {
    Type t;
    t.kind = TypeKind::Array;
    t.arguments = {element};
    t.fixedSize = size;
    return intern(std::move(t));
}

std::uint32_t TypeArena::taskType(std::uint32_t payload) const {
    Type t;
    t.kind = TypeKind::Task;
    // `name` is the declaring name of a nominal type (see Type::name). Rendering
    // did not need it - kindName supplies "Task" - but every consumer that asks
    // "which class is this?" does, and a method call on a Task<T> receiver came
    // back with no class name at all.
    t.name = "Task";
    t.arguments = {payload};
    return intern(std::move(t));
}

std::uint32_t TypeArena::sharedType(std::uint32_t payload) const {
    Type t;
    t.kind = TypeKind::Shared;
    t.name = "Shared"; // see taskType
    t.arguments = {payload};
    return intern(std::move(t));
}

std::uint32_t TypeArena::optionType(std::uint32_t payload) const {
    Type t;
    t.kind = TypeKind::Option;
    t.name = "Option";
    t.arguments = {payload};
    return intern(std::move(t));
}

std::uint32_t TypeArena::resultType(std::uint32_t ok, std::uint32_t error) const {
    Type t;
    t.kind = TypeKind::Result;
    t.name = "Result";
    t.arguments = {ok, error};
    return intern(std::move(t));
}


std::uint32_t TypeArena::threadType() const {
    Type t;
    t.kind = TypeKind::Thread;
    t.name = "Thread";
    return intern(std::move(t));
}

std::uint32_t TypeArena::channelType(std::uint32_t element) const {
    Type t;
    t.kind = TypeKind::Channel;
    t.name = "Channel";
    t.arguments = {element};
    return intern(std::move(t));
}

std::uint32_t TypeArena::mutexType() const {
    Type t;
    t.kind = TypeKind::Mutex;
    t.name = "Mutex";
    return intern(std::move(t));
}

std::uint32_t TypeArena::rwLockType() const {
    Type t;
    t.kind = TypeKind::RwLock;
    t.name = "RwLock";
    return intern(std::move(t));
}

std::uint32_t TypeArena::atomicType() const {
    Type t;
    t.kind = TypeKind::Atomic;
    t.name = "Atomic";
    return intern(std::move(t));
}

std::uint32_t TypeArena::semaphoreType() const {
    Type t;
    t.kind = TypeKind::Semaphore;
    t.name = "Semaphore";
    return intern(std::move(t));
}

std::uint32_t TypeArena::conditionType() const {
    Type t;
    t.kind = TypeKind::Condition;
    t.name = "Condition";
    return intern(std::move(t));
}

std::uint32_t TypeArena::nativeHandleType() const {
    Type t;
    t.kind = TypeKind::NativeHandle;
    t.name = "NativeHandle";
    return intern(std::move(t));
}

std::uint32_t TypeArena::nativeBufferType() const {
    Type t;
    t.kind = TypeKind::NativeBuffer;
    t.name = "NativeBuffer";
    return intern(std::move(t));
}

std::uint32_t TypeArena::nativeStructType() const {
    Type t;
    t.kind = TypeKind::NativeStruct;
    t.name = "NativeStruct";
    return intern(std::move(t));
}

std::uint32_t TypeArena::nativeCallbackType(FunctionSignature signature) const {
    Type t;
    t.kind = TypeKind::NativeCallback;
    t.name = "NativeCallback";
    t.signature = std::move(signature);
    return intern(std::move(t));
}

std::uint32_t TypeArena::nativeCallbackType() const {
    FunctionSignature signature;
    signature.hasSignature = false;
    signature.returnType = unknownType();
    return nativeCallbackType(std::move(signature));
}

std::uint32_t TypeArena::functionType(FunctionSignature signature) const {
    Type t;
    t.kind = TypeKind::Function;
    t.signature = std::move(signature);
    return intern(std::move(t));
}

std::uint32_t TypeArena::unionType(std::vector<std::uint32_t> members) const {
    Type t;
    t.kind = TypeKind::Union;
    t.arguments = std::move(members);
    return intern(std::move(t));
}

std::uint32_t TypeArena::typeParam(const std::string& name) const {
    Type t;
    t.kind = TypeKind::TypeParam;
    t.name = name;
    return intern(std::move(t));
}

std::string TypeArena::render(std::uint32_t id) const {
    const Type* type = find(id);
    if (!type) return "<invalid-type>";
    if (!renderings_[id].empty()) return renderings_[id];

    const auto renderArguments = [&](char open, char close) {
        std::string out;
        out += open;
        for (std::size_t i = 0; i < type->arguments.size(); ++i) {
            if (i) out += ",";
            out += render(type->arguments[i]);
        }
        out += close;
        return out;
    };

    std::string out;
    switch (type->kind) {
        case TypeKind::Object:
            out = type->name.empty() ? std::string("object") : type->name;
            if (!type->arguments.empty()) out += renderArguments('<', '>');
            break;
        case TypeKind::List:
        case TypeKind::Set:
            out = std::string(typeName(type->kind)) + renderArguments('<', '>');
            break;
        case TypeKind::Map:
            out = std::string("map") + renderArguments('<', '>');
            break;
        case TypeKind::Array: {
            // The `[N]` suffix is only part of the spelling when a size was
            // actually stated - `array<int>` is a dynamic array, not
            // `array[0]<int>`. Both describeTypeAnnotation and describeTypeName
            // spell it this way, so MIR must too or a round trip through a
            // rendered name changes the type.
            if (type->fixedSize) {
                out = "array[" + std::to_string(*type->fixedSize) + "]" + renderArguments('<', '>');
            } else {
                out = "array" + renderArguments('<', '>');
            }
            break;
        }
        case TypeKind::Task:
            out = "Task" + renderArguments('<', '>');
            break;
        case TypeKind::Shared:
            out = "Shared" + renderArguments('<', '>');
            break;
        case TypeKind::Channel:
            // Untyped channels (the current `Channel.create`) render bare;
            // typed ones carry the element type. `Channel<unknown>` would be
            // noise: the source never spells it.
            if (type->arguments.size() == 1) {
                const Type* element = find(type->arguments.front());
                if (element && element->kind == TypeKind::Unknown) {
                    out = "Channel";
                    break;
                }
            }
            out = "Channel";
            if (!type->arguments.empty()) out += renderArguments('<', '>');
            break;
        case TypeKind::Thread:
        case TypeKind::Mutex:
        case TypeKind::RwLock:
        case TypeKind::Atomic:
        case TypeKind::Semaphore:
        case TypeKind::Condition:
        case TypeKind::NativeHandle:
        case TypeKind::NativeBuffer:
        case TypeKind::NativeStruct:
            out = typeName(type->kind);
            break;
        case TypeKind::NativeCallback: {
            if (!type->signature.hasSignature) {
                out = "NativeCallback";
                break;
            }
            out = "NativeCallback(";
            for (std::size_t i = 0; i < type->signature.parameterTypes.size(); ++i) {
                if (i) out += ",";
                out += render(type->signature.parameterTypes[i]);
            }
            out += "):";
            out += render(type->signature.returnType);
            break;
        }
        case TypeKind::Option:
            out = "Option" + renderArguments('<', '>');
            break;
        case TypeKind::Result:
            out = "Result" + renderArguments('<', '>');
            break;
        case TypeKind::Union: {
            for (std::size_t i = 0; i < type->arguments.size(); ++i) {
                if (i) out += "|";
                out += render(type->arguments[i]);
            }
            break;
        }
        case TypeKind::Function: {
            if (!type->signature.hasSignature) { out = "func"; break; }
            out = "func(";
            for (std::size_t i = 0; i < type->signature.parameterTypes.size(); ++i) {
                if (i) out += ",";
                out += render(type->signature.parameterTypes[i]);
            }
            out += "):";
            out += type->signature.hasSignature ? render(type->signature.returnType) : std::string("unknown");
            break;
        }
        case TypeKind::TypeParam:
            out = type->name;
            break;
        default:
            out = typeName(type->kind);
            break;
    }
    renderings_[id] = out;
    return out;
}

} // namespace zl::mir
