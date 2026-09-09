#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zl/mir/type.hpp"
#include "zl/mir/value.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// Instruction taxonomy
// ---------------------------------------------------------------------------
//
// The single structural rule of the MIR: **instructions compute, terminators
// transfer control.** `Opcode` never contains a control-transfer operation and
// `TerminatorKind` never produces a value. A basic block is a list of
// instructions followed by exactly one terminator, so "where can execution go
// next" is answerable by reading one field instead of scanning a block, and
// "what does this block compute" is answerable without worrying about a jump
// hiding in the middle.
//
// Every opcode declares, in `opcodeShape`, exactly how many operands it takes,
// whether it produces a result, and whether it may raise. That table is what
// the verifier checks the MIR against, so adding an opcode without stating its
// shape is a compile-time-visible omission rather than a silent hole.

enum class Opcode : std::uint16_t {
    Nop,

    // --- arithmetic -------------------------------------------------------
    // Operand types decide the operation's meaning; there are no separate
    // int/double opcodes. `Add` on two strings is concatenation, matching ZL.
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Pow,
    // Unary arithmetic negation.
    Neg,

    // --- bitwise (Java-style, int operands only) --------------------------
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    Shl,
    Shr,
    Ushr,

    // --- comparison -------------------------------------------------------
    // Result is always bool. Eq/Ne follow ZL's equality rule and additionally
    // accept nil against a nullable reference. The ordered forms follow ZL's
    // ordering rule, which is numeric-only: `zl::OperatorRules` is the single
    // source of truth for both, so the MIR cannot drift from the language.
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,

    // --- logical ----------------------------------------------------------
    // Not is the unary complement. And/Or are the *non*-short-circuiting
    // boolean forms: ZL's `&&`/`||` lower to Branch terminators because they
    // must skip evaluating the right-hand side, and lowering keeps that
    // behaviour. And/Or exist for backends that prove the right-hand side
    // effect-free and want to recover a single instruction.
    Not,
    And,
    Or,

    // --- conversion / type refinement -------------------------------------
    // Lossless int -> double widening.
    Widen,
    // Runtime type assertion: produces the operand retyped to `resultType`,
    // raising when the value does not match. This is the typed form of the
    // VM's AssertType, and how a union member is narrowed after a match.
    Refine,
    // bool: does the operand's runtime value match `testedType`? This is the
    // non-raising partner of Refine and the typed form of the VM's MatchType -
    // a match arm has to *ask* the question and fall through to the next arm on
    // a "no", which Refine cannot express because it raises instead.
    TypeTest,

    // --- null handling ----------------------------------------------------
    // Produces the operand unchanged, raising if it is nil. Backends may elide
    // a check dominated by an earlier one on the same value.
    NullCheck,
    // bool: true when the operand is nil.
    IsNull,

    // --- memory: locals ---------------------------------------------------
    // Slots are the mutable-variable abstraction; temps are SSA.
    Load,  // slot -> temp
    Store, // (slot, value) -> no result

    // --- memory: aggregate access -----------------------------------------
    // Field access on an object/record. `name` is the field name; the
    // resolved field type is the instruction's result (or stored value) type.
    FieldLoad,
    FieldStore,
    // Indexed access. Works for list/array (int index) and map (key operand).
    IndexLoad,
    IndexStore,
    // Static class field access.
    StaticLoad,
    StaticStore,

    // --- object construction ----------------------------------------------
    // Allocates an instance of `className` with the recorded generic arguments
    // and default-initialised fields, without running a constructor. Lowering
    // follows it with the constructor Call/InvokeSuper.
    Alloc,

    // --- calls ------------------------------------------------------------
    // Direct call to a MIR function.
    Call,
    // Virtual dispatch on the receiver's runtime class.
    InvokeMethod,
    // Non-virtual call to the declaring class's implementation (`super.m()`).
    InvokeSuper,
    // Static method call.
    InvokeStatic,
    // Call through a function value (closure or named function reference).
    CallIndirect,
    // Call into the native catalog / FFI boundary.
    CallNative,
    // Builds a closure value: a MIR function plus its captured operands.
    MakeClosure,

    // --- async / task -----------------------------------------------------
    // Suspends until the task operand completes and produces its payload.
    // Only legal inside a function flagged isAsync.
    Await,
    // Creates an already-completed Task wrapping a value, or an empty Task for
    // a nil operand. Used where ZL hands back a Task directly.
    TaskCreate,

    // --- ownership / resource lifetime ------------------------------------
    // Move the value out of a slot, leaving the slot moved. Only legal for an
    // `owned` slot. Produces the moved value.
    Move,
    // Establish a borrow: `slot` is the destination borrow slot (its ownership
    // must be `borrow`) and operands[0] is the owner being borrowed from. The
    // borrow stays live until a matching EndBorrow, which is what lets a later
    // pass prove it does not outlive the owner.
    Borrow,
    // End the borrow held in `slot`, releasing the region claim.
    EndBorrow,
    // Deterministically release a value. Rejected for a value that came from a
    // borrow slot: a borrow is released by EndBorrow, not by dropping it.
    Drop,

    // --- collection construction ------------------------------------------
    // Builds an empty builtin collection of the result type. Lowering fills it
    // with IndexStore / native push calls.
    NewCollection,

