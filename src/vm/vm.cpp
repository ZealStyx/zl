#include "zl/vm/vm.hpp"
#include "zl/vm/gc.hpp"
#include "zl/vm/gc_safepoint.hpp"

#include <cmath>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <memory>
#include <cstdlib>

#include "zl/vm/native.hpp"
#include "zl/vm/runtime_task.hpp"
#include <algorithm>
#include <unordered_set>

namespace zl {
namespace {

[[noreturn]] void throwNativeError(const std::exception& e,
                                   const Chunk& chunk,
                                   const ExecutionState& state) {
    const std::string rawMessage = e.what();
    const std::string regexPrefix = "RegexError: ";
    const bool isRegexError = rawMessage.rfind(regexPrefix, 0) == 0;
    const std::string className = isRegexError ? "RegexError" : "NativeError";
    const std::string message = isRegexError ? rawMessage.substr(regexPrefix.size()) : rawMessage;
    Value objectValue = makeEmptyObject(className);
    auto object = std::get<ObjectRef>(objectValue);
    object->fields["message"] = message;
    object->fields["stackTrace"] = [&state]() {
        std::string trace;
        for (const auto& name : state.callStackNames()) {
            if (!trace.empty()) trace += "\n";
            trace += "at " + name;
        }
        return trace.empty() ? std::string("at <native>") : trace;
    }();
    auto it = chunk.classReflection.find(className);
    if (it != chunk.classReflection.end()) object->runtimeType = it->second.runtimeType;
    throw ZlThrownException(std::move(object));
}

[[noreturn]] void throwReflectionException(const std::string& message,
                                            const Chunk* chunk,
                                            const ExecutionState* state) {
    std::string className = "ReflectionError";
    std::string clean = message;
    const std::string invalid = "ReflectionError.InvalidArguments: ";
    const std::string access = "ReflectionError.AccessViolation: ";
    if (clean.rfind(invalid, 0) == 0) {
        className = "InvalidArguments";
        clean.erase(0, invalid.size());
    } else if (clean.rfind(access, 0) == 0) {
        className = "AccessViolation";
        clean.erase(0, access.size());
    } else if (clean.rfind("ReflectionError: ", 0) == 0) {
        clean.erase(0, std::string("ReflectionError: ").size());
    }
    Value objectValue = makeEmptyObject(className);
    auto object = std::get<ObjectRef>(objectValue);
    object->fields["message"] = clean;
    std::string trace;
    if (state) {
        for (const auto& name : state->callStackNames()) {
            if (!trace.empty()) trace += "\n";
            trace += "at " + name;
        }
    }
    object->fields["stackTrace"] = trace.empty() ? std::string("at <reflection>") : trace;
    if (chunk) {
        auto it = chunk->classReflection.find(className);
        if (it != chunk->classReflection.end()) object->runtimeType = it->second.runtimeType;
    }
    throw ZlThrownException(std::move(object));
}

} // namespace


VM::VM(std::shared_ptr<RuntimeScheduler> sharedScheduler)
    : ownedScheduler_(sharedScheduler ? nullptr : std::make_shared<RuntimeScheduler>()),
      scheduler_(sharedScheduler ? std::move(sharedScheduler) : ownedScheduler_),
      gcParticipantId_(GCSafepointCoordinator::instance().registerParticipant()) {
    if (const char* env = std::getenv("ZL_MAX_CALL_DEPTH")) {
        try {
            const unsigned long long parsed = std::stoull(env);
            if (parsed > 0 && parsed <= static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
                state_ = ExecutionState(static_cast<std::size_t>(parsed));
            }
        } catch (...) {
            // Invalid configuration falls back to the safe default.
        }
    }
}

VM::~VM() {
    if (gcParticipantId_ != 0) {
        GCSafepointCoordinator::instance().unregisterParticipant(gcParticipantId_);
        gcParticipantId_ = 0;
    }
}


namespace {
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

// Replace whole type-parameter identifier tokens in a rendered type name with
// their concrete instantiations. Generic methods are compiled once with their
// owner's parameters (T, K, V) left in the signature names; at a call the
// receiver carries its concrete instantiation (e.g. "List<string>"), so we
// substitute T->string before asserting. Tokens are matched as whole
// identifiers so `T` does not rewrite letters inside another name.
std::string substituteTypeParams(const std::string& typeName,
                                 const std::vector<std::string>& params,
                                 const std::vector<std::string>& args) {
    if (params.empty() || typeName.empty() || typeName == "unknown" || typeName == "void")
        return typeName;
    std::string out;
    out.reserve(typeName.size());
    for (std::size_t i = 0; i < typeName.size();) {
        const unsigned char c = static_cast<unsigned char>(typeName[i]);
        if (!(std::isalnum(c) || c == '_')) { out.push_back(typeName[i]); ++i; continue; }
        std::size_t start = i;
        while (i < typeName.size()) {
            const unsigned char d = static_cast<unsigned char>(typeName[i]);
            if (!(std::isalnum(d) || d == '_')) break;
            ++i;
        }
        const std::string token = typeName.substr(start, i - start);
        std::string replacement = token;
        for (std::size_t p = 0; p < params.size() && p < args.size(); ++p) {
            if (token == params[p]) { replacement = args[p]; break; }
        }
        out += replacement;
    }
    return out;
}

// Whether a rendered type name still contains an unresolved generic parameter
// token after substitution. A concrete runtime type is a builtin name (int,
// string, ...), a generic-class name, or a class present in the chunk's
// reflection table; anything else left as a bare identifier is a type parameter
// that could not be resolved from the receiver's instantiation (it belongs to a
// different generic class or the receiver is un-instantiated) and must be
// skipped here - the static checker enforces those cases.
bool typeNameHasUnresolvedParam(const std::string& typeName, const Chunk& chunk) {
    if (typeName.empty() || typeName == "unknown" || typeName == "void")
        return false;
    for (std::size_t i = 0; i < typeName.size();) {
        const unsigned char c = static_cast<unsigned char>(typeName[i]);
        if (!(std::isalnum(c) || c == '_')) { ++i; continue; }
        std::size_t start = i;
        while (i < typeName.size()) {
            const unsigned char d = static_cast<unsigned char>(typeName[i]);
            if (!(std::isalnum(d) || d == '_')) break;
            ++i;
        }
        const std::string token = typeName.substr(start, i - start);
        static const std::unordered_set<std::string> builtins = {
            "int", "double", "float", "string", "bool", "void", "nil", "null",
            "func", "unknown", "list", "map", "set", "array", "Task"
        };
        if (builtins.count(token)) continue;
        // Capitalised generic-class names (List, Map, Set, a user class) are
        // concrete; they are always followed by arguments which we also walk.
        if (token == "List" || token == "Map" || token == "Set" || token == "Array" ||
            chunk.classReflection.count(token))
            continue;
        // A bare identifier that is neither a builtin nor a known class is an
        // unresolved type parameter.
        return true;
    }
    return false;
}

// Resolve the concrete type-argument instantiation of a generic-object
// receiver. Returns (parameter names, concrete argument names) for the
// receiver's class, e.g. for a List<string> receiver (["T"], ["string"]).
// Empty vectors for a non-generic or un-instantiated receiver.
std::pair<std::vector<std::string>, std::vector<std::string>>
receiverTypeArgs(const ObjectBox& receiver, const Chunk& chunk) {
    const auto metaIt = chunk.classReflection.find(receiver.className);
    if (metaIt == chunk.classReflection.end() || metaIt->second.typeParameters.empty())
        return {};
    const std::vector<std::string>& params = metaIt->second.typeParameters;
    if (receiver.genericTypeName.empty()) return {};
    const ReflectionTypeSpec spec = ReflectionTypeParser(receiver.genericTypeName).parse();
    if (spec.args.empty()) return {};
    std::vector<std::string> args;
    args.reserve(spec.args.size());
    for (const auto& a : spec.args) args.push_back(reflectionTypeSpecName(a));
    return {params, std::move(args)};
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
            // The parent dispatch name is intentionally erased, so continue to
            // its runtime metadata to keep Base<int> distinguishable from
            // Base<string>. But a real subclass instantiation still has to widen
            // to its parameterized parent - that is the whole point of
            // `class Some<T> extends Option<T>`. Accept it when the object's own
            // instantiation carries the same type arguments as the expected
            // type, which keeps Some<int> -> Option<int> legal while still
            // rejecting Some<string> -> Option<int>. Reuse the reflection type
            // parser rather than hand-scanning the angle brackets here.
            if (expectedIsParameterized) {
                const auto expectedSpec = ReflectionTypeParser(expectedName).parse();
                const auto actualSpec = ReflectionTypeParser(object->genericTypeName).parse();
                // Compare the type arguments only - the base names legitimately
                // differ here (Some vs Option); the parent walk already proved
                // the relationship. Structural/recursive equality means a nested
                // generic (`List<int>` vs `List<string>`) is compared by shape.
                if (!expectedSpec.args.empty() && actualSpec.args.size() == expectedSpec.args.size()) {
                    bool sameArgs = true;
                    for (std::size_t i = 0; i < expectedSpec.args.size(); ++i) {
                        if (!reflectionTypeSpecsEqual(actualSpec.args[i], expectedSpec.args[i])) {
                            sameArgs = false;
                            break;
                        }
                    }
                    if (sameArgs) return true;
                }
            }
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
        const auto actualReturn = ReflectionTypeParser((*closure)->returnTypeName.empty() ? "void" : (*closure)->returnTypeName).parse();
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
    if (name == "Shared") {
        const auto object = std::get_if<ObjectRef>(&value);
        if (!object || !reflectiveObjectMatches(*object, "Shared", chunk)) return false;
        if (spec.args.empty()) return true;
        const auto it = (*object)->fields.find("__value");
        return it != (*object)->fields.end() && reflectiveMatchesSpec(it->second, spec.args.front(), chunk);
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

bool reflectiveTypeMatchesName(const Value& value, const std::string& typeName, const Chunk* chunk = nullptr) {
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
    if (auto obj = std::get_if<ObjectRef>(&value); obj && *obj) return (*obj)->className;
    if (std::holds_alternative<ClosureRef>(value)) return "func";
    if (std::holds_alternative<TaskRef>(value)) return "Task";
    if (std::holds_alternative<ThreadRef>(value)) return "Thread";
    if (std::holds_alternative<NativeHandleRef>(value)) return "NativeHandle";
    return "unknown";
}

bool runtimeAssignableToType(const Value& value, const std::string& typeName, const Chunk* chunk) {
    const ReflectionTypeSpec spec = ReflectionTypeParser(typeName).parse();
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
        return base == "string" || base == "list" || base == "map" || base == "set" ||
               base == "array" || base == "func" || base == "object" || base == "Task" ||
               base.find('<') != std::string::npos;
    }
    return reflectiveMatchesSpec(value, spec, chunk);
}

bool reflectiveTypeMatches(const Value& value, const DispatchType& expected) {
    switch (expected.kind) {
        case DispatchTypeKind::INT: return reflectiveTypeMatchesName(value, "int");
        case DispatchTypeKind::DOUBLE: return reflectiveTypeMatchesName(value, "double");
        case DispatchTypeKind::STRING: return reflectiveTypeMatchesName(value, "string");
        case DispatchTypeKind::BOOL: return reflectiveTypeMatchesName(value, "bool");
        case DispatchTypeKind::VOID_TYPE: return reflectiveTypeMatchesName(value, "void");
        case DispatchTypeKind::LIST: return reflectiveTypeMatchesName(value, "list");
        case DispatchTypeKind::ARRAY: return reflectiveTypeMatchesName(value, "array");
        case DispatchTypeKind::SET: return reflectiveTypeMatchesName(value, "set");
        case DispatchTypeKind::MAP: return reflectiveTypeMatchesName(value, "map");
        case DispatchTypeKind::FUNCTION: return reflectiveTypeMatchesName(value, "func");
        case DispatchTypeKind::GENERIC_OBJECT: return reflectiveTypeMatchesName(value, "object");
        case DispatchTypeKind::OBJECT: return reflectiveTypeMatchesName(value, expected.className);
    }
    return false;
}
}

namespace {
bool isNumeric(const Value& v) { return isNumericValue(v); } // local alias, keeps call sites below unchanged

bool isSubclass(const Chunk& chunk, const std::string& child, const std::string& parent) {
    if (child == parent) return true;
    auto it = chunk.classReflection.find(child);
    std::size_t guard = 0;
    while (it != chunk.classReflection.end() && !it->second.baseClassName.empty() && guard++ < 1024) {
        if (it->second.baseClassName == parent) return true;
        it = chunk.classReflection.find(it->second.baseClassName);
    }
    return false;
}
} // namespace

void VM::pushNativeRoots(const std::vector<Value>& roots) {
    nativeRootFrames_.push_back(roots);
}

void VM::popNativeRoots() {
    if (!nativeRootFrames_.empty()) nativeRootFrames_.pop_back();
}

void VM::appendNativeRoots(std::vector<Value>& roots) const {
    for (const auto& frame : nativeRootFrames_) {
        roots.insert(roots.end(), frame.begin(), frame.end());
    }
}

std::vector<Value> VM::gcRoots() const {
    std::vector<Value> roots;
    state_.appendGCRoots(roots);
    appendNativeRoots(roots);
    if (entryTask_) roots.emplace_back(entryTask_);
    if (asyncInvocation_) {
        roots.insert(roots.end(), asyncInvocation_->args.begin(), asyncInvocation_->args.end());
        if (asyncInvocation_->receiver) roots.push_back(*asyncInvocation_->receiver);
        if (asyncInvocation_->task) roots.emplace_back(asyncInvocation_->task);
        if (asyncInvocation_->awaitedTask) roots.emplace_back(asyncInvocation_->awaitedTask);
    }
    if (pendingResumeException_) {
        try {
            std::rethrow_exception(pendingResumeException_);
        } catch (const ZlThrownException& e) {
            if (e.value()) roots.emplace_back(e.value());
        } catch (...) {
            // Native/runtime exceptions do not retain managed values.
        }
    }
    return roots;
}

void VM::beginBlockingNativeCall() {
    GCSafepointCoordinator::instance().beginBlockingNative(gcParticipantId_, gcRoots());
}

void VM::endBlockingNativeCall() const {
    if (gcParticipantId_ != 0)
        GCSafepointCoordinator::instance().endBlockingNative(gcParticipantId_);
}

Value VM::binaryArith(OpCode op, const Value& a, const Value& b) const {
    // `+` doubles as string concatenation if EITHER side is a string -
    // convenient for building log() messages like "Hello, " + name.
    if (op == OpCode::Add && (std::holds_alternative<std::string>(a) || std::holds_alternative<std::string>(b))) {
        return valueToString(a) + valueToString(b);
    }

    if (!isNumeric(a) || !isNumeric(b)) {
        throw std::runtime_error("arithmetic operators require numbers (or strings, for +)");
    }

    // Integer power stays integral when both operands are integers and the
    // non-negative exponent produces a representable int64 result. Negative
    // or fractional exponents retain the historical floating-point behavior.
    if (op == OpCode::Pow) {
        const bool bothInts = std::holds_alternative<std::int64_t>(a) &&
                              std::holds_alternative<std::int64_t>(b);
        if (bothInts) {
            const auto base = std::get<std::int64_t>(a);
            const auto exponent = std::get<std::int64_t>(b);
            if (exponent >= 0) {
                std::int64_t result = 1;
                std::int64_t factor = base;
                std::uint64_t n = static_cast<std::uint64_t>(exponent);
                auto mulChecked = [](std::int64_t x, std::int64_t y, std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
                    return !__builtin_mul_overflow(x, y, &out);
#else
                    if (x == 0 || y == 0) { out = 0; return true; }
                    if (x == -1) { if (y == std::numeric_limits<std::int64_t>::min()) return false; out = -y; return true; }
                    if (y == -1) { if (x == std::numeric_limits<std::int64_t>::min()) return false; out = -x; return true; }
                    if (x > 0) {
                        if (y > 0) { if (x > std::numeric_limits<std::int64_t>::max() / y) return false; }
                        else { if (y < std::numeric_limits<std::int64_t>::min() / x) return false; }
                    } else {
                        if (y > 0) { if (x < std::numeric_limits<std::int64_t>::min() / y) return false; }
                        else { if (x != 0 && y < std::numeric_limits<std::int64_t>::max() / x) return false; }
                    }
                    out = x * y; return true;
#endif
                };
                bool exact = true;
                while (n != 0) {
                    if (n & 1u) {
                        std::int64_t next = 0;
                        if (!mulChecked(result, factor, next)) { exact = false; break; }
                        result = next;
                    }
                    n >>= 1u;
                    if (n != 0) {
                        std::int64_t next = 0;
                        if (!mulChecked(factor, factor, next)) { exact = false; break; }
                        factor = next;
                    }
                }
                if (exact) return result;
                throw std::runtime_error("integer overflow in exponentiation");
            }
            throw std::runtime_error("negative integer exponent requires floating-point result");
        }
        return std::pow(toDouble(a), toDouble(b));
    }

    // Whole-number math stays whole-number; if either side is a decimal, both promote to double.
    bool bothInts = std::holds_alternative<std::int64_t>(a) && std::holds_alternative<std::int64_t>(b);
    if (bothInts) {
        std::int64_t x = std::get<std::int64_t>(a);
        std::int64_t y = std::get<std::int64_t>(b);
        switch (op) {
            case OpCode::Add: {
                std::int64_t result;
#if defined(__GNUC__) || defined(__clang__)
                if (__builtin_add_overflow(x, y, &result))
                    throw std::runtime_error("integer overflow in addition");
#else
                if ((y > 0 && x > std::numeric_limits<std::int64_t>::max() - y) ||
                    (y < 0 && x < std::numeric_limits<std::int64_t>::min() - y))
                    throw std::runtime_error("integer overflow in addition");
                result = x + y;
#endif
                return result;
            }
            case OpCode::Sub: {
                std::int64_t result;
#if defined(__GNUC__) || defined(__clang__)
                if (__builtin_sub_overflow(x, y, &result))
                    throw std::runtime_error("integer overflow in subtraction");
#else
                if ((y < 0 && x > std::numeric_limits<std::int64_t>::max() + y) ||
                    (y > 0 && x < std::numeric_limits<std::int64_t>::min() + y))
                    throw std::runtime_error("integer overflow in subtraction");
                result = x - y;
#endif
                return result;
            }
            case OpCode::Mul: {
                std::int64_t result;
#if defined(__GNUC__) || defined(__clang__)
                if (__builtin_mul_overflow(x, y, &result))
                    throw std::runtime_error("integer overflow in multiplication");
#else
                if (x != 0 && y != 0) {
                    if ((x == -1 && y == std::numeric_limits<std::int64_t>::min()) ||
                        (y == -1 && x == std::numeric_limits<std::int64_t>::min()))
                        throw std::runtime_error("integer overflow in multiplication");
                    if (x > 0) {
                        if (y > 0 && x > std::numeric_limits<std::int64_t>::max() / y) throw std::runtime_error("integer overflow in multiplication");
                        if (y < 0 && y < std::numeric_limits<std::int64_t>::min() / x) throw std::runtime_error("integer overflow in multiplication");
                    } else {
                        if (y > 0 && x < std::numeric_limits<std::int64_t>::min() / y) throw std::runtime_error("integer overflow in multiplication");
                        if (y < 0 && x < std::numeric_limits<std::int64_t>::max() / y) throw std::runtime_error("integer overflow in multiplication");
                    }
                }
                result = x * y;
#endif
                return result;
            }
            case OpCode::Div:
                if (y == 0) throw std::runtime_error("division by zero");
                if (x == std::numeric_limits<std::int64_t>::min() && y == -1)
                    throw std::runtime_error("integer overflow in division");
                return x / y;
            case OpCode::Mod:
                if (y == 0) throw std::runtime_error("modulo by zero");
                if (x == std::numeric_limits<std::int64_t>::min() && y == -1)
                    return std::int64_t{0};
                return x % y;
            default: break;
        }
    }

    double x = toDouble(a);
    double y = toDouble(b);
    switch (op) {
        case OpCode::Add: return x + y;
        case OpCode::Sub: return x - y;
        case OpCode::Mul: return x * y;
        case OpCode::Div:
            if (y == 0.0) throw std::runtime_error("division by zero");
            return x / y;
        case OpCode::Mod:
            if (y == 0.0) throw std::runtime_error("modulo by zero");
            return std::fmod(x, y);
        default:
            throw std::runtime_error("VM: not an arithmetic opcode");
    }
}

Value VM::binaryBitwise(OpCode op, const Value& a, const Value& b) const {
    const std::int64_t x = toInt64Strict(a);
    const std::int64_t y = toInt64Strict(b);
    switch (op) {
        case OpCode::BitAnd: return x & y;
        case OpCode::BitOr:  return x | y;
        case OpCode::BitXor: return x ^ y;
        case OpCode::Shl: {
            if (y < 0 || y >= 64) throw std::runtime_error("shift count must be in the range 0..63");
            const auto shift = static_cast<unsigned>(y);
            return static_cast<std::int64_t>(static_cast<std::uint64_t>(x) << shift);
        }
        case OpCode::Shr: {
            if (y < 0 || y >= 64) throw std::runtime_error("shift count must be in the range 0..63");
            const auto shift = static_cast<unsigned>(y);
            const std::uint64_t ux = static_cast<std::uint64_t>(x);
            std::uint64_t shifted = ux >> shift;
            if (x < 0 && shift != 0) shifted |= (~std::uint64_t{0}) << (64 - shift);
            return static_cast<std::int64_t>(shifted);
        }
        case OpCode::Ushr: {
            if (y < 0 || y >= 64) throw std::runtime_error("shift count must be in the range 0..63");
            const auto shift = static_cast<unsigned>(y);
            return static_cast<std::int64_t>(static_cast<std::uint64_t>(x) >> shift);
        }
        default:
            throw std::runtime_error("VM: not a bitwise opcode");
    }
}

Value VM::binaryCompare(OpCode op, const Value& a, const Value& b) const {
    if (op == OpCode::Eq || op == OpCode::Neq) {
        bool equal = valuesEqual(a, b);
        return op == OpCode::Eq ? equal : !equal;
    }

    // <, >, <=, >= only make sense for numbers here
    if (!isNumeric(a) || !isNumeric(b)) {
        throw std::runtime_error("comparison operators require numbers");
    }
    const int cmp = compareNumericValues(a, b);
    if (cmp == 2) return false; // NaN: all ordered comparisons are false
    switch (op) {
        case OpCode::Lt:  return cmp < 0;
        case OpCode::Gt:  return cmp > 0;
        case OpCode::Lte: return cmp <= 0;
        case OpCode::Gte: return cmp >= 0;
        default:
            throw std::runtime_error("VM: not a comparison opcode");
    }
}

Value VM::binaryLogical(OpCode op, const Value& a, const Value& b) const {
    // NOTE: the compiler no longer emits OpCode::And/Or - it compiles &&/||
    // with JumpIfFalse/Jump so the right operand is only evaluated when it
    // actually needs to be (see Compiler::compileBinary). This func and
    // the And/Or opcodes are kept only as a defensive fallback for any other
    // bytecode producer; the VM should never hit them from compiled ZL source.
    bool x = isTruthy(a);
    bool y = isTruthy(b);
    return op == OpCode::And ? (x && y) : (x || y);
}

Value VM::unary(OpCode op, const Value& a) const {
    switch (op) {
        case OpCode::Neg:
            if (auto p = std::get_if<std::int64_t>(&a)) {
                if (*p == std::numeric_limits<std::int64_t>::min())
                    throw std::runtime_error("integer overflow in negation");
                return -*p;
            }
            if (auto p = std::get_if<double>(&a)) return -*p;
            throw std::runtime_error("unary '-' requires a number");
        case OpCode::Not:
            return !isTruthy(a);
        case OpCode::BitNot:
            return ~toInt64Strict(a);
        default:
            throw std::runtime_error("VM: not a unary opcode");
    }
}

bool VM::rangeContinue(const Value& current, const Value& end, const Value& step) const {
    if (!isNumeric(current) || !isNumeric(end) || !isNumeric(step)) {
        throw std::runtime_error("for-loop range/step must be numbers");
    }
    if (std::holds_alternative<std::int64_t>(current) &&
        std::holds_alternative<std::int64_t>(end) &&
        std::holds_alternative<std::int64_t>(step)) {
        const auto s = std::get<std::int64_t>(step);
        const auto c = std::get<std::int64_t>(current);
        const auto e = std::get<std::int64_t>(end);
        if (s > 0) return c < e;
        if (s < 0) return c > e;
        throw std::runtime_error("for-loop step cannot be 0 (the loop would never end)");
    }
    double s = toDouble(step);
    if (s > 0.0) return toDouble(current) < toDouble(end);
    if (s < 0.0) return toDouble(current) > toDouble(end);
    throw std::runtime_error("for-loop step cannot be 0 (the loop would never end)");
}

void VM::pumpSchedulerUntilTerminal(const TaskRef& task) {
    // The scheduler is cooperative: pump ready async frames while the task is
    // pending so synchronous code cannot deadlock by blocking the very
    // scheduler that owns the task. If another executor later completes the
    // task externally (a native async op finishing on a worker thread enqueues
    // our continuation), keep pumping rather than sleeping indefinitely while
    // owning the scheduler that must resume it.
    pushNativeRoots({task});
    struct TaskRootGuard {
        VM* vm;
        ~TaskRootGuard() { vm->popNativeRoots(); }
    } taskRootGuard{this};
    BlockingNativeCall blocked(this);
    while (!task->isTerminal()) {
        if (scheduler_->runOne()) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int VM::run(const Chunk& chunk, const std::vector<std::string>& programArgs) {
    const ExecuteStatus result = execute(chunk, 0, false, programArgs, nullptr);
    if (result == ExecuteStatus::Suspended) {
        throw std::runtime_error("VM: top-level execution unexpectedly suspended");
    }
    if (entryTask_ && !entryTask_->isTerminal()) {
        // `main` is an async func and suspended on an await. Draining only the
        // frames that are ready right now is not enough: a native async
        // operation completes on a worker thread and enqueues our continuation
        // later. Pump the scheduler until the entry task settles so the rest of
        // main actually runs.
        pumpSchedulerUntilTerminal(entryTask_);
    }
    {
        BlockingNativeCall blocked(this);
        scheduler_->runUntilIdle();
    }
    // The process-wide heap must never be collected from just this VM's roots
    // while other VMs/threads are still running.
    GCSafepointCoordinator::instance().poll(gcParticipantId_, gcRoots());
    return 0;
}

VM::ExecuteStatus VM::execute(const Chunk& chunk, std::size_t startIp, bool stopAtReturn,
                const std::vector<std::string>& programArgs, Value* returnValue) {
    const Chunk* previousChunk = activeChunk_;
    activeChunk_ = &chunk;
    // execute() is re-entrant (a native can drive a nested execute() for a
    // closure: Mutex.withLock, reflection invoke, thread/task entries). Every
    // exit - normal completion OR an uncaught exception leaving the nested
    // run - must hand the caller's chunk back, otherwise the outer loop resumes
    // the caller's ip against the nested chunk and re-runs code (the
    // exception-in-withLock "continuation runs twice" bug).
    struct ActiveChunkGuard {
        VM* vm;
        const Chunk* previous;
        ~ActiveChunkGuard() { vm->activeChunk_ = previous; }
    } activeChunkGuard{this, previousChunk};
    const std::size_t initialCallDepth = state_.callDepth();
    std::size_t ip = startIp;

    std::size_t instructionsSinceSafePoint = 0;
    while (true) {
      try {
        if (++instructionsSinceSafePoint >= 128 || TracingGC::instance().shouldCollect()) {
            GCSafepointCoordinator::instance().poll(gcParticipantId_, gcRoots());
            instructionsSinceSafePoint = 0;
        }
        if (ip >= chunk.code.size()) throw std::runtime_error("VM: instruction pointer out of bounds");
        const Instruction& instr = chunk.code[ip];

        if (pendingResumeException_) {
            auto error = pendingResumeException_;
            pendingResumeException_ = nullptr;
            std::rethrow_exception(error);
        }

        switch (instr.op) {
            case OpCode::PushConst:
                if (instr.operand >= chunk.constants.size())
                    throw std::runtime_error("VM: constant index out of bounds");
                state_.push(chunk.constants[instr.operand]);
                ip++;
                break;

            case OpCode::PushProgramArgs: {
                Value list = makeEmptyList();
                auto& items = std::get<ListRef>(list)->items;
                for (const auto& arg : programArgs) items.emplace_back(arg);
                state_.push(list);
                ip++;
                break;
            }

            case OpCode::Pop:
                state_.pop();
                ip++;
                break;

            case OpCode::Dup:
                state_.push(state_.top());
                ip++;
                break;

            case OpCode::DefineVar: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: name index out of bounds");
                const std::string& name = chunk.names[instr.operand];
                Value v = state_.pop();
                state_.setVariable(name, v);
                ip++;
                break;
            }

            case OpCode::LoadVar: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: name index out of bounds");
                const std::string& name = chunk.names[instr.operand];
                state_.push(state_.loadVariable(name));
                ip++;
                break;
            }

            case OpCode::MoveVar: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: name index out of bounds");
                const std::string& name = chunk.names[instr.operand];
                Value v = state_.loadVariable(name);
                state_.setVariable(name, Value{});
                state_.push(std::move(v));
                ip++;
                break;
            }

            case OpCode::DropVar: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: name index out of bounds");
                const std::string& name = chunk.names[instr.operand];
                state_.dropLocal(name);
                ip++;
                break;
            }

            case OpCode::Add: case OpCode::Sub: case OpCode::Mul:
            case OpCode::Div: case OpCode::Mod: case OpCode::Pow: {
                Value b = state_.pop();
                Value a = state_.pop();
                state_.push(binaryArith(instr.op, a, b));
                ip++;
                break;
            }

            case OpCode::BitAnd: case OpCode::BitOr: case OpCode::BitXor:
            case OpCode::Shl: case OpCode::Shr: case OpCode::Ushr: {
                Value b = state_.pop();
                Value a = state_.pop();
                state_.push(binaryBitwise(instr.op, a, b));
                ip++;
                break;
            }

            case OpCode::Eq: case OpCode::Neq: case OpCode::Lt:
            case OpCode::Gt: case OpCode::Lte: case OpCode::Gte: {
                Value b = state_.pop();
                Value a = state_.pop();
                state_.push(binaryCompare(instr.op, a, b));
                ip++;
                break;
            }

            case OpCode::And: case OpCode::Or: {
                Value b = state_.pop();
                Value a = state_.pop();
                state_.push(binaryLogical(instr.op, a, b));
                ip++;
                break;
            }

            case OpCode::Neg: case OpCode::Not: case OpCode::BitNot: {
                Value a = state_.pop();
                state_.push(unary(instr.op, a));
                ip++;
                break;
            }

            case OpCode::Jump:
                ip = instr.operand; // no ip++ - we ARE the jump
                break;

            case OpCode::JumpIfFalse: {
                Value cond = state_.pop();
                ip = isTruthy(cond) ? ip + 1 : instr.operand;
                break;
            }

            case OpCode::RangeContinue: {
                Value step = state_.pop();
                Value end = state_.pop();
                Value current = state_.pop();
                state_.push(rangeContinue(current, end, step));
                ip++;
                break;
            }

            case OpCode::CallNative: {
                const auto& natives = nativeFunctionTable();
                if (instr.operand >= natives.size()) throw std::runtime_error("VM: native func index out of bounds");
                const NativeFunction& native = natives[instr.operand];
                const std::size_t arity = native.arity();
                std::vector<Value> args(arity);
                for (std::size_t i = 0; i < arity; ++i) {
                    args[arity - 1 - i] = state_.pop();
                }
                pushNativeRoots(args);
                struct NativeRootGuard {
                    VM* vm;
                    VM* previous;
                    ~NativeRootGuard() {
                        setCurrentNativeVm(previous);
                        vm->popNativeRoots();
                    }
                } nativeRootGuard{this, setCurrentNativeVm(this)};
                try {
                    state_.push(native.fn(args));
                } catch (const ZlThrownException&) {
                    throw;
                } catch (const SystemExitException&) {
                    throw;
                } catch (const std::exception& e) {
                    const std::string message = e.what();
                    if (message.rfind("Reflection.", 0) == 0) {
                        throwReflectionException("ReflectionError: " + message, &chunk, &state_);
                    }
                    throwNativeError(e, chunk, state_);
                }
                ip++;
                break;
            }

            case OpCode::TaskBlock: {
                Value value = state_.pop();
                auto* taskRef = std::get_if<TaskRef>(&value);
                if (!taskRef || !(*taskRef)) {
                    throw std::runtime_error("VM: Task.block() expects a Task value");
                }
                // Pump the cooperative scheduler until the task settles; once
                // terminal, observe() collects its result (and safely waits on
                // the task condition variable if an external executor is still
                // finalising it).
                pumpSchedulerUntilTerminal(*taskRef);
                state_.push((*taskRef)->observe());
                ++ip;
                break;
            }

            case OpCode::TaskIgnore: {
                Value value = state_.pop();
                auto* taskRef = std::get_if<TaskRef>(&value);
                if (!taskRef || !(*taskRef)) {
                    throw std::runtime_error("VM: Task.ignore() expects a Task value");
                }
                (*taskRef)->ignore();
                state_.push(Value{});
                ++ip;
                break;
            }

            case OpCode::MatchType: {
                Value expectedValue = state_.pop();
                Value value = state_.pop();
                auto name = std::get_if<std::string>(&expectedValue);
                bool matches = false;
                if (name) {
                    if (*name == "int") matches = std::holds_alternative<std::int64_t>(value);
                    else if (*name == "double") matches = std::holds_alternative<double>(value) || std::holds_alternative<std::int64_t>(value);
                    else if (*name == "string") matches = std::holds_alternative<std::string>(value);
                    else if (*name == "bool") matches = std::holds_alternative<bool>(value);
                    else if (*name == "func") matches = std::holds_alternative<ClosureRef>(value);
                    else if (*name == "list" || *name == "array" || *name == "set") matches = std::holds_alternative<ListRef>(value);
                    else if (*name == "map") matches = std::holds_alternative<MapRef>(value);
                    else if (*name == "void") matches = std::holds_alternative<std::monostate>(value);
                    else if (auto obj = std::get_if<ObjectRef>(&value); obj && *obj) {
                        matches = reflectiveObjectMatches(*obj, *name, activeChunk_);
                    }
                }
                state_.push(matches);
                ++ip;
                break;
            }

            case OpCode::AssertType: {
                Value value = state_.pop();
                if (instr.operand >= chunk.names.size()) {
                    throw std::runtime_error("VM: AssertType name index out of bounds");
                }
                const std::string& expected = chunk.names[instr.operand];
                if (!runtimeAssignableToType(value, expected, &chunk)) {
                    throw std::runtime_error("type assertion failed: expected " + expected + ", got " + runtimeValueTypeName(value));
                }
                state_.push(value);
                ++ip;
                break;
            }

            case OpCode::TaskCancel: {
                Value value = state_.pop();
                auto* taskRef = std::get_if<TaskRef>(&value);
                if (!taskRef || !(*taskRef)) {
                    throw std::runtime_error("VM: Task.cancel() expects a Task value");
                }
                const TaskStatus status = (*taskRef)->status();
                if (status == TaskStatus::Pending) {
                    (*taskRef)->cancel();
                } else if (status == TaskStatus::Running) {
                    (*taskRef)->requestCancellation();
                }
                state_.push(Value{});
                ++ip;
                break;
            }

            case OpCode::Await: {
                Value awaited = state_.pop();
                auto* taskRef = std::get_if<TaskRef>(&awaited);
                if (!taskRef || !(*taskRef)) {
                    throw std::runtime_error("VM: await expects a Task value");
                }
                if (asyncInvocation_ && asyncInvocation_->task && asyncInvocation_->task->cancellationRequested()) {
                    throw ZlThrownException([&]() {
                        Value object = makeEmptyObject("CancellationException");
                        auto ex = std::get<ObjectRef>(object);
                        ex->fields["message"] = std::string("task was cancelled");
                        return ex;
                    }());
                }
                const TaskStatus status = (*taskRef)->status();
                if (status == TaskStatus::Succeeded || status == TaskStatus::Failed || status == TaskStatus::Cancelled) {
                    state_.push((*taskRef)->observe());
                    ++ip;
                    break;
                }
                if (!state_.inFunction()) {
                    throw std::runtime_error("VM: await used outside a function");
                }
                if (!asyncInvocation_ || !asyncInvocation_->task) {
                    throw std::runtime_error("VM: await used outside an async invocation");
                }
                auto awaitedTask = *taskRef;
                if (asyncInvocation_->task.get() == awaitedTask.get()) {
                    throw std::runtime_error("VM: async func cannot await its own Task");
                }
                asyncInvocation_->resumeIp = ip + 1;
                asyncInvocation_->awaitedTask = awaitedTask;
                auto vmSelf = shared_from_this();
                asyncInvocation_->task->onCancellation([vmSelf]() {
                    vmSelf->scheduler_->enqueue(std::make_shared<AsyncFrame>([vmSelf]() {
                        vmSelf->resumeAsyncInvocation();
                    }));
                });
                awaitedTask->then([vmSelf, awaitedTask]() {
                    vmSelf->scheduler_->enqueue(std::make_shared<AsyncFrame>([vmSelf, awaitedTask]() {
                        // Resume owns reactivation and restores the awaited value
                        // only after the collector has released this parked VM.
                        if (vmSelf->asyncInvocation_ && vmSelf->asyncInvocation_->awaitedTask == awaitedTask)
                            vmSelf->resumeAsyncInvocation();
                    }));
                });
                return ExecuteStatus::Suspended;
            }

            case OpCode::Call: {
                if (instr.operand >= chunk.functions.size())
                    throw std::runtime_error("VM: func index out of bounds");
                const FunctionInfo& fn = chunk.functions[instr.operand];
                std::size_t paramCount = fn.paramNames.size();

                std::vector<Value> args(paramCount);
                for (std::size_t i = 0; i < paramCount; ++i) {
                    args[paramCount - 1 - i] = state_.pop();
                }
                for (std::size_t i = 0; i < paramCount && i < fn.parameterTypeNames.size(); ++i) {
                    const auto& expected = fn.parameterTypeNames[i];
                    // Skip an unresolved generic-parameter signature (a bare
                    // self-call inside generic code carries no concrete receiver
                    // here); concrete types are enforced.
                    if (!expected.empty() && expected != "unknown" &&
                        !typeNameHasUnresolvedParam(expected, chunk) &&
                        !runtimeAssignableToType(args[i], expected, &chunk)) {
                        throw std::runtime_error("type assertion failed for argument " + std::to_string(i + 1) + ": expected " + expected + ", got " + runtimeValueTypeName(args[i]));
                    }
                }
                if (fn.isAsync) {
                    auto task = std::make_shared<RuntimeTaskState>(fn.returnTypeName.empty() ? "void" : fn.returnTypeName);
                    auto chunkRef = std::make_shared<Chunk>(chunk);
                    std::optional<Value> receiver;
                    if (state_.inFunction()) {
                        auto& callerLocals = state_.currentFrame().locals;
                        auto thisIt = callerLocals.find("this");
                        if (thisIt != callerLocals.end()) receiver = thisIt->second;
                    }
                    auto child = std::make_shared<VM>(scheduler_);
                    child->beginAsyncInvocation(chunkRef, instr.operand, std::move(args), receiver, task);
                    scheduler_->enqueue(std::make_shared<AsyncFrame>([child]() mutable { child->resumeAsyncInvocation(); }));
                    if (!state_.inFunction() && !entryTask_) entryTask_ = task;
                    state_.push(Value{std::move(task)});
                    ip++;
                    break;
                }

                ExecutionState::CallFrame frame;
                frame.returnIp = ip + 1;
                frame.functionName = fn.name;
                frame.ownedLocalNames = fn.ownedLocalNames;
                // `foo(x)` (a bare same-class call, as opposed to
                // `this.foo(x)`) is, by construction, always made from
                // inside some method/constructor of some class - so the
                // calling frame's own `this` (if it has one) needs to
                // propagate into the callee's frame, exactly like
                // InvokeMethod/InvokeSuper already do, or `this.field`
                // reads/writes and further bare self-calls inside the
                // callee crash with "undefined variable 'this'". The one
                // caller that legitimately has no `this` to propagate is
                // the program's own top-level entry call into main() -
                // state_.callStack_ is empty there, so this is a no-op for it,
                // which is correct: main() runs with no receiver object.
                if (state_.inFunction()) {
                    auto& callerLocals = state_.currentFrame().locals;
                    auto thisIt = callerLocals.find("this");
                    if (thisIt != callerLocals.end()) {
                        frame.locals["this"] = thisIt->second;
                    }
                }
                for (std::size_t i = 0; i < paramCount; ++i) {
                    frame.locals[fn.paramNames[i]] = args[i];
                }
                state_.enterFrame(std::move(frame));
                ip = fn.entryAddress;
                break;
            }

            case OpCode::MakeClosure: {
                if (instr.operand >= chunk.functions.size())
                    throw std::runtime_error("VM: closure func index out of bounds");
                const FunctionInfo& fn = chunk.functions[instr.operand];
                auto box = makeGCClosure();
                box->functionName = fn.name;
                box->paramNames = fn.paramNames;
                box->parameterTypeNames = fn.parameterTypeNames;
                box->returnTypeName = fn.returnTypeName;
                box->isAsync = fn.isAsync;
                box->isNative = fn.isNative;
                box->entryAddress = fn.entryAddress;
                box->functionIndex = instr.operand;
                box->chunk = std::make_shared<Chunk>(chunk);
                const auto scope = state_.snapshotScope();
                if (!fn.capturesEvaluationScope) {
                    // Named function values have no lexical environment.
                } else {
                    // captureNames is computed for every lambda by
                    // TypeChecker::analyzeLambdaCaptures, so an empty list means
                    // the body references nothing - capture nothing. Snapshotting
                    // the whole scope instead made a closure that used no
                    // variables carry every local in the enclosing function,
                    // which then failed the Shared<T> check on Thread.start even
                    // for locals it never touched.
                    for (const auto& name : fn.captureNames) {
                        auto it = scope.find(name);
                        if (it != scope.end()) box->captured.emplace(name, it->second);
                    }
                }
                state_.push(Value{std::move(box)});
                ip++;
                break;
            }

            case OpCode::CallValue: {
                std::size_t argCount = instr.operand2;
                Value callee = state_.pop();
                auto* closureRef = std::get_if<ClosureRef>(&callee);
                if (!closureRef || !(*closureRef)) {
                    throw std::runtime_error("VM: attempted to call a non-func value");
                }
                const ClosureBox& closure = **closureRef;
                if (closure.paramNames.size() != argCount) {
                    throw std::runtime_error(
                        "VM: closure expects " + std::to_string(closure.paramNames.size()) +
                        " argument(s), got " + std::to_string(argCount));
                }

                std::vector<Value> args(argCount);
                for (std::size_t i = 0; i < argCount; ++i) {
                    args[argCount - 1 - i] = state_.pop();
                }
                for (std::size_t i = 0; i < argCount && i < closure.parameterTypeNames.size(); ++i) {
                    const auto& expected = closure.parameterTypeNames[i];
                    if (!expected.empty() && expected != "unknown" &&
                        !typeNameHasUnresolvedParam(expected, *closure.chunk) &&
                        !runtimeAssignableToType(args[i], expected, closure.chunk.get())) {
                        throw std::runtime_error("type assertion failed for argument " + std::to_string(i + 1) + ": expected " + expected + ", got " + runtimeValueTypeName(args[i]));
                    }
                }

                if (closure.isAsync) {
                    auto task = std::make_shared<RuntimeTaskState>(closure.returnTypeName.empty() ? "void" : closure.returnTypeName);
                    auto child = std::make_shared<VM>(scheduler_);
                    auto chunkRef = std::make_shared<Chunk>(*closure.chunk);
                    child->beginAsyncInvocation(chunkRef, closure.functionIndex, std::move(args), std::nullopt, task);
                    scheduler_->enqueue(std::make_shared<AsyncFrame>([child]() mutable { child->resumeAsyncInvocation(); }));
                    state_.push(Value{std::move(task)});
                    ip++;
                    break;
                }

                ExecutionState::CallFrame frame;
                frame.returnIp = ip + 1;
                frame.functionName = "<lambda>";
                if (closure.functionIndex < closure.chunk->functions.size())
                    frame.ownedLocalNames = closure.chunk->functions[closure.functionIndex].ownedLocalNames;
                // The closure's OWN captured-scope snapshot, not the
                // CALLER's locals - this is what makes capture-by-value
                // actually stick (the lambda body sees the scope it was
                // CREATED in, not the scope it's CALLED from).
                frame.locals = closure.captured;
                for (std::size_t i = 0; i < argCount; ++i) {
                    frame.locals[closure.paramNames[i]] = args[i];
                }
                frame.activeClosure = *closureRef; // see Return's write-back
                state_.enterFrame(std::move(frame));
                ip = closure.entryAddress;
                break;
            }

            case OpCode::NewObject: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: invalid class-name index");
                const std::string& className = chunk.names[instr.operand];
                Value obj = makeEmptyObject(className);
                if (std::holds_alternative<ObjectRef>(obj)) {
                    auto objectRef = std::get<ObjectRef>(obj);
                    auto metaIt = chunk.classReflection.find(className);
                    if (metaIt != chunk.classReflection.end()) {
                        objectRef->runtimeType = metaIt->second.runtimeType;
                    }
                    if (instr.operand3 != Chunk::INVALID_FUNCTION_INDEX && instr.operand3 < chunk.names.size()) {
                        objectRef->genericTypeName = chunk.names[instr.operand3];
                    }
                }
                state_.push(std::move(obj));
                ip++;
                break;
            }

            case OpCode::CopyObject: {
                Value source = state_.pop();
                if (!std::holds_alternative<ObjectRef>(source) || !std::get<ObjectRef>(source)) {
                    throw std::runtime_error("CopyObject expected object");
                }
                ObjectRef src = std::get<ObjectRef>(source);
                auto copy = makeGCObject();
                copy->genericTypeName = src->genericTypeName;
                copy->className = src->className;
                copy->runtimeType = src->runtimeType;
                copy->fields = src->fields;
                state_.push(ObjectRef(copy));
                ip++;
                break;
            }
            case OpCode::GetStaticField: {
                if (instr.operand >= chunk.names.size() || instr.operand2 >= chunk.names.size())
                    throw std::runtime_error("VM: invalid static field metadata index");
                const std::string key = chunk.names[instr.operand] + "." + chunk.names[instr.operand2];
                auto metaIt = chunk.staticFields.find(key);
                if (metaIt == chunk.staticFields.end()) throw std::runtime_error("VM: unknown static field '" + key + "'");
                const auto& meta = metaIt->second;
                std::shared_ptr<StaticFieldState> state;
                {
                    std::lock_guard<std::mutex> mapLock(chunk.staticStorage->mapMutex);
                    auto& slot = chunk.staticStorage->fields[key];
                    if (!slot) {
                        slot = std::make_shared<StaticFieldState>();
                        slot->initializerFunction = meta.initializerFunction;
                    }
                    state = slot;
                }
                for (;;) {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    if (state->status == StaticFieldState::Status::Initialized) {
                        state_.push(state->value);
                        break;
                    }
                    if (state->status == StaticFieldState::Status::Failed) {
                        std::rethrow_exception(state->failure);
                    }
                    if (state->status == StaticFieldState::Status::Initializing) {
                        if (state->ownerThread == std::this_thread::get_id()) {
                            throw std::runtime_error("StaticInitializationError: re-entrant initialization of '" + key + "'");
                        }
                        state->cv.wait(lock, [&] { return state->status != StaticFieldState::Status::Initializing; });
                        continue;
                    }
                    state->status = StaticFieldState::Status::Initializing;
                    state->ownerThread = std::this_thread::get_id();
                    lock.unlock();
                    try {
                        Value result = invokeFunction(chunk, state->initializerFunction, {}, std::nullopt);
                        lock.lock();
                        state->value = result;
                        state->status = StaticFieldState::Status::Initialized;
                        state->ownerThread = {};
                        lock.unlock();
                        state->cv.notify_all();
                        state_.push(std::move(result));
                    } catch (...) {
                        auto failure = std::current_exception();
                        lock.lock();
                        state->failure = failure;
                        state->status = StaticFieldState::Status::Failed;
                        state->ownerThread = {};
                        lock.unlock();
                        state->cv.notify_all();
                        std::rethrow_exception(failure);
                    }
                    break;
                }
                ip++;
                break;
            }

            case OpCode::SetStaticField: {
                if (instr.operand >= chunk.names.size() || instr.operand2 >= chunk.names.size())
                    throw std::runtime_error("VM: invalid static field metadata index");
                const std::string key = chunk.names[instr.operand] + "." + chunk.names[instr.operand2];
                auto value = state_.pop();
                auto metaIt = chunk.staticFields.find(key);
                if (metaIt == chunk.staticFields.end()) throw std::runtime_error("VM: unknown static field '" + key + "'");
                std::shared_ptr<StaticFieldState> state;
                {
                    std::lock_guard<std::mutex> mapLock(chunk.staticStorage->mapMutex);
                    auto& slot = chunk.staticStorage->fields[key];
                    if (!slot) { slot = std::make_shared<StaticFieldState>(); slot->initializerFunction = metaIt->second.initializerFunction; }
                    state = slot;
                }
                std::unique_lock<std::mutex> lock(state->mutex);
                if (state->status == StaticFieldState::Status::Initializing && state->ownerThread != std::this_thread::get_id())
                    state->cv.wait(lock, [&] { return state->status != StaticFieldState::Status::Initializing; });
                state->value = value;
                state->status = StaticFieldState::Status::Initialized;
                state->failure = nullptr;
                state->ownerThread = {};
                lock.unlock();
                state->cv.notify_all();
                state_.push(std::move(value));
                ip++;
                break;
            }

            case OpCode::GetIndex: {
                Value indexValue = state_.pop();
                Value object = state_.pop();
                ListRef list;
                if (const auto* listRef = std::get_if<ListRef>(&object)) {
                    list = *listRef;
                } else if (const auto* objRef = std::get_if<ObjectRef>(&object); objRef && *objRef) {
                    auto it = (*objRef)->fields.find("__native");
                    if (it != (*objRef)->fields.end()) {
                        if (const auto* nativeList = std::get_if<ListRef>(&it->second)) list = *nativeList;
                    }
                }
                if (!list) {
                    throw std::runtime_error("VM: indexed access requires a list");
                }
                if (!std::holds_alternative<std::int64_t>(indexValue)) throw std::runtime_error("VM: list index must be an int");
                const auto index = std::get<std::int64_t>(indexValue);
                const std::size_t logicalSize = list->items.size() - std::min(list->frontIndex, list->items.size());
                if (index < 0 || static_cast<std::size_t>(index) >= logicalSize) throw std::runtime_error("VM: list index out of bounds");
                state_.push(list->items[list->frontIndex + static_cast<std::size_t>(index)]);
                ++ip;
                break;
            }

            case OpCode::GetField: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: field-name index out of bounds");
                const std::string& fieldName = chunk.names[instr.operand];
                Value object = state_.pop();
                auto* objRef = std::get_if<ObjectRef>(&object);
                if (!objRef || !(*objRef)) {
                    throw std::runtime_error("VM: cannot read field '" + fieldName + "' of non-object (or nil)");
                }
                
                auto& fields = (*objRef)->fields;
                auto it = fields.find(fieldName);
                if (it != fields.end()) {
                    state_.push(it->second);
                } else {
                    // Field isn't present - currently defaults to nil
                    state_.push(Value{}); 
                }
                ip++;
                break;
            }

