#include "zl/compiler/dispatch_table.hpp"
#include "zl/common/type_annotation.hpp"

#include <algorithm>
#include <unordered_set>

namespace zl {

namespace {
DispatchType dispatchTypeFor(const Param& param, const std::vector<std::string>& genericTypeParams) {
    if (!param.type.unionOf.empty()) {
        if (containsTypeParameter(param.type, genericTypeParams)) return {DispatchTypeKind::GENERIC_OBJECT, {}};
        return {DispatchTypeKind::OBJECT, describeTypeAnnotation(param.type)};
    }
    // The erased base name of a concrete instantiation (`Option<int>` ->
    // "Option"): the runtime keeps one body per generic class, but two
    // declarations over different generic classes are different declarations
    // and must not share a dispatch identity.
    const auto erasedBase = [](const std::string& rendered) {
        const auto open = rendered.find('<');
        return open == std::string::npos ? rendered : rendered.substr(0, open);
    };
    const std::string& name = param.type.name;
    if (name == "int") return {DispatchTypeKind::INT, {}};
    if (name == "double" || name == "float") return {DispatchTypeKind::DOUBLE, {}};
    if (name == "string") return {DispatchTypeKind::STRING, {}};
    if (name == "bool") return {DispatchTypeKind::BOOL, {}};
    if (name == "void") return {DispatchTypeKind::VOID_TYPE, {}};
    if (name == "list") return {DispatchTypeKind::LIST, {}};
    if (name == "array") return {DispatchTypeKind::ARRAY, {}};
    if (name == "set") return {DispatchTypeKind::SET, {}};
    if (name == "map") return {DispatchTypeKind::MAP, {}};
    if (name == "func") return {DispatchTypeKind::FUNCTION, {}};
    if (name == "unknown" || name == "nil" || name == "object") return {DispatchTypeKind::GENERIC_OBJECT, {}};
    if (std::find(genericTypeParams.begin(), genericTypeParams.end(), name) != genericTypeParams.end()) {
        // The class's own type parameter: erased all the way down, because the
        // one template body serves every instantiation.
        return {DispatchTypeKind::GENERIC_OBJECT, {}};
    }
    if (!param.type.typeArgs.empty()) {
        // A concrete instantiation of a generic class. Distinct base classes
        // stay distinct; instantiations of the same base share the body.
        return {DispatchTypeKind::GENERIC_OBJECT, erasedBase(name)};
    }
    return {DispatchTypeKind::OBJECT, name};
}

DispatchSignature signatureFor(const FunctionDecl* fn,
                                const std::vector<std::string>& genericTypeParams) {
    DispatchSignature signature;
    signature.name = fn->name;
    signature.parameters.reserve(fn->params.size());
    for (const auto& param : fn->params) {
        signature.parameters.push_back(dispatchTypeFor(param, genericTypeParams));
    }
    return signature;
}
}

DispatchSignature dispatchSignatureForFunction(const FunctionDecl& fn,
                                               const std::vector<std::string>& genericTypeParams) {
    return signatureFor(&fn, genericTypeParams);
}

DispatchTableSet buildDispatchTables(
    const std::vector<const FunctionDecl*>& functions,
    const std::unordered_map<std::string, std::vector<std::string>>& classTypeParams,
    const std::unordered_map<std::string, std::string>& classParents) {
    DispatchTableSet result;

    for (const auto* fn : functions) {
        if (fn->isStatic || fn->ownerClassName.empty()) continue;
        const auto& typeParams = classTypeParams.at(fn->ownerClassName);
        const DispatchSignature key = signatureFor(fn, typeParams);
        if (!result.methodSlots.count(key)) result.methodSlots[key] = result.methodSlots.size();
    }

    const std::size_t slotCount = result.methodSlots.size();
    auto buildForClass = [&](const std::string& className) {
        std::vector<std::size_t> vtable(slotCount, Chunk::INVALID_FUNCTION_INDEX);
        std::vector<std::string> chain;
        std::string cur = className;
        while (!cur.empty()) {
            chain.push_back(cur);
            auto parentIt = classParents.find(cur);
            cur = parentIt != classParents.end() ? parentIt->second : std::string();
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const std::string& owner = *it;
            const auto& typeParams = classTypeParams.at(owner);
            for (std::size_t i = 0; i < functions.size(); ++i) {
                const FunctionDecl* fn = functions[i];
                if (fn->isStatic || fn->ownerClassName != owner) continue;
                const DispatchSignature key = signatureFor(fn, typeParams);
                vtable[result.methodSlots.at(key)] = i;
            }
        }
        result.classVTables[className] = std::move(vtable);
    };

    std::unordered_set<std::string> classes;
    for (const auto* fn : functions) {
        if (!fn->isStatic && !fn->ownerClassName.empty()) classes.insert(fn->ownerClassName);
    }
    for (const auto& [child, parent] : classParents) {
        (void)parent;
        classes.insert(child);
    }
    for (const auto& className : classes) buildForClass(className);

    return result;
}

} // namespace zl
