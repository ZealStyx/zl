#include "zl/vm/runtime_type_checks.hpp"
#include "zl/compiler/bytecode.hpp"
#include "zl/vm/runtime_fault.hpp"

#include <algorithm>

namespace zl {
namespace {
std::recursive_mutex& storageMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}
bool unconstrained(const TypeName& type) {
    return type.args.empty() && type.unionMembers.empty() &&
           (type.name == "unknown" || type.name == "object");
}
bool sameContract(const NativeContainerType& a, const NativeContainerType& b) {
    if (a.fixedSize != b.fixedSize || a.arguments.size() != b.arguments.size()) return false;
    for (std::size_t i = 0; i < a.arguments.size(); ++i)
        if (!typeNamesEqual(a.arguments[i], b.arguments[i])) return false;
    return true;
}
}

RuntimeTypeCheck::RuntimeTypeCheck(const Chunk* chunk) : chunk_(chunk), lock_(storageMutex()) {}

NativeContainerTypeRef RuntimeTypeCheck::proposed(NativeContainerTypeRef& slot) const {
    const auto it = std::find_if(pending_.rbegin(), pending_.rend(), [&](const auto& entry) { return entry.slot == &slot; });
    return it == pending_.rend() ? slot : it->type;
}

bool RuntimeTypeCheck::check(const Value& value, const std::string& typeName, Mode mode) {
    auto before = pending_;
    if (matches(value, parseTypeName(typeName), mode)) return true;
    pending_ = std::move(before);
    return false;
}

void RuntimeTypeCheck::require(const Value& value, const std::string& typeName) {
    if (!check(value, typeName))
        throwTypeError("type assertion failed: expected " + typeName + ", got " + runtimeValueTypeName(value));
}

bool RuntimeTypeCheck::matches(const Value& value, const TypeName& type, Mode mode) {
    if (!type.unionMembers.empty()) {
        const auto before = pending_;
        for (const auto& member : type.unionMembers) {
            if (matches(value, member, mode)) return true;
            pending_ = before;
        }
        return false;
    }
    if (std::holds_alternative<std::monostate>(value))
        return mode == Mode::Pattern ? reflectiveTypeMatchesName(value, describeTypeName(type), chunk_)
                                     : runtimeAssignableToType(value, describeTypeName(type), chunk_);
    if (type.name == "list" || type.name == "set" || type.name == "array" || type.name == "map")
        return container(value, type);
    return mode == Mode::Pattern ? reflectiveTypeMatchesName(value, describeTypeName(type), chunk_)
                                 : runtimeAssignableToType(value, describeTypeName(type), chunk_);
}

bool RuntimeTypeCheck::container(const Value& value, const TypeName& type) {
    const bool map = type.name == "map";
    ListRef list;
    MapRef dictionary;
    if (map) {
        const auto* ref = std::get_if<MapRef>(&value);
        if (!ref || !*ref) return false;
        dictionary = *ref;
    } else {
        const auto* ref = std::get_if<ListRef>(&value);
        if (!ref || !*ref) return false;
        list = *ref;
    }
    const std::size_t arity = map ? 2 : 1;
    if (!type.args.empty() && type.args.size() != arity) return false;
    auto& slot = map ? dictionary->storageType : list->storageType;
    const auto prior = proposed(slot);
    NativeContainerType wanted = prior ? *prior : NativeContainerType{};
    if (wanted.arguments.empty()) wanted.arguments.resize(arity, TypeName{"unknown", {}, {}, {}});
    for (std::size_t i = 0; i < type.args.size(); ++i) {
        if (unconstrained(type.args[i])) continue;
        if (!unconstrained(wanted.arguments[i]) && !typeNamesEqual(wanted.arguments[i], type.args[i])) return false;
        wanted.arguments[i] = type.args[i];
    }
    if (type.fixedSize) {
        if (wanted.fixedSize && wanted.fixedSize != type.fixedSize) return false;
        const auto size = list->items.size() - std::min(list->frontIndex, list->items.size());
        if (size != *type.fixedSize) return false;
        wanted.fixedSize = type.fixedSize;
    }
    if (prior && sameContract(*prior, wanted)) return true;
    if (!prior && !wanted.fixedSize && std::all_of(wanted.arguments.begin(), wanted.arguments.end(), unconstrained)) return true;

    // Publish into the *plan* before descending, both to terminate cycles and
    // to detect conflicting contracts reached through two aliases. Nothing is
    // attached to the heap until the entire boundary succeeds.
    pending_.push_back({&slot, std::make_shared<const NativeContainerType>(wanted)});
    if (map) {
        for (const auto& entry : dictionary->entries)
            if (!matches(entry.first, wanted.arguments[0]) || !matches(entry.second, wanted.arguments[1])) return false;
    } else {
        for (std::size_t i = std::min(list->frontIndex, list->items.size()); i < list->items.size(); ++i)
            if (!matches(list->items[i], wanted.arguments[0])) return false;
    }
    return true;
}

void RuntimeTypeCheck::listWrite(ListRef list, const Value* inserted, std::size_t resultingSize) {
    const auto type = proposed(list->storageType);
    if (!type) return;
    if (type->fixedSize && resultingSize != *type->fixedSize)
        throw std::runtime_error("container write would resize array[" + std::to_string(*type->fixedSize) + "]");
    if (inserted) require(*inserted, describeTypeName(type->arguments.front()));
}

void RuntimeTypeCheck::mapWrite(MapRef map, const Value& key, const Value& value) {
    const auto type = proposed(map->storageType);
    if (!type) return;
    require(key, describeTypeName(type->arguments[0]));
    require(value, describeTypeName(type->arguments[1]));
}

void RuntimeTypeCheck::commit() noexcept {
    for (auto& entry : pending_) *entry.slot = std::move(entry.type);
    pending_.clear();
}

std::string runtimeFieldType(const Chunk& chunk, const std::string& className,
                             const std::string& fieldName, const ObjectBox* receiver) {
    const auto cls = chunk.classReflection.find(className);
    if (cls == chunk.classReflection.end()) throw std::runtime_error("VM: missing class metadata for " + className);
    const auto& fields = cls->second.fields;
    const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return f.name == fieldName; });
    if (field == fields.end()) throw std::runtime_error("VM: unknown field '" + className + "." + fieldName + "'");
    if (field->isStatic == (receiver != nullptr)) throw std::runtime_error("VM: invalid static/instance field access");
    if (!receiver) return field->typeName;
    return substituteTypeParams(field->typeName, receiverTypeBindings(*receiver, chunk, field->ownerClassName));
}
} // namespace zl
