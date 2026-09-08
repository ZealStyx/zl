#include "zl/vm/runtime_type_checks.hpp"
#include "zl/compiler/bytecode.hpp"
#include "zl/vm/runtime_task.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

namespace zl {
struct ReflectionTypeSpec {
    std::string name;
    std::vector<ReflectionTypeSpec> args;
    std::vector<ReflectionTypeSpec> unionMembers;
    std::optional<std::size_t> fixedSize;
};

class ReflectionTypeParser {
public:
    explicit ReflectionTypeParser(const std::string& text) : text_(text) {}

    ReflectionTypeSpec parse() {
        auto result = parseUnion();
        skipWhitespace();
        return pos_ == text_.size() ? result : ReflectionTypeSpec{"unknown", {}, {}};
    }

private:
    ReflectionTypeSpec parseUnion() {
        std::vector<ReflectionTypeSpec> members;
        members.push_back(parsePrimary());
        skipWhitespace();
        while (consume('|')) {
            members.push_back(parsePrimary());
            skipWhitespace();
        }
        if (members.size() == 1) return members.front();
        ReflectionTypeSpec result;
        result.name = "union";
        result.unionMembers = std::move(members);
        return result;
    }

    ReflectionTypeSpec parsePrimary() {
        skipWhitespace();
        const std::size_t start = pos_;
        while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_' || text_[pos_] == '.')) ++pos_;
        if (start == pos_) return ReflectionTypeSpec{"unknown", {}, {}};
        ReflectionTypeSpec result;
        result.name = text_.substr(start, pos_ - start);
        skipWhitespace();
        if (result.name == "array" && consume('[')) {
            const std::size_t sizeStart = pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
            if (sizeStart == pos_ || !consume(']')) return ReflectionTypeSpec{"unknown", {}, {}};
            try {
                result.fixedSize = std::stoull(text_.substr(sizeStart, pos_ - sizeStart - 1));
            } catch (...) {
                return ReflectionTypeSpec{"unknown", {}, {}};
            }
            skipWhitespace();
        }
        if (consume('<')) {
            skipWhitespace();
            if (!consume('>')) {
                while (true) {
                    result.args.push_back(parseUnion());
                    skipWhitespace();
                    if (consume('>')) break;
                    if (!consume(',')) return ReflectionTypeSpec{"unknown", {}, {}};
                }
            }
        } else if (result.name == "func" && consume('(')) {
            while (true) {
                skipWhitespace();
                if (consume(')')) break;
                result.args.push_back(parseUnion());
                skipWhitespace();
                if (consume(')')) break;
                if (!consume(',')) return ReflectionTypeSpec{"unknown", {}, {}};
            }
            skipWhitespace();
            if (!consume(':')) return ReflectionTypeSpec{"unknown", {}, {}};
            result.args.push_back(parseUnion());
        }
        return result;
    }

    bool consume(char c) {
        skipWhitespace();
        if (pos_ >= text_.size() || text_[pos_] != c) return false;
        ++pos_;
        return true;
    }

