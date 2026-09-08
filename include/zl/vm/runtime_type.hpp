#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "zl/common/ownership.hpp"

namespace zl {

using RuntimeTypeId = std::uint32_t;
using RuntimeTypeBindings = std::unordered_map<std::string, std::string>;

// Durable runtime metadata for a user-defined type. The object graph owns a
// shared instance rather than copying reflection strings into each object.
// Richer field/method signatures can be added here as reflection grows.
struct RuntimeFieldInfo {
    std::string name;
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
};

using RuntimeTypeRef = std::shared_ptr<const RuntimeTypeInfo>;

} // namespace zl
