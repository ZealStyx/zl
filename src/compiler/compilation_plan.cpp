#include "zl/compiler/compiler.hpp"
#include "zl/compiler/dispatch_table.hpp"
#include "zl/parser/type_annotation.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace zl {


CompilationPlan buildCompilationPlan(const Program& program) {
    CompilationPlan plan;

    for (const auto& decl : program.declarations) {
        if (decl->kind == NodeKind::DataDecl) {
            const auto* data = static_cast<const DataDecl*>(decl.get());
            plan.classTypeParams[data->name] = {};
            if (!data->extendsName.empty()) plan.classParents[data->name] = data->extendsName;
            for (const auto& member : data->members) {
                if (member->kind != NodeKind::FunctionDecl) continue;
                const auto* fn = static_cast<const FunctionDecl*>(member.get());
                plan.allFunctions.push_back(fn);
            }
            continue;
        }
        if (decl->kind != NodeKind::ClassDecl) continue;
        const auto* cls = static_cast<const ClassDecl*>(decl.get());
        plan.classTypeParams[cls->name] = cls->typeParams;
        if (!cls->extendsName.empty()) plan.classParents[cls->name] = cls->extendsName;

        for (const auto& member : cls->members) {
            if (member->kind != NodeKind::FunctionDecl) continue;
            const auto* fn = static_cast<const FunctionDecl*>(member.get());
            plan.allFunctions.push_back(fn);
            if (fn->name == "main" && plan.mainFunction == nullptr) plan.mainFunction = fn;
        }
    }

    std::unordered_map<std::string, ClassReflectionInfo> localReflection;
    std::unordered_map<std::string, std::vector<std::string>> interfaceParents;
    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::InterfaceDecl) continue;
        const auto* iface = static_cast<const InterfaceDecl*>(decl.get());
        interfaceParents[iface->name] = iface->extendsNames;
    }

    auto collectInterfaceClosure = [&](const std::vector<std::string>& direct) {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        std::vector<std::string> pending = direct;
        while (!pending.empty()) {
            std::string current = std::move(pending.back());
            pending.pop_back();
            if (!seen.insert(current).second) continue;
            result.push_back(current);
            auto it = interfaceParents.find(current);
            if (it != interfaceParents.end()) {
                pending.insert(pending.end(), it->second.begin(), it->second.end());
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    };

    RuntimeTypeId nextRuntimeTypeId = 1;
    for (const auto& decl : program.declarations) {
        if (decl->kind == NodeKind::EnumDecl) {
            const auto* en = static_cast<const EnumDecl*>(decl.get());
            auto& info = localReflection[en->name];
            info.id = nextRuntimeTypeId++;
            info.isEnumType = true;
            info.enumMembers = en->members;
            continue;
        }
        if (decl->kind == NodeKind::DataDecl) {
            const auto* data = static_cast<const DataDecl*>(decl.get());
            auto& info = localReflection[data->name];
            info.id = nextRuntimeTypeId++;
            info.isDataType = true;
            info.baseClassName = data->extendsName;
            for (const auto& field : data->fields) { RuntimeFieldInfo rf; rf.name = field.name; rf.ownerClassName = data->name; rf.typeName = describeTypeAnnotation(field.type); rf.access = "public"; rf.isStatic = false; rf.ownership = field.ownership; info.fields.push_back(std::move(rf)); }
            for (const auto& member : data->members) {
                const auto* fn = static_cast<const FunctionDecl*>(member.get());
                RuntimeMethodInfo method;
                method.name = fn->name;
                method.ownerClassName = data->name;
                for (const auto& param : fn->params) method.parameterTypes.push_back(describeTypeAnnotation(param.type));
                method.dispatchSignature = dispatchSignatureForFunction(*fn, {}).describe();
                const auto prefix = method.name;
                if (method.dispatchSignature.rfind(prefix, 0) == 0) method.dispatchSignature.erase(0, prefix.size());
                method.returnType = (fn->returnType.name.empty() && fn->returnType.unionOf.empty()) ? "void" : describeTypeAnnotation(fn->returnType);
                method.access = "public";
                method.isStatic = false;
                method.isAsync = fn->isAsync;
                info.methods.push_back(std::move(method));
            }
            continue;
        }
        if (decl->kind != NodeKind::ClassDecl) continue;
        const auto* cls = static_cast<const ClassDecl*>(decl.get());
        auto& info = localReflection[cls->name];
        if (info.id == 0) info.id = nextRuntimeTypeId++;
        info.baseClassName = cls->extendsName;
        TypeAnnotation baseType;
        baseType.name = cls->extendsName;
        baseType.typeArgs = cls->extendsTypeArgs;
        info.baseTypeName = describeTypeAnnotation(baseType);
        info.typeParameters = cls->typeParams;
        info.interfaces = cls->implementsNames;
        for (const auto& member : cls->members) {
            if (member->kind == NodeKind::VarDecl) {
                const auto* field = static_cast<const VarDecl*>(member.get());
                RuntimeFieldInfo rf;
                rf.name = field->name;
                rf.ownerClassName = cls->name;
                rf.typeName = field->hasExplicitType ? describeTypeAnnotation(field->type) : "unknown";
                rf.access = field->access == AccessModifier::PRIVATE ? "private" :
                            field->access == AccessModifier::PROTECTED ? "protected" : "public";
                rf.isStatic = field->isStatic;
                rf.ownership = field->ownership;
                info.fields.push_back(std::move(rf));
            } else if (member->kind == NodeKind::FunctionDecl) {
                const auto* fn = static_cast<const FunctionDecl*>(member.get());
                if (fn->isConstructor) {
                    RuntimeConstructorInfo ctor;
                    ctor.ownerClassName = cls->name;
                    ctor.access = fn->access == AccessModifier::PRIVATE ? "private" : fn->access == AccessModifier::PROTECTED ? "protected" : "public";
                    for (const auto& param : fn->params) ctor.parameterTypes.push_back(describeTypeAnnotation(param.type));
                    ctor.dispatchSignature = dispatchSignatureForFunction(*fn, plan.classTypeParams.at(cls->name)).describe();
                    // describe() includes the constructor name; reflection stores only the parameter suffix.
                    const auto prefix = ctor.ownerClassName;
                    if (ctor.dispatchSignature.rfind(prefix, 0) == 0) ctor.dispatchSignature.erase(0, prefix.size());
                    info.constructors.push_back(std::move(ctor));
                } else {
                    RuntimeMethodInfo method;
                    method.name = fn->name;
                    method.ownerClassName = cls->name;
                    for (const auto& param : fn->params) method.parameterTypes.push_back(describeTypeAnnotation(param.type));
                    method.dispatchSignature = dispatchSignatureForFunction(*fn, plan.classTypeParams.at(cls->name)).describe();
                    const auto prefix = method.name;
                    if (method.dispatchSignature.rfind(prefix, 0) == 0) method.dispatchSignature.erase(0, prefix.size());
                    method.returnType = (fn->returnType.name.empty() && fn->returnType.unionOf.empty()) ? "void" : describeTypeAnnotation(fn->returnType);
                    method.access = fn->access == AccessModifier::PRIVATE ? "private" :
                                    fn->access == AccessModifier::PROTECTED ? "protected" : "public";
                    method.isStatic = fn->isStatic;
                    method.isAsync = fn->isAsync;
                    info.methods.push_back(std::move(method));
                }
            }
        }
    }

    for (const auto& [className, _] : localReflection) {
        std::vector<std::string> chain;
        std::string cur = className;
        while (!cur.empty()) {
            chain.push_back(cur);
            auto it = plan.classParents.find(cur);
            cur = (it != plan.classParents.end()) ? it->second : std::string();
        }

        ClassReflectionInfo merged;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto& src = localReflection[*it];
            if (!src.baseClassName.empty()) merged.baseClassName = src.baseClassName;
            for (const auto& field : src.fields) {
                const auto duplicate = std::find_if(merged.fields.begin(), merged.fields.end(),
                    [&](const auto& existing) { return existing.name == field.name; });
                if (duplicate == merged.fields.end()) merged.fields.push_back(field);
            }
            for (const auto& method : src.methods) {
                const auto duplicate = std::find_if(merged.methods.begin(), merged.methods.end(),
                    [&](const auto& existing) { return existing.name == method.name && existing.parameterTypes == method.parameterTypes; });
                if (duplicate == merged.methods.end()) merged.methods.push_back(method);
                else *duplicate = method;
            }
            for (const auto& ctor : src.constructors) merged.constructors.push_back(ctor);
        }

        const auto& direct = localReflection[className];
        merged.baseClassName = direct.baseClassName;
        merged.baseTypeName = direct.baseTypeName;
        merged.interfaces = collectInterfaceClosure(direct.interfaces);
        merged.typeParameters = direct.typeParameters;
        merged.isDataType = direct.isDataType;
        merged.isEnumType = direct.isEnumType;
        merged.enumMembers = direct.enumMembers;

        auto runtimeType = std::make_shared<RuntimeTypeInfo>();
        runtimeType->id = merged.id;
        runtimeType->name = className;
        runtimeType->baseClassName = merged.baseClassName;
        runtimeType->interfaces = merged.interfaces;
        runtimeType->typeParameters = merged.typeParameters;
        runtimeType->isDataType = merged.isDataType;
        runtimeType->isEnumType = merged.isEnumType;
        runtimeType->enumMembers = merged.enumMembers;
        runtimeType->fields = merged.fields;
        runtimeType->methods = merged.methods;
        runtimeType->constructors = merged.constructors;
        merged.runtimeType = std::move(runtimeType);

        plan.classReflection[className] = std::move(merged);
    }

    if (!plan.mainFunction) {
        throw std::runtime_error(
            "Compiler: no 'main' func found "
            "(expected e.g. class Program { func main(): void { ... } })");
    }

    return plan;
}

} // namespace zl
