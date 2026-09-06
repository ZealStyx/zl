#include "zl/compiler/type_resolver.hpp"

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
}

ZlType TypeResolver::resolveType(const TypeAnnotation& annotation,
                                 const std::string& currentClassName,
                                 const std::vector<std::string>& currentClassTypeParams,
                                 std::string* outClassName) {
    const std::string& n = annotation.name;
    if (n == "int") return ZlType::INT;
    if (n == "double") return ZlType::DOUBLE;
    if (n == "float") return ZlType::DOUBLE;
    if (n == "string") return ZlType::STRING;
    if (n == "bool") return ZlType::BOOL;
    if (n == "unknown") return ZlType::UNKNOWN;
    if (n == "void") return ZlType::VOID_TYPE;
    if (n == "list" || n == "array" || n == "set" || n == "map") {
        if (!annotation.unionOf.empty()) return ZlType::UNKNOWN;
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
    if (n == "func") return ZlType::FUNCTION;
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
    if (!annotation.unionOf.empty()) return ZlType::UNKNOWN;

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

    auto canonicalTypeName = [](const ResolvedTypeArg& value) {
        if (!value.className.empty()) return value.className;
        switch (value.type) {
            case ZlType::INT: return std::string("int");
            case ZlType::DOUBLE: return std::string("double");
            case ZlType::STRING: return std::string("string");
            case ZlType::BOOL: return std::string("bool");
            case ZlType::VOID_TYPE: return std::string("void");
            case ZlType::NIL: return std::string("nil");
            case ZlType::LIST: return std::string("list");
            case ZlType::MAP: return std::string("map");
            case ZlType::SET: return std::string("set");
            case ZlType::ARRAY: return std::string("array");
            case ZlType::OBJECT: return std::string("object");
            case ZlType::FUNCTION: return std::string("func");
            case ZlType::TASK: return std::string("Task");
            case ZlType::UNKNOWN: return std::string("unknown");
        }
        return std::string("unknown");
    };

    std::function<std::string(const std::string&)> substituteCanonicalName;
    substituteCanonicalName = [&](const std::string& name) {
        const auto lt = name.find('<');
        if (lt == std::string::npos || name.empty() || name.back() != '>') return name;
        const std::string base = name.substr(0, lt);
        const std::string body = name.substr(lt + 1, name.size() - lt - 2);
        std::vector<std::string> parts;
        std::size_t start = 0;
        std::size_t depth = 0;
        for (std::size_t i = 0; i <= body.size(); ++i) {
            if (i == body.size() || (body[i] == ',' && depth == 0)) {
                parts.push_back(body.substr(start, i - start));
                start = i + 1;
            } else if (body[i] == '<') {
                ++depth;
            } else if (body[i] == '>') {
                --depth;
            }
        }
        for (auto& part : parts) {
            for (std::size_t i = 0; i < generic.typeParams.size(); ++i) {
                if (part == generic.typeParams[i]) {
                    part = canonicalTypeName(typeArgs[i]);
                    break;
                }
            }
            // A nested generic type can itself contain one of our type
            // parameters (e.g. Box<T> inside list<Box<T>>). Recurse through
            // the canonical spelling instead of treating the whole argument
            // as opaque metadata.
            const auto nestedLt = part.find('<');
            if (nestedLt != std::string::npos && !part.empty() && part.back() == '>') {
                std::string rebuilt = substituteCanonicalName(part);
                part = std::move(rebuilt);
            }
        }
        std::string rebuilt = base + "<";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) rebuilt += ",";
            rebuilt += parts[i];
        }
        rebuilt += ">";
        return rebuilt;
    };

    std::function<ResolvedTypeArg(const std::string&, std::size_t)> resolveCanonicalType;
    resolveCanonicalType = [&](const std::string& name, std::size_t nestedLine) -> ResolvedTypeArg {
        if (name == "int") return {ZlType::INT, ""};
        if (name == "double") return {ZlType::DOUBLE, ""};
        if (name == "string") return {ZlType::STRING, ""};
        if (name == "bool") return {ZlType::BOOL, ""};
        if (name == "void") return {ZlType::VOID_TYPE, ""};
        if (name == "nil") return {ZlType::NIL, ""};
        if (name == "unknown") return {ZlType::UNKNOWN, ""};
        const auto lt = name.find('<');
        if (lt == std::string::npos || name.empty() || name.back() != '>') return {ZlType::OBJECT, name};
        const std::string base = name.substr(0, lt);
        const std::string body = name.substr(lt + 1, name.size() - lt - 2);
        std::vector<std::string> parts;
        std::size_t start = 0;
        std::size_t depth = 0;
        for (std::size_t i = 0; i <= body.size(); ++i) {
            if (i == body.size() || (body[i] == ',' && depth == 0)) {
                parts.push_back(body.substr(start, i - start));
                start = i + 1;
            } else if (body[i] == '<') {
                ++depth;
            } else if (body[i] == '>') {
                if (depth == 0) typeError("malformed generic type '" + name + "'", nestedLine);
                --depth;
            }
        }
        const auto* genericShape = semanticModel_.findClass(base);
        if (genericShape == nullptr || genericShape->typeParams.empty()) {
            return {ZlType::OBJECT, name};
        }
        if (parts.size() != genericShape->typeParams.size()) {
            typeError("generic type '" + base + "' requires " + std::to_string(genericShape->typeParams.size()) +
                      " type argument(s)", nestedLine);
        }
        std::vector<ResolvedTypeArg> args;
        args.reserve(parts.size());
        for (const auto& part : parts) args.push_back(resolveCanonicalType(part, nestedLine));
        return {ZlType::OBJECT, instantiateGenericClass(base, args, nestedLine)};
    };

    auto substitute = [&](ZlType type, const std::string& className) -> ResolvedTypeArg {
        if (type == ZlType::OBJECT) {
            for (std::size_t i = 0; i < generic.typeParams.size(); ++i) {
                if (generic.typeParams[i] == className) return typeArgs[i];
            }
        }
        if (className.empty()) return {type, className};

        const std::string substitutedName = substituteCanonicalName(className);
        if (substitutedName != className) {
            if (type == ZlType::OBJECT && substitutedName.find('<') != std::string::npos) {
                return resolveCanonicalType(substitutedName, line);
            }
            return {type, substitutedName};
        }

        if (type == ZlType::OBJECT) {
            auto nestedIt = genericInstantiationByName_.find(className);
            if (nestedIt != genericInstantiationByName_.end()) {
                const GenericInstantiation& nested = nestedIt->second;
                std::vector<ResolvedTypeArg> nestedArgs;
                nestedArgs.reserve(nested.args.size());
                for (const auto& nestedArg : nested.args) {
                    auto ownParam = std::find(generic.typeParams.begin(), generic.typeParams.end(), nestedArg.className);
                    if (nestedArg.type == ZlType::OBJECT && ownParam != generic.typeParams.end()) {
                        nestedArgs.push_back(typeArgs[static_cast<std::size_t>(ownParam - generic.typeParams.begin())]);
                    } else {
                        nestedArgs.push_back(nestedArg);
                    }
                }
                return {ZlType::OBJECT, instantiateGenericClass(nested.base, nestedArgs, line)};
            }
        }
        return {type, className};
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
