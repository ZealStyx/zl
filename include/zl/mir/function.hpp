#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zl/common/ownership.hpp"
#include "zl/mir/instruction.hpp"
#include "zl/mir/source_location.hpp"
#include "zl/mir/type.hpp"
#include "zl/mir/value.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// Basic blocks, control flow, functions, modules
// ---------------------------------------------------------------------------

// Why a block is entered. This is part of the block, not of the edge, because
// the block's *shape* depends on it: a Catch block binds the caught value to a
// slot before its first instruction, and a Finally block must be able to
// resume either normally or by rethrowing.
enum class BlockKind : std::uint8_t {
    Normal,
    // Entered only along an exception edge. Binds the caught value.
    Catch,
    // Entered both normally and along an exception edge; runs cleanup and then
    // either falls through or rethrows.
    Cleanup,
};

[[nodiscard]] const char* blockKindName(BlockKind kind) noexcept;

// One handler in a block's dynamic exception chain, innermost first. Mirrors
// ZL's `try { } catch (T) e { } ... finally { }` structure: the chain is
// searched in order and the first handler whose type admits the thrown value
// runs.
struct ExceptionHandler {
    // The caught type, or 0 for a catch-all (`catch e` with no type).
    std::uint32_t catchType{0};
    BlockId block{kNoBlock};
    // Slot the caught value is bound to in the target block. Required for a
    // Catch block.
    SlotId catchSlot{0};
    // A `finally` handler: catch-all in the same sense as an untyped catch,
    // but the runtime rethrows after the cleanup runs, so the target block
    // binds the thrown *object* (not the stringified message) and must throw
    // it again when it finishes. Marked so the backend can translate the two
    // catch-all spellings differently (`PushFinallyHandler` vs `PushHandler`).
    bool isFinally{false};
};

// How an edge was created. Recorded on the edge so a consumer can tell real
// control flow from an unwind path without re-deriving it, and so the verifier
// can check that exception edges and normal edges were not confused.
enum class EdgeKind : std::uint8_t {
    Normal,
    Unwind,
};

struct ControlFlowEdge {
    BlockId from{kNoBlock};
    BlockId to{kNoBlock};
    EdgeKind kind{EdgeKind::Normal};

    friend bool operator==(const ControlFlowEdge& a, const ControlFlowEdge& b) noexcept {
        return a.from == b.from && a.to == b.to && a.kind == b.kind;
    }
};

// A block parameter: the value a block's incoming edges agree to hand it. This
// is the phi node, written as a block argument rather than as a list of
// `(value, predecessor)` pairs on the *use* side. Two consequences worth
// knowing:
//
//   * the merged value lives at the block, so it is defined exactly once no
//     matter how many predecessors there are, and it has a name the rest of the
//     block can use;
//   * the *edge* carries the argument, so "what does this phi take on this
//     path" is answered by the predecessor, which is the only place that knows.
//
// A block with parameters must supply one argument per incoming normal edge;
// the verifier checks the count and the types, and that the parameter's block
// dominates every use.
struct BlockParameter {
    BlockParamId id{kNoBlockParam};
    std::string name;      // for diagnostics and for readable MIR dumps
    std::uint32_t type{0}; // TypeId; never void
    SourceLocation location;
};

// A maximal straight-line instruction sequence with exactly one entry point
// (the top) and exactly one exit (the terminator).
struct BasicBlock {
    BlockId id{kNoBlock};
    BlockKind kind{BlockKind::Normal};
    // Values bound on entry, one argument per incoming normal edge. Empty for
    // the entry block (it has no predecessors) and for any block that does not
    // merge values.
    std::vector<BlockParameter> parameters;
    // Value-producing and effectful instructions, in order. Never contains a
    // control transfer: that is what `terminator` is for.
    std::vector<Instruction> instructions;
    Terminator terminator;
    // Dynamic exception chain for instructions in this block, innermost first.
    std::vector<ExceptionHandler> exceptionHandlers;
    SourceLocation location;
};

// What a function may do when an exception reaches its boundary.
enum class ExceptionBehavior : std::uint8_t {
    // Not analysed. The default; backends must assume the function can raise.
    Unknown,
    // Proven not to raise past its own handlers.
    NoThrow,
    // May raise.
    MayThrow,
};

[[nodiscard]] const char* exceptionBehaviorName(ExceptionBehavior behavior) noexcept;

