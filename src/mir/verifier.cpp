#include "zl/mir/verifier.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "zl/compiler/operator_rules.hpp"
#include "zl/mir/analysis.hpp"

namespace zl::mir {
namespace {

// MIR types are mapped onto the language's own ZlType tags so that operator
// typing in the MIR is answered by `zl::OperatorRules` - the same table the type
// checker uses. A second, MIR-local copy of the arithmetic rules would be a
// second place for the language's semantics to be wrong.
zl::ZlType toZlType(const Type* type) {
    if (!type) return zl::ZlType::UNKNOWN;
    switch (type->kind) {
        case TypeKind::Void: return zl::ZlType::VOID_TYPE;
        case TypeKind::Nil: return zl::ZlType::NIL;
        case TypeKind::Bool: return zl::ZlType::BOOL;
        case TypeKind::Int: return zl::ZlType::INT;
        case TypeKind::Double: return zl::ZlType::DOUBLE;
        case TypeKind::String: return zl::ZlType::STRING;
        case TypeKind::List: return zl::ZlType::LIST;
        case TypeKind::Map: return zl::ZlType::MAP;
        case TypeKind::Set: return zl::ZlType::SET;
        case TypeKind::Array: return zl::ZlType::ARRAY;
        case TypeKind::Object: return zl::ZlType::OBJECT;
        case TypeKind::Task: return zl::ZlType::TASK;
        // `Shared<T>` is an ordinary generic class in ZL, not a runtime kind of
        // its own, so it types as an object.
        case TypeKind::Shared: return zl::ZlType::OBJECT;
        case TypeKind::Function: return zl::ZlType::FUNCTION;
        case TypeKind::Union: return zl::ZlType::UNION;
        // An unsubstituted generic parameter resolves to OBJECT for typing
        // purposes, matching TypeResolver::resolveType.
        case TypeKind::TypeParam: return zl::ZlType::OBJECT;
        case TypeKind::Unknown: return zl::ZlType::UNKNOWN;
    }
    return zl::ZlType::UNKNOWN;
}

zl::TokenType binaryToken(Opcode opcode) {
    switch (opcode) {
        case Opcode::Add: return zl::TokenType::PLUS;
        case Opcode::Sub: return zl::TokenType::MINUS;
        case Opcode::Mul: return zl::TokenType::STAR;
        case Opcode::Div: return zl::TokenType::SLASH;
        case Opcode::Mod: return zl::TokenType::PERCENT;
        case Opcode::Pow: return zl::TokenType::POW;
        case Opcode::BitAnd: return zl::TokenType::BIT_AND;
        case Opcode::BitOr: return zl::TokenType::BIT_OR;
        case Opcode::BitXor: return zl::TokenType::BIT_XOR;
        case Opcode::Shl: return zl::TokenType::SHL;
        case Opcode::Shr: return zl::TokenType::SHR;
        case Opcode::Ushr: return zl::TokenType::USHR;
        case Opcode::Eq: return zl::TokenType::EQ;
        case Opcode::Ne: return zl::TokenType::NEQ;
        case Opcode::Lt: return zl::TokenType::LT;
        case Opcode::Le: return zl::TokenType::LTE;
        case Opcode::Gt: return zl::TokenType::GT;
        case Opcode::Ge: return zl::TokenType::GTE;
        case Opcode::And: return zl::TokenType::AND;
        case Opcode::Or: return zl::TokenType::OR;
        default: return zl::TokenType::UNKNOWN;
    }
}

// The MIR type a ZlType result from OperatorRules corresponds to. Only used for
// the primitive results the operator table can produce.
std::uint32_t primitiveTypeFor(const TypeArena& arena, zl::ZlType type) {
    switch (type) {
        case zl::ZlType::BOOL: return arena.boolType();
        case zl::ZlType::INT: return arena.intType();
        case zl::ZlType::DOUBLE: return arena.doubleType();
        case zl::ZlType::STRING: return arena.stringType();
        default: return 0;
    }
}

bool isBinaryArithmetic(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Div: case Opcode::Mod: case Opcode::Pow:
            return true;
        default: return false;
    }
}

bool isBitwise(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::BitAnd: case Opcode::BitOr: case Opcode::BitXor:
        case Opcode::Shl: case Opcode::Shr: case Opcode::Ushr:
            return true;
        default: return false;
    }
}

bool isComparison(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Eq: case Opcode::Ne: case Opcode::Lt:
        case Opcode::Le: case Opcode::Gt: case Opcode::Ge:
            return true;
        default: return false;
    }
}

// Opcode families come from the shared table in instruction.cpp rather than
// being re-listed here, so the verifier and the printer cannot disagree about
// which field an opcode reads.

} // namespace

const char* severityName(DiagnosticSeverity severity) noexcept {
    switch (severity) {
        case DiagnosticSeverity::Error: return "error";
        case DiagnosticSeverity::Warning: return "warning";
    }
    return "error";
}

std::string Diagnostic::describe() const {
    std::ostringstream out;
    out << severityName(severity) << ": ";
    if (!function.empty()) out << "in " << function << ": ";
    if (block != kNoBlock) out << "block b" << block;
    if (instructionIndex >= 0) out << "[" << instructionIndex << "]";
    if (block != kNoBlock || instructionIndex >= 0) out << ": ";
    out << message;
    if (location.valid()) out << " (" << location.describe() << ")";
    return out.str();
}

std::size_t VerificationReport::errorCount() const {
    std::size_t count = 0;
    for (const auto& d : diagnostics) {
        if (d.severity == DiagnosticSeverity::Error) ++count;
    }
    return count;
}

std::size_t VerificationReport::warningCount() const {
    std::size_t count = 0;
    for (const auto& d : diagnostics) {
        if (d.severity == DiagnosticSeverity::Warning) ++count;
    }
    return count;
}

std::string VerificationReport::describe() const {
    if (diagnostics.empty()) return "MIR verification: ok";
    std::ostringstream out;
    for (const auto& d : diagnostics) out << d.describe() << "\n";
    out << "MIR verification: " << errorCount() << " error(s), " << warningCount() << " warning(s)";
    return out.str();
}

namespace {

// One function's worth of verification state.
class FunctionVerifier {
public:
    FunctionVerifier(const Module& module, const Function& function, const VerifierOptions& options)
        : module_(module), function_(function), options_(options), cfg_(function) {}

    void run(VerificationReport& report) {
        report_ = &report;
        checkSignature();
        if (function_.blocks.empty()) return;
        collectDefinitions();
        checkControlFlow();
        checkInstructions();
        checkDominance();
        if (options_.checkOwnership) checkOwnershipFlow();
        report_ = nullptr;
    }

private:
    // --- diagnostics ------------------------------------------------------
    bool saturated() const {
        return options_.maxErrors != 0 && report_ && report_->errorCount() >= options_.maxErrors;
    }

    void error(const std::string& message, BlockId block = kNoBlock, long index = -1,
               SourceLocation location = {}) {
        if (!report_ || saturated()) return;
        Diagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.function = function_.name;
        d.block = block;
        d.instructionIndex = index;
        d.message = message;
        d.location = location;
        report_->diagnostics.push_back(std::move(d));
    }

    void warn(const std::string& message, BlockId block = kNoBlock, long index = -1,
              SourceLocation location = {}) {
        if (!report_) return;
        Diagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.function = function_.name;
        d.block = block;
        d.instructionIndex = index;
        d.message = message;
        d.location = location;
        report_->diagnostics.push_back(std::move(d));
    }

    // --- type helpers -----------------------------------------------------
    const Type* typeOf(std::uint32_t id) const { return module_.types.find(id); }

    std::string render(std::uint32_t id) const { return module_.types.render(id); }

    // True when `id` names a type that exists in this module's arena.
    bool validType(std::uint32_t id, const char* what, BlockId block, long index, SourceLocation loc) {
        if (id == 0) {
            error(std::string(what) + " has no type", block, index, loc);
            return false;
        }
        if (!typeOf(id)) {
            error(std::string(what) + " refers to unknown type id " + std::to_string(id), block, index, loc);
            return false;
        }
        return true;
    }

    // Assignability, mirroring TypeChecker::isAssignable for the cases the MIR
    // can decide without a symbol table. Unknown on either side is permissive:
    // it means semantic analysis did not resolve a type, and rejecting those
    // would turn every dynamically-typed corner of ZL into a MIR error.
    bool assignable(std::uint32_t fromId, std::uint32_t toId) const {
        if (fromId == toId) return true;
        const Type* from = typeOf(fromId);
        const Type* to = typeOf(toId);
        if (!from || !to) return false;
        if (from->kind == TypeKind::Unknown || to->kind == TypeKind::Unknown) return true;
        // An unsubstituted generic parameter stands for some concrete type that
        // is not knowable here.
        if (from->kind == TypeKind::TypeParam || to->kind == TypeKind::TypeParam) return true;
        // Nullability is the arena's question, not this function's: only the
        // arena can see through a union to its members.
        if (from->kind == TypeKind::Nil) return module_.types.isNullable(toId);
        if (from->kind == TypeKind::Int && to->kind == TypeKind::Double) return true;
        if (to->kind == TypeKind::Union) {
            return std::find(to->arguments.begin(), to->arguments.end(), fromId) != to->arguments.end();
        }
        if (from->kind == TypeKind::Union) {
            return std::all_of(from->arguments.begin(), from->arguments.end(),
                               [&](std::uint32_t member) { return assignable(member, toId); });
        }
        if (from->kind == TypeKind::Object && to->kind == TypeKind::Object) {
            // Walk every subtype edge a name can have. Only the unparameterized
            // base name is compared, which matches how the semantic model keys
            // inheritance for instantiations.
            //
            // There are three such edges, and leaving any one out rejects a
            // legal program rather than merely missing an optimisation:
            //
            //   class   --extends-->     class       (layout.parent)
            //   class   --implements-->  interface   (layout.interfaces)
            //   interface --extends-->   interface   (InterfaceInfo.bases)
            //
            // The first two are what makes `Shape masked = new Circle(...)`
            // verify; the third is what makes `Named n = shape` verify when
            // `interface Shape extends Named`. The search is breadth-first with
            // a visited set, because diamonds in either hierarchy would
            // otherwise be re-expanded, and a step guard because both tables are
            // input data rather than a guarantee.
            if (baseName(from->name) != baseName(to->name)) {
                const std::string wanted = baseName(to->name);
                std::vector<std::string> pending{from->name};
                std::vector<std::string> seen;
                std::size_t guard = 0;
                while (!pending.empty() && guard++ < 256) {
                    const std::string currentName = pending.back();
                    pending.pop_back();
                    if (std::find(seen.begin(), seen.end(), currentName) != seen.end()) continue;
                    seen.push_back(currentName);
                    if (baseName(currentName) == wanted) return true;
                    if (const ClassLayout* current = module_.classLayout(currentName)) {
                        if (!current->parent.empty()) pending.push_back(current->parent);
                        for (const auto& implemented : current->interfaces) {
                            pending.push_back(implemented);
                        }
                    }
                    if (const InterfaceInfo* info = module_.interfaceInfo(currentName)) {
                        for (const auto& base : info->bases) pending.push_back(base);
                    }
                }
                return false;
            }
            // Same base class: require equal arity and pairwise assignability.
            if (from->arguments.size() != to->arguments.size()) return false;
            for (std::size_t i = 0; i < from->arguments.size(); ++i) {
                if (!assignable(from->arguments[i], to->arguments[i])) return false;
            }
            return true;
        }
        // Same kind, different shape: compare structurally.
        if (from->kind != to->kind) return false;
        if (from->name != to->name) return false;
        if (from->arguments.size() != to->arguments.size()) return false;
        for (std::size_t i = 0; i < from->arguments.size(); ++i) {
            if (!assignable(from->arguments[i], to->arguments[i])) return false;
        }
        return true;
    }

