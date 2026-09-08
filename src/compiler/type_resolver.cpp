#include "zl/compiler/type_resolver.hpp"
#include "zl/common/type_annotation.hpp"
#include "zl/common/type_name.hpp"

#include <algorithm>
#include <functional>

namespace zl {
namespace {
[[noreturn]] void typeError(const std::string& message, std::size_t line) {
    throw TypeCheckError("type error (line " + std::to_string(line) + "): " + message);
}
}

void TypeResolver::reset() {
    genericInstantiationCache_.clear();
    genericInstantiationByName_.clear();
    unions_.clear();
}

const std::vector<ResolvedTypeArg>& TypeResolver::unionMembers(const std::string& name) const {
    return unions_.at(name);
}

ResolvedTypeArg TypeResolver::makeUnion(std::vector<ResolvedTypeArg> members) {
    std::vector<ResolvedTypeArg> flattened;
    for (const auto& member : members) {
        if (member.type == ZlType::UNION) {
            const auto& nested = unionMembers(member.className);
            flattened.insert(flattened.end(), nested.begin(), nested.end());
        } else flattened.push_back(member);
    }
    const auto nameOf = [](const ResolvedTypeArg& member) {
        auto name = member.className.empty() ? zlTypeName(member.type) : member.className;
        if (member.type == ZlType::FUNCTION && name != "func") name = "(" + name + ")";
        return name;
    };
    std::sort(flattened.begin(), flattened.end(), [&](const auto& a, const auto& b) { return nameOf(a) < nameOf(b); });
    flattened.erase(std::unique(flattened.begin(), flattened.end()), flattened.end());
    std::string name;
    for (const auto& member : flattened) { if (!name.empty()) name += "|"; name += nameOf(member); }
    unions_[name] = std::move(flattened);
    return {ZlType::UNION, name};
}

ZlType TypeResolver::resolveType(const TypeAnnotation& annotation,
                                 const std::string& currentClassName,
                                 const std::vector<std::string>& currentClassTypeParams,
                                 std::string* outClassName) {
    if (!annotation.unionOf.empty()) {
        std::vector<ResolvedTypeArg> members;
        for (const auto& member : annotation.unionOf) {
            std::string name;
            auto type = resolveType(member, currentClassName, currentClassTypeParams, &name);
            members.push_back({type, std::move(name)});
        }
        const auto result = makeUnion(std::move(members));
        if (outClassName) *outClassName = result.className;
        return result.type;
    }
    const std::string& n = annotation.name;
    if (n == "nil" || n == "null") return ZlType::NIL;
    if (n == "int") return ZlType::INT;
    if (n == "double") return ZlType::DOUBLE;
    if (n == "float") return ZlType::DOUBLE;
    if (n == "string") return ZlType::STRING;
    if (n == "bool") return ZlType::BOOL;
    if (n == "unknown") return ZlType::UNKNOWN;
    if (n == "void") return ZlType::VOID_TYPE;
    if (n == "list" || n == "array" || n == "set" || n == "map") {
        // Bare collection types remain valid as runtime/untyped collection
        // slots (the builtin classes use these for their native backing fields).
        // Only an explicitly parameterized collection gets a canonical generic
        // identity for type-safe assignability.
        if (annotation.typeArgs.empty()) {
            return n == "list" ? ZlType::LIST
                 : n == "array" ? ZlType::ARRAY
                 : n == "set" ? ZlType::SET
                 : ZlType::MAP;
        }
        const std::size_t required = (n == "map") ? 2u : 1u;
        if (annotation.typeArgs.size() != required) {
            typeError(n + " requires " + std::to_string(required) + " type argument(s)", annotation.line);
        }
        std::vector<std::string> resolved;
        resolved.reserve(annotation.typeArgs.size());
        for (const auto& arg : annotation.typeArgs) {
            std::string argClass;
            const ZlType argType = resolveType(arg, currentClassName, currentClassTypeParams, &argClass);
            std::string rendered = argClass;
            if (rendered.empty()) {
                switch (argType) {
                    case ZlType::INT: rendered = "int"; break;
                    case ZlType::DOUBLE: rendered = "double"; break;
                    case ZlType::STRING: rendered = "string"; break;
                    case ZlType::BOOL: rendered = "bool"; break;
                    case ZlType::NIL: rendered = "nil"; break;
                    default: rendered = zlTypeName(argType); break;
                }
            }
            resolved.push_back(std::move(rendered));
        }
        if (outClassName) {
            *outClassName = n + "<";
            for (std::size_t i = 0; i < resolved.size(); ++i) {
                if (i) *outClassName += ",";
                *outClassName += resolved[i];
            }
            *outClassName += ">";
            if (n == "array" && annotation.fixedSize) {
                *outClassName = "array[" + std::to_string(*annotation.fixedSize) + "]" + outClassName->substr(5);
            }
        }
        return n == "list" ? ZlType::LIST
             : n == "array" ? ZlType::ARRAY
             : n == "set" ? ZlType::SET
             : ZlType::MAP;
    }
    if (n == "func") {
        if (outClassName && annotation.functionHasSignature) *outClassName = describeTypeAnnotation(annotation);
        return ZlType::FUNCTION;
    }
    if (n == "Task") {
        if (annotation.typeArgs.size() != 1) {
            typeError("Task requires exactly one type argument, for example Task<int>", annotation.line);
        }
        std::string valueClassName;
        const ZlType valueType = resolveType(annotation.typeArgs[0], currentClassName, currentClassTypeParams, &valueClassName);
        std::string valueName = valueClassName;
        if (valueName.empty()) valueName = zlTypeName(valueType);
        if (outClassName) *outClassName = "Task<" + valueName + ">";
        return ZlType::TASK;
    }

    for (const std::string& param : currentClassTypeParams) {
        if (n == param) {
            if (outClassName) *outClassName = n;
            return ZlType::OBJECT;
        }
    }

    const auto* classIt = semanticModel_.findClass(n);
    if (classIt != nullptr && !classIt->typeParams.empty() && !annotation.typeArgs.empty()) {
        const bool selfGenericReference = n == classIt->name &&
            annotation.typeArgs.size() == classIt->typeParams.size() &&
            (currentClassName.empty() || !currentClassTypeParams.empty());
        if (selfGenericReference) {
            bool allOwnParams = true;
            GenericInstantiation deferred{n, {}};
            deferred.args.reserve(annotation.typeArgs.size());
            for (const auto& arg : annotation.typeArgs) {
                if (std::find(currentClassTypeParams.begin(), currentClassTypeParams.end(), arg.name) !=
                    currentClassTypeParams.end()) {
                    deferred.args.push_back({ZlType::OBJECT, arg.name});
                    continue;
                }
                allOwnParams = false;
                if (arg.name == "int") deferred.args.push_back({ZlType::INT, ""});
                else if (arg.name == "double" || arg.name == "float") deferred.args.push_back({ZlType::DOUBLE, ""});
                else if (arg.name == "string") deferred.args.push_back({ZlType::STRING, ""});
                else if (arg.name == "bool") deferred.args.push_back({ZlType::BOOL, ""});
                else deferred.args.push_back({ZlType::OBJECT, arg.name});
            }
            if (allOwnParams || currentClassName.empty()) {
                const std::string key = deferred.describe();
                genericInstantiationByName_[key] = deferred;
                if (outClassName) *outClassName = key;
                return ZlType::OBJECT;
            }
        }

        std::vector<ResolvedTypeArg> resolvedArgs;
        resolvedArgs.reserve(annotation.typeArgs.size());
        for (const auto& argAnnotation : annotation.typeArgs) {
            std::string argClassName;
            ZlType argType = resolveType(argAnnotation, currentClassName, currentClassTypeParams, &argClassName);
            ResolvedTypeArg arg{argType, argClassName};
            if (argType == ZlType::FUNCTION && argAnnotation.functionHasSignature) {
                arg.functionHasSignature = true;
                for (const auto& paramAnnotation : argAnnotation.functionParamTypes) {
                    std::string paramClass;
                    ZlType paramType = resolveType(paramAnnotation, currentClassName, currentClassTypeParams, &paramClass);
                    arg.functionParamTypes.push_back(ResolvedTypeArg{paramType, paramClass});
                }
                if (argAnnotation.functionReturnType) {
                    std::string returnClass;
                    ZlType returnType = resolveType(*argAnnotation.functionReturnType, currentClassName, currentClassTypeParams, &returnClass);
                    arg.functionReturnType = std::make_shared<ResolvedTypeArg>(ResolvedTypeArg{returnType, returnClass});
                }
            }
            resolvedArgs.push_back(std::move(arg));
        }
        std::string syntheticKey = instantiateGenericClass(n, resolvedArgs, annotation.line);
        if (outClassName) *outClassName = syntheticKey;
        return ZlType::OBJECT;
    }

    if (classIt != nullptr) {
        if (!classIt->typeParams.empty()) {
            typeError("generic type '" + n + "' requires " + std::to_string(classIt->typeParams.size()) +
                      " type argument(s), for example '" + n + "<...>'", annotation.line);
        }
        if (outClassName) *outClassName = n;
        return ZlType::OBJECT;
    }

    if (semanticModel_.hasInterface(n)) {
        if (outClassName) *outClassName = n;
        return ZlType::OBJECT;
    }
    return ZlType::UNKNOWN;
}

ClassFieldInfo TypeResolver::fieldInContext(const ClassFieldInfo& field,
        const std::string& receiverClass, const std::string& declaringClass,
        const std::string& currentClass, const std::vector<std::string>& currentTypeParams) {
    // A template body still has a raw `this` class. Walk declared parent
    // arguments without instantiating a fictitious concrete receiver (which
    // would incorrectly validate symbolic parameters as concrete arguments).
    std::unordered_map<std::string, std::string> bindings;
    std::string owner = receiverClass;
    while (owner != declaringClass) {
        const auto* shape = semanticModel_.findClass(owner);
        if (!shape || shape->parentName.empty()) break;
        const auto* parent = semanticModel_.findClass(shape->parentName);
        if (!parent) break;
        std::unordered_map<std::string, std::string> next;
        for (std::size_t i = 0; i < shape->parentTypeArgs.size() && i < parent->typeParams.size(); ++i)
            next.emplace(parent->typeParams[i], substituteTypeParams(describeTypeAnnotation(shape->parentTypeArgs[i]), bindings));
        bindings = std::move(next);
        owner = shape->parentName;
    }
    if (bindings.empty()) return field;
    auto result = field;
    const auto substitute = [&](ZlType& type, std::string& name) {
        const auto original = name.empty() ? zlTypeName(type) : name;
        const auto resolved = substituteTypeParams(original, bindings);
        if (resolved == original) return;
        name.clear();
        type = resolveType(typeAnnotationFromName(parseTypeName(resolved)), currentClass, currentTypeParams, &name);
    };
    substitute(result.type, result.className);
    for (std::size_t i = 0; i < result.functionParamTypes.size(); ++i)
        substitute(result.functionParamTypes[i], result.functionParamClassNames[i]);
    substitute(result.functionReturnType, result.functionReturnClassName);
    return result;
}

std::string TypeResolver::instantiateGenericClass(const std::string& genericName,
                                                   const std::vector<ResolvedTypeArg>& typeArgs,
                                                   std::size_t line) {
    const auto* genericIt = semanticModel_.findClass(genericName);
    if (genericIt == nullptr) typeError("unknown generic class '" + genericName + "'", line);
    const ClassShapeInfo& generic = *genericIt;
    if (typeArgs.size() != generic.typeParams.size()) {
        typeError("class '" + genericName + "' takes " + std::to_string(generic.typeParams.size()) +
                  " type argument(s), got " + std::to_string(typeArgs.size()), line);
    }

    GenericInstantiation identity{genericName, typeArgs};
    if (auto cached = genericInstantiationCache_.find(identity); cached != genericInstantiationCache_.end()) {
        return cached->second;
    }
    const std::string key = identity.describe();
    if (semanticModel_.hasClass(key)) {
        genericInstantiationCache_.emplace(identity, key);
        genericInstantiationByName_[key] = identity;
        return key;
    }
    genericInstantiationCache_.emplace(identity, key);
    genericInstantiationByName_[key] = identity;

    const auto canonicalTypeName = [](const ResolvedTypeArg& value) {
        return value.className.empty() ? zlTypeName(value.type) : value.className;
    };

    std::unordered_map<std::string, std::string> bindings;
    for (std::size_t i = 0; i < generic.typeParams.size(); ++i) bindings.emplace(generic.typeParams[i], canonicalTypeName(typeArgs[i]));

    std::function<ResolvedTypeArg(const TypeName&)> resolveCanonicalType;
    resolveCanonicalType = [&](const TypeName& name) -> ResolvedTypeArg {
        if (!name.unionMembers.empty()) {
            std::vector<ResolvedTypeArg> members;
            for (const auto& member : name.unionMembers) members.push_back(resolveCanonicalType(member));
            return makeUnion(std::move(members));
        }
        auto type = zlTypeFromBaseName(name.name);
        const auto rendered = describeTypeName(name);
        if (type == ZlType::FUNCTION) {
            ResolvedTypeArg result{type, name.args.empty() ? "" : rendered};
            if (!name.args.empty()) {
                result.functionHasSignature = true;
                for (std::size_t i = 0; i + 1 < name.args.size(); ++i) result.functionParamTypes.push_back(resolveCanonicalType(name.args[i]));
                result.functionReturnType = std::make_shared<ResolvedTypeArg>(resolveCanonicalType(name.args.back()));
            }
            return result;
        }
        if (type == ZlType::OBJECT && !name.args.empty()) {
            const auto* shape = semanticModel_.findClass(name.name);
            if (shape && !shape->typeParams.empty()) {
                std::vector<ResolvedTypeArg> args;
                for (const auto& arg : name.args) args.push_back(resolveCanonicalType(arg));
                return {type, instantiateGenericClass(name.name, args, line)};
            }
        }
        return {type, (type == ZlType::OBJECT || !name.args.empty()) ? rendered : std::string{}};
    };

    const auto substitute = [&](ZlType type, const std::string& className) -> ResolvedTypeArg {
        if (type == ZlType::OBJECT) {
            const auto own = std::find(generic.typeParams.begin(), generic.typeParams.end(), className);
            if (own != generic.typeParams.end()) return typeArgs[static_cast<std::size_t>(own - generic.typeParams.begin())];
        }
        if (className.empty()) return {type, className};
        const auto resolvedName = substituteTypeParams(className, bindings);
        if (resolvedName == className) return {type, className};
        return resolveCanonicalType(parseTypeName(resolvedName));
    };

    auto substituteCallable = [&](std::vector<ZlType>& paramTypes,
                                  std::vector<std::string>& paramClassNames,
                                  ZlType& returnType,
                                  std::string& returnClassName,
                                  bool hasSignature) {
        if (!hasSignature) return;
        const std::size_t paramCount = paramTypes.size();
        paramClassNames.resize(paramCount);
        for (std::size_t i = 0; i < paramCount; ++i) {
            auto resolvedParam = substitute(paramTypes[i], paramClassNames[i]);
            paramTypes[i] = resolvedParam.type;
            paramClassNames[i] = resolvedParam.className;
        }
        auto resolvedReturn = substitute(returnType, returnClassName);
        returnType = resolvedReturn.type;
        returnClassName = resolvedReturn.className;
    };

    ClassShapeInfo instantiated;
    instantiated.name = key;
    instantiated.parentName = generic.parentName;
    instantiated.implementsNames = generic.implementsNames;
    instantiated.typeParamInterfaceConstraints = generic.typeParamInterfaceConstraints;
    instantiated.isDeprecated = generic.isDeprecated;

    for (const auto& [constrainedParam, interfaces] : generic.typeParamInterfaceConstraints) {
        auto pos = std::find(generic.typeParams.begin(), generic.typeParams.end(), constrainedParam);
        if (pos == generic.typeParams.end()) continue;
        const std::size_t index = static_cast<std::size_t>(pos - generic.typeParams.begin());
        if (index >= typeArgs.size()) continue;
        const auto& actual = typeArgs[index];
        if (actual.type != ZlType::OBJECT || actual.className.empty()) {
            typeError("generic type parameter '" + constrainedParam + "' of class '" + genericName + "' must implement its interface constraint(s)", line);
        }
        for (const auto& iface : interfaces) {
            if (!semanticModel_.implementsInterface(actual.className, iface)) {
                typeError("generic type parameter '" + constrainedParam + "' of class '" + genericName + "' requires interface '" + iface + "', got '" + actual.className + "'", line);
            }
        }
    }

    for (const std::string& constrainedParam : generic.numericTypeParams) {
        auto pos = std::find(generic.typeParams.begin(), generic.typeParams.end(), constrainedParam);
        if (pos == generic.typeParams.end()) continue;
        const std::size_t index = static_cast<std::size_t>(pos - generic.typeParams.begin());
        if (index >= typeArgs.size()) continue;
        if (typeArgs[index].type != ZlType::INT && typeArgs[index].type != ZlType::DOUBLE) {
            typeError("generic type parameter '" + constrainedParam + "' of class '" + genericName +
                      "' must be numeric (int or double), got " + zlTypeName(typeArgs[index].type), line);
        }
    }

    if (!generic.parentTypeArgs.empty()) {
        std::vector<ResolvedTypeArg> parentArgs;
        parentArgs.reserve(generic.parentTypeArgs.size());
        for (const auto& argAnnotation : generic.parentTypeArgs) {
            std::string argClassName;
            ZlType argType = resolveType(argAnnotation, generic.name, generic.typeParams, &argClassName);
            auto substitutedArg = substitute(argType, argClassName);
            parentArgs.push_back(substitutedArg);
        }
        instantiated.parentName = instantiateGenericClass(generic.parentName, parentArgs, line);
    }

    for (const auto& [fieldName, field] : generic.fields) {
        auto substitutedFieldType = substitute(field.type, field.className);
        const ZlType substType = substitutedFieldType.type;
        const std::string& substClassName = substitutedFieldType.className;
        ClassFieldInfo substitutedField{substType, substClassName, field.access};
        substitutedField.functionParamTypes = field.functionParamTypes;
        substitutedField.functionParamClassNames = field.functionParamClassNames;
        substitutedField.functionReturnType = field.functionReturnType;
        substitutedField.functionReturnClassName = field.functionReturnClassName;
        substitutedField.functionHasSignature = field.functionHasSignature;
        substituteCallable(substitutedField.functionParamTypes,
                           substitutedField.functionParamClassNames,
                           substitutedField.functionReturnType,
                           substitutedField.functionReturnClassName,
                           substitutedField.functionHasSignature);
        if (substType == ZlType::FUNCTION && substitutedField.functionParamTypes.empty() &&
            !field.functionHasSignature) {
            for (const auto& candidate : typeArgs) {
                if (candidate.type != ZlType::FUNCTION || !candidate.functionHasSignature) continue;
                substitutedField.functionHasSignature = true;
                for (const auto& param : candidate.functionParamTypes) {
                    substitutedField.functionParamTypes.push_back(param.type);
                    substitutedField.functionParamClassNames.push_back(param.className);
                }
                if (candidate.functionReturnType) {
                    substitutedField.functionReturnType = candidate.functionReturnType->type;
                    substitutedField.functionReturnClassName = candidate.functionReturnType->className;
                }
                break;
            }
        }
        instantiated.fields[fieldName] = std::move(substitutedField);
        instantiated.fieldOrder.push_back(fieldName);
    }
    for (const auto& [methodName, overloads] : generic.methods) {
        std::vector<ClassMethodInfo> substituted;
        substituted.reserve(overloads.size());
        for (const auto& overload : overloads) {
            ClassMethodInfo m = overload;
            auto substitutedReturn = substitute(overload.returnType, overload.returnClassName);
            const ZlType retType = substitutedReturn.type;
            const std::string& retClassName = substitutedReturn.className;
            m.returnType = retType;
            m.returnClassName = retClassName;
            if (retType == ZlType::FUNCTION && !m.returnFunctionHasSignature) {
                for (const auto& candidate : typeArgs) {
                    if (candidate.type != ZlType::FUNCTION || !candidate.functionHasSignature) continue;
                    m.returnFunctionHasSignature = true;
                    for (const auto& param : candidate.functionParamTypes) {
                        m.returnFunctionParamTypes.push_back(param.type);
                        m.returnFunctionParamClassNames.push_back(param.className);
                    }
                    if (candidate.functionReturnType) {
                        m.returnFunctionReturnType = candidate.functionReturnType->type;
                        m.returnFunctionReturnClassName = candidate.functionReturnType->className;
                    }
                    break;
                }
            }
            substituteCallable(m.returnFunctionParamTypes,
                               m.returnFunctionParamClassNames,
                               m.returnFunctionReturnType,
                               m.returnFunctionReturnClassName,
                               m.returnFunctionHasSignature);
            for (std::size_t i = 0; i < m.paramTypes.size(); ++i) {
                auto substitutedParam = substitute(m.paramTypes[i], m.paramClassNames[i]);
                const ZlType pType = substitutedParam.type;
                const std::string& pClassName = substitutedParam.className;
                m.paramTypes[i] = pType;
                m.paramClassNames[i] = pClassName;
                if (pType == ZlType::FUNCTION &&
                    (i >= m.functionParamTypes.size() || m.functionParamTypes[i].empty())) {
                    for (const auto& candidate : typeArgs) {
                        if (candidate.type != ZlType::FUNCTION || !candidate.functionHasSignature) continue;
                        if (i >= m.functionParamTypes.size()) {
                            m.functionParamTypes.resize(m.paramTypes.size());
                            m.functionParamClassNames.resize(m.paramTypes.size());
                            m.functionReturnTypes.resize(m.paramTypes.size(), ZlType::UNKNOWN);
                            m.functionReturnClassNames.resize(m.paramTypes.size());
                        }
                        for (const auto& param : candidate.functionParamTypes) {
                            m.functionParamTypes[i].push_back(param.type);
                            m.functionParamClassNames[i].push_back(param.className);
                        }
                        m.functionReturnTypes[i] = candidate.functionReturnType ? candidate.functionReturnType->type : ZlType::UNKNOWN;
                        m.functionReturnClassNames[i] = candidate.functionReturnType ? candidate.functionReturnType->className : std::string();
                        if (m.functionParamHasSignature.size() < m.paramTypes.size()) m.functionParamHasSignature.resize(m.paramTypes.size(), false);
                        m.functionParamHasSignature[i] = true;
                        break;
                    }
                }
                if (i < m.functionParamTypes.size() && i < m.functionParamHasSignature.size() &&
                    m.functionParamHasSignature[i]) {
                    substituteCallable(m.functionParamTypes[i],
                                       m.functionParamClassNames[i],
                                       m.functionReturnTypes[i],
                                       m.functionReturnClassNames[i],
                                       true);
                }
            }
            substituted.push_back(std::move(m));
        }
        instantiated.methods[methodName] = std::move(substituted);
    }
    for (const auto& ctor : generic.constructors) {
        ClassMethodInfo m = ctor;
        auto substitutedReturn = substitute(ctor.returnType, ctor.returnClassName);
        const ZlType retType = substitutedReturn.type;
        const std::string& retClassName = substitutedReturn.className;
        m.returnType = retType;
        m.returnClassName = retClassName;
        substituteCallable(m.returnFunctionParamTypes,
                           m.returnFunctionParamClassNames,
                           m.returnFunctionReturnType,
                           m.returnFunctionReturnClassName,
                           m.returnFunctionHasSignature);
        for (std::size_t i = 0; i < m.paramTypes.size(); ++i) {
            auto substitutedParam = substitute(m.paramTypes[i], m.paramClassNames[i]);
            const ZlType pType = substitutedParam.type;
            const std::string& pClassName = substitutedParam.className;
            m.paramTypes[i] = pType;
            m.paramClassNames[i] = pClassName;
            if (i < m.functionParamTypes.size() && i < m.functionParamHasSignature.size() &&
                m.functionParamHasSignature[i]) {
                substituteCallable(m.functionParamTypes[i],
                                   m.functionParamClassNames[i],
                                   m.functionReturnTypes[i],
                                   m.functionReturnClassNames[i],
                                   true);
            }
        }
        instantiated.constructors.push_back(std::move(m));
    }

    semanticModel_.defineClass(std::move(instantiated));
    return key;
}

bool TypeResolver::isCurrentGenericTypeParam(const std::string& name,
                                              const std::vector<std::string>& currentClassTypeParams) const {
    return !name.empty() &&
           std::find(currentClassTypeParams.begin(), currentClassTypeParams.end(), name) != currentClassTypeParams.end();
}

void TypeResolver::requireNumericGenericTypeParam(const std::string& name,
                                                  const std::string& currentClassName,
                                                  const std::vector<std::string>& currentClassTypeParams) {
    if (!isCurrentGenericTypeParam(name, currentClassTypeParams) || currentClassName.empty()) return;
    if (semanticModel_.hasClass(currentClassName)) semanticModel_.markNumericGenericTypeParam(currentClassName, name);
}

} // namespace zl