            case OpCode::SetField: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: field-name index out of bounds");
                const std::string& fieldName = chunk.names[instr.operand];
                Value value = state_.pop();
                Value object = state_.pop();
                auto* objRef = std::get_if<ObjectRef>(&object);
                if (!objRef || !(*objRef)) {
                    throw std::runtime_error("VM: cannot write field '" + fieldName + "' of non-object (or nil)");
                }
                
                (*objRef)->fields[fieldName] = value;
                state_.push(value); // assignment leaves the value on the stack
                ip++;
                break;
            }

            case OpCode::InvokeMethod: {
                const std::size_t methodSlotIndex = instr.operand;
                const std::size_t argCount = instr.operand2;

                std::vector<Value> args(argCount);
                for (std::size_t i = 0; i < argCount; ++i) {
                    args[argCount - 1 - i] = state_.pop();
                }

                Value object = state_.pop();
                auto* objRef = std::get_if<ObjectRef>(&object);
                if (!objRef || !(*objRef)) {
                    throw std::runtime_error("VM: cannot invoke method on non-object (or nil)");
                }

                auto vtableIt = chunk.classVTables.find((*objRef)->className);
                if (vtableIt == chunk.classVTables.end() || methodSlotIndex >= vtableIt->second.size()) {
                    throw std::runtime_error("VM: no method slot " + std::to_string(methodSlotIndex) +
                                             " for class '" + (*objRef)->className + "'");
                }

                const std::size_t functionIndex = vtableIt->second[methodSlotIndex];
                if (functionIndex == Chunk::INVALID_FUNCTION_INDEX || functionIndex >= chunk.functions.size()) {
                    throw std::runtime_error("VM: class '" + (*objRef)->className +
                                             "' does not implement method slot " + std::to_string(methodSlotIndex));
                }

                const FunctionInfo& fn = chunk.functions[functionIndex];
                if (fn.paramNames.size() != argCount) {
                    throw std::runtime_error(
                        "VM: method '" + fn.name + "' expects " + std::to_string(fn.paramNames.size()) +
                        " argument(s), got " + std::to_string(argCount));
                }
                // A generic method is compiled once with its owner's type
                // parameters left in the signature names (e.g. List.push(T)).
                // Substitute the receiver's concrete instantiation (e.g.
                // List<string> -> T=string) before enforcing, so a wrong-typed
                // write is caught at the boundary even where the static
                // checker's element type was erased.
                const auto [typeParams, typeArgs] = receiverTypeArgs(**objRef, chunk);
                for (std::size_t i = 0; i < argCount && i < fn.parameterTypeNames.size(); ++i) {
                    const std::string raw = fn.parameterTypeNames[i];
                    const std::string expected = substituteTypeParams(raw, typeParams, typeArgs);
                    // Skip when the assertion type still names an unresolved
                    // generic parameter (an un-instantiated receiver, or a
                    // parameter owned by a different generic class than the
                    // receiver - e.g. List.push(T) on a List<K> built inside
                    // Map.keys()); the static checker enforces those. Concrete
                    // types are enforced here.
                    if (!expected.empty() && expected != "unknown" &&
                        !typeNameHasUnresolvedParam(expected, chunk) &&
                        !runtimeAssignableToType(args[i], expected, &chunk)) {
                        throw std::runtime_error("type assertion failed for argument " + std::to_string(i + 1) + ": expected " + expected + ", got " + runtimeValueTypeName(args[i]));
                    }
                }

                if (fn.isAsync) {
                    auto task = std::make_shared<RuntimeTaskState>(fn.returnTypeName.empty() ? "void" : fn.returnTypeName);
                    auto chunkRef = std::make_shared<Chunk>(chunk);
                    auto child = std::make_shared<VM>(scheduler_);
                    child->beginAsyncInvocation(chunkRef, functionIndex, std::move(args), object, task);
                    scheduler_->enqueue(std::make_shared<AsyncFrame>([child]() mutable { child->resumeAsyncInvocation(); }));
                    state_.push(Value{std::move(task)});
                    ip++;
                    break;
                }

                ExecutionState::CallFrame frame;
                frame.returnIp = ip + 1;
                frame.functionName = fn.name;
                frame.ownedLocalNames = fn.ownedLocalNames;
                frame.locals["this"] = object;
                for (std::size_t i = 0; i < argCount; ++i) {
                    frame.locals[fn.paramNames[i]] = args[i];
                }
                state_.enterFrame(std::move(frame));
                ip = fn.entryAddress;
                break;
            }