    static std::string baseName(const std::string& name) {
        const auto open = name.find('<');
        return open == std::string::npos ? name : name.substr(0, open);
    }

    // --- signature --------------------------------------------------------
    void checkSignature() {
        if (function_.name.empty()) error("function has no name");
        if (function_.blocks.empty()) {
            error("function has no basic blocks");
            return;
        }
        if (function_.entryBlock == kNoBlock) {
            error("function has no entry block");
        } else if (function_.blocks.front().id != function_.entryBlock) {
            error("entry block b" + std::to_string(function_.entryBlock) +
                  " is not the function's first block (b" + std::to_string(function_.blocks.front().id) + ")");
        }
        if (!validType(function_.returnType, "return type", kNoBlock, -1, function_.location)) {
            // Everything downstream depends on a real return type.
            return;
        }
        const Type* returnType = typeOf(function_.returnType);
        if (returnType->kind == TypeKind::Nil) {
            error("return type cannot be nil; use void for a function with no result",
                  kNoBlock, -1, function_.location);
        }
        if (returnType->kind == TypeKind::Union) {
            error("return type cannot be a union at the MIR level", kNoBlock, -1, function_.location);
        }
        // An async function's MIR return type is the payload T; callers observe
        // Task<T>. A Task return type here would mean the payload is itself a
        // Task, which is never what `async func f(): T` produces.
        if (function_.isAsync && returnType->kind == TypeKind::Task) {
            error("async function return type must be the task payload, not Task<...>",
                  kNoBlock, -1, function_.location);
        }
        if (function_.isAsync && returnType->kind == TypeKind::Void) {
            // Legal in ZL (`async func f()` yields Task<void>), but worth
            // flagging because it is usually a lowering mistake.
            warn("async function returns void; callers receive Task<void>", kNoBlock, -1, function_.location);
        }

        std::unordered_set<std::string> parameterNames;
        for (std::size_t i = 0; i < function_.parameters.size(); ++i) {
            const auto& parameter = function_.parameters[i];
            const std::string what = "parameter " + std::to_string(i) + " ('" + parameter.name + "')";
            if (parameter.name.empty()) error(what + " has no name", kNoBlock, -1, parameter.location);
            if (!parameterNames.insert(parameter.name).second) {
                error(what + " duplicates an earlier parameter name", kNoBlock, -1, parameter.location);
            }
            if (!validType(parameter.type, what.c_str(), kNoBlock, -1, parameter.location)) continue;
            const Type* type = typeOf(parameter.type);
            if (type->kind == TypeKind::Void) error(what + " cannot have type void", kNoBlock, -1, parameter.location);
            if (type->kind == TypeKind::Nil) error(what + " cannot have type nil", kNoBlock, -1, parameter.location);
            if (parameter.ownership == zl::OwnershipKind::OWNED && type->kind == TypeKind::TypeParam) {
                warn(what + " is owned but its type is an unsubstituted generic parameter",
                     kNoBlock, -1, parameter.location);
            }
            checkTypeParamScope(parameter.type, "parameter " + std::to_string(i), kNoBlock, -1, parameter.location);
        }

        if (function_.isGenericTemplate && function_.typeParameters.empty()) {
            error("function is marked as a generic template but declares no type parameters",
                  kNoBlock, -1, function_.location);
        }
        if (!function_.isGenericTemplate && !function_.genericArguments.empty() && function_.typeParameters.empty()) {
            warn("function records generic arguments but declares no type parameters",
                 kNoBlock, -1, function_.location);
        }
        if (!function_.captures.empty() && !function_.isLambda) {
            warn("function records captures but is not a lambda", kNoBlock, -1, function_.location);
        }
        for (const auto& capture : function_.captures) {
            if (!validType(capture.type, ("capture '" + capture.name + "'").c_str(), kNoBlock, -1, function_.location))
                continue;
        }

        for (std::size_t i = 0; i < function_.slots.size(); ++i) {
            const auto& slot = function_.slots[i];
            const std::string what = "slot " + std::to_string(i + 1) + " ('" + slot.name + "')";
            if (!validType(slot.type, what.c_str(), kNoBlock, -1, slot.location)) continue;
            const Type* type = typeOf(slot.type);
            if (type->kind == TypeKind::Void) error(what + " cannot have type void", kNoBlock, -1, slot.location);
            if (slot.ownership == zl::OwnershipKind::BORROW && !slot.isMutable) {
                error(what + " is a borrow but is marked immutable", kNoBlock, -1, slot.location);
            }
            if (slot.ownership != zl::OwnershipKind::GC && !isReferenceKind(type->kind)) {
                error(what + " has storage '" + std::string(zl::ownershipName(slot.ownership)) +
                      "' but its type " + render(slot.type) + " is not a reference type",
                     kNoBlock, -1, slot.location);
            }
            checkTypeParamScope(slot.type, what, kNoBlock, -1, slot.location);
        }
    }

    // An unsubstituted generic parameter is only meaningful inside a template.
    void checkTypeParamScope(std::uint32_t id, const std::string& what, BlockId block, long index,
                             SourceLocation loc) {
        const Type* type = typeOf(id);
        if (!type) return;
        if (type->kind == TypeKind::TypeParam) {
            if (function_.typeParameters.empty()) {
                error(what + " uses generic parameter '" + type->name +
                      "' but the function is not a generic template", block, index, loc);
            } else if (std::find(function_.typeParameters.begin(), function_.typeParameters.end(), type->name) ==
                       function_.typeParameters.end()) {
                error(what + " uses generic parameter '" + type->name +
                      "' which the function does not declare", block, index, loc);
            }
            return;
        }
        for (std::uint32_t argument : type->arguments) checkTypeParamScope(argument, what, block, index, loc);
    }

    // --- SSA definitions --------------------------------------------------
    void collectDefinitions() {
        for (const auto& block : function_.blocks) {
            for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                const auto& instruction = block.instructions[i];
                if (instruction.result == kNoTemp) continue;
                const auto inserted = definitions_.emplace(instruction.result,
                                                           Definition{block.id, static_cast<long>(i)});
                if (!inserted.second) {
                    error("temp %" + std::to_string(instruction.result) + " is defined more than once "
                          "(first in block b" + std::to_string(inserted.first->second.block) + ")",
                          block.id, static_cast<long>(i), instruction.location);
                }
                // Where a value came from, for the ownership checks. A value
                // loaded or moved out of a slot inherits that slot's storage
                // contract; anything else has no slot provenance.
                if (instruction.opcode == Opcode::Load || instruction.opcode == Opcode::Move) {
                    provenance_[instruction.result] = instruction.slot;
                }
            }
        }
    }

    // --- control flow -----------------------------------------------------
    void checkControlFlow() {
        std::unordered_set<BlockId> seenIds;
        std::unordered_map<BlockId, BlockId> normalPredecessorCount;
        std::unordered_set<BlockId> unwindTargets;

        for (std::size_t i = 0; i < function_.blocks.size(); ++i) {
            const auto& block = function_.blocks[i];
            if (!seenIds.insert(block.id).second) {
                error("duplicate block id b" + std::to_string(block.id));
                continue;
            }
            // Dense, 1-based, position-matching ids. Backends index blocks
            // directly, so a gap or a reorder is a real hazard, not cosmetics.
            if (block.id != static_cast<BlockId>(i + 1)) {
                error("block at position " + std::to_string(i) + " has id b" + std::to_string(block.id) +
                      "; block ids must be 1-based and match their position");
            }
            if (block.instructions.empty() && block.terminator.kind == TerminatorKind::None) {
                error("block b" + std::to_string(block.id) + " is empty and has no terminator", block.id);
            }
            for (const auto& handler : block.exceptionHandlers) {
                if (handler.block == kNoBlock) {
                    error("block b" + std::to_string(block.id) + " has an exception handler with no target",
                          block.id, -1, block.location);
                    continue;
                }
                if (!cfg_.isValid(handler.block)) {
                    error("block b" + std::to_string(block.id) + " names exception handler block b" +
                          std::to_string(handler.block) + ", which does not exist", block.id, -1, block.location);
                    continue;
                }
                unwindTargets.insert(handler.block);
                if (handler.catchType != 0 && !typeOf(handler.catchType)) {
                    error("block b" + std::to_string(block.id) + " has an exception handler with unknown catch type id " +
                          std::to_string(handler.catchType), block.id, -1, block.location);
                }
                const BasicBlock* target = function_.block(handler.block);
                if (target && target->kind == BlockKind::Catch) {
                    if (handler.catchSlot == 0) {
                        error("exception handler for catch block b" + std::to_string(handler.block) +
                              " does not name a slot for the caught value", block.id, -1, block.location);
                    } else if (!function_.slot(handler.catchSlot)) {
                        error("exception handler for catch block b" + std::to_string(handler.block) +
                              " names unknown catch slot " + std::to_string(handler.catchSlot),
                              block.id, -1, block.location);
                    }
                } else if (handler.catchSlot != 0 && target && target->kind == BlockKind::Normal) {
                    error("exception handler names catch slot " + std::to_string(handler.catchSlot) +
                          " but its target block b" + std::to_string(handler.block) + " is not a catch block",
                          block.id, -1, block.location);
                }
            }
            for (BlockId successor : block.terminator.successors()) {
                if (!cfg_.isValid(successor)) {
                    error("terminator of block b" + std::to_string(block.id) + " refers to block b" +
                          std::to_string(successor) + ", which does not exist", block.id, -1,
                          block.terminator.location);
                    continue;
                }
                normalPredecessorCount[successor] = normalPredecessorCount[successor] + 1;
            }
            checkTerminator(block);
        }

        // A catch block must be reachable only by raising. If it also has a
        // normal predecessor, two different executions arrive with different
        // expectations about the catch slot, which no backend can honour.
        for (const auto& block : function_.blocks) {
            if (block.kind != BlockKind::Catch) continue;
            if (!cfg_.predecessors(block.id).empty()) {
                error("catch block b" + std::to_string(block.id) +
                      " is reachable by normal control flow; a catch block must only be entered along an "
                      "exception edge", block.id, -1, block.location);
            }
            if (!unwindTargets.count(block.id)) {
                error("catch block b" + std::to_string(block.id) +
                      " is not the target of any exception handler", block.id, -1, block.location);
            }
        }

        // The entry block is where execution starts, so nothing may jump back
        // into it. Allowing it would make "the first block" ambiguous.
        if (!cfg_.predecessors(function_.entryBlock).empty()) {
            error("entry block b" + std::to_string(function_.entryBlock) + " has predecessors",
                  function_.entryBlock);
        }

        // Reachability. Unwind-only blocks are reachable in the "an exception
        // can get here" sense, so only blocks that no edge at all reaches are
        // genuinely dead.
        for (const auto& block : function_.blocks) {
            if (cfg_.isReachable(block.id)) continue;
            if (cfg_.reachableWithUnwind().count(block.id)) continue;
            const std::string message = "block b" + std::to_string(block.id) +
                                        " is unreachable from the entry block";
            if (options_.unreachableBlocksAreErrors) error(message, block.id, -1, block.location);
            else warn(message, block.id, -1, block.location);
        }

        checkBlockParameters();
        checkEdgesConsistency();
    }

