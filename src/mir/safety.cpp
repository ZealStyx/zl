#include "zl/mir/safety.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include "zl/common/json.hpp"
#include "zl/mir/analysis.hpp"

namespace zl::mir {
const char* propertyName(SafetyProperty p) noexcept {
    switch (p) {
        case SafetyProperty::Structure: return "structure";
        case SafetyProperty::TypeFlow: return "type-flow";
        case SafetyProperty::Definition: return "definition";
        case SafetyProperty::Move: return "move";
        case SafetyProperty::Ownership: return "ownership";
        case SafetyProperty::Borrow: return "borrow";
        case SafetyProperty::Reachability: return "reachability";
        case SafetyProperty::ControlFlow: return "control-flow";
        case SafetyProperty::TypeAssumption: return "type-assumption";
        case SafetyProperty::DynamicBoundary: return "dynamic-boundary";
        case SafetyProperty::Return: return "return";
        case SafetyProperty::NativeCall: return "native-call";
        case SafetyProperty::ResourceLifetime: return "resource-lifetime";
        case SafetyProperty::Concurrency: return "concurrency";
    }
    return "structure";
}

SafetyProperty instructionProperty(Opcode op) noexcept {
    switch (op) {
        case Opcode::Move: return SafetyProperty::Move;
        case Opcode::Borrow: case Opcode::EndBorrow: return SafetyProperty::Borrow;
        case Opcode::Drop: return SafetyProperty::ResourceLifetime;
        case Opcode::Refine: case Opcode::TypeTest: return SafetyProperty::TypeAssumption;
        case Opcode::CallNative: case Opcode::FfiCall: return SafetyProperty::NativeCall;
        case Opcode::HandleClose: case Opcode::HandleConsume: case Opcode::HandleBorrow:
        case Opcode::CallbackClose: return SafetyProperty::ResourceLifetime;
        default: break;
    }
    if (opcodeIsThreadBoundary(op) || opcodeIsScopedLock(op) || opcodeIsSuspension(op) ||
        opcodeIsBlocking(op)) return SafetyProperty::Concurrency;
    return SafetyProperty::TypeFlow;
}

std::string VerificationReport::toJson() const {
    using zl::common::jsonString;
    std::ostringstream out;
    out << "{\"ok\":" << (ok() ? "true" : "false") << ",\"errors\":" << errorCount()
        << ",\"warnings\":" << warningCount() << ",\"diagnostics\":[";
    bool first = true;
    for (const auto& d : diagnostics) {
        if (!first) out << ',';
        first = false;
        out << "{\"property\":" << jsonString(propertyName(d.property))
            << ",\"severity\":" << jsonString(severityName(d.severity))
            << ",\"function\":" << jsonString(d.function)
            << ",\"block\":" << d.block << ",\"instruction\":" << d.instructionIndex
            << ",\"message\":" << jsonString(d.message)
            << ",\"location\":{\"file\":" << jsonString(d.location.file)
            << ",\"line\":" << d.location.line << ",\"column\":" << d.location.column << "}}";
    }
    return out.str() + "]}";
}

namespace {
using Slots = std::set<SlotId>;
struct State {
    Slots initialized;                 // MUST: intersection at joins
    std::set<ValueId> ended;            // MAY: union at joins
    bool operator==(const State& rhs) const {
        return initialized == rhs.initialized && ended == rhs.ended;
    }
};
State merge(const State& a, const State& b) {
    State result;
    std::set_intersection(a.initialized.begin(), a.initialized.end(),
                          b.initialized.begin(), b.initialized.end(),
                          std::inserter(result.initialized, result.initialized.end()));
    result.ended = a.ended;
    result.ended.insert(b.ended.begin(), b.ended.end());
    return result;
}

class SafetyAnalysis {
    const Module& module;
    const Function& fn;
    const VerifierOptions& options;
    VerificationReport& report;
    ControlFlowGraph cfg;
    // Only identity-preserving SSA aliases. Heap/slot aliases are NOT inferred
    // from a global def map: mutable storage requires reaching definitions.
    std::map<ValueId, ValueId> aliases;
    std::map<ValueId, SlotId> loads;
    std::set<ValueId> borrowedHandles;
    bool emitting{false};