            case OpCode::InvokeSuper: {
                if (instr.operand >= chunk.functions.size())
                    throw std::runtime_error("VM: super func index out of bounds");
                const std::size_t argCount = instr.operand2;
                const FunctionInfo& fn = chunk.functions[instr.operand];

                std::vector<Value> args(argCount);
                for (std::size_t i = 0; i < argCount; ++i) {
                    args[argCount - 1 - i] = state_.pop();
                }

                Value object = state_.pop();
                auto* objRef = std::get_if<ObjectRef>(&object);
                if (!objRef || !(*objRef)) {
                    throw std::runtime_error("VM: cannot call super func on non-object (or nil)");
                }
                if (fn.paramNames.size() != argCount) {
                    throw std::runtime_error(
                        "VM: super func '" + fn.name + "' expects " + std::to_string(fn.paramNames.size()) +
                        " argument(s), got " + std::to_string(argCount));
                }

                if (fn.isAsync) {
                    auto task = std::make_shared<RuntimeTaskState>(fn.returnTypeName.empty() ? "void" : fn.returnTypeName);
                    auto chunkRef = std::make_shared<Chunk>(chunk);
                    auto child = std::make_shared<VM>(scheduler_);
                    child->beginAsyncInvocation(chunkRef, instr.operand, std::move(args), object, task);
                    scheduler_->enqueue(std::make_shared<AsyncFrame>([child]() mutable { child->resumeAsyncInvocation(); }));
                    state_.push(Value{std::move(task)});
                    ip++;
                    break;
                }

                ExecutionState::CallFrame frame;
                frame.returnIp = ip + 1;
                frame.functionName = fn.name;
                frame.ownedLocalNames = fn.ownedLocalNames;
                // 'this' still refers to the SAME receiver as the calling
                // method/constructor - super.method() runs the parent's
                // implementation against the full (child) object, exactly
                // like Java.
                frame.locals["this"] = object;
                for (std::size_t i = 0; i < argCount; ++i) {
                    frame.locals[fn.paramNames[i]] = args[i];
                }
                state_.enterFrame(std::move(frame));
                ip = fn.entryAddress;
                break;
            }

