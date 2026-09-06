#include "zl/compiler/semantic_model.hpp"
#include <algorithm>

namespace zl {

namespace {
bool sameSignature(const ClassMethodInfo& a, const ClassMethodInfo& b) {
    return a.returnType == b.returnType && a.returnClassName == b.returnClassName &&
           a.paramTypes == b.paramTypes;
}

std::string methodParamSignatureKey(const ClassMethodInfo& method) {
    std::string s = "(";
    for (std::size_t i = 0; i < method.paramTypes.size(); ++i) {
        if (i) s += ",";
        const bool generic = i < method.paramIsGeneric.size() && method.paramIsGeneric[i];
        if (generic) {
            s += "<generic:";
            s += (i < method.paramClassNames.size() ? method.paramClassNames[i] : "?");
            s += ">";
        } else if (method.paramTypes[i] == ZlType::OBJECT &&
                   i < method.paramClassNames.size() && !method.paramClassNames[i].empty()) {
            s += "object:" + method.paramClassNames[i];
        } else {
            s += zlTypeName(method.paramTypes[i]);
        }
    }
    s += ")";
    return s;
}

std::string paramTypesKey(const std::vector<ZlType>& paramTypes) {
    std::string s = "(";
    for (std::size_t i = 0; i < paramTypes.size(); ++i) {
        if (i) s += ",";
        s += zlTypeName(paramTypes[i]);
    }
    s += ")";
    return s;
}
}

const ClassShapeInfo* SemanticModel::findClass(const std::string& name) const {
    auto it = classes_.find(name);
    return it == classes_.end() ? nullptr : &it->second;
}

const InterfaceShapeInfo* SemanticModel::findInterface(const std::string& name) const {
    auto it = interfaces_.find(name);
    return it == interfaces_.end() ? nullptr : &it->second;
}

const std::string& SemanticModel::parentClassName(const std::string& name) const {
    return classes_.at(name).parentName;
}

void SemanticModel::setParentClass(const std::string& name, std::string parentName,
                                   std::vector<TypeAnnotation> parentTypeArgs) {
    auto& shape = classes_.at(name);
    shape.parentName = std::move(parentName);
    shape.parentTypeArgs = std::move(parentTypeArgs);
}

void SemanticModel::markNumericGenericTypeParam(const std::string& className, const std::string& typeParam) {
    auto& shape = classes_.at(className);
    shape.numericTypeParams.insert(typeParam);
}

const ClassFieldInfo* SemanticModel::findFieldInHierarchy(const std::string& className,
                                                           const std::string& fieldName,
                                                           std::string* outOwner) const {
    std::string cur = className;
    while (!cur.empty()) {
        const auto* shape = findClass(cur);
        if (!shape) return nullptr;
        auto fieldIt = shape->fields.find(fieldName);
        if (fieldIt != shape->fields.end()) {
            if (outOwner) *outOwner = cur;
            return &fieldIt->second;
        }
        cur = shape->parentName;
    }
    return nullptr;
}

std::vector<std::pair<std::string, ClassMethodInfo>>
SemanticModel::collectMethodOverloads(const std::string& className, const std::string& methodName) const {
    std::unordered_map<std::string, std::pair<std::string, ClassMethodInfo>> bySignature;
    std::string cur = className;
    while (!cur.empty()) {
        const auto* shape = findClass(cur);
        if (!shape) break;
        auto methodIt = shape->methods.find(methodName);
        if (methodIt != shape->methods.end()) {
            for (const auto& overload : methodIt->second) {
                bySignature.emplace(methodParamSignatureKey(overload), std::make_pair(cur, overload));
            }
        }
        cur = shape->parentName;
    }

    std::vector<std::pair<std::string, ClassMethodInfo>> result;
    result.reserve(bySignature.size());
    for (const auto& [signature, ownerAndInfo] : bySignature) {
        (void)signature;
        result.push_back(ownerAndInfo);
    }

    // `bySignature` is an unordered_map, so iteration order must never leak
    // into overload resolution or diagnostics. Keep candidates deterministic
    // by ordering first by canonical parameter signature and then owner.
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        const std::string aKey = methodParamSignatureKey(a.second);
        const std::string bKey = methodParamSignatureKey(b.second);
        if (aKey != bKey) return aKey < bKey;
        return a.first < b.first;
    });
    return result;
}