    void checkTerminator(const BasicBlock& block) {
        const auto& t = block.terminator;
        const BlockId id = block.id;
        if (t.kind == TerminatorKind::None) {
            error("block b" + std::to_string(id) + " has no terminator; every block must end in exactly one",
                  id, -1, block.location);
            return;
        }
        switch (t.kind) {
            case TerminatorKind::Return: {
                if (t.target != kNoBlock || t.elseBlock != kNoBlock || !t.cases.empty()) {
                    error("return terminator of block b" + std::to_string(id) + " carries branch targets",
                          id, -1, t.location);
                }
                const Type* returnType = typeOf(function_.returnType);
                const bool returnsVoid = returnType && returnType->kind == TypeKind::Void;
                if (returnsVoid) {
                    if (!t.value.isNone()) {
                        error("block b" + std::to_string(id) +
                              " returns a value from a void function", id, -1, t.location);
                    }
                } else if (t.value.isNone()) {
                    error("block b" + std::to_string(id) + " returns no value from a function returning " +
                          render(function_.returnType), id, -1, t.location);
                } else {
                    if (!checkOperand(t.value, "return value", id, -1, t.location)) break;
                    if (!assignable(t.value.type, function_.returnType)) {
                        error("block b" + std::to_string(id) + " returns " + render(t.value.type) +
                              " but the function returns " + render(function_.returnType),
                              id, -1, t.location);
                    }
                }
                break;
            }
            case TerminatorKind::Jump:
                if (t.target == kNoBlock) {
                    error("jump terminator of block b" + std::to_string(id) + " has no target", id, -1, t.location);
                }
                if (t.elseBlock != kNoBlock || !t.cases.empty() || !t.value.isNone()) {
                    error("jump terminator of block b" + std::to_string(id) + " carries unused operands",
                          id, -1, t.location);
                }
                break;
            case TerminatorKind::Branch: {
                if (t.target == kNoBlock || t.elseBlock == kNoBlock) {
                    error("branch terminator of block b" + std::to_string(id) +
                          " requires both a then and an else target", id, -1, t.location);
                }
                if (!t.cases.empty()) {
                    error("branch terminator of block b" + std::to_string(id) + " carries switch cases",
                          id, -1, t.location);
                }
                if (t.value.isNone()) {
                    error("branch terminator of block b" + std::to_string(id) + " has no condition",
                          id, -1, t.location);
                    break;
                }
                if (!checkOperand(t.value, "branch condition", id, -1, t.location)) break;
                const Type* condition = typeOf(t.value.type);
                if (!condition || condition->kind != TypeKind::Bool) {
                    error("branch condition of block b" + std::to_string(id) + " has type " +
                          render(t.value.type) + " but must be bool", id, -1, t.location);
                }
                break;
            }
            case TerminatorKind::Switch: {
                if (t.cases.empty()) {
                    error("switch terminator of block b" + std::to_string(id) + " has no cases", id, -1, t.location);
                }
                if (t.target == kNoBlock) {
                    error("switch terminator of block b" + std::to_string(id) + " has no default target",
                          id, -1, t.location);
                }
                if (t.elseBlock != kNoBlock) {
                    error("switch terminator of block b" + std::to_string(id) + " carries an else target",
                          id, -1, t.location);
                }
                std::set<std::int64_t> seen;
                for (const auto& entry : t.cases) {
                    if (!seen.insert(entry.value).second) {
                        error("switch terminator of block b" + std::to_string(id) + " has duplicate case value " +
                              std::to_string(entry.value), id, -1, t.location);
                    }
                }
                if (t.value.isNone()) {
                    error("switch terminator of block b" + std::to_string(id) + " has no subject", id, -1, t.location);
                    break;
                }
                if (!checkOperand(t.value, "switch subject", id, -1, t.location)) break;
                const Type* subject = typeOf(t.value.type);
                if (!subject || subject->kind != TypeKind::Int) {
                    error("switch subject of block b" + std::to_string(id) + " has type " + render(t.value.type) +
                          " but must be int", id, -1, t.location);
                }
                break;
            }
            case TerminatorKind::Throw:
                if (t.value.isNone()) {
                    error("throw terminator of block b" + std::to_string(id) + " has no value", id, -1, t.location);
                    break;
                }
                if (t.target != kNoBlock || t.elseBlock != kNoBlock || !t.cases.empty()) {
                    error("throw terminator of block b" + std::to_string(id) + " carries branch targets",
                          id, -1, t.location);
                }
                checkOperand(t.value, "thrown value", id, -1, t.location);
                break;
            case TerminatorKind::Unreachable:
                if (!t.value.isNone() || t.target != kNoBlock || t.elseBlock != kNoBlock || !t.cases.empty()) {
                    error("unreachable terminator of block b" + std::to_string(id) + " carries operands",
                          id, -1, t.location);
                }
                break;
            case TerminatorKind::None:
                break;
        }
    }

    // --- block parameters and edge arguments ------------------------------
    //
    // A block parameter is a definition with no instruction behind it, so
    // nothing in the instruction checks would catch a malformed one. These are
    // the rules that make "the value a join block reads" well defined:
    //
    //   * one definition per parameter id, in exactly one block;
    //   * a parameter's type is a real, non-void type;
    //   * the entry block has none (nothing can hand it a value);
    //   * every *normal* successor receives exactly as many arguments as the
    //     target has parameters, each with the parameter's type;
    //   * a terminator with no successors carries no arguments.
    //
    // An unwind edge gets no arguments on purpose: a catch block is entered by
    // an exception, not by the throwing block deciding what to pass it.
    void checkBlockParameters() {
        std::unordered_map<BlockParamId, BlockId> definedIn;
        for (const auto& block : function_.blocks) {
            for (const auto& parameter : block.parameters) {
                const std::string what = "block parameter ^" + std::to_string(parameter.id) +
                                         " ('" + parameter.name + "')";
                if (parameter.id == kNoBlockParam) {
                    error("block b" + std::to_string(block.id) +
                          " declares a block parameter with id 0; ids are 1-based", block.id, -1,
                          block.location);
                    continue;
                }
                const auto inserted = definedIn.emplace(parameter.id, block.id);
                if (!inserted.second) {
                    error(what + " is defined by both block b" +
                          std::to_string(inserted.first->second) + " and block b" +
                          std::to_string(block.id) + "; a value has one definition",
                          block.id, -1, parameter.location);
                    continue;
                }
                if (!validType(parameter.type, what.c_str(), block.id, -1, parameter.location)) continue;
                const Type* type = typeOf(parameter.type);
                if (type && type->kind == TypeKind::Void) {
                    error(what + " cannot have type void", block.id, -1, parameter.location);
                }
                checkTypeParamScope(parameter.type, what, block.id, -1, parameter.location);
            }
            if (block.id == function_.entryBlock && !block.parameters.empty()) {
                error("entry block b" + std::to_string(block.id) +
                      " declares block parameters; nothing can supply them on entry",
                      block.id, -1, block.location);
            }
        }

        for (const auto& block : function_.blocks) {
            const auto& terminator = block.terminator;
            const auto successors = terminator.successors();
            if (successors.empty()) {
                if (!terminator.edgeArguments.empty()) {
                    error("block b" + std::to_string(block.id) + " has a " +
                          std::string(terminatorKindName(terminator.kind)) +
                          " terminator that carries block-parameter arguments but transfers nowhere",
                          block.id, -1, terminator.location);
                }
                continue;
            }
            // A block reached by falling through is given a value by this
            // terminator, so the argument list has to be complete.
            for (std::size_t i = 0; i < successors.size(); ++i) {
                const BasicBlock* target = function_.block(successors[i]);
                const std::string edge = "block b" + std::to_string(block.id) + " -> b" +
                                         std::to_string(successors[i]);
                const std::size_t expected = target ? target->parameters.size() : 0;
                if (i >= terminator.edgeArguments.size()) {
                    if (expected != 0) {
                        error(edge + " supplies no arguments but block b" +
                              std::to_string(successors[i]) + " has " + std::to_string(expected) +
                              " block parameter(s)", block.id, -1, terminator.location);
                    }
                    continue;
                }
                const auto& arguments = terminator.edgeArguments[i];
                if (arguments.size() != expected) {
                    error(edge + " supplies " + std::to_string(arguments.size()) +
                          " block-parameter argument(s) but block b" + std::to_string(successors[i]) +
                          " has " + std::to_string(expected), block.id, -1, terminator.location);
                    continue;
                }
                for (std::size_t a = 0; a < arguments.size(); ++a) {
                    const auto& argument = arguments[a];
                    const auto& parameter = target->parameters[a];
                    const std::string what = edge + " argument " + std::to_string(a) + " for ^" +
                                             std::to_string(parameter.id) + " ('" + parameter.name + "')";
                    if (!checkOperand(argument, what, block.id, -1, terminator.location)) continue;
                    if (!assignable(argument.type, parameter.type)) {
                        error(what + " is " + render(argument.type) + " but the parameter is " +
                              render(parameter.type), block.id, -1, terminator.location);
                    }
                }
            }
            if (terminator.edgeArguments.size() > successors.size()) {
                error("block b" + std::to_string(block.id) + " supplies arguments for " +
                      std::to_string(terminator.edgeArguments.size()) + " successors but has " +
                      std::to_string(successors.size()), block.id, -1, terminator.location);
            }
        }

        // Every normal predecessor must hand a parameter its value; a parameter
        // whose block has a predecessor that never supplies it would read
        // whatever was left behind on that path.
        for (const auto& block : function_.blocks) {
            if (block.parameters.empty()) continue;
            for (BlockId pred : cfg_.predecessors(block.id)) {
                const BasicBlock* predecessor = function_.block(pred);
                if (!predecessor) continue; // reported elsewhere
                const auto successors = predecessor->terminator.successors();
                std::size_t edgeIndex = successors.size();
                for (std::size_t i = 0; i < successors.size(); ++i) {
                    if (successors[i] == block.id) { edgeIndex = i; break; }
                }
                if (edgeIndex >= predecessor->terminator.edgeArguments.size()) {
                    error("block b" + std::to_string(pred) + " reaches block b" +
                          std::to_string(block.id) + " without supplying its " +
                          std::to_string(block.parameters.size()) + " block parameter(s)",
                          pred, -1, predecessor->terminator.location);
                }
            }
        }
    }