    ValueId root(ValueId v) const {
        std::set<ValueId> seen;
        while (aliases.count(v) && seen.insert(v).second) v = aliases.at(v);
        return v;
    }
    bool isBorrowedHandle(ValueId v) const {
        std::set<ValueId> seen;
        while (seen.insert(v).second) {
            if (borrowedHandles.count(v)) return true;
            const auto it = aliases.find(v);
            if (it == aliases.end()) return false;
            v = it->second;
        }
        return false;
    }
    void diagnostic(SafetyProperty property, const std::string& message, const BasicBlock& b,
                    long index, SourceLocation loc, bool warning = false) {
        if (!emitting) return;
        if (!warning && options.maxErrors && report.errorCount() >= options.maxErrors) return;
        if (!loc.valid()) loc = b.location.valid() ? b.location : fn.location;
        if (loc.file.empty()) loc.file = fn.location.file;
        Diagnostic d;
        d.property = property;
        d.severity = warning ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error;
        d.function = fn.name; d.block = b.id; d.instructionIndex = index;
        d.message = message; d.location = std::move(loc);
        report.diagnostics.push_back(std::move(d));
    }
    void use(const Operand& operand, const State& state, const BasicBlock& b, long index, SourceLocation loc) {
        const auto v = root(valueOf(operand));
        if (v.valid() && state.ended.count(v))
            diagnostic(SafetyProperty::ResourceLifetime,
                       "use of value after close, consume, or release on a possible path", b, index, loc);
    }
    void audit(const Instruction& inst, const BasicBlock& b, long index) {
        if (!options.auditBoundaries) return;
        bool dynamic = false;
        for (const auto& operand : inst.operands) {
            const auto* type = module.types.find(operand.type);
            dynamic |= type && (type->kind == TypeKind::Unknown || type->kind == TypeKind::TypeParam ||
                       (type->kind == TypeKind::Function && !type->signature.hasSignature));
        }
        if (dynamic)
            diagnostic(SafetyProperty::DynamicBoundary,
                       std::string(opcodeName(inst.opcode)) +
                       " depends on dynamic/unsubstituted input; runtime checks remain an obligation",
                       b, index, inst.location, true);
        if (inst.opcode == Opcode::FfiCall || inst.opcode == Opcode::CallNative)
            diagnostic(SafetyProperty::NativeCall,
                       "native contract verified only from MIR metadata; registry/ABI, handle validity and external effects require runtime validation",
                       b, index, inst.location, true);
        if (opcodeIsThreadBoundary(inst.opcode) || opcodeIsScopedLock(inst.opcode)) {
            bool known = false;
            if (!inst.operands.empty()) {
                auto id = valueOf(inst.operands[0]);
                for (const auto& block : fn.blocks) for (const auto& def : block.instructions)
                    known |= id == tempValue(def.result) && def.opcode == Opcode::MakeClosure;
            }
            if (!known) diagnostic(SafetyProperty::Concurrency,
                "opaque closure at concurrency boundary; capture/effect safety requires runtime validation",
                b, index, inst.location, true);
        }
    }
    State transfer(const BasicBlock& b, State state, State* exceptional = nullptr) {
        const State entry = state;
        bool hasExceptional = false;
        const auto capture = [&] {
            if (!exceptional) return;
            *exceptional = hasExceptional ? merge(*exceptional, state) : state;
            hasExceptional = true;
        };
        for (std::size_t i = 0; i < b.instructions.size(); ++i) {
            const auto& inst = b.instructions[i];
            const bool throws = opcodeShape(inst.opcode).mayThrow;
            if (throws) capture();
            for (const auto& operand : inst.operands) use(operand, state, b, i, inst.location);
            const auto* slot = fn.slot(inst.slot);
            if ((inst.opcode == Opcode::Load || inst.opcode == Opcode::Move) && slot &&
                !state.initialized.count(inst.slot))
                diagnostic(SafetyProperty::Definition,
                    "local '" + slot->name + "' may be read before definition", b, i, inst.location);
            if (inst.opcode == Opcode::Store || inst.opcode == Opcode::Borrow)
                state.initialized.insert(inst.slot);
            // A generative SSA definition inside a loop creates a fresh value
            // on each execution; a refine is an alias, not a fresh resource.
            if (inst.result != kNoTemp && !aliases.count(tempValue(inst.result)))
                state.ended.erase(tempValue(inst.result));
            const auto end = [&](const Operand& operand) {
                if (isBorrowedHandle(valueOf(operand))) {
                    diagnostic(SafetyProperty::Ownership, "a borrowed native handle cannot be closed or consumed",
                               b, i, inst.location);
                    return;
                }
                const auto v = root(valueOf(operand));
                if (v.valid()) state.ended.insert(v);
            };
            if (options.checkOwnership) {
                if ((inst.opcode == Opcode::HandleClose || inst.opcode == Opcode::HandleConsume ||
                     inst.opcode == Opcode::CallbackClose || inst.opcode == Opcode::Drop) &&
                    !inst.operands.empty()) end(inst.operands[0]);
                const auto& ownership = inst.opcode == Opcode::FfiCall ? inst.target.ffiParamOwnership
                                                                     : inst.target.nativeParamOwnership;
                if (inst.opcode == Opcode::FfiCall || inst.opcode == Opcode::CallNative) {
                    for (std::size_t o = 0; o < std::min(ownership.size(), inst.operands.size()); ++o)
                        if (ownership[o] == NativeOwnership::Consumed) end(inst.operands[o]);
                }
            }
            audit(inst, b, i);
            // External operations may consume then fail; retain both states.
            if (throws) capture();
        }
        const auto& t = b.terminator;
        use(t.value, state, b, -1, t.location);
        for (const auto& args : t.edgeArguments) for (const auto& arg : args) use(arg, state, b, -1, t.location);
        if (options.checkOwnership && t.kind == TerminatorKind::Return) {
            const auto v = root(valueOf(t.value));
            bool borrowed = false;
            if (v.kind == OperandKind::Param) {
                const auto* p = fn.parameter(v.index);
                borrowed = p && p->ownership == zl::OwnershipKind::BORROW;
            }
            if (loads.count(v)) {
                const auto* s = fn.slot(loads.at(v));
                borrowed = s && s->ownership == zl::OwnershipKind::BORROW;
            }
            if (borrowed) diagnostic(SafetyProperty::Borrow,
                "return of a borrowed value escapes its local/caller lifetime", b, -1, t.location);
        }
        if (t.kind == TerminatorKind::Throw) capture();
        if (exceptional && !hasExceptional) *exceptional = entry;
        return state;
    }
public:
    SafetyAnalysis(const Module& m, const Function& f, const VerifierOptions& o, VerificationReport& r)
        : module(m), fn(f), options(o), report(r), cfg(f) {}
    void run() {
        if (!cfg.isValid(fn.entryBlock)) return;
        // Malformed block identities are structural errors, not dataflow input.
        std::set<BlockId> ids;
        for (const auto& b : fn.blocks) if (!ids.insert(b.id).second) return;
        for (const auto& b : fn.blocks) for (const auto& inst : b.instructions) {
            if (inst.result == kNoTemp) continue;
            if ((inst.opcode == Opcode::Refine || inst.opcode == Opcode::HandleBorrow) && !inst.operands.empty())
                aliases[tempValue(inst.result)] = valueOf(inst.operands[0]);
            if (inst.opcode == Opcode::HandleBorrow) borrowedHandles.insert(tempValue(inst.result));
            if (inst.opcode == Opcode::Load) loads[tempValue(inst.result)] = inst.slot;
        }
        const auto count = fn.blocks.size();
        std::vector<State> in(count), out(count), unwind(count);
        std::vector<bool> visited(count, false);
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& b : fn.blocks) {
                if (!cfg.reachableWithUnwind().count(b.id)) continue;
                const auto bi = cfg.indexOf(b.id);
                State input;
                bool first = b.id != fn.entryBlock;
                const auto add = [&](State incoming) {
                    if (first) { input = std::move(incoming); first = false; }
                    else input = merge(input, incoming);
                };
                for (auto pred : cfg.predecessors(b.id)) {
                    auto pi = cfg.indexOf(pred);
                    if (visited[pi]) add(out[pi]);
                }
                for (auto pred : cfg.unwindPredecessors(b.id)) {
                    auto pi = cfg.indexOf(pred);
                    if (!visited[pi]) continue;
                    // Each handler edge defines its own catch binding, not all
                    // slots marked isCatchBinding throughout the function.
                    for (const auto& handler : fn.block(pred)->exceptionHandlers) {
                        if (handler.block != b.id) continue;
                        State incoming = unwind[pi];
                        if (handler.catchSlot) incoming.initialized.insert(handler.catchSlot);
                        add(std::move(incoming));
                    }
                }
                if (first) continue;
                in[bi] = input;
                State exceptional;
                State next = transfer(b, input, &exceptional);
                if (!visited[bi] || !(out[bi] == next) || !(unwind[bi] == exceptional)) {
                    out[bi] = std::move(next); unwind[bi] = std::move(exceptional);
                    visited[bi] = true; changed = true;
                }
            }
        }
        emitting = true;
        for (const auto& b : fn.blocks) if (visited[cfg.indexOf(b.id)]) transfer(b, in[cfg.indexOf(b.id)]);
    }
};
} // namespace

void appendSafetyAnalysis(const Module& m, const Function& f, const VerifierOptions& o, VerificationReport& r) {
    SafetyAnalysis(m, f, o, r).run();
}
} // namespace zl::mir
