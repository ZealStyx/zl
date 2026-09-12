#include "zl/mir/reachability.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zl/compiler/native_catalog.hpp"

namespace zl::mir {

namespace {

// The class hierarchy as dispatch needs it.
//
// `closure(name)` answers "which classes can this value's runtime class be", and
// `supertypesOf(name)` answers "where can the method be declared". Resolving a
// virtual call needs both: `Derived d; d.m()` runs `Derived.m` when `Derived`
// overrides it and the inherited `Base.m` when it does not.
//
// Parents and interfaces are walked to a fixpoint, because an interface can
// extend an interface and a class can reach one through its parent. The type
// checker rules out cycles, but every walk still checks for revisiting, so a
// malformed module cannot make a diagnostic hang.
class Hierarchy {
public:
    explicit Hierarchy(const Module& module) {
        for (const auto& layout : module.classes) layoutByName_.emplace(layout.name, &layout);
        for (const auto& interface : module.interfaces) interfaceBases_.emplace(interface.name, interface.bases);
        // Every class's supertypes, computed once, so the closure queries below
        // are set lookups rather than repeated graph walks.
        for (const auto& [name, layout] : layoutByName_) {
            (void)layout;
            (void)supertypesOf(name);
        }
    }

    // Every class name that a value of static type `name` can actually be: the
    // name itself, plus its subtypes - and, when `name` is an interface, the
    // classes that implement it.
    [[nodiscard]] const std::unordered_set<std::string>& closure(const std::string& name) const {
        const auto cached = closure_.find(name);
        if (cached != closure_.end()) return cached->second;

        std::unordered_set<std::string> result;
        result.insert(name);
        for (const auto& [className, layout] : layoutByName_) {
            (void)layout;
            if (supertypesOf(className).count(name) != 0) result.insert(className);
        }
        return closure_.emplace(name, std::move(result)).first->second;
    }