// A captured variable of a closure. ZL lambdas capture by value, so a capture
// is an operand copied into the closure at MakeClosure.
struct CaptureSpec {
    std::string name;
    // The captured variable's storage name (unique per declaration), or the
    // source name when none was recorded. The runtime identifies captures by
    // this name - the MakeClosure site binds the value under it, and the
    // closure body reads it back under it - so two same-named locals in
    // different scopes capture independently instead of colliding.
    std::string storage;
    std::uint32_t type{0};
    bool usesThis{false};
};

// One declared function parameter. Parameters are SSA values, so they are
// declared here rather than as slots; lowering promotes a parameter to a slot
// only when the body assigns to it.
struct Parameter {
    std::string name;
    std::uint32_t type{0};
    zl::OwnershipKind ownership{zl::OwnershipKind::GC};
    // Non-empty for a `borrow` parameter: the region it borrows from.
    std::string borrowSource;
    SourceLocation location;
};

// One lowered function.
// Member visibility, as the surface language spells it.
//
// MIR records this because reflection reports it and a backend cannot invent
// it. Leaving it out is not a missing optimization: `Type.fields()` prints each
// field's access, so a backend with no visibility to consult prints `public` for
// a `private` field and the program's observable output changes. That is a
// silent miscompile, which is the one thing the backends must never do.
//
// `DEFAULT` (no modifier written) maps to Public, matching how the reference
// path reports it.
enum class MemberAccess { Public, Protected, Private };

struct Function {
    FunctionId id{kNoFunction};
    // Fully qualified identity, e.g. "Greeter.greet(string)". Unique within a
    // module.
    std::string name;
    // Declaring class, empty for a free function or lambda.
    std::string ownerClass;
    // Unqualified source name.
    std::string simpleName;

    std::vector<Parameter> parameters;
    // True when parameters[0] is the implicit receiver of an instance method or
    // constructor. The receiver is a real parameter rather than a side channel
    // so that a direct call's arity is simply `parameters.size()` and a backend
    // never has to know which convention a given call used.
    bool hasThisParameter{false};
    // Return type. For an async function this is the *payload* type T, not
    // Task<T>: callers see the Task, the body returns T. `isAsync` records the
    // distinction so it is never implicit.
    std::uint32_t returnType{0};
    zl::OwnershipKind returnOwnership{zl::OwnershipKind::GC};

    std::vector<Slot> slots;
    std::vector<BasicBlock> blocks;
    BlockId entryBlock{kNoBlock};

    // Control-flow edges, rebuilt by rebuildEdges(). Kept materialised because
    // both the verifier and every later pass need predecessor lists, and
    // deriving them on demand invites two passes disagreeing about the CFG.
    std::vector<ControlFlowEdge> edges;

    // --- semantic metadata preserved for later stages ----------------------
    bool isAsync{false};
    bool isNative{false};
    bool isConstructor{false};
    bool isStatic{false};
    bool isLambda{false};
    bool isOperator{false};
    // Visibility of a class member, carried for reflection (see MemberAccess).
    MemberAccess access{MemberAccess::Public};
    // True for a generic class member lowered as a template: its body may use
    // TypeParam types named in `typeParameters`. A concrete instantiation
    // records the substitution in `genericArguments` instead.
    bool isGenericTemplate{false};
    std::vector<std::string> typeParameters;
    std::vector<std::uint32_t> genericArguments;
    ExceptionBehavior exceptionBehavior{ExceptionBehavior::Unknown};
    // Captured variables, for closures only.
    std::vector<CaptureSpec> captures;
    // Set when lowering could not represent part of the source body. A module
    // containing such a function is still well-formed MIR - the function is
    // simply not a complete translation - and the flag is what stops a backend
    // from treating it as one.
    bool incomplete{false};
    std::vector<std::string> incompleteReasons;

    SourceLocation location;

    [[nodiscard]] const BasicBlock* block(BlockId id) const;
    [[nodiscard]] BasicBlock* block(BlockId id);
    [[nodiscard]] const Parameter* parameter(ParamId id) const;
    [[nodiscard]] const Slot* slot(SlotId id) const;

    // The block parameter with this id, or nullptr. The owning block is what
    // gives the parameter its definition point, so the lookup returns both:
    // `block` is null exactly when the parameter does not exist.
    [[nodiscard]] const BlockParameter* blockParameter(BlockParamId id) const;
    [[nodiscard]] const BasicBlock* blockOfParameter(BlockParamId id) const;
    // One past the largest block parameter id in use, which is where a pass
    // that adds parameters starts allocating.
    [[nodiscard]] BlockParamId nextBlockParameterId() const;