            case OpCode::Return: {
                Value resultValue = state_.pop();
                if (!state_.inFunction()) {
                    throw std::runtime_error("'return' used outside of a func");
                }
                ExecutionState::CallFrame finishedFrame = state_.leaveFrame();
                if (stopAtReturn && state_.callDepth() < initialCallDepth) {
                    if (returnValue) *returnValue = resultValue;
                    return ExecuteStatus::Completed;
                }
                // A handler installed inside the func that just returned

                // can no longer legally catch exceptions from its caller.
                state_.discardHandlersForFinishedFrames();
                // Write this call's (possibly mutated) locals back into the
                // closure it belongs to, so a closure's own mutations to its
                // captured variables persist the next time IT is called -
                // see CallFrame::activeClosure's comment.
                if (finishedFrame.activeClosure) {
                    finishedFrame.activeClosure->captured = std::move(finishedFrame.locals);
                }
                finishedFrame.dropOwnedLocals();
                state_.push(resultValue);
                ip = finishedFrame.returnIp;
                break;
            }

            case OpCode::PushHandler: {
                std::string catchClassName;
                if (instr.operand2 != 0) {
                    const std::size_t nameIndex = instr.operand2 - 1;
                    if (nameIndex >= chunk.names.size()) throw std::runtime_error("VM: catch type name index out of bounds");
                    catchClassName = chunk.names[nameIndex];
                }
                state_.pushHandler(instr.operand, std::move(catchClassName), false, instr.operand3);
                ip++;
                break;
            }