    // The materialised edge list must be exactly what the terminators imply. A
    // stale list is how a pass that rewrote control flow silently desynchronises
    // itself from every consumer that reads predecessors.
    void checkEdgesConsistency() {
        Function copy = function_;
        copy.rebuildEdges();
        if (copy.edges == function_.edges) return;
        error("control-flow edge list does not match the block terminators; call Function::rebuildEdges()",
              kNoBlock, -1, function_.location);
    }

    // --- operands ---------------------------------------------------------
    bool checkOperand(const Operand& operand, const std::string& what, BlockId block, long index,
                      SourceLocation loc) {
        if (operand.kind == OperandKind::None) {
            error(what + " is absent", block, index, loc);
            return false;
        }
        if (!validType(operand.type, what.c_str(), block, index, loc)) return false;
        switch (operand.kind) {
            case OperandKind::Const: {
                const Constant* constant = module_.constant(operand.index);
                if (!constant) {
                    error(what + " refers to unknown constant " + std::to_string(operand.index), block, index, loc);
                    return false;
                }
                const Type* type = typeOf(operand.type);
                if (!type) return false;
                const bool matches =
                    (constant->kind == ConstKind::Bool && type->kind == TypeKind::Bool) ||
                    (constant->kind == ConstKind::Int && type->kind == TypeKind::Int) ||
                    (constant->kind == ConstKind::Double && type->kind == TypeKind::Double) ||
                    (constant->kind == ConstKind::String && type->kind == TypeKind::String) ||
                    (constant->kind == ConstKind::Nil && module_.types.isNullable(operand.type)) ||
                    (constant->kind == ConstKind::EnumMember && type->kind == TypeKind::Object &&
                     type->name == constant->enumTypeName);
                if (!matches) {
                    error(what + " is the " + std::string(constKindName(constant->kind)) + " constant " +
                          std::to_string(operand.index) + " but has type " + render(operand.type),
                          block, index, loc);
                    return false;
                }
                return true;
            }
            case OperandKind::Temp: {
                if (operand.index == kNoTemp) {
                    error(what + " refers to no temp", block, index, loc);
                    return false;
                }
                const auto it = definitions_.find(operand.index);
                if (it == definitions_.end()) {
                    error(what + " refers to temp %" + std::to_string(operand.index) +
                          ", which is never defined in this function", block, index, loc);
                    return false;
                }
                if (tempTypes_.count(operand.index) && tempTypes_[operand.index] != operand.type) {
                    error(what + " refers to temp %" + std::to_string(operand.index) + " as " +
                          render(operand.type) + " but it is defined as " + render(tempTypes_[operand.index]),
                          block, index, loc);
                    return false;
                }
                return true;
            }
            case OperandKind::Param: {
                const Parameter* parameter = function_.parameter(operand.index);
                if (!parameter) {
                    error(what + " refers to unknown parameter " + std::to_string(operand.index), block, index, loc);
                    return false;
                }
                if (parameter->type != operand.type) {
                    error(what + " refers to parameter " + std::to_string(operand.index) + " ('" + parameter->name +
                          "') as " + render(operand.type) + " but it is declared " + render(parameter->type),
                          block, index, loc);
                    return false;
                }
                return true;
            }
            case OperandKind::BlockParam: {
                if (operand.index == kNoBlockParam) {
                    error(what + " refers to no block parameter", block, index, loc);
                    return false;
                }
                const BlockParameter* parameter = function_.blockParameter(operand.index);
                if (!parameter) {
                    error(what + " refers to block parameter ^" + std::to_string(operand.index) +
                          ", which no block defines", block, index, loc);
                    return false;
                }
                if (parameter->type != operand.type) {
                    error(what + " refers to block parameter ^" + std::to_string(operand.index) + " ('" +
                          parameter->name + "') as " + render(operand.type) + " but it is declared " +
                          render(parameter->type), block, index, loc);
                    return false;
                }
                // A block parameter is defined on entry to its block, so using
                // one is only sound where that block dominates - the same rule
                // a temp's definition is held to. Without this check a value
                // computed on one branch could be read on another.
                const BasicBlock* owner = function_.blockOfParameter(operand.index);
                const BasicBlock* usingBlock = function_.block(block);
                if (!owner || !usingBlock) return true; // reported above
                if (owner->id != block && !cfg_.dominates(owner->id, block)) {
                    error(what + " uses block parameter ^" + std::to_string(operand.index) + " defined by block b" +
                          std::to_string(owner->id) + ", which does not dominate block b" +
                          std::to_string(block), block, index, loc);
                    return false;
                }
                return true;
            }
            case OperandKind::Static: {
                const StaticField* field = module_.staticField(operand.index);
                if (!field) {
                    error(what + " refers to unknown static field " + std::to_string(operand.index),
                          block, index, loc);
                    return false;
                }
                if (field->type != operand.type) {
                    error(what + " refers to static " + field->className + "." + field->name + " as " +
                          render(operand.type) + " but it is declared " + render(field->type),
                          block, index, loc);
                    return false;
                }
                return true;
            }
            case OperandKind::None:
                break;
        }
        return false;
    }