const ClassMethodInfo* SemanticModel::findExactMethodInHierarchy(const std::string& className,
                                                                  const std::string& methodName,
                                                                  const std::vector<ZlType>& paramTypes,
                                                                  std::string* outOwner) const {
    const std::string wanted = paramTypesKey(paramTypes);
    std::string cur = className;
    while (!cur.empty()) {
        const auto* shape = findClass(cur);
        if (!shape) return nullptr;
        auto methodIt = shape->methods.find(methodName);
        if (methodIt != shape->methods.end()) {
            for (const auto& overload : methodIt->second) {
                if (paramTypesKey(overload.paramTypes) == wanted) {
                    if (outOwner) *outOwner = cur;
                    return &overload;
                }
            }
        }
        cur = shape->parentName;
    }
    return nullptr;
}

bool SemanticModel::isSubclassOf(const std::string& className, const std::string& ancestorName) const {
    std::string cur = className;
    while (!cur.empty()) {
        if (cur == ancestorName) return true;
        const auto* shape = findClass(cur);
        if (!shape) return false;
        cur = shape->parentName;
    }
    return false;
}

bool SemanticModel::interfaceExtends(const std::string& interfaceName, const std::string& ancestorName) const {
    if (interfaceName == ancestorName) return true;
    const auto* shape = findInterface(interfaceName);
    if (!shape) return false;
    std::vector<std::string> stack = shape->extendsNames;
    std::unordered_set<std::string> seen;
    while (!stack.empty()) {
        std::string cur = std::move(stack.back());
        stack.pop_back();
        if (!seen.insert(cur).second) continue;
        if (cur == ancestorName) return true;
        const auto* parent = findInterface(cur);
        if (parent) {
            stack.insert(stack.end(), parent->extendsNames.begin(), parent->extendsNames.end());
        }
    }
    return false;
}

bool SemanticModel::implementsInterface(const std::string& className, const std::string& interfaceName) const {
    if (className == interfaceName) return true;
    if (hasInterface(className)) return interfaceExtends(className, interfaceName);

    std::unordered_set<std::string> seenClasses;
    std::string cur = className;
    while (!cur.empty() && seenClasses.insert(cur).second) {
        const auto* shape = findClass(cur);
        if (!shape) return false;
        for (const auto& iface : shape->implementsNames) {
            if (iface == interfaceName || interfaceExtends(iface, interfaceName)) return true;
        }
        cur = shape->parentName;
    }
    return false;
}

const InterfaceShapeInfo* SemanticModel::resolveInterfaceMethods(const std::string& name) {
    auto it = interfaces_.find(name);
    if (it == interfaces_.end()) return nullptr;

    InterfaceShapeInfo& info = it->second;
    if (info.methodsResolved) return &info;
    if (info.visiting) {
        throw TypeCheckError("type error (line " + std::to_string(info.line) + "): circular interface inheritance detected involving '" + name + "'");
    }

    info.visiting = true;
    std::unordered_map<std::string, ClassMethodInfo> merged;
    std::unordered_map<std::string, std::string> mergedFrom;

    for (const auto& parentName : info.extendsNames) {
        const InterfaceShapeInfo* parent = resolveInterfaceMethods(parentName);
        if (!parent) continue;
        for (const auto& [methodName, sig] : parent->methods) {
            auto existing = merged.find(methodName);
            if (existing == merged.end()) {
                merged.emplace(methodName, sig);
                mergedFrom[methodName] = parentName;
            } else if (!sameSignature(existing->second, sig)) {
                throw TypeCheckError(
                    "type error (line " + std::to_string(info.line) + "): interface '" + name +
                    "' inherits conflicting signatures for '" + methodName + "' from '" +
                    mergedFrom[methodName] + "' and '" + parentName + "'"
                );
            }
        }
    }

    for (const auto& [methodName, sig] : info.ownMethods) {
        merged[methodName] = sig;
    }

    info.methods = std::move(merged);
    info.methodsResolved = true;
    info.visiting = false;
    return &info;
}

void SemanticModel::defineClass(ClassShapeInfo shape) {
    classes_[shape.name] = std::move(shape);
}

void SemanticModel::defineInterface(InterfaceShapeInfo shape) {
    interfaces_[shape.name] = std::move(shape);
}

void SemanticModel::clear() {
    classes_.clear();
    interfaces_.clear();
}

} // namespace zl