            case OpCode::PushFinallyHandler:
                state_.pushHandler(instr.operand, {}, true);
                ip++;
                break;

            case OpCode::PopHandler:
                if (state_.hasHandler()) state_.popHandler();
                ip++;
                break;

            case OpCode::Throw: {
                Value thrown = state_.pop();
                if (!std::holds_alternative<ObjectRef>(thrown) || !std::get<ObjectRef>(thrown)) {
                    throw std::runtime_error("throw requires an Exception-derived object");
                }
                auto ex = std::get<ObjectRef>(std::move(thrown));
                if (ex->fields.find("stackTrace") != ex->fields.end() || ex->className == "Exception" || isSubclass(chunk, ex->className, "Exception")) {
                    std::string trace;
                    for (const auto& name : state_.callStackNames()) {
                        if (!trace.empty()) trace += "\n";
                        trace += "at " + name;
                    }
                    if (trace.empty()) trace = "at <runtime>";
                    ex->fields["stackTrace"] = trace;
                }
                throw ZlThrownException(std::move(ex));
            }

            case OpCode::Log:
                std::cout << valueToString(state_.pop()) << "\n";
                ip++;
                break;

            case OpCode::Halt:
                return ExecuteStatus::Completed;
        }
      } catch (const ZlThrownException& e) {
          // execute() is re-entrant: a nested run (a closure driven from a
          // native, started at initialCallDepth) must only consume handlers
          // pushed within THAT nested run. Handlers owned by the caller frame
          // (callStackSize < initialCallDepth) belong to the outer run; letting
          // the nested run catch on them resumes the caller's catchIp against
          // the closure's chunk and re-runs the caller (the exception-in-
          // withLock double-continuation bug). If no in-scope handler matches,
          // rethrow so the outer run handles it in its own chunk.
          bool matched = false;
          ExecutionState::Handler h{};
          while (state_.hasHandler() && state_.topHandlerCallDepth() >= initialCallDepth) {
              h = state_.popHandler();
              if (h.catchClassName.empty() ||
                  e.value()->className == h.catchClassName ||
                  chunk.classReflection.count(e.value()->className) &&
                      isSubclass(chunk, e.value()->className, h.catchClassName)) {
                  matched = true;
                  break;
              }
          }
          if (!matched) throw;
          state_.unwindTo(h);
          state_.removeHandlerGroup(h.groupId);
          if (h.rethrowAfterHandler) {
              state_.push(Value(e.value()));
          } else if (h.catchClassName.empty()) {
              state_.push(Value(e.value()->fields.count("message") ? e.value()->fields.at("message") : valueToString(Value(e.value()))));
          } else {
              state_.push(Value(e.value()));
          }
          ip = h.catchIp;
      } catch (const std::runtime_error& e) {
          // Same nested-run scoping as the ZlThrownException handler above:
          // only a catch-all handler owned by this (nested) run applies.
          if (!state_.hasHandler() || state_.topHandlerCallDepth() < initialCallDepth) throw;
          ExecutionState::Handler h = state_.popHandler();
          if (!h.catchClassName.empty()) throw;
          state_.unwindTo(h);
          state_.removeHandlerGroup(h.groupId);
          state_.push(Value(std::string(e.what())));
          ip = h.catchIp;
      }
    }
}