    // Recomputes `edges` from the terminators and exception handler chains.
    void rebuildEdges();
};

// A module-level static field. ZL statics are lazily initialised shared state,
// so they are addresses in the MIR rather than constants.
struct StaticField {
    StaticId id{kNoStatic};
    std::string className;
    std::string name;
    std::uint32_t type{0};
    // The MIR function that computes the initial value, or kNoFunction.
    FunctionId initializer{kNoFunction};
    SourceLocation location;
};

// Field layout of a class or record, so a backend can resolve `FieldLoad` by
// index instead of by name, and so the verifier can check that a field access
// names a field that exists with the type the access claims.
struct FieldLayout {
    std::string name;
    std::uint32_t type{0};
    zl::OwnershipKind ownership{zl::OwnershipKind::GC};
    bool isStatic{false};
    MemberAccess access{MemberAccess::Public};
};

struct ClassLayout {
    std::string name;
    std::vector<std::string> typeParameters;
    std::string parent;
    // The rendered extends clause including the parent's generic arguments
    // ("Base<B>" for `class Proj<A,B> extends Base<B>`), exactly what runtime
    // reflection's baseTypeName must carry so a projection's bindings thread
    // through to the base's parameters. Empty when there is no parent or the
    // parent is not generic; consumers fall back to `parent`.
    std::string parentTypeName;
    std::vector<std::string> interfaces;
    std::vector<FieldLayout> fields;
    bool isData{false};
    bool isEnum{false};
    std::vector<std::string> enumMembers;
    SourceLocation location;

    [[nodiscard]] const FieldLayout* field(const std::string& fieldName) const;
};

// An interface declaration's place in the interface hierarchy:
//   interface Derived extends Base1, Base2 { ... }
//
// An interface is a contract, not a class: it has no fields, no dispatch rows
// and no instance layout, so it deliberately does not get a `ClassLayout` and
// does not appear in `Module::classes`. What the rest of the IR needs from it is
// exactly one thing - which contract entails which other one - because that is
// what decides whether a value of one interface type may be used where another
// is expected (`Named n = shape`). Keeping only that edge means the backend's
// reflection and dispatch tables never see a phantom class.
// One method an interface declares. Interfaces carry signatures rather than
// bodies, so this is a signature and not a `Function`: there is no block to
// verify and no slot to address. It is recorded because a call through an
// interface-typed receiver has to be dispatched, and the interface's own
// declaration is the only place that fixes the signature the call must match -
// guessing from whichever class happens to implement the interface would be
// exactly the kind of inference this IR exists to avoid.
struct InterfaceMethod {
    std::string name;
    // Explicit parameter types. There is no receiver parameter: an interface
    // method is only ever called through an instance.
    std::vector<std::uint32_t> parameterTypes;
    std::uint32_t returnType{0};
};

struct InterfaceInfo {
    std::string name;
    std::vector<std::string> bases;
    std::vector<InterfaceMethod> methods;
    SourceLocation location;

    [[nodiscard]] const InterfaceMethod* method(const std::string& methodName) const;
};

// One MIR translation unit.
struct Module {
    std::string name;
    // Owns every type in the module. Must outlive every Function that
    // references its ids.
    TypeArena types;
    std::vector<Function> functions;
    std::vector<StaticField> statics;
    std::vector<ClassLayout> classes;
    // Interface hierarchy, for assignability. Empty when the module declares no
    // interfaces; interfaces with no bases still get an entry, so "is this name
    // an interface?" is answerable.
    std::vector<InterfaceInfo> interfaces;
    std::vector<Constant> constants;
    // The program entry point, or kNoFunction when the module has none.
    FunctionId entryPoint{kNoFunction};

    [[nodiscard]] const Function* function(FunctionId id) const;
    [[nodiscard]] Function* function(FunctionId id);
    [[nodiscard]] const ClassLayout* classLayout(const std::string& name) const;
    // The declared bases of an interface, or nullptr when `name` is not an
    // interface this module declares.
    [[nodiscard]] const InterfaceInfo* interfaceInfo(const std::string& name) const;
    [[nodiscard]] const Constant* constant(ConstId id) const;
    [[nodiscard]] const StaticField* staticField(StaticId id) const;

    // Pool lookup-or-insert helpers. Constants are deduplicated so equal
    // literals share one slot.
    [[nodiscard]] ConstId internConstant(const Constant& value);
};

} // namespace zl::mir