    void skipWhitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    const std::string& text_;
    std::size_t pos_{0};
};

bool reflectiveMatchesSpec(const Value& value, const ReflectionTypeSpec& spec, const Chunk* chunk);
bool reflectionTypeSpecsEqual(const ReflectionTypeSpec& a, const ReflectionTypeSpec& b);
std::string reflectionTypeSpecName(const ReflectionTypeSpec& spec);

std::string substituteTypeParams(const std::string& typeName, const RuntimeTypeBindings& bindings) {
    if (bindings.empty()) return typeName;
    std::string out;
    for (std::size_t i = 0; i < typeName.size();) {
        const unsigned char c = static_cast<unsigned char>(typeName[i]);
        if (!(std::isalnum(c) || c == '_')) { out.push_back(typeName[i++]); continue; }
        const std::size_t start = i;
        while (i < typeName.size() &&
               (std::isalnum(static_cast<unsigned char>(typeName[i])) || typeName[i] == '_')) ++i;
        const std::string token = typeName.substr(start, i - start);
        const auto it = bindings.find(token);
        out += it == bindings.end() ? token : it->second;
    }
    return out;
}

static RuntimeTypeBindings bindTypeParameters(const ClassReflectionInfo& info, const std::string& typeName) {
    const auto spec = ReflectionTypeParser(typeName).parse();
    RuntimeTypeBindings bindings;
    if (info.typeParameters.size() != spec.args.size()) return bindings;
    for (std::size_t i = 0; i < spec.args.size(); ++i) {
        bindings.emplace(info.typeParameters[i], reflectionTypeSpecName(spec.args[i]));
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

std::string reflectionTypeSpecName(const ReflectionTypeSpec& spec) {
    if (!spec.unionMembers.empty()) {
        std::string out;
        for (std::size_t i = 0; i < spec.unionMembers.size(); ++i) {
            if (i) out += " | ";
            out += reflectionTypeSpecName(spec.unionMembers[i]);
        }
        return out;
    }
    std::string out = spec.name;
    if (spec.fixedSize && spec.name == "array") {
        out = "array[" + std::to_string(*spec.fixedSize) + "]";
    }
    if (!spec.args.empty()) {
        // Function types are handled by reflectiveMatchesSpec directly.
        if (spec.name == "func") {
            out += "(";
            for (std::size_t i = 0; i + 1 < spec.args.size(); ++i) {
                if (i) out += ",";
                out += reflectionTypeSpecName(spec.args[i]);
            }
            out += "):" + reflectionTypeSpecName(spec.args.back());
            return out;
        }
        out += "<";
        for (std::size_t i = 0; i < spec.args.size(); ++i) {
            if (i) out += ",";
            out += reflectionTypeSpecName(spec.args[i]);
        }
        out += ">";
    }
    return out;
}

void initializeObjectType(Value& value, const std::string& typeName, const Chunk& chunk) {
    auto* object = std::get_if<ObjectRef>(&value);
    const auto spec = ReflectionTypeParser(typeName).parse();
    if (!object || !*object || (*object)->className != spec.name) {
        throw std::runtime_error("VM: factory returned a value incompatible with " + typeName);
    }
    (*object)->genericTypeName = spec.args.empty() ? std::string{} : reflectionTypeSpecName(spec);
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
            if (!actual.empty() && reflectionTypeSpecsEqual(ReflectionTypeParser(actual).parse(),
                                                           ReflectionTypeParser(expectedName).parse())) return true;
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

bool reflectiveMatchesSpec(const Value& value, const ReflectionTypeSpec& spec, const Chunk* chunk) {
    if (!spec.unionMembers.empty()) {
        for (const auto& member : spec.unionMembers) if (reflectiveMatchesSpec(value, member, chunk)) return true;
        return false;
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
            const auto actual = ReflectionTypeParser((*closure)->parameterTypeNames[i]).parse();
            if (!reflectionTypeSpecsEqual(actual, spec.args[i])) return false;
        }
        std::string returnType = (*closure)->returnTypeName.empty() ? "void" : (*closure)->returnTypeName;
        if ((*closure)->isAsync) returnType = "Task<" + returnType + ">";
        const auto actualReturn = ReflectionTypeParser(returnType).parse();
        return reflectionTypeSpecsEqual(actualReturn, spec.args.back());
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
        const auto actual = ReflectionTypeParser(actualName).parse();
        return reflectionTypeSpecsEqual(actual, spec.args.front());
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
    const std::string expectedObjectName = reflectionTypeSpecName(spec);
    return reflectiveObjectMatches(std::get_if<ObjectRef>(&value) ? *std::get_if<ObjectRef>(&value) : ObjectRef{}, expectedObjectName, chunk);
}

bool reflectionTypeSpecsEqual(const ReflectionTypeSpec& a, const ReflectionTypeSpec& b) {
    if (!a.unionMembers.empty() || !b.unionMembers.empty()) {
        if (a.unionMembers.size() != b.unionMembers.size()) return false;
        for (std::size_t i = 0; i < a.unionMembers.size(); ++i) {
            if (!reflectionTypeSpecsEqual(a.unionMembers[i], b.unionMembers[i])) return false;
        }
        return true;
    }
    if (a.name != b.name || a.fixedSize != b.fixedSize || a.args.size() != b.args.size()) return false;
    for (std::size_t i = 0; i < a.args.size(); ++i) {
        if (!reflectionTypeSpecsEqual(a.args[i], b.args[i])) return false;
    }
    return true;
}

bool reflectiveTypeMatchesName(const Value& value, const std::string& typeName, const Chunk* chunk) {
    return reflectiveMatchesSpec(value, ReflectionTypeParser(typeName).parse(), chunk);
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

bool runtimeAssignableToType(const Value& value, const std::string& typeName, const Chunk* chunk) {
    const ReflectionTypeSpec spec = ReflectionTypeParser(typeName).parse();
    // `object` is the language's dynamic annotation; `unknown` is its
    // inferred counterpart. Concrete class names are checked below.
    if (spec.name == "object" || spec.name == "unknown") return true;
    // An enum member is stored as its name string, so a runtime type name of
    // "string" is correct for a parameter or return annotated with the enum.
    // Without this, every call that passes an enum value fails the assertion
    // even though the compile-time check already proved it is a valid member.
    if (chunk && spec.unionMembers.empty() && spec.args.empty() &&
        std::holds_alternative<std::string>(value)) {
        auto enumIt = chunk->classReflection.find(spec.name);
        if (enumIt != chunk->classReflection.end() && enumIt->second.isEnumType) {
            const auto& members = enumIt->second.enumMembers;
            if (members.empty()) return true;
            const auto& member = std::get<std::string>(value);
            return std::find(members.begin(), members.end(), member) != members.end();
        }
    }
    if (!spec.unionMembers.empty()) return reflectiveMatchesSpec(value, spec, chunk);
    if (spec.name == "double" && std::holds_alternative<std::int64_t>(value)) return true;
    if (std::holds_alternative<std::monostate>(value)) {
        const std::string base = spec.name;
        return base == "void" || base == "nil" || base == "string" || base == "list" || base == "map" || base == "set" ||
               base == "array" || base == "func" || base == "object" || base == "Task" ||
               (chunk && chunk->classReflection.count(base) && !chunk->classReflection.at(base).isEnumType);
    }
    return reflectiveMatchesSpec(value, spec, chunk);
}


} // namespace zl