void VM::beginAsyncInvocation(std::shared_ptr<const Chunk> chunk, std::size_t functionIndex,
                               std::vector<Value> args, std::optional<Value> receiver, TaskRef task) {
    if (asyncInvocation_) throw std::logic_error("VM: async invocation already active");
    asyncInvocation_ = AsyncInvocation{std::move(chunk), functionIndex, std::move(args), std::move(receiver), std::move(task), 0, false};
    // A queued/suspended VM owns roots but is not a running mutator.
    beginBlockingNativeCall();
}

void VM::resumeAsyncInvocation() {
    if (!asyncInvocation_) return;
    endBlockingNativeCall();
    struct SuspendGuard {
        VM* vm;
        ~SuspendGuard() { vm->beginBlockingNativeCall(); }
    } suspendGuard{this};
    try {
        if (asyncInvocation_->awaitedTask && asyncInvocation_->awaitedTask->isTerminal()) {
            try {
                state_.push(asyncInvocation_->awaitedTask->observe());
            } catch (...) {
                pendingResumeException_ = std::current_exception();
            }
            asyncInvocation_->awaitedTask.reset();
        }
        if (!asyncInvocation_->started) {
            const TaskStatus taskStatus = asyncInvocation_->task->status();
            if (taskStatus == TaskStatus::Cancelled) {
                asyncInvocation_.reset();
                return;
            }
            if (taskStatus != TaskStatus::Pending) {
                throw std::logic_error("VM: invalid initial async task state");
            }
            asyncInvocation_->task->start();
            const auto& fn = asyncInvocation_->chunk->functions[asyncInvocation_->functionIndex];
            ExecutionState::CallFrame frame;
            frame.returnIp = asyncInvocation_->chunk->code.size();
            frame.functionName = asyncInvocation_->chunk->functions[asyncInvocation_->functionIndex].name;
            if (asyncInvocation_->receiver) frame.locals["this"] = *asyncInvocation_->receiver;
            for (std::size_t i = 0; i < asyncInvocation_->args.size(); ++i) {
                frame.locals[fn.paramNames[i]] = asyncInvocation_->args[i];
            }
            state_.enterFrame(std::move(frame));
            asyncInvocation_->resumeIp = fn.entryAddress;
            asyncInvocation_->started = true;
        }
        if (asyncInvocation_->task->cancellationRequested()) {
            Value object = makeEmptyObject("CancellationException");
            auto ex = std::get<ObjectRef>(object);
            ex->fields["message"] = std::string("task was cancelled");
            pendingResumeException_ = std::make_exception_ptr(ZlThrownException(std::move(ex)));
        }
        Value result;
        const ExecuteStatus status = execute(*asyncInvocation_->chunk, asyncInvocation_->resumeIp, true, {}, &result);
        if (status == ExecuteStatus::Completed) {
            auto task = asyncInvocation_->task;
            asyncInvocation_.reset();
            task->succeed(std::move(result));
        }
    } catch (const ZlThrownException& e) {
        auto task = asyncInvocation_->task;
        asyncInvocation_.reset();
        // Cancellation/terminal completion may win a race after the VM began
        // resuming this frame. Never turn that stale completion into a second
        // terminal transition (and never leak its logic_error to the scheduler).
        if (task->isTerminal()) return;
        if (e.value() && e.value()->className == "CancellationException") {
            try { task->cancel(); } catch (const std::logic_error&) {}
        } else {
            try { task->fail(std::current_exception()); } catch (const std::logic_error&) {}
        }
    } catch (...) {
        auto task = asyncInvocation_->task;
        asyncInvocation_.reset();
        if (task->isTerminal()) return;
        try { task->fail(std::current_exception()); } catch (const std::logic_error&) {}
    }
}


