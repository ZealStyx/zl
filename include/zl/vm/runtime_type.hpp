#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "zl/common/ownership.hpp"
#include "zl/common/type_name.hpp"

namespace zl {

using RuntimeTypeId = std::uint32_t;
using RuntimeTypeBindings = std::unordered_map<std::string, std::string>;

// Durable runtime metadata for a user-defined type. The object graph owns a
// shared instance rather than copying reflection strings into each object.
// Richer field/method signatures can be added here as reflection grows.
// A raw native container acquires an immutable contract at a typed boundary.
// Lists/sets/arrays share an element contract; fixed arrays additionally freeze
// the logical length. Wildcard arguments do not erase an existing contract.
struct NativeContainerType {
    std::vector<TypeName> arguments;
    std::optional<std::size_t> fixedSize;
};
using NativeContainerTypeRef = std::shared_ptr<const NativeContainerType>;

struct RuntimeFieldInfo {
    std::string name;
    std::string ownerClassName;
    std::string typeName;
    std::string access;
    bool isStatic{false};
    OwnershipKind ownership{OwnershipKind::GC};
};

struct RuntimeMethodInfo {
    std::string name;
    std::string ownerClassName;
    std::vector<std::string> parameterTypes;
    std::string dispatchSignature;
    std::string returnType;
    std::string access;
    bool isStatic{false};
    bool isAsync{false};
    std::size_t functionIndex{static_cast<std::size_t>(-1)};
};

struct RuntimeConstructorInfo {
    std::string ownerClassName;
    std::vector<std::string> parameterTypes;
    std::string dispatchSignature;
    std::size_t functionIndex{static_cast<std::size_t>(-1)};
    std::string access{"public"};
};

struct RuntimeTypeInfo {
    RuntimeTypeId id{0};
    std::string name;
    std::string baseClassName;
    std::vector<std::string> interfaces;
    std::vector<std::string> typeParameters;
    bool isDataType{false};
    bool isEnumType{false};
    std::vector<std::string> enumMembers;
    std::vector<RuntimeFieldInfo> fields;
    std::vector<RuntimeMethodInfo> methods;
    std::vector<RuntimeConstructorInfo> constructors;

    // Flat field storage layout (Phase 0, memory-domains.md §9). An instance's
    // declared fields live in ObjectBox::fields at these slots: slot i is the
    // i-th distinct non-static field name in `fields` order (base classes
    // first - the merge order both compiler pipelines build). Static fields
    // are excluded: they live in chunk static storage, not per-instance.
    // Build with buildRuntimeFieldIndex() BEFORE the type is shared as
    // RuntimeTypeRef; every consumer (VM GetField/SetField, printing, tracing,
    // structural equality, the natives) reads the layout through this one
    // table, so both pipelines agree by construction.
    std::size_t instanceFieldCount{0};
    std::vector<std::string> instanceFieldNames;
    std::unordered_map<std::string, std::size_t> fieldIndex;
};

using RuntimeTypeRef = std::shared_ptr<const RuntimeTypeInfo>;

// Fill the flat-layout index from `type.fields`. Idempotent and cheap
// (one pass over the field list); call it once at metadata construction.
inline void buildRuntimeFieldIndex(RuntimeTypeInfo& type) {
    type.instanceFieldCount = 0;
    type.instanceFieldNames.clear();
    type.fieldIndex.clear();
    for (const auto& field : type.fields) {
        if (field.isStatic) continue;
        if (type.fieldIndex.count(field.name)) continue; // first occurrence wins
        type.fieldIndex.emplace(field.name, type.instanceFieldCount);
        type.instanceFieldNames.push_back(field.name);
        ++type.instanceFieldCount;
    }
}

} // namespace zl