    // --- instructions -----------------------------------------------------
    void checkInstructions() {
        for (const auto& block : function_.blocks) {
            for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                const auto& instruction = block.instructions[i];
                checkInstruction(block, instruction, static_cast<long>(i));
            }
        }
    }

    void checkInstruction(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const OpcodeShape& shape = opcodeShape(instruction.opcode);
        const SourceLocation& loc = instruction.location;
        const char* name = opcodeName(instruction.opcode);

        if (instruction.opcode == Opcode::Nop) {
            // A Nop is a placeholder, not a value producer.
            if (instruction.result != kNoTemp) {
                error(std::string(name) + " must not define a result", id, index, loc);
            }
            return;
        }

        // Operand count.
        if (shape.variadicOperands) {
            if (instruction.operands.size() < shape.operandCount) {
                error(std::string(name) + " requires at least " + std::to_string(shape.operandCount) +
                      " operand(s) but has " + std::to_string(instruction.operands.size()), id, index, loc);
                return;
            }
        } else if (instruction.operands.size() != shape.operandCount) {
            error(std::string(name) + " requires " + std::to_string(shape.operandCount) + " operand(s) but has " +
                  std::to_string(instruction.operands.size()), id, index, loc);
            return;
        }

        // Result presence.
        const bool resultPresent = instruction.result != kNoTemp;
        if (resultPresent && !shape.producesResult) {
            error(std::string(name) + " does not produce a value but defines temp %" +
                  std::to_string(instruction.result), id, index, loc);
        }
        if (!resultPresent && shape.producesResult && !shape.optionalResult) {
            error(std::string(name) + " produces a value but defines no temp", id, index, loc);
        }
        if (resultPresent) {
            validType(instruction.resultType, (std::string(name) + " result").c_str(), id, index, loc);
            tempTypes_[instruction.result] = instruction.resultType;
        } else if (instruction.resultType != 0) {
            error(std::string(name) + " defines no temp but records result type " +
                  render(instruction.resultType), id, index, loc);
        }

        // Every operand must resolve and be consistently typed.
        for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
            checkOperand(instruction.operands[i], std::string(name) + " operand " + std::to_string(i),
                         id, index, loc);
        }

        // Slot operands.
        if (opcodeUsesSlot(instruction.opcode)) {
            const Slot* slot = function_.slot(instruction.slot);
            if (!slot) {
                error(std::string(name) + " refers to unknown slot " + std::to_string(instruction.slot),
                      id, index, loc);
                return;
            }
            checkSlotRules(block, instruction, index, *slot);
            return;
        }
        if (instruction.slot != 0) {
            error(std::string(name) + " does not take a slot but names slot " +
                  std::to_string(instruction.slot), id, index, loc);
        }

        checkValueRules(block, instruction, index);
    }

    void checkSlotRules(const BasicBlock& block, const Instruction& instruction, long index, const Slot& slot) {
        const BlockId id = block.id;
        const char* name = opcodeName(instruction.opcode);
        const SourceLocation& loc = instruction.location;
        const std::string slotName = "slot " + std::to_string(instruction.slot) + " ('" + slot.name + "')";

        switch (instruction.opcode) {
            case Opcode::Load:
                if (instruction.resultType != slot.type) {
                    error(std::string(name) + " from " + slotName + " of type " + render(slot.type) +
                          " produces " + render(instruction.resultType), id, index, loc);
                }
                break;
            case Opcode::Store: {
                if (!slot.isMutable) {
                    error("store to immutable " + slotName, id, index, loc);
                }
                if (instruction.operands.empty()) break;
                const Operand& value = instruction.operands[0];
                if (value.isNone()) break;
                if (!assignable(value.type, slot.type)) {
                    error("store of " + render(value.type) + " into " + slotName + " of type " +
                          render(slot.type), id, index, loc);
                }
                break;
            }
            case Opcode::Move:
                if (slot.ownership != zl::OwnershipKind::OWNED) {
                    error("move from " + slotName + " whose storage is '" +
                          std::string(zl::ownershipName(slot.ownership)) + "'; only an owned slot can be moved",
                          id, index, loc);
                }
                if (instruction.resultType != slot.type) {
                    error(std::string(name) + " from " + slotName + " of type " + render(slot.type) +
                          " produces " + render(instruction.resultType), id, index, loc);
                }
                break;
            case Opcode::Borrow: {
                if (slot.ownership != zl::OwnershipKind::BORROW) {
                    error("borrow into " + slotName + " whose storage is '" +
                          std::string(zl::ownershipName(slot.ownership)) + "'; the destination must be a borrow slot",
                          id, index, loc);
                }
                if (instruction.operands.empty()) break;
                const Operand& owner = instruction.operands[0];
                if (owner.isNone()) break;
                const Type* ownerType = typeOf(owner.type);
                if (ownerType && !isReferenceKind(ownerType->kind)) {
                    error("borrow of " + render(owner.type) + ", which is not a reference type", id, index, loc);
                }
                break;
            }
            case Opcode::EndBorrow:
                if (slot.ownership != zl::OwnershipKind::BORROW) {
                    error("end_borrow of " + slotName + " whose storage is '" +
                          std::string(zl::ownershipName(slot.ownership)) + "'", id, index, loc);
                }
                break;
            default:
                break;
        }
    }

    void checkValueRules(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const Opcode opcode = instruction.opcode;
        const char* name = opcodeName(opcode);
        const SourceLocation& loc = instruction.location;
        const auto& operands = instruction.operands;

        auto operandType = [&](std::size_t i) -> std::uint32_t {
            return i < operands.size() ? operands[i].type : 0;
        };

        if (isBinaryArithmetic(opcode) || isBitwise(opcode) || opcode == Opcode::And || opcode == Opcode::Or) {
            checkBinaryOperator(block, instruction, index);
            return;
        }
        if (opcode == Opcode::Neg || opcode == Opcode::BitNot || opcode == Opcode::Not) {
            checkUnaryOperator(block, instruction, index);
            return;
        }
        if (isComparison(opcode)) {
            checkComparison(block, instruction, index);
            return;
        }

        switch (opcode) {
            case Opcode::Widen: {
                const Type* from = typeOf(operandType(0));
                if (from && from->kind != TypeKind::Int) {
                    error("widen requires an int operand but has " + render(operandType(0)), id, index, loc);
                }
                if (instruction.resultType != module_.types.doubleType()) {
                    error("widen must produce double but produces " + render(instruction.resultType),
                          id, index, loc);
                }
                break;
            }
            case Opcode::Refine: {
                if (!assignable(instruction.resultType, operandType(0))) {
                    error("refine from " + render(operandType(0)) + " to unrelated type " +
                          render(instruction.resultType), id, index, loc);
                }
                if (instruction.resultType == operandType(0)) {
                    warn("refine to the same type is a no-op", id, index, loc);
                }
                break;
            }
            case Opcode::TypeTest: {
                if (instruction.resultType != module_.types.boolType()) {
                    error("type_test must produce bool but produces " + render(instruction.resultType),
                          id, index, loc);
                }
                if (instruction.testedType == kNoType) {
                    error("type_test has no tested type", id, index, loc);
                    break;
                }
                const Type* tested = typeOf(instruction.testedType);
                if (!tested) {
                    error("type_test tests an unknown type id", id, index, loc);
                } else if (tested->kind == TypeKind::Void) {
                    error("type_test cannot test against void", id, index, loc);
                } else if (tested->kind == TypeKind::TypeParam) {
                    // A type parameter stands for a type only the instantiation
                    // knows, so nothing here can be checked at all.
                    error("type_test against unresolved type parameter " + render(instruction.testedType),
                          id, index, loc);
                }
                const Type* actual = typeOf(operandType(0));
                if (actual && tested && actual->kind != TypeKind::Unknown &&
                    tested->kind != TypeKind::Unknown &&
                    !assignable(instruction.testedType, operandType(0)) &&
                    !assignable(operandType(0), instruction.testedType)) {
                    // Neither direction relates: the answer is always "no", so
                    // the arm this test guards is dead code.
                    warn("type_test of " + render(operandType(0)) + " against unrelated " +
                         render(instruction.testedType) + " can never match", id, index, loc);
                }
                break;
            }
            case Opcode::NullCheck: {
                const Type* type = typeOf(operandType(0));
                if (type && type->kind != TypeKind::Unknown && !module_.types.isNullable(operandType(0))) {
                    error("null_check on " + render(operandType(0)) + ", which cannot be nil", id, index, loc);
                }
                if (instruction.resultType != operandType(0)) {
                    error("null_check must produce its operand's type", id, index, loc);
                }
                break;
            }
            case Opcode::IsNull: {
                const Type* type = typeOf(operandType(0));
                if (type && type->kind != TypeKind::Unknown && !module_.types.isNullable(operandType(0))) {
                    error("is_null on " + render(operandType(0)) + ", which cannot be nil", id, index, loc);
                }
                requireResultType(instruction, module_.types.boolType(), "is_null", id, index, loc);
                break;
            }
            case Opcode::FieldLoad:
            case Opcode::FieldStore:
                checkFieldAccess(block, instruction, index);
                break;
            case Opcode::IndexLoad:
            case Opcode::IndexStore:
                checkIndexAccess(block, instruction, index);
                break;
            case Opcode::StaticLoad:
            case Opcode::StaticStore:
                checkStaticAccess(block, instruction, index);
                break;
            case Opcode::Alloc:
                checkAlloc(block, instruction, index);
                break;
            case Opcode::Call:
            case Opcode::InvokeMethod:
            case Opcode::InvokeSuper:
            case Opcode::InvokeStatic:
            case Opcode::CallIndirect:
            case Opcode::CallNative:
                checkCall(block, instruction, index);
                break;
            case Opcode::MakeClosure:
                checkMakeClosure(block, instruction, index);
                break;
            case Opcode::Await:
                checkAwait(block, instruction, index);
                break;
            case Opcode::TaskCreate: {
                const Type* result = typeOf(instruction.resultType);
                if (!result || result->kind != TypeKind::Task) {
                    error("task_create must produce Task<T> but produces " + render(instruction.resultType),
                          id, index, loc);
                    break;
                }
                const Type* payload = typeOf(operandType(0));
                if (!payload) break;
                if (payload->kind == TypeKind::Nil) {
                    if (!result->arguments.empty() && typeOf(result->arguments.front()) &&
                        typeOf(result->arguments.front())->kind != TypeKind::Void) {
                        error("task_create from nil must produce Task<void>", id, index, loc);
                    }
                } else if (!result->arguments.empty() && !assignable(operandType(0), result->arguments.front())) {
                    error("task_create wraps " + render(operandType(0)) + " but produces " +
                          render(instruction.resultType), id, index, loc);
                }
                break;
            }
            case Opcode::Drop: {
                // Dropping a borrowed value would release something this
                // function does not own; the borrow is released by EndBorrow.
                if (operandType(0) == 0) break;
                const auto provenance = provenance_.find(operands[0].kind == OperandKind::Temp ? operands[0].index : 0);
                if (provenance != provenance_.end()) {
                    if (const Slot* slot = function_.slot(provenance->second)) {
                        if (slot->ownership == zl::OwnershipKind::BORROW) {
                            error("drop of a value borrowed from slot " + std::to_string(provenance->second) +
                                  " ('" + slot->name + "'); end the borrow instead", id, index, loc);
                        }
                    }
                }
                break;
            }
            case Opcode::NewCollection: {
                const Type* result = typeOf(instruction.resultType);
                if (!result || !isCollectionType(*result)) {
                    error("new_collection must produce list/set/map but produces " +
                          render(instruction.resultType), id, index, loc);
                }
                break;
            }
            case Opcode::RangeInBounds: {
                for (std::size_t i = 0; i < operands.size(); ++i) {
                    const Type* type = typeOf(operandType(i));
                    if (type && !isNumericKind(type->kind)) {
                        error(std::string(name) + " operand " + std::to_string(i) + " has type " +
                              render(operandType(i)) + " but must be numeric", id, index, loc);
                    }
                }
                requireResultType(instruction, module_.types.boolType(), name, id, index, loc);
                break;
            }
            case Opcode::Log:
                if (operands.empty() || operands[0].isNone()) {
                    error("log requires a value", id, index, loc);
                }
                break;
            case Opcode::Nop:
                break;
            default:
                // Any opcode without an explicit rule here is a gap in the
                // verifier, not a licence for the instruction. Say so loudly.
                error(std::string("verifier has no typing rule for opcode '") + name + "'", id, index, loc);
                break;
        }
    }

    void requireResultType(const Instruction& instruction, std::uint32_t expected, const char* name,
                           BlockId id, long index, SourceLocation loc) {
        if (instruction.resultType != expected) {
            error(std::string(name) + " must produce " + render(expected) + " but produces " +
                  render(instruction.resultType), id, index, loc);
        }
    }

    void checkBinaryOperator(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const Opcode opcode = instruction.opcode;
        const char* name = opcodeName(opcode);
        const SourceLocation& loc = instruction.location;
        const std::uint32_t leftId = instruction.operands[0].type;
        const std::uint32_t rightId = instruction.operands[1].type;
        const Type* left = typeOf(leftId);
        const Type* right = typeOf(rightId);
        if (!left || !right) return;

        // Ask the language's own operator table. This is the single source of
        // truth for what `a op b` means, so the MIR cannot disagree with the
        // type checker about, say, `string + int` or `int - double`.
        const auto result = zl::OperatorRules::binaryResult(binaryToken(opcode), toZlType(left), toZlType(right));
        if (!result) {
            error(std::string(name) + " on " + render(leftId) + " and " + render(rightId) + ": " +
                  zl::OperatorRules::binaryError(binaryToken(opcode)), id, index, loc);
            return;
        }
        if (opcode == Opcode::And || opcode == Opcode::Or) {
            if (left->kind != TypeKind::Bool || right->kind != TypeKind::Bool) {
                error(std::string(name) + " requires bool operands but has " + render(leftId) + " and " +
                      render(rightId), id, index, loc);
            }
        }
        const std::uint32_t expected = primitiveTypeFor(module_.types, *result);
        if (expected == 0) {
            error(std::string(name) + " produced non-primitive result " + zlTypeName(*result), id, index, loc);
            return;
        }
        requireResultType(instruction, expected, name, id, index, loc);
    }

    void checkUnaryOperator(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const Opcode opcode = instruction.opcode;
        const char* name = opcodeName(opcode);
        const SourceLocation& loc = instruction.location;
        const std::uint32_t operandId = instruction.operands[0].type;
        const Type* operand = typeOf(operandId);
        if (!operand) return;

        const zl::TokenType token = opcode == Opcode::Neg    ? zl::TokenType::MINUS
                                    : opcode == Opcode::BitNot ? zl::TokenType::BIT_NOT
                                                               : zl::TokenType::NOT;
        const auto result = zl::OperatorRules::unaryResult(token, toZlType(operand));
        if (!result) {
            error(std::string(name) + " on " + render(operandId) + ": " + zl::OperatorRules::unaryError(token),
                  id, index, loc);
            return;
        }
        const std::uint32_t expected = primitiveTypeFor(module_.types, *result);
        if (expected == 0) {
            error(std::string(name) + " produced non-primitive result " + zlTypeName(*result), id, index, loc);
            return;
        }
        requireResultType(instruction, expected, name, id, index, loc);
    }

    void checkComparison(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const Opcode opcode = instruction.opcode;
        const char* name = opcodeName(opcode);
        const SourceLocation& loc = instruction.location;
        const std::uint32_t leftId = instruction.operands[0].type;
        const std::uint32_t rightId = instruction.operands[1].type;
        const Type* left = typeOf(leftId);
        const Type* right = typeOf(rightId);
        if (!left || !right) return;

        requireResultType(instruction, module_.types.boolType(), name, id, index, loc);

        // Equality additionally admits nil against a nullable reference, which
        // is how ZL spells a null test.
        if (opcode == Opcode::Eq || opcode == Opcode::Ne) {
            if (left->kind == TypeKind::Nil || right->kind == TypeKind::Nil) {
                const Type* other = left->kind == TypeKind::Nil ? right : left;
                const std::uint32_t otherId = left->kind == TypeKind::Nil ? rightId : leftId;
                if (!module_.types.isNullable(otherId) && other->kind != TypeKind::Unknown) {
                    error(std::string(name) + " compares nil with " +
                          render(otherId) + ", which cannot be nil",
                          id, index, loc);
                }
                return;
            }
            if (leftId == rightId) return;
            if (isNumericKind(left->kind) && isNumericKind(right->kind)) return;
            if (assignable(leftId, rightId) || assignable(rightId, leftId)) return;
            error(std::string(name) + " between unrelated types " + render(leftId) + " and " + render(rightId),
                  id, index, loc);
            return;
        }

        const auto result = zl::OperatorRules::binaryResult(binaryToken(opcode), toZlType(left), toZlType(right));
        if (!result) {
            error(std::string(name) + " on " + render(leftId) + " and " + render(rightId) + ": " +
                  zl::OperatorRules::binaryError(binaryToken(opcode)), id, index, loc);
        }
    }

    // A field is reachable through its class OR any class it inherits from, so
    // the lookup has to walk `parent`. Searching only the receiver's own layout
    // rejects `this.name` in a subclass for a field the parent declares - which
    // is ordinary inheritance, not malformed MIR.
    [[nodiscard]] const FieldLayout* findFieldLayout(const std::string& className,
                                                    const std::string& fieldName) const {
        std::vector<std::string> visited;
        std::string current = className;
        while (!current.empty()) {
            if (std::find(visited.begin(), visited.end(), current) != visited.end()) return nullptr; // cycle guard
            visited.push_back(current);
            const ClassLayout* layout = module_.classLayout(current);
            if (!layout) return nullptr;
            if (const FieldLayout* field = layout->field(fieldName)) return field;
            current = layout->parent;
        }
        return nullptr;
    }

    void checkFieldAccess(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const Opcode opcode = instruction.opcode;
        const char* name = opcodeName(opcode);
        const SourceLocation& loc = instruction.location;
        const Operand& base = instruction.operands[0];
        const Type* baseType = typeOf(base.type);

        if (instruction.name.empty()) {
            error(std::string(name) + " has no field name", id, index, loc);
            return;
        }
        if (!baseType) return;
        // `Shared<T>` has its own kind rather than being a plain Object, but it
        // is a class with real fields (`__value`), so its methods store to them
        // like any other class does.
        if (baseType->kind != TypeKind::Object && baseType->kind != TypeKind::Shared &&
            baseType->kind != TypeKind::Unknown) {
            error(std::string(name) + " on " + render(base.type) + ", which has no fields", id, index, loc);
            return;
        }
        if (baseType->kind == TypeKind::Unknown) return;

        if (!module_.classLayout(baseType->name)) {
            // A module may legitimately omit layouts for classes it does not
            // own. Flag it, but do not call the MIR invalid.
            warn(std::string(name) + " on class '" + baseType->name +
                 "' which has no layout in this module; the field type cannot be checked", id, index, loc);
            return;
        }
        const FieldLayout* field = findFieldLayout(baseType->name, instruction.name);
        if (!field) {
            error(std::string(name) + " of field '" + instruction.name + "' which class '" + baseType->name +
                  "' and its parents do not declare", id, index, loc);
            return;
        }
        if (field->isStatic) {
            error(std::string(name) + " of static field '" + instruction.name +
                  "'; use static_load/static_store", id, index, loc);
            return;
        }
        if (opcode == Opcode::FieldLoad) {
            if (!assignable(field->type, instruction.resultType)) {
                error("field_load of '" + instruction.name + "' declared " + render(field->type) +
                      " produces " + render(instruction.resultType), id, index, loc);
            }
        } else {
            const Operand& value = instruction.operands[1];
            if (!assignable(value.type, field->type)) {
                error("field_store of " + render(value.type) + " into field '" + instruction.name +
                      "' declared " + render(field->type), id, index, loc);
            }
        }
    }

    void checkIndexAccess(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const Opcode opcode = instruction.opcode;
        const char* name = opcodeName(opcode);
        const SourceLocation& loc = instruction.location;
        const Operand& base = instruction.operands[0];
        const Operand& key = instruction.operands[1];
        const Type* baseType = typeOf(base.type);
        if (!baseType) return;

        std::uint32_t elementType = 0;
        // ZL spells a collection two ways: the `list`/`set`/`map` keyword forms
        // arrive as their own kinds, and the `List`/`Set`/`Map` class forms
        // arrive as an Object carrying that name. Which of the two it is decides
        // how the key operand is read, so classify once instead of repeating the
        // test down every arm - and so a spelling cannot end up handled for
        // construction but not for indexing.
        const bool keyed = baseType->kind == TypeKind::Map ||
                           (baseType->kind == TypeKind::Object && baseType->name == "Map");
        const bool positional = baseType->kind == TypeKind::List ||
                                baseType->kind == TypeKind::Set ||
                                baseType->kind == TypeKind::Array ||
                                (baseType->kind == TypeKind::Object &&
                                 (baseType->name == "List" || baseType->name == "Set"));
        if (baseType->kind == TypeKind::Unknown) return;
        if (keyed) {
            // A map is indexed by its key rather than by position, so the key
            // operand is checked against the map's key type and the element
            // type is the map's value type. The taxonomy has always said
            // index_load/index_store cover maps; only this check was missing.
            if (baseType->arguments.size() >= 1 &&
                !assignable(key.type, baseType->arguments[0]) &&
                typeOf(key.type) && typeOf(key.type)->kind != TypeKind::Unknown) {
                error(std::string(name) + " on " + render(base.type) + " with key of type " +
                      render(key.type) + "; the key must be " + render(baseType->arguments[0]),
                      id, index, loc);
            }
            if (baseType->arguments.size() >= 2) elementType = baseType->arguments[1];
        } else if (positional) {
            // A set is written positionally - a set literal is built by storing
            // each element in turn - so it indexes like a list even though the
            // language has no positional read for one.
            const Type* keyType = typeOf(key.type);
            if (keyType && keyType->kind != TypeKind::Int && keyType->kind != TypeKind::Unknown) {
                error(std::string(name) + " on " + render(base.type) + " with index of type " +
                      render(key.type) + "; the index must be int", id, index, loc);
            }
            if (!baseType->arguments.empty()) elementType = baseType->arguments.front();
        } else {
            // ZL allows `x[i]` on a List<T> and on nothing else - see
            // TypeChecker::inferIndexAccess, which requires a List<T>.
            error(std::string(name) + " on " + render(base.type) +
                  "; indexed access requires a List<T>", id, index, loc);
            return;
        }

        if (elementType == 0) return;
        if (opcode == Opcode::IndexLoad) {
            if (!assignable(elementType, instruction.resultType)) {
                error("index_load from " + render(base.type) + " yields " + render(elementType) +
                      " but produces " + render(instruction.resultType), id, index, loc);
            }
        } else {
            const Operand& value = instruction.operands[2];
            if (!assignable(value.type, elementType)) {
                error("index_store of " + render(value.type) + " into " + render(base.type) +
                      " whose element type is " + render(elementType), id, index, loc);
            }
        }
    }

    void checkStaticAccess(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const char* name = opcodeName(instruction.opcode);
        const SourceLocation& loc = instruction.location;
        if (instruction.target.className.empty() || instruction.name.empty()) {
            error(std::string(name) + " requires both a class name and a field name", id, index, loc);
            return;
        }
        const StaticField* match = nullptr;
        for (const auto& candidate : module_.statics) {
            if (candidate.className == instruction.target.className && candidate.name == instruction.name) {
                match = &candidate;
                break;
            }
        }
        if (!match) {
            warn(std::string(name) + " of static '" + instruction.target.className + "." + instruction.name +
                 "' which this module does not declare", id, index, loc);
            return;
        }
        if (instruction.opcode == Opcode::StaticLoad) {
            if (!assignable(match->type, instruction.resultType)) {
                error("static_load of '" + match->name + "' declared " + render(match->type) + " produces " +
                      render(instruction.resultType), id, index, loc);
            }
        } else if (!instruction.operands.empty()) {
            if (!assignable(instruction.operands[0].type, match->type)) {
                error("static_store of " + render(instruction.operands[0].type) + " into '" + match->name +
                      "' declared " + render(match->type), id, index, loc);
            }
        }
    }

    void checkAlloc(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const SourceLocation& loc = instruction.location;
        if (instruction.className.empty()) {
            error("alloc has no class name", id, index, loc);
            return;
        }
        const Type* result = typeOf(instruction.resultType);
        if (!result) return;
        // `Shared<T>` is its own kind rather than a plain Object, but it is a
        // heap reference to an instance of a class just the same, so it is a
        // legal thing for a construction to produce.
        if (result->kind != TypeKind::Object && result->kind != TypeKind::Shared) {
            error("alloc must produce an object but produces " + render(instruction.resultType), id, index, loc);
            return;
        }
        if (baseName(result->name) != baseName(instruction.className)) {
            error("alloc of class '" + instruction.className + "' produces " + render(instruction.resultType),
                  id, index, loc);
            return;
        }
        if (result->arguments.size() != instruction.typeArguments.size()) {
            error("alloc of '" + instruction.className + "' records " +
                  std::to_string(instruction.typeArguments.size()) + " generic argument(s) but produces " +
                  render(instruction.resultType), id, index, loc);
            return;
        }
        for (std::size_t i = 0; i < result->arguments.size(); ++i) {
            if (result->arguments[i] != instruction.typeArguments[i]) {
                error("alloc of '" + instruction.className + "' generic argument " + std::to_string(i) +
                      " does not match its result type", id, index, loc);
            }
        }
        if (const ClassLayout* layout = module_.classLayout(instruction.className)) {
            if (!layout->typeParameters.empty() && instruction.typeArguments.size() != layout->typeParameters.size()) {
                error("alloc of generic class '" + instruction.className + "' supplies " +
                      std::to_string(instruction.typeArguments.size()) + " type argument(s) but the class declares " +
                      std::to_string(layout->typeParameters.size()), id, index, loc);
            }
        }
    }

    // A MIR function is a template: its parameters keep the class's own type
    // parameters (`this: Set<T>`), and only the call site knows which
    // instantiation it selected. Substituting the callee's `typeParameters`
    // with the call's `typeArguments` is what makes "passes Set<int> but the
    // parameter is Set<T>" checkable instead of a false mismatch. Returns the
    // declared type unchanged when the callee is not generic, or when the call
    // did not record a matching instantiation.
    [[nodiscard]] std::uint32_t instantiate(std::uint32_t declared, const Function& callee,
                                            const Instruction& instruction) const {
        if (callee.typeParameters.empty()) return declared;
        if (instruction.typeArguments.size() != callee.typeParameters.size()) return declared;
        std::unordered_map<std::string, std::uint32_t> substitution;
        for (std::size_t i = 0; i < callee.typeParameters.size(); ++i) {
            substitution[callee.typeParameters[i]] = instruction.typeArguments[i];
        }
        return substitute(declared, substitution);
    }

    [[nodiscard]] std::uint32_t substitute(std::uint32_t id,
                                           const std::unordered_map<std::string, std::uint32_t>& map) const {
        const Type* type = typeOf(id);
        if (!type) return id;
        if (type->kind == TypeKind::TypeParam) {
            const auto found = map.find(type->name);
            return found == map.end() ? id : found->second;
        }
        if (type->arguments.empty()) return id;
        std::vector<std::uint32_t> arguments;
        arguments.reserve(type->arguments.size());
        bool changed = false;
        for (const std::uint32_t argument : type->arguments) {
            const std::uint32_t replaced = substitute(argument, map);
            changed = changed || replaced != argument;
            arguments.push_back(replaced);
        }
        if (!changed) return id;
        switch (type->kind) {
            case TypeKind::Object: return module_.types.objectType(type->name, std::move(arguments));
            case TypeKind::Task: return module_.types.taskType(arguments.front());
            case TypeKind::Shared: return module_.types.sharedType(arguments.front());
            case TypeKind::List: return module_.types.listType(arguments.front());
            case TypeKind::Set: return module_.types.setType(arguments.front());
            case TypeKind::Array: return module_.types.arrayType(arguments.front(), type->fixedSize);
            case TypeKind::Map:
                return arguments.size() >= 2 ? module_.types.mapType(arguments[0], arguments[1]) : id;
            default: return id;
        }
    }

    void checkCall(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const Opcode opcode = instruction.opcode;
        const char* name = opcodeName(opcode);
        const SourceLocation& loc = instruction.location;
        const auto& operands = instruction.operands;
        const std::size_t receiverCount =
            (opcode == Opcode::InvokeMethod || opcode == Opcode::InvokeSuper || opcode == Opcode::CallIndirect) ? 1 : 0;

        if (operands.size() < receiverCount) {
            error(std::string(name) + " is missing its receiver operand", id, index, loc);
            return;
        }

        // Argument types recorded on the instruction must line up with the
        // operands actually present, or a backend reading only one of the two
        // gets a different call than the other.
        if (instruction.target.argumentTypes.size() != operands.size() - receiverCount) {
            error(std::string(name) + " records " + std::to_string(instruction.target.argumentTypes.size()) +
                  " argument type(s) but has " + std::to_string(operands.size() - receiverCount) + " argument(s)",
                  id, index, loc);
        }
        for (std::size_t i = 0; i < instruction.target.argumentTypes.size() && i + receiverCount < operands.size(); ++i) {
            if (instruction.target.argumentTypes[i] != operands[i + receiverCount].type) {
                error(std::string(name) + " argument " + std::to_string(i) + " is recorded as " +
                      render(instruction.target.argumentTypes[i]) + " but the operand has type " +
                      render(operands[i + receiverCount].type), id, index, loc);
            }
        }

        std::uint32_t expectedResult = 0;
        bool returnsVoid = false;

        switch (opcode) {
            case Opcode::Call:
            case Opcode::InvokeSuper:
            case Opcode::InvokeStatic: {
                const Function* callee = module_.function(instruction.target.function);
                if (!callee) {
                    if (instruction.target.function == kNoFunction && opcode != Opcode::InvokeStatic) {
                        error(std::string(name) + " names no target function", id, index, loc);
                    } else {
                        error(std::string(name) + " refers to unknown function " +
                              std::to_string(instruction.target.function), id, index, loc);
                    }
                    return;
                }
                if (callee->parameters.size() != operands.size() - receiverCount) {
                    error(std::string(name) + " of '" + callee->name + "' passes " +
                          std::to_string(operands.size() - receiverCount) + " argument(s) but it takes " +
                          std::to_string(callee->parameters.size()), id, index, loc);
                    return;
                }
                for (std::size_t i = 0; i < callee->parameters.size(); ++i) {
                    const Operand& argument = operands[i + receiverCount];
                    const std::uint32_t parameterType = instantiate(callee->parameters[i].type, *callee, instruction);
                    if (!assignable(argument.type, parameterType)) {
                        error(std::string(name) + " of '" + callee->name + "' argument " + std::to_string(i) +
                              " ('" + callee->parameters[i].name + "') passes " + render(argument.type) +
                              " but the parameter is " + render(parameterType), id, index, loc);
                    }
                }
                // Calling an async function hands back the Task, not the payload.
                expectedResult = callee->isAsync ? module_.types.taskType(callee->returnType) : callee->returnType;
                returnsVoid = !callee->isAsync && isVoidType(callee->returnType);
                break;
            }
            case Opcode::InvokeMethod: {
                if (instruction.target.className.empty() || instruction.target.methodName.empty()) {
                    error("invoke_method requires a class name and a method name", id, index, loc);
                    return;
                }
                const Operand& receiver = operands[0];
                const Type* receiverType = typeOf(receiver.type);
                // Shared<T> and Task<T> are reference types with methods of
                // their own (Shared.withLock, Task.block, ...), so a method call
                // on one is ordinary, not malformed.
                if (receiverType && receiverType->kind != TypeKind::Object &&
                    receiverType->kind != TypeKind::Shared && receiverType->kind != TypeKind::Task &&
                    receiverType->kind != TypeKind::Unknown && receiverType->kind != TypeKind::TypeParam) {
                    error("invoke_method receiver has type " + render(receiver.type) +
                          " but must be an object", id, index, loc);
                }
                expectedResult = instruction.target.resultType;
                returnsVoid = isVoidType(expectedResult);
                break;
            }
            case Opcode::CallIndirect: {
                const Operand& callee = operands[0];
                const Type* calleeType = typeOf(callee.type);
                if (!calleeType) return;
                if (calleeType->kind != TypeKind::Function) {
                    error("call_indirect through " + render(callee.type) + ", which is not callable",
                          id, index, loc);
                    return;
                }
                const FunctionSignature& signature = calleeType->signature;
                // A bare `func` carries no signature, so arity and parameter
                // types are checked at the call boundary by the runtime rather
                // than here. Inventing a check would reject programs the type
                // checker accepted.
                if (!signature.hasSignature) {
                    if (instruction.result == kNoTemp && !isVoidType(instruction.resultType)) {
                        error("call_indirect through an unparameterised callable defines no temp",
                              id, index, loc);
                    }
                    return;
                }
                if (signature.parameterTypes.size() != operands.size() - 1) {
                    error("call_indirect through " + render(callee.type) + " passes " +
                          std::to_string(operands.size() - 1) + " argument(s) but the callable takes " +
                          std::to_string(signature.parameterTypes.size()), id, index, loc);
                    return;
                }
                for (std::size_t i = 0; i < signature.parameterTypes.size(); ++i) {
                    const Operand& argument = operands[i + 1];
                    if (!assignable(argument.type, signature.parameterTypes[i])) {
                        error("call_indirect argument " + std::to_string(i) + " passes " + render(argument.type) +
                              " but the callable expects " + render(signature.parameterTypes[i]),
                              id, index, loc);
                    }
                }
                expectedResult = signature.isAsync ? module_.types.taskType(signature.returnType)
                                                   : signature.returnType;
                returnsVoid = !signature.isAsync && isVoidType(signature.returnType);
                break;
            }
            case Opcode::CallNative: {
                if (instruction.target.nativeName.empty()) {
                    error("call_native has no native target name", id, index, loc);
                    return;
                }
                expectedResult = instruction.target.resultType;
                returnsVoid = isVoidType(expectedResult);
                break;
            }
            default:
                break;
        }

        if (expectedResult == 0) return;
        if (returnsVoid) {
            if (instruction.result != kNoTemp) {
                error(std::string(name) + " of a void callee must not define a temp", id, index, loc);
            }
            return;
        }
        if (instruction.result == kNoTemp) {
            error(std::string(name) + " produces " + render(expectedResult) + " but defines no temp",
                  id, index, loc);
            return;
        }
        if (!assignable(expectedResult, instruction.resultType)) {
            error(std::string(name) + " produces " + render(expectedResult) + " but the temp is typed " +
                  render(instruction.resultType), id, index, loc);
        }
    }

    bool isVoidType(std::uint32_t id) const {
        const Type* type = typeOf(id);
        return type && type->kind == TypeKind::Void;
    }

    void checkMakeClosure(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const SourceLocation& loc = instruction.location;
        const Function* callee = module_.function(instruction.target.function);
        if (!callee) {
            error("make_closure refers to unknown function " + std::to_string(instruction.target.function),
                  id, index, loc);
            return;
        }
        const Type* result = typeOf(instruction.resultType);
        if (!result || result->kind != TypeKind::Function) {
            error("make_closure must produce a func value but produces " + render(instruction.resultType),
                  id, index, loc);
            return;
        }
        // Captures are leading parameters of the closure body, so the callable's
        // own arity is the parameter count minus the capture count.
        const std::size_t declaredParameters =
            callee->parameters.size() >= callee->captures.size()
                ? callee->parameters.size() - callee->captures.size()
                : 0;
        if (result->signature.hasSignature && result->signature.parameterTypes.size() != declaredParameters) {
            error("make_closure of '" + callee->name + "' produces a callable taking " +
                  std::to_string(result->signature.parameterTypes.size()) + " argument(s) but the function takes " +
                  std::to_string(declaredParameters), id, index, loc);
        }
        if (result->signature.hasSignature && result->signature.isAsync != callee->isAsync) {
            error("make_closure of '" + callee->name + "' records isAsync=" +
                  std::string(result->signature.isAsync ? "true" : "false") +
                  " but the function disagrees", id, index, loc);
        }
        // Captures are copied by value at MakeClosure, so the operand count is
        // the closure's capture count.
        if (callee->captures.size() != instruction.operands.size()) {
            error("make_closure of '" + callee->name + "' supplies " +
                  std::to_string(instruction.operands.size()) + " capture(s) but the function declares " +
                  std::to_string(callee->captures.size()), id, index, loc);
            return;
        }
        for (std::size_t i = 0; i < callee->captures.size(); ++i) {
            if (!assignable(instruction.operands[i].type, callee->captures[i].type)) {
                error("make_closure capture '" + callee->captures[i].name + "' supplies " +
                      render(instruction.operands[i].type) + " but the closure expects " +
                      render(callee->captures[i].type), id, index, loc);
            }
        }
    }

    void checkAwait(const BasicBlock& block, const Instruction& instruction, long index) {
        const BlockId id = block.id;
        const SourceLocation& loc = instruction.location;
        if (!function_.isAsync) {
            // ZL rejects `await` outside an async func; a MIR that contains one
            // was not lowered from a program the type checker accepted.
            error("await in a non-async function", id, index, loc);
            return;
        }
        const Type* task = typeOf(instruction.operands[0].type);
        if (!task) return;
        if (task->kind != TypeKind::Task) {
            error("await on " + render(instruction.operands[0].type) + ", which is not a Task", id, index, loc);
            return;
        }
        const std::uint32_t payload = task->arguments.empty() ? module_.types.voidType() : task->arguments.front();
        if (isVoidType(payload)) {
            if (instruction.result != kNoTemp) {
                error("await of Task<void> must not define a temp", id, index, loc);
            }
            return;
        }
        if (!assignable(payload, instruction.resultType)) {
            error("await of " + render(instruction.operands[0].type) + " produces " + render(payload) +
                  " but the temp is typed " + render(instruction.resultType), id, index, loc);
        }
    }

    // --- dominance --------------------------------------------------------
    void checkDominance() {
        for (const auto& block : function_.blocks) {
            for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                const auto& instruction = block.instructions[i];
                for (std::size_t o = 0; o < instruction.operands.size(); ++o) {
                    checkUseDominatesDef(instruction.operands[o],
                                         opcodeName(instruction.opcode) + std::string(" operand ") +
                                             std::to_string(o),
                                         block.id, static_cast<long>(i), instruction.location);
                }
                if (instruction.opcode == Opcode::Borrow && !instruction.operands.empty()) {
                    // Already covered above; nothing extra to do.
                }
            }
            const auto& t = block.terminator;
            if (!t.value.isNone()) {
                checkUseDominatesDef(t.value, std::string(terminatorKindName(t.kind)) + " value",
                                     block.id, -1, t.location);
            }
            // Block-parameter arguments are evaluated by this block, so their
            // definitions must dominate *it* - not the block they are handed to.
            for (std::size_t edge = 0; edge < t.edgeArguments.size(); ++edge) {
                for (std::size_t a = 0; a < t.edgeArguments[edge].size(); ++a) {
                    checkUseDominatesDef(t.edgeArguments[edge][a],
                                         "block-parameter argument " + std::to_string(a) +
                                             " of successor " + std::to_string(edge),
                                         block.id, -1, t.location);
                }
            }
        }
    }

    void checkUseDominatesDef(const Operand& operand, const std::string& what, BlockId block, long index,
                              SourceLocation loc) {
        if (operand.kind != OperandKind::Temp) return;
        const auto it = definitions_.find(operand.index);
        if (it == definitions_.end()) return; // already reported as undefined
        const Definition& definition = it->second;
        if (definition.block == block) {
            if (index >= 0 && definition.index >= index) {
                error(what + " uses temp %" + std::to_string(operand.index) + " before it is defined",
                      block, index, loc);
            }
            return;
        }
        if (!cfg_.dominates(definition.block, block)) {
            error(what + " uses temp %" + std::to_string(operand.index) + " defined in block b" +
                  std::to_string(definition.block) + ", which does not dominate block b" +
                  std::to_string(block), block, index, loc);
        }
    }

    // --- ownership / move state -------------------------------------------
    struct FlowState {
        std::set<SlotId> moved;
        // borrow slot -> owner slot (0 when the owner is not a known slot)
        std::map<SlotId, SlotId> borrows;

        friend bool operator==(const FlowState& a, const FlowState& b) noexcept {
            return a.moved == b.moved && a.borrows == b.borrows;
        }
    };

    static FlowState join(const FlowState& a, const FlowState& b) {
        FlowState out;
        // A slot moved on any incoming path is moved at the join: the MIR must
        // not assume a path that might not have been taken.
        out.moved = a.moved;
        out.moved.insert(b.moved.begin(), b.moved.end());
        // A borrow survives the join only if it is active on every incoming
        // path. Conflicting owners collapse to "unknown owner" rather than
        // being dropped, so a later move still sees the claim.
        for (const auto& [slot, owner] : a.borrows) {
            const auto other = b.borrows.find(slot);
            if (other == b.borrows.end()) continue;
            out.borrows.emplace(slot, other->second == owner ? owner : 0);
        }
        return out;
    }

    void checkOwnershipFlow() {
        const std::size_t count = function_.blocks.size();
        std::vector<FlowState> outState(count);
        std::vector<bool> hasOut(count, false);

        // Iterate in reverse post-order until the state stops changing. Both
        // components are monotone (moved only grows, borrows only shrink), so
        // this terminates.
        bool changed = true;
        std::size_t guard = 0;
        while (changed && guard++ < count + 8) {
            changed = false;
            for (BlockId id : cfg_.reversePostOrder()) {
                const std::size_t index = cfg_.indexOf(id);
                FlowState input;
                bool first = true;
                for (BlockId pred : cfg_.predecessors(id)) {
                    const std::size_t predIndex = cfg_.indexOf(pred);
                    if (!hasOut[predIndex]) continue;
                    if (first) { input = outState[predIndex]; first = false; }
                    else input = join(input, outState[predIndex]);
                }
                FlowState next = input;
                transfer(*function_.block(id), input, next, id);
                if (!hasOut[index] || !(outState[index] == next)) {
                    outState[index] = next;
                    hasOut[index] = true;
                    changed = true;
                }
            }
        }
    }

    void transfer(const BasicBlock& block, const FlowState& input, FlowState& state, BlockId id) {
        state = input;
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            const auto& instruction = block.instructions[i];
            const long index = static_cast<long>(i);
            const SourceLocation& loc = instruction.location;
            const Slot* slot = function_.slot(instruction.slot);
            const std::string slotName = slot ? "local '" + slot->name + "'" : "an unknown local";

            switch (instruction.opcode) {
                case Opcode::Store:
                    if (state.moved.count(instruction.slot)) {
                        error("assignment to " + slotName + " after it was moved", id, index, loc);
                    }
                    state.moved.erase(instruction.slot);
                    break;
                case Opcode::Load:
                    if (state.moved.count(instruction.slot)) {
                        error("read of " + slotName + " after it was moved", id, index, loc);
                    }
                    break;
                case Opcode::Move:
                    if (state.moved.count(instruction.slot)) {
                        error("move of " + slotName + " after it was already moved", id, index, loc);
                    }
                    for (const auto& [borrowSlot, owner] : state.borrows) {
                        if (owner != instruction.slot && owner != 0) continue;
                        const Slot* borrow = function_.slot(borrowSlot);
                        error("move of " + slotName + " while it is borrowed by " +
                              (borrow ? "local '" + borrow->name + "'" : "an unknown local"), id, index, loc);
                        break;
                    }
                    state.moved.insert(instruction.slot);
                    break;
                case Opcode::Borrow: {
                    const SlotId ownerSlot = ownerSlotOf(instruction.operands.empty()
                                                             ? Operand::none()
                                                             : instruction.operands[0]);
                    if (ownerSlot != 0 && state.moved.count(ownerSlot)) {
                        const Slot* owner = function_.slot(ownerSlot);
                        error("borrow of " + (owner ? "local '" + owner->name + "'" : "an unknown local") +
                              " after it was moved", id, index, loc);
                    }
                    state.borrows[instruction.slot] = ownerSlot;
                    break;
                }
                case Opcode::EndBorrow:
                    if (!state.borrows.count(instruction.slot)) {
                        error("end_borrow of " + slotName + ", which does not hold an active borrow here",
                              id, index, loc);
                    }
                    state.borrows.erase(instruction.slot);
                    break;
                default:
                    break;
            }
        }
    }

    // The slot a value was loaded or moved from, or 0 when it has no slot
    // provenance (a constant, a parameter, or a computed temp).
    SlotId ownerSlotOf(const Operand& operand) const {
        if (operand.kind != OperandKind::Temp) return 0;
        const auto it = provenance_.find(operand.index);
        return it == provenance_.end() ? 0 : it->second;
    }

    const Module& module_;
    const Function& function_;
    const VerifierOptions& options_;
    ControlFlowGraph cfg_;
    VerificationReport* report_{nullptr};

    struct Definition { BlockId block; long index; };
    std::unordered_map<TempId, Definition> definitions_;
    std::unordered_map<TempId, std::uint32_t> tempTypes_;
    std::unordered_map<TempId, SlotId> provenance_;
};

} // namespace

