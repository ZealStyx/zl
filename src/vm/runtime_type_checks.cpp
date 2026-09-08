#include "zl/vm/runtime_type_checks.hpp"
#include "zl/compiler/bytecode.hpp"
#include "zl/vm/runtime_task.hpp"

#include <algorithm>

namespace zl {
static RuntimeTypeBindings bindTypeParameters(const ClassReflectionInfo& info, const std::string& typeName) {
    const auto spec = parseTypeName(typeName);
    RuntimeTypeBindings bindings;
    if (info.typeParameters.size() != spec.args.size()) return bindings;
    for (std::size_t i = 0; i < spec.args.size(); ++i) {
        bindings.emplace(info.typeParameters[i], describeTypeName(spec.args[i]));
    }
    return bindings;
}

// Resolve the declared extends arguments at each step; positional copying is
// wrong for Derived<K,V> extends Base<V> and for concrete/nested parent args.
static std::string typeInHierarchy(const ObjectBox& receiver, const Chunk& chunk,
                                   const std::string& declaringClass) {
    std::string className = receiver.className;
    std::string typeName = receiver.genericTypeName.empty() ? className : receiver.genericTypeName;
    for (std::size_t depth = 0; depth <= chunk.classReflection.size(); ++depth) {
        if (className == declaringClass) return typeName;
        const auto it = chunk.classReflection.find(className);
        if (it == chunk.classReflection.end() || it->second.baseClassName.empty()) break;
        const auto& info = it->second;
        typeName = substituteTypeParams(info.baseTypeName.empty() ? info.baseClassName : info.baseTypeName,
                                       bindTypeParameters(info, typeName));
        className = info.baseClassName;
    }
    return {};
}

RuntimeTypeBindings receiverTypeBindings(const ObjectBox& receiver, const Chunk& chunk,
                                        const std::string& declaringClass) {
    const auto it = chunk.classReflection.find(declaringClass);
    if (it == chunk.classReflection.end()) return {};
    return bindTypeParameters(it->second, typeInHierarchy(receiver, chunk, declaringClass));
}


void initializeObjectType(Value& value, const std::string& typeName, const Chunk& chunk) {
    auto* object = std::get_if<ObjectRef>(&value);
    const auto spec = parseTypeName(typeName);
    if (!object || !*object || (*object)->className != spec.name) {
        throw std::runtime_error("VM: factory returned a value incompatible with " + typeName);
    }
    (*object)->genericTypeName = spec.args.empty() ? std::string{} : describeTypeName(spec);
    const auto meta = chunk.classReflection.find(spec.name);
    if (meta != chunk.classReflection.end()) (*object)->runtimeType = meta->second.runtimeType;
}

bool reflectiveObjectMatches(const ObjectRef& object, const std::string& expectedName, const Chunk* chunk) {
    if (!object) return false;
    const auto genericPos = expectedName.find('<');
    const bool expectedIsParameterized = genericPos != std::string::npos;
    const auto baseName = expectedIsParameterized ? expectedName.substr(0, genericPos) : expectedName;

    // A parameterized reflection type must preserve the concrete instantiated
    // identity when runtime metadata knows it. Do not accept an erased
    // GenericBox object merely because its dispatch class is also GenericBox.
    if (expectedIsParameterized) {
        if (object->genericTypeName == expectedName) return true;
        if (object->runtimeType && object->runtimeType->name == expectedName) return true;
    } else if (object->className == baseName) {
        return true;
    }
    if (!chunk || !object->runtimeType) return false;

    // Preserve compiler assignability for both transitive class inheritance
    // and interface implementation. Compilation metadata stores the complete
    // interface closure, including interfaces extended by other interfaces.
    if (std::find(object->runtimeType->interfaces.begin(),
                  object->runtimeType->interfaces.end(), baseName) != object->runtimeType->interfaces.end()) {
        return true;
    }

    auto current = object->runtimeType;
    std::size_t guard = 0;
    while (current && !current->baseClassName.empty() && guard++ < 1024) {
        if (current->baseClassName == baseName) {
            if (!expectedIsParameterized) return true;
            const auto actual = typeInHierarchy(*object, *chunk, baseName);
            if (!actual.empty() && typeNamesEqual(parseTypeName(actual),
                                                           parseTypeName(expectedName))) return true;
        }
        auto it = chunk->classReflection.find(current->baseClassName);
        if (it != chunk->classReflection.end() && it->second.runtimeType) {
            if (expectedIsParameterized && it->second.runtimeType->name == expectedName) return true;
        }
        if (it == chunk->classReflection.end()) break;
        if (std::find(it->second.interfaces.begin(), it->second.interfaces.end(), baseName) != it->second.interfaces.end()) {
            return true;
        }
        current = it->second.runtimeType;
    }
    return false;
}

bool reflectiveMatchesSpec(const Value& value, const TypeName& spec, const Chunk* chunk) {
    if (!spec.unionMembers.empty()) {
        for (const auto& member : spec.unionMembers) if (reflectiveMatchesSpec(value, member, chunk)) return true;
        return false;
    }
    if (chunk && spec.args.empty()) {
        const auto type = chunk->classReflection.find(spec.name);
        if (type != chunk->classReflection.end() && type->second.isEnumType) {
            const auto* member = std::get_if<std::string>(&value);
            const auto& members = type->second.enumMembers;
            return member && std::find(members.begin(), members.end(), *member) != members.end();
        }
    }
    const auto& name = spec.name;
    if (name == "unknown") return true;
    if (name == "nil") return std::holds_alternative<std::monostate>(value);
    if (name == "object") return std::holds_alternative<ObjectRef>(value) || std::holds_alternative<std::monostate>(value);
    if (name == "int") return std::holds_alternative<std::int64_t>(value);
    if (name == "double" || name == "float") return std::holds_alternative<double>(value) || std::holds_alternative<std::int64_t>(value);
    if (name == "string") return std::holds_alternative<std::string>(value);
    if (name == "bool") return std::holds_alternative<bool>(value);
    if (name == "void") return std::holds_alternative<std::monostate>(value);
    if (name == "Thread") return std::holds_alternative<ThreadRef>(value);
    if (name == "NativeHandle") return std::holds_alternative<NativeHandleRef>(value);
    if (name == "func") {
        const auto closure = std::get_if<ClosureRef>(&value);
        if (!closure || !*closure) return false;
        if (spec.args.empty()) return true;
        const std::size_t expectedParamCount = spec.args.size() - 1;
        if ((*closure)->parameterTypeNames.size() != expectedParamCount) return false;
        for (std::size_t i = 0; i < expectedParamCount; ++i) {
            const auto actual = parseTypeName((*closure)->parameterTypeNames[i]);
            if (!typeNamesEqual(actual, spec.args[i])) return false;
        }
        std::string returnType = (*closure)->returnTypeName.empty() ? "void" : (*closure)->returnTypeName;
        if ((*closure)->isAsync) returnType = "Task<" + returnType + ">";
        const auto actualReturn = parseTypeName(returnType);
        return typeNamesEqual(actualReturn, spec.args.back());
    }
    if (name == "Task") {
        const auto task = std::get_if<TaskRef>(&value);
        if (!task || !(*task)) return false;
        if (spec.args.empty()) return true;
        if (spec.args.size() != 1) return false;
        const std::string actualName = (*task)->valueTypeName();
        // Some native tasks (notably generic Channel.receiveAsync) cannot know
        // their concrete T at runtime. Preserve the historical permissive
        // outer-Task check when no concrete value type metadata is available.
        if (actualName.empty()) return true;
        const auto actual = parseTypeName(actualName);
        return typeNamesEqual(actual, spec.args.front());
    }
    if (name == "list" || name == "array" || name == "set") {
        const auto list = std::get_if<ListRef>(&value);
        if (!list || !*list) return false;
        const std::size_t begin = std::min((*list)->frontIndex, (*list)->items.size());
        const std::size_t size = (*list)->items.size() - begin;
        if (name == "array" && spec.fixedSize && size != *spec.fixedSize) return false;
        if (spec.args.empty()) return true;
        for (std::size_t i = begin; i < (*list)->items.size(); ++i) {
            if (!reflectiveMatchesSpec((*list)->items[i], spec.args.front(), chunk)) return false;
        }
        return true;
    }
    if (name == "map") {
        const auto map = std::get_if<MapRef>(&value);
        if (!map || !*map) return false;
        if (spec.args.empty()) return true;
        if (spec.args.size() != 2) return false;
        for (const auto& entry : (*map)->entries) {
            if (!reflectiveMatchesSpec(entry.first, spec.args[0], chunk) || !reflectiveMatchesSpec(entry.second, spec.args[1], chunk)) return false;
        }
        return true;
    }
    const std::string expectedObjectName = describeTypeName(spec);
    return reflectiveObjectMatches(std::get_if<ObjectRef>(&value) ? *std::get_if<ObjectRef>(&value) : ObjectRef{}, expectedObjectName, chunk);
}


bool reflectiveTypeMatchesName(const Value& value, const std::string& typeName, const Chunk* chunk) {
    return reflectiveMatchesSpec(value, parseTypeName(typeName), chunk);
}

std::string runtimeValueTypeName(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) return "void";
    if (std::holds_alternative<std::int64_t>(value)) return "int";
    if (std::holds_alternative<double>(value)) return "double";
    if (std::holds_alternative<std::string>(value)) return "string";
    if (std::holds_alternative<bool>(value)) return "bool";
    if (std::holds_alternative<ListRef>(value)) return "list";
    if (std::holds_alternative<MapRef>(value)) return "map";
    if (auto obj = std::get_if<ObjectRef>(&value); obj && *obj)
        return (*obj)->genericTypeName.empty() ? (*obj)->className : (*obj)->genericTypeName;
    if (std::holds_alternative<ClosureRef>(value)) return "func";
    if (std::holds_alternative<TaskRef>(value)) return "Task";
    if (std::holds_alternative<ThreadRef>(value)) return "Thread";
    if (std::holds_alternative<NativeHandleRef>(value)) return "NativeHandle";
    return "unknown";
}

static bool runtimeAssignableToSpec(const Value& value, const TypeName& spec, const Chunk* chunk) {
    if (!spec.unionMembers.empty()) {
        return std::any_of(spec.unionMembers.begin(), spec.unionMembers.end(), [&](const auto& member) {
            return runtimeAssignableToSpec(value, member, chunk);
        });
    }
    // `object` is the language's dynamic annotation; `unknown` is its
    // inferred counterpart. Concrete class names are checked below.
    if (spec.name == "object" || spec.name == "unknown") return true;
    if (spec.name == "double" && std::holds_alternative<std::int64_t>(value)) return true;
    if (std::holds_alternative<std::monostate>(value)) {
        const std::string base = spec.name;
        return base == "void" || base == "nil" || base == "string" || base == "list" || base == "map" || base == "set" ||
               base == "array" || base == "func" || base == "object" || base == "Task" ||
               (chunk && chunk->classReflection.count(base) && !chunk->classReflection.at(base).isEnumType);
    }
    return reflectiveMatchesSpec(value, spec, chunk);
}

bool runtimeAssignableToType(const Value& value, const std::string& typeName, const Chunk* chunk) {
    return runtimeAssignableToSpec(value, parseTypeName(typeName), chunk);
}

} // namespace zl