void VM::invokeThreadClosure(const ClosureRef& closure) {
    if (!closure || !closure->chunk) throw std::runtime_error("VM: invalid thread closure");
    if (closure->functionIndex >= closure->chunk->functions.size()) throw std::runtime_error("VM: thread closure function index out of bounds");
    const auto& fn = closure->chunk->functions[closure->functionIndex];
    if (fn.isAsync) throw std::runtime_error("VM: async closure cannot run as a raw Thread entry");
    if (!closure->paramNames.empty()) throw std::runtime_error("VM: thread closure expects zero arguments");
    ExecutionState::CallFrame frame;
    frame.returnIp = closure->chunk->code.size();
    frame.functionName = "<thread>";
    frame.locals = closure->captured;
    frame.activeClosure = closure;
    const std::size_t callerFrames = state_.callDepth();
    const std::size_t callerStackSize = state_.valueStackSize();
    state_.enterFrame(std::move(frame));
    Value result;
    try {
        const ExecuteStatus status = execute(*closure->chunk, fn.entryAddress, true, {}, &result);
        if (status != ExecuteStatus::Completed) throw std::runtime_error("VM: thread closure suspended unexpectedly");
    } catch (...) {
        state_.restoreToDepth(callerStackSize, callerFrames);
        throw;
    }
}

Value VM::invokeTaskClosure(const ClosureRef& closure) {
    if (!closure || !closure->chunk) throw std::runtime_error("VM: invalid task closure");
    if (closure->functionIndex >= closure->chunk->functions.size()) throw std::runtime_error("VM: invalid task function index");
    const auto& fn = closure->chunk->functions[closure->functionIndex];
    if (fn.isAsync) throw std::runtime_error("VM: task closure must be synchronous");
    ExecutionState::CallFrame frame;
    frame.returnIp = closure->chunk->code.size();
    frame.functionName = "<task>";
    frame.locals = closure->captured;
    frame.activeClosure = closure;
    const std::size_t callerFrames = state_.callDepth();
    const std::size_t callerStackSize = state_.valueStackSize();
    state_.enterFrame(std::move(frame));
    Value result;
    try {
        const ExecuteStatus status = execute(*closure->chunk, fn.entryAddress, true, {}, &result);
        if (status != ExecuteStatus::Completed) throw std::runtime_error("VM: task closure suspended unexpectedly");
    } catch (...) {
        // The closure exited without reaching its Return (an uncaught throw),
        // so its frame - and any handlers it pushed - were never torn down.
        // Restore the caller's frame/handler/stack state before propagating,
        // otherwise the outer execute resumes against a stale, deeper stack.
        state_.restoreToDepth(callerStackSize, callerFrames);
        throw;
    }
    return result;
}