    // Transitive parents and interfaces of one class, memoised.
    [[nodiscard]] const std::unordered_set<std::string>& supertypesOf(const std::string& name) const {
        const auto cached = supertypes_.find(name);
        if (cached != supertypes_.end()) return cached->second;

        std::unordered_set<std::string> supers;
        std::unordered_set<std::string> seen;
        std::vector<std::string> work{name};
        while (!work.empty()) {
            const std::string current = work.back();
            work.pop_back();
            if (!seen.insert(current).second) continue;

            const auto layout = layoutByName_.find(current);
            if (layout != layoutByName_.end()) {
                const auto& parent = layout->second->parent;
                if (!parent.empty() && parent != current) {
                    supers.insert(parent);
                    work.push_back(parent);
                }
                for (const auto& interfaceName : layout->second->interfaces) {
                    if (interfaceName == current) continue;
                    supers.insert(interfaceName);
                    work.push_back(interfaceName);
                }
            }
            const auto bases = interfaceBases_.find(current);
            if (bases != interfaceBases_.end()) {
                for (const auto& base : bases->second) {
                    if (base == current) continue;
                    supers.insert(base);
                    work.push_back(base);
                }
            }
        }
        // A class is never its own supertype; `closure` adds the name back.
        supers.erase(name);
        return supertypes_.emplace(name, std::move(supers)).first->second;
    }

private:
    std::unordered_map<std::string, const ClassLayout*> layoutByName_;
    std::unordered_map<std::string, std::vector<std::string>> interfaceBases_;
    mutable std::unordered_map<std::string, std::unordered_set<std::string>> supertypes_;
    mutable std::unordered_map<std::string, std::unordered_set<std::string>> closure_;
};

std::string functionName(const Module& module, FunctionId id) {
    const Function* function = module.function(id);
    return function != nullptr ? function->name : std::string{"<missing>"};
}

} // namespace

std::string ReachabilityReport::describe(const Module& module) const {
    if (!hasEntryPoint) {
        return "no entry point: a library module, so nothing is known to run";
    }
    std::string text = std::to_string(functions.size()) + " of " +
                       std::to_string(module.functions.size()) +
                       " function(s) can run, from " + functionName(module, module.entryPoint);
    if (dynamicEntry) {
        text += "; more through reflection (" + dynamicEntryReason + ")";
    }
    return text;
}

ReachabilityReport reachableFunctions(const Module& module) {
    ReachabilityReport report;
    if (module.entryPoint == kNoFunction || module.function(module.entryPoint) == nullptr) {
        return report;
    }
    report.hasEntryPoint = true;

    const Hierarchy hierarchy(module);

    // Name indexes, built once: a dispatch site should not scan the module's
    // functions, and this analysis runs on modules with hundreds of them.
    std::unordered_map<std::string, std::vector<FunctionId>> functionsByName;
    std::unordered_map<std::string, std::vector<FunctionId>> methodsByClass;
    for (const auto& function : module.functions) {
        // Keyed by the declared name: a dispatch site names "speak", not
        // "speak()". Overloads of one name share a key on purpose - keeping all
        // of them is the over-approximation this analysis promises.
        functionsByName[function.declaredName()].push_back(function.id);
        if (!function.ownerClass.empty()) methodsByClass[function.ownerClass].push_back(function.id);
    }

    std::size_t dispatchKept = 0;
    std::unordered_set<FunctionId> queued;
    std::vector<FunctionId> work;

    const auto enqueue = [&](FunctionId id) {
        if (id == kNoFunction || module.function(id) == nullptr) return;
        if (queued.insert(id).second) work.push_back(id);
    };
    const auto enqueueStaticInitializer = [&](StaticId id) {
        const StaticField* field = module.staticField(id);
        if (field != nullptr) enqueue(field->initializer);
    };
    // Static access names its field the way the verifier reads it: class plus
    // field name, not an id.
    const auto enqueueStaticInitializerOf = [&](const std::string& className, const std::string& fieldName) {
        if (className.empty() || fieldName.empty()) return;
        for (const auto& field : module.statics) {
            if (field.className == className && field.name == fieldName) {
                enqueue(field.initializer);
                return;
            }
        }
    };
    // The sound fallback when a dispatch site names a class or method this
    // module does not have: keep every function with that method name. An empty
    // answer here would be an absence of evidence read as evidence of absence.
    const auto enqueueByName = [&](const std::string& methodName) {
        const auto candidates = functionsByName.find(methodName);
        if (candidates == functionsByName.end()) return;
        dispatchKept += candidates->second.size();
        for (const FunctionId candidate : candidates->second) enqueue(candidate);
    };

    enqueue(module.entryPoint);

    while (!work.empty()) {
        const FunctionId id = work.back();
        work.pop_back();
        report.functions.insert(id);

        const Function* function = module.function(id);
        if (function == nullptr) continue;

        for (const auto& block : function->blocks) {
            for (const auto& instruction : block.instructions) {
                switch (instruction.opcode) {
                    case Opcode::Call:
                    case Opcode::InvokeSuper:
                    case Opcode::InvokeStatic:
                    case Opcode::CallIndirect:
                    case Opcode::MakeClosure:
                        enqueue(instruction.target.function);
                        break;
                    case Opcode::InvokeMethod: {
                        const std::string& className = instruction.target.className;
                        const std::string& methodName = instruction.target.methodName;
                        if (className.empty() || methodName.empty()) {
                            enqueueByName(methodName);
                            break;
                        }
                        std::size_t kept = 0;
                        const auto& runtimeClasses = hierarchy.closure(className);
                        const auto& declaringClasses = hierarchy.supertypesOf(className);
                        const auto consider = [&](const std::string& candidateClass) {
                            const auto methods = methodsByClass.find(candidateClass);
                            if (methods == methodsByClass.end()) return;
                            for (const FunctionId method : methods->second) {
                                const Function* callee = module.function(method);
                                if (callee == nullptr || callee->declaredName() != methodName) continue;
                                enqueue(method);
                                ++kept;
                            }
                        };
                        for (const auto& candidateClass : runtimeClasses) consider(candidateClass);
                        for (const auto& candidateClass : declaringClasses) consider(candidateClass);
                        if (kept == 0) enqueueByName(methodName);
                        else dispatchKept += kept;
                        break;
                    }
                    case Opcode::CallNative:
                        if (instruction.target.nativeId >= 0 &&
                            ::zl::nativeEntersCodeByName(
                                static_cast<::zl::NativeId>(instruction.target.nativeId))) {
                            if (!report.dynamicEntry) {
                                report.dynamicEntry = true;
                                report.dynamicEntryReason =
                                    instruction.target.nativeName + " enters code by name";
                            }
                        }
                        break;
                    case Opcode::StaticLoad:
                    case Opcode::StaticStore:
                        enqueueStaticInitializerOf(instruction.target.className, instruction.name);
                        break;
                    default:
                        break;
                }
                for (const auto& operand : instruction.operands) {
                    if (operand.kind == OperandKind::Static) enqueueStaticInitializer(operand.index);
                }
            }
        }
    }

    if (dispatchKept != 0) {
        report.notes.push_back("virtual dispatch resolved over the class hierarchy: " +
                               std::to_string(dispatchKept) + " override candidate(s) kept");
    }
    if (report.dynamicEntry) {
        report.notes.push_back("reflection makes the call graph open: this is a lower bound");
    }
    return report;
}

} // namespace zl::mir