VerificationReport verifyFunction(const Module& module, const Function& function, const VerifierOptions& options) {
    VerificationReport report;
    FunctionVerifier(module, function, options).run(report);
    return report;
}

VerificationReport verifyModule(const Module& module, const VerifierOptions& options) {
    VerificationReport report;

    auto error = [&](const std::string& message) {
        Diagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.message = message;
        report.diagnostics.push_back(std::move(d));
    };
    auto warn = [&](const std::string& message) {
        Diagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.message = message;
        report.diagnostics.push_back(std::move(d));
    };

    // Function ids are 1-based and positional, matching Module::function().
    std::unordered_set<std::string> names;
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        const auto& function = module.functions[i];
        if (function.id != static_cast<FunctionId>(i + 1)) {
            error("function '" + function.name + "' has id " + std::to_string(function.id) +
                  " but occupies position " + std::to_string(i + 1));
        }
        if (!function.name.empty() && !names.insert(function.name).second) {
            error("duplicate function name '" + function.name + "'");
        }
    }
    if (module.entryPoint != kNoFunction && !module.function(module.entryPoint)) {
        error("module entry point refers to unknown function " + std::to_string(module.entryPoint));
    }

    for (const auto& field : module.statics) {
        if (!module.types.find(field.type)) {
            error("static '" + field.className + "." + field.name + "' has unknown type id " +
                  std::to_string(field.type));
        }
        if (field.initializer != kNoFunction && !module.function(field.initializer)) {
            error("static '" + field.className + "." + field.name + "' has unknown initializer function " +
                  std::to_string(field.initializer));
        }
    }

    std::unordered_set<std::string> classNames;
    for (const auto& layout : module.classes) {
        if (!classNames.insert(layout.name).second) {
            error("duplicate class layout '" + layout.name + "'");
        }
        for (const auto& field : layout.fields) {
            if (!module.types.find(field.type)) {
                error("field '" + layout.name + "." + field.name + "' has unknown type id " +
                      std::to_string(field.type));
            }
        }
    }
    for (const auto& layout : module.classes) {
        if (layout.parent.empty()) continue;
        if (!module.classLayout(layout.parent)) {
            warn("class '" + layout.name + "' extends '" + layout.parent +
                 "', which has no layout in this module");
        }
    }

    for (const auto& function : module.functions) {
        VerificationReport functionReport = verifyFunction(module, function, options);
        for (auto& d : functionReport.diagnostics) report.diagnostics.push_back(std::move(d));
        if (options.maxErrors != 0 && report.errorCount() >= options.maxErrors) break;
    }
    return report;
}

} // namespace zl::mir