Value VM::invokeReflectiveFunction(const Value& functionValue, const Value& argsList) {
    auto functionObj = std::get_if<ObjectRef>(&functionValue);
    auto list = std::get_if<ListRef>(&argsList);
    if (!functionObj || !(*functionObj) || (*functionObj)->className != "Function" || !list || !(*list))
        throwReflectionException("ReflectionError.InvalidArguments: malformed function invocation", activeChunk_, &state_);
    auto cit = (*functionObj)->fields.find("__closure");
    if (cit == (*functionObj)->fields.end()) throwReflectionException("ReflectionError: invalid function handle", activeChunk_, &state_);
    auto closure = std::get_if<ClosureRef>(&cit->second);
    if (!closure || !(*closure) || !(*closure)->chunk)
        throwReflectionException("ReflectionError: invalid function closure", activeChunk_, &state_);
    if ((*closure)->functionIndex == Chunk::INVALID_FUNCTION_INDEX ||
        (*closure)->functionIndex >= (*closure)->chunk->functions.size())
        throwReflectionException("ReflectionError: invalid function index", activeChunk_, &state_);
    const auto& fn = (*closure)->chunk->functions[(*closure)->functionIndex];
    const auto& values = (*list)->items;
    if (fn.paramNames.size() != values.size())
        throwReflectionException("ReflectionError.InvalidArguments: expected " + std::to_string(fn.paramNames.size()) + " argument(s), got " + std::to_string(values.size()), activeChunk_, &state_);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::string expected = i < (*closure)->parameterTypeNames.size()
            ? (*closure)->parameterTypeNames[i] : "unknown";
        if (!reflectiveTypeMatchesName(values[i], expected, activeChunk_))
            throwReflectionException("ReflectionError.InvalidArguments: argument type mismatch at index " + std::to_string(i), activeChunk_, &state_);
    }
    if (fn.isAsync) {
        auto task = std::make_shared<RuntimeTaskState>(fn.returnTypeName.empty() ? "void" : fn.returnTypeName);
        auto child = std::make_shared<VM>(scheduler_);
        auto chunkRef = std::make_shared<Chunk>(*(*closure)->chunk);
        child->beginAsyncInvocation(chunkRef, (*closure)->functionIndex, values, std::nullopt, task);
        scheduler_->enqueue(std::make_shared<AsyncFrame>([child]() mutable { child->resumeAsyncInvocation(); }));
        return Value{std::move(task)};
    }
    return invokeFunction(*(*closure)->chunk, (*closure)->functionIndex, values, std::nullopt);
}

Value VM::invokeReflectiveMethod(const Value& methodValue, const Value& receiver, const Value& argsList) {
    auto methodObj = std::get_if<ObjectRef>(&methodValue);
    auto list = std::get_if<ListRef>(&argsList);
    if (!methodObj || !*methodObj || (*methodObj)->className != "Method" || !list || !*list)
        throwReflectionException("ReflectionError.InvalidArguments: malformed method invocation", activeChunk_, &state_);
    auto fit = (*methodObj)->fields.find("__functionIndex");
    auto ait = (*methodObj)->fields.find("access");
    auto sit = (*methodObj)->fields.find("static");
    if (fit == (*methodObj)->fields.end() || !std::holds_alternative<std::int64_t>(fit->second))
        throwReflectionException("ReflectionError: invalid method handle", activeChunk_, &state_);
    const auto access = ait != (*methodObj)->fields.end() && std::holds_alternative<std::string>(ait->second) ? std::get<std::string>(ait->second) : "public";
    if (access != "public") throwReflectionException("ReflectionError.AccessViolation: method is not public", activeChunk_, &state_);
    const bool isStatic = sit != (*methodObj)->fields.end() && std::holds_alternative<bool>(sit->second) && std::get<bool>(sit->second);
    if (!isStatic && (!std::holds_alternative<ObjectRef>(receiver) || !std::get<ObjectRef>(receiver)))
        throwReflectionException("ReflectionError.InvalidArguments: instance method requires object receiver", activeChunk_, &state_);
    const auto index = static_cast<std::size_t>(std::get<std::int64_t>(fit->second));
    if (!activeChunk_ || index == Chunk::INVALID_FUNCTION_INDEX || index >= activeChunk_->functions.size())
        throwReflectionException("ReflectionError: invalid method function index", activeChunk_, &state_);
    const auto& fn = activeChunk_->functions[index];
    const auto& values = (*list)->items;
    if (fn.paramNames.size() != values.size())
        throwReflectionException("ReflectionError.InvalidArguments: expected " + std::to_string(fn.paramNames.size()) + " argument(s), got " + std::to_string(values.size()), activeChunk_, &state_);
    std::vector<std::string> reflectedParameterTypes;
    if (auto pit = (*methodObj)->fields.find("parameters"); pit != (*methodObj)->fields.end()) {
        if (auto plist = std::get_if<ListRef>(&pit->second); plist && *plist) {
            for (const auto& entry : (*plist)->items) {
                if (auto text = std::get_if<std::string>(&entry)) reflectedParameterTypes.push_back(*text);
            }
        }
    }
    // Substitute the receiver's concrete generic instantiation (e.g. T=string
    // for List.push on a List<string>) so a reflective invoke enforces the
    // element type rather than asserting the raw parameter token.
    std::vector<std::string> rParams, rArgs;
    if (!isStatic && std::holds_alternative<ObjectRef>(receiver) && std::get<ObjectRef>(receiver)) {
        std::tie(rParams, rArgs) = receiverTypeArgs(*std::get<ObjectRef>(receiver), *activeChunk_);
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::string expected = i < reflectedParameterTypes.size()
            ? reflectedParameterTypes[i]
            : (i < fn.parameterTypeNames.size() ? fn.parameterTypeNames[i] : "unknown");
        expected = substituteTypeParams(expected, rParams, rArgs);
        if (expected != "unknown" && !typeNameHasUnresolvedParam(expected, *activeChunk_) &&
            !reflectiveTypeMatchesName(values[i], expected, activeChunk_))
            throwReflectionException("ReflectionError.InvalidArguments: argument type mismatch at index " + std::to_string(i), activeChunk_, &state_);
    }
    if (fn.isAsync) {
        auto task = std::make_shared<RuntimeTaskState>(fn.returnTypeName.empty() ? "void" : fn.returnTypeName);
        auto chunkRef = std::make_shared<Chunk>(*activeChunk_);
        auto child = std::make_shared<VM>(scheduler_);
        std::optional<Value> r = isStatic ? std::nullopt : std::optional<Value>(receiver);
        child->beginAsyncInvocation(chunkRef, index, values, r, task);
        scheduler_->enqueue(std::make_shared<AsyncFrame>([child]() mutable { child->resumeAsyncInvocation(); }));
        return Value{std::move(task)};
    }
    return invokeFunction(*activeChunk_, index, values, isStatic ? std::nullopt : std::optional<Value>(receiver));
}

Value VM::invokeReflectiveConstructor(const Value& constructorValue, const Value& argsList) {
    auto ctorObj = std::get_if<ObjectRef>(&constructorValue);
    auto list = std::get_if<ListRef>(&argsList);
    if (!ctorObj || !*ctorObj || (*ctorObj)->className != "Constructor" || !list || !*list)
        throwReflectionException("ReflectionError.InvalidArguments: malformed constructor invocation", activeChunk_, &state_);
    auto fit = (*ctorObj)->fields.find("__functionIndex");
    auto oit = (*ctorObj)->fields.find("__owner");
    auto ait = (*ctorObj)->fields.find("__access");
    if (fit == (*ctorObj)->fields.end() || !std::holds_alternative<std::int64_t>(fit->second) || oit == (*ctorObj)->fields.end() || !std::holds_alternative<std::string>(oit->second))
        throwReflectionException("ReflectionError: invalid constructor handle", activeChunk_, &state_);
    const auto access = ait != (*ctorObj)->fields.end() && std::holds_alternative<std::string>(ait->second) ? std::get<std::string>(ait->second) : "public";
    if (access != "public") throwReflectionException("ReflectionError.AccessViolation: constructor is not public", activeChunk_, &state_);
    const auto index = static_cast<std::size_t>(std::get<std::int64_t>(fit->second));
    if (!activeChunk_ || index == Chunk::INVALID_FUNCTION_INDEX || index >= activeChunk_->functions.size())
        throwReflectionException("ReflectionError: invalid constructor function index", activeChunk_, &state_);
    const auto& fn = activeChunk_->functions[index];
    const auto& values = (*list)->items;
    if (fn.paramNames.size() != values.size())
        throwReflectionException("ReflectionError.InvalidArguments: expected " + std::to_string(fn.paramNames.size()) + " argument(s), got " + std::to_string(values.size()), activeChunk_, &state_);
    std::vector<std::string> reflectedParameterTypes;
    if (auto pit = (*ctorObj)->fields.find("parameters"); pit != (*ctorObj)->fields.end()) {
        if (auto plist = std::get_if<ListRef>(&pit->second); plist && *plist) {
            for (const auto& entry : (*plist)->items) {
                if (auto text = std::get_if<std::string>(&entry)) reflectedParameterTypes.push_back(*text);
            }
        }
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::string expected = i < reflectedParameterTypes.size()
            ? reflectedParameterTypes[i]
            : (i < fn.parameterTypeNames.size() ? fn.parameterTypeNames[i] : "unknown");
        if (!reflectiveTypeMatchesName(values[i], expected, activeChunk_))
            throwReflectionException("ReflectionError.InvalidArguments: argument type mismatch at index " + std::to_string(i), activeChunk_, &state_);
    }
    Value object = makeEmptyObject(std::get<std::string>(oit->second));
    auto obj = std::get<ObjectRef>(object);
    auto meta = activeChunk_->classReflection.find(obj->className);
    if (meta != activeChunk_->classReflection.end()) obj->runtimeType = meta->second.runtimeType;
    (void)invokeFunction(*activeChunk_, index, values, object);
    return object;
}

Value VM::invokeFunction(const Chunk& chunk, std::size_t functionIndex,
                         const std::vector<Value>& args, const std::optional<Value>& receiver) {
    if (functionIndex >= chunk.functions.size()) {
        throw std::runtime_error("VM: function index out of bounds");
    }
    const FunctionInfo& fn = chunk.functions[functionIndex];
    if (fn.paramNames.size() != args.size()) {
        throw std::runtime_error("VM: function argument count mismatch");
    }
    ExecutionState::CallFrame frame;
    frame.returnIp = chunk.code.size();
    if (receiver) frame.locals["this"] = *receiver;
    for (std::size_t i = 0; i < args.size(); ++i) frame.locals[fn.paramNames[i]] = args[i];
    const std::size_t callerFrames = state_.callDepth();
    const std::size_t callerStackSize = state_.valueStackSize();
    state_.enterFrame(std::move(frame));
    Value result;
    try {
        const ExecuteStatus status = execute(chunk, fn.entryAddress, true, {}, &result);
        if (status != ExecuteStatus::Completed) throw std::runtime_error("VM: function execution suspended unexpectedly");
    } catch (...) {
        state_.restoreToDepth(callerStackSize, callerFrames);
        throw;
    }
    return result;
}

} // namespace zl