    // --- loop support -----------------------------------------------------
    // ZL's direction-aware range test: `for i in a..b step s`. Preserves the
    // language's exact semantics, including raising when the step is zero.
    // Kept as one instruction rather than a comparison chain because splitting
    // it would silently change what happens when `step` is 0.
    RangeInBounds,

    // --- output -----------------------------------------------------------
    // The `log(...)` builtin.
    Log,
};

[[nodiscard]] const char* opcodeName(Opcode opcode) noexcept;

enum class TerminatorKind : std::uint8_t {
    // No terminator. A block in this state is unfinished; the verifier rejects
    // it. Represented explicitly rather than by an absent optional so a
    // half-built block cannot be mistaken for a complete one.
    None,
    // Normal completion. `value` is None exactly when the function returns void.
    Return,
    // Unconditional transfer.
    Jump,
    // Two-way transfer on a bool.
    Branch,
    // Multi-way transfer on an int or enum-member value.
    Switch,
    // Raises the operand and transfers along the innermost matching exception
    // edge. Not a value-producing instruction: it ends the block.
    Throw,
    // Statically unreachable. Emitted only where the language proves control
    // cannot arrive (an exhaustive match's default, code after an unconditional
    // throw).
    Unreachable,
};

[[nodiscard]] const char* terminatorKindName(TerminatorKind kind) noexcept;

// Static shape of an opcode: the contract the verifier enforces.
struct OpcodeShape {
    Opcode opcode{Opcode::Nop};
    // Required operand count. A negative-free sentinel: use `operands` plus
    // `variadicOperands` for varargs opcodes.
    std::uint8_t operandCount{0};
    // When true, `operandCount` is a minimum and extra operands are permitted.
    bool variadicOperands{false};
    // True when the instruction produces a result temp.
    bool producesResult{false};
    // True when the temp is produced only for a non-void result. A call to a
    // void function and an `await` of a `Task<void>` both yield nothing, so
    // these opcodes must be allowed to define no temp even though they are
    // value-producing in general - whether they do is a property of the result
    // type, which a static opcode table cannot see.
    bool optionalResult{false};
    // True when execution may leave the block along an exception edge.
    bool mayThrow{false};
    // True when the instruction mutates memory or has an externally visible
    // effect, so it cannot be deleted or reordered freely.
    bool hasSideEffects{false};
};

[[nodiscard]] const OpcodeShape& opcodeShape(Opcode opcode) noexcept;

// True when the opcode addresses a mutable local through `Instruction::slot`
// rather than through an operand. Shared by the verifier and the printer so
// they cannot disagree about which field an opcode reads.
[[nodiscard]] bool opcodeUsesSlot(Opcode opcode) noexcept;

// True for the call opcodes, which are the one family allowed to omit a result
// when the callee returns void.
[[nodiscard]] bool opcodeIsCall(Opcode opcode) noexcept;

// One Switch destination.
struct SwitchTarget {
    std::int64_t value{0};
    BlockId block{kNoBlock};
};

// One call target. A call instruction names its callee in exactly one of these
// ways; which one is implied by the opcode.
struct CallTarget {
    // Direct/indirect: the MIR function being called (Call, InvokeStatic,
    // InvokeSuper, MakeClosure).
    FunctionId function{kNoFunction};
    // Virtual dispatch: receiver class plus method identity.
    std::string className;
    std::string methodName;
    // Native calls: catalog identity. `nativeName` is the qualified name
    // (e.g. "zl.lang.Math.sqrt"); `nativeId` is its catalog id, or -1 when the
    // target is an `@ffi` binding rather than a catalog entry.
    std::string nativeName;
    std::int32_t nativeId{-1};
    // Resolved argument types, so a backend never has to reconstruct the
    // callee's signature from the operands alone.
    std::vector<std::uint32_t> argumentTypes;
    // Resolved result type.
    std::uint32_t resultType{0};
    bool isAsync{false};
};

// One MIR instruction.
//
// Field usage by opcode is fixed and stated in `opcodeShape`; the verifier
// checks both the count and each operand's type. `result` is the temp this
// instruction defines and is `kNoTemp` exactly when the opcode produces no
// result.
struct Instruction {
    Opcode opcode{Opcode::Nop};
    TempId result{kNoTemp};
    // The type of `result`. Must equal 0 when there is no result.
    std::uint32_t resultType{0};
    std::vector<Operand> operands;
    // Operand slot for aggregate/static access and construction.
    std::string name;
    CallTarget target;
    // For Alloc: the concrete class identity (a generic instantiation key such
    // as "Box<int>" when the source named a generic class).
    std::string className;
    std::vector<std::uint32_t> typeArguments;
    // For TypeTest: the type the operand is tested against. `resultType` is
    // always bool there, so the tested type needs a slot of its own.
    std::uint32_t testedType{kNoType};
    // Slot operand for Load/Store/Move/EndBorrow/Drop.
    SlotId slot{0};
    SourceLocation location;
};

// The single control-transfer operation that ends a basic block.
struct Terminator {
    TerminatorKind kind{TerminatorKind::None};
    // Return value (Return) or thrown value (Throw).
    Operand value;
    // Jump: one target. Branch: thenBlock/elseBlock. Switch: defaultBlock.
    BlockId target{kNoBlock};
    BlockId elseBlock{kNoBlock};
    std::vector<SwitchTarget> cases;
    SourceLocation location;

    // Every block this terminator can transfer control to, in a stable order.
    [[nodiscard]] std::vector<BlockId> successors() const;
};

} // namespace zl::mir
