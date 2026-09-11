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
    //
    // This is the ONLY suspension point in the current language: every other
    // task operation either creates a Task without suspending (Call to an
    // async function, TaskSpawn, ChannelSendAsync, ...) or blocks the current
    // thread without suspending the async frame (TaskBlock, ThreadJoin,
    // ChannelSend, ...). The distinction matters for codegen: a suspension
    // parks the async frame on the scheduler and preserves every live value
    // (temps, slots, block parameters, owned values, live borrows with
    // owned-frame lifetimes) across the resume; a block publishes GC roots
    // and waits on the current thread.
    //
    // Semantics (see OpCode::Await in vm.cpp): if the current task already
    // requested cancellation, throws CancellationException without suspending;
    // if the awaited task is terminal, observes it immediately (result or
    // thrown failure/cancellation); awaiting a task's own Task is an error;
    // otherwise suspends and resumes via the scheduler when the awaited task
    // settles. `await` of `Task<void>` defines no temp.
    Await,
    // Creates an already-completed Task wrapping a value, or an empty Task for
    // a nil operand. Used where ZL hands back a Task directly.
    //
    // This is a completed-task value, NOT a pending one: it never touches the
    // scheduler or the executor. Pending tasks come from calling an async
    // function, TaskSpawn, Time.sleepAsync, or the Channel async operations.
    TaskCreate,
    // --- task lifecycle ---------------------------------------------------
    // `Task.spawn(closure)`: runs a synchronous zero-argument func on the
    // shared CPU executor and returns its pending Task. The closure must be
    // non-async, take no parameters, and capture only thread-safe values
    // (Shared<T> and the synchronisation primitives); anything else is a
    // compile error at the source level, a verifier error here, and a runtime
    // error at the backstop gate (capturesAreExplicitlyShared). The worker
    // runs on a fresh VM (invokeTaskClosure); a worker exception fails the
    // task rather than escaping. Cooperative cancellation only: TaskCancel on
    // a Running task sets the flag the worker may poll; there is no automatic
    // propagation to children, no timeout, and no combinators (whenAll/whenAny
    // do not exist).
    TaskSpawn,
    // `task.block()`: pumps the cooperative scheduler until the task settles,
    // then observes it (payload or thrown failure/cancellation). Only legal
    // in a NON-async function (`Task.block() cannot be used inside an async
    // func`); async code suspends with `await` instead. This BLOCKS the
    // current thread; it does not suspend the async frame.
    TaskBlock,
    // `task.ignore()`: marks the task's failure as observed (suppressing the
    // unobserved-failure diagnostic) and returns void. Never waits.
    TaskIgnore,
    // `task.cancel()`: Pending -> terminal Cancelled; Running -> cooperative
    // request (sets the flag; the task's next await/resume throws
    // CancellationException); already terminal -> no-op. Never throws for a
    // valid Task beyond the type check. There is no cancellation tree: each
    // task carries its own flag.
    TaskCancel,

    // --- threads ----------------------------------------------------------
    // `Thread.start(closure)`: runs a synchronous zero-argument non-async
    // func on a real OS thread and returns its Thread handle. Same capture
    // rule as TaskSpawn (thread-safe captures only, no `this`). The worker
    // runs on a fresh VM; an uncaught worker exception is captured and
    // rethrown by ThreadJoin in the joining thread (catchable by its real
    // type), never process-fatal.
    ThreadStart,
    // `Thread.join(thread)`: blocks until the worker finishes (at a stable VM
    // boundary with GC roots published), rethrowing a worker failure in the
    // joining thread. Synchronous; may throw the worker's exception.
    ThreadJoin,
    // `Thread.isAlive(thread)`: reads the atomic done flag. Volatile: the
    // result may change concurrently, so it must not be CSE'd or reordered
    // across a join/start.
    ThreadIsAlive,

    // --- channels ---------------------------------------------------------
    // A channel is a bounded, thread-safe queue. All operations validate the
    // receiver is a Channel; violations raise. Blocking operations wait with
    // GC roots published (BlockingNativeCall) and honour the capacity.
    //
    // `Channel.create(capacity)`: capacity must be > 0. Produces an untyped
    // Channel (element `unknown`); typed channels do not exist yet.
    ChannelCreate,
    // `Channel.send(channel, value)`: enqueues, blocking while full.
    ChannelSend,
    // `Channel.receive(channel)`: dequeues, blocking while empty. Produces
    // `unknown` (the channel is untyped); callers refine to the expected type.
    ChannelReceive,
    // `Channel.size(channel)`: current queue length. Volatile.
    ChannelSize,
    // `Channel.sendAsync(channel, value)`: returns a Task<void> that is
    // already completed when the value could be handed off or buffered
    // immediately, or pending (with cancellation cleanup removing the queued
    // send) when the channel is full. Never blocks, never suspends; awaiting
    // the Task is what suspends.
    ChannelSendAsync,
    // `Channel.receiveAsync(channel)`: returns a Task for the next value,
    // completed immediately when one is available or a sender is waiting, or
    // pending (with cancellation cleanup) otherwise. Never blocks.
    ChannelReceiveAsync,

    // --- mutex / rwlock ---------------------------------------------------
    // Scoped exclusion: the lock is held while the zero-argument closure runs
    // on the CURRENT thread (invokeTaskClosure, not a worker), so captures do
    // NOT cross a thread boundary and need no confinement check. The closure
    // must be synchronous and non-async; awaiting, spawning, or starting a
    // thread while the capability is held is rejected (heldLockDepth), as is a
    // lock-order inversion. The lock releases even when the body throws, and
    // the body's value (or exception) is the operation's own. Acquisition may
    // block with GC roots published.
    //
    // `Mutex.withLock(mutex, closure)`.
    MutexWithLock,
    // `RwLock.withRead(lock, closure)`: shared read hold; many readers may
    // hold concurrently.
    RwLockWithRead,
    // `RwLock.withWrite(lock, closure)`: exclusive write hold.
    RwLockWithWrite,

    // --- atomics ----------------------------------------------------------
    // Sequentially consistent, safe under contention. Every operation validates
    // the receiver is an Atomic. Volatile: results must not be CSE'd or
    // reordered across other atomic accesses to the same cell.
    //
    // FAMILY WARNING (accurate per the implementation, not the stdlib
    // comment): the int, bool, double, and ref families use INDEPENDENT
    // storage (atomicState, atomicBoolState, atomicDoubleState,
    // atomicRefState). Mixing families on one cell reads independent defaults,
    // not a coherent slot. Pick one family per cell.
    AtomicLoad,        // (Atomic) -> int
    AtomicStore,       // (Atomic, int) -> void
    AtomicAdd,         // (Atomic, delta:int) -> int NEW value (fetch_add + delta)
    AtomicLoadBool,    // (Atomic) -> bool
    AtomicStoreBool,   // (Atomic, bool) -> void
    AtomicLoadDouble,  // (Atomic) -> double
    AtomicStoreDouble, // (Atomic, double) -> void
    // Object slot. Unlike the primitive families this is mutex-protected, not
    // lock-free: a mutex guards a GC-managed Value copy.
    AtomicLoadRef,     // (Atomic) -> object
    AtomicStoreRef,    // (Atomic, object|null) -> void

    // --- semaphores -------------------------------------------------------
    // A counting semaphore starting at ZERO permits: `setPermits` (or the
    // `withPermits` helper) must grant permits before any acquire, or acquire
    // blocks forever by definition. All operations validate the receiver.
    SemaphoreAcquire,    // blocks while permits == 0, then takes one
    SemaphoreRelease,    // grants one permit (notify_one)
    SemaphoreAvailable,  // current permit count (volatile)
    SemaphoreSetPermits, // sets the count outright (notify_all); negative raises
    SemaphoreTryAcquire, // non-blocking: takes a permit and returns true, or false
    SemaphoreReleaseMany,// grants `count` permits (notify_all); negative raises

    // --- conditions -------------------------------------------------------
    // A condition variable with NO predicate and NO timeout on the bare wait.
    // UNSAFE BY DESIGN (explicit, not hidden): a notify delivered before the
    // waiter blocks is LOST and that waiter sleeps until the next
    // notification; a spurious wakeup returns early. Never use a bare wait as
    // the only synchronisation; pair it with a state check or use the bounded
    // `waitFor` polling helpers (Condition.waitUntil). The verifier warns on
    // every bare wait site so the risk is visible in MIR, not just in a
    // comment.
    ConditionWait,      // blocks until notified (no predicate, losable)
    ConditionWaitFor,   // bounded wait -> bool (true if notified, false on timeout)
    ConditionNotifyOne, // wakes one waiter, if any
    ConditionNotifyAll, // wakes all waiters, if any

    // --- shared state -----------------------------------------------------
    // `Shared<T>` marks a capture as shareable; each individual access is
    // synchronised (recursive_mutex + type-checked store), but a read followed
    // by a write is NOT atomic: `setValue(get() + 1)` can lose updates under
    // contention. Use Atomic for counters and Mutex/RwLock/withLock for larger
    // critical sections. The verifier warns where a get/set pair is not
    // enclosed in withLock so the non-atomicity is visible.
    //
    // `share(value)`: allocates a Shared<T> cell holding the value. Pure
    // allocation: no synchronisation happens until the cell is accessed.
    SharedCreate,
    // `Shared<T>.get()`: synchronised load of the cell payload.
    SharedGet,
    // `Shared<T>.setValue(v)`: synchronised, type-checked store.
    SharedSet,
    // `Shared<T>.withLock(closure)`: runs the zero-argument closure while
    // holding the cell's recursive mutex on the current thread (no thread
    // crossing, same held-lock restrictions as Mutex: no await/spawn/start
    // inside). Returns the closure's value.
    SharedWithLock,

    // --- FFI / native resources -------------------------------------------
    // Calls through the stable ZlNative ABI (see native_abi.hpp) to a
    // `@ffi(library, symbol)` / `@ffi(symbol)` export or a registered native
    // module. Unlike CallNative (the Value-based catalog boundary), FfiCall
    // carries explicit ABI tags and ownership per parameter plus the result,
    // so a backend can marshal without re-deriving the signature. Synchronous
    // only; buffer/struct views are borrowed into the call and cannot be
    // returned; only an Owned handle may be returned. Failures become a typed
    // NativeError. The VM backend does NOT execute FfiCall yet (fail closed);
    // it is the native-codegen boundary.
    FfiCall,
    // Validates a handle token is live in the registry and produces the same
    // token with Borrowed ownership. Raises on an invalid/closed handle.
    // A borrow never owns or frees; it becomes invalid when the owner is
    // consumed or closed.
    HandleBorrow,
    // Transfers ownership out of the registry (consume): the token becomes
    // invalid and the caller owns the resource exactly once. Raises on an
    // invalid handle.
    HandleConsume,
    // Deterministically destroys the registry entry (consume + drop). Raises
    // on an invalid handle. Releasing twice is an error, not a no-op.
    HandleClose,
    // Registers a ZL closure as a native callback and returns its token. The
    // registry owns the entry; each invocation takes a lease. The closure must
    // be synchronous (native callbacks never suspend).
    CallbackRegister,
    // Invokes a callback token through its lease with explicitly typed
    // arguments. Raises when the token is invalid or closed; a close waits for
    // in-flight invocations to drain and rejects late calls.
    CallbackInvoke,
    // Closes a callback token: waits for in-flight invocations, then rejects
    // further ones. Raises on an invalid token.
    CallbackClose,

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

// MIR-local mirror of ZlNativeTypeTag (see native_abi.hpp). Defined here
// rather than including the compiler header so this layer stays independent of
// the AST, the type checker, and the VM; lowering.hpp is the single boundary
// that maps between the two.
enum class NativeAbiTag : std::uint8_t {
    I64,
    F64,
    Bool,
    Handle,
    BufferView,
    StructView,
    Callback,
};

[[nodiscard]] const char* nativeAbiTagName(NativeAbiTag tag) noexcept;

// MIR-local mirror of ZlNativeOwnershipTag. NONE:nullopt-style defaults are
// spelled None; Borrowed never transfers; Owned/Consumed transfer exactly once.
enum class NativeOwnership : std::uint8_t {
    None,
    Borrowed,
    Owned,
    Consumed,
};

[[nodiscard]] const char* nativeOwnershipName(NativeOwnership ownership) noexcept;

// True when the opcode suspends the current async frame on the scheduler
// (parking it and preserving every live value across the resume). Currently
// only Await: task/thread creation, channel async operations, and all blocking
// waits do NOT suspend.
[[nodiscard]] bool opcodeIsSuspension(Opcode opcode) noexcept;

// True when the opcode blocks the CURRENT thread (publishing GC roots while
// it waits) rather than suspending the async frame. Blocking inside an async
// function stalls the thread that runs the scheduler; the verifier warns but
// the runtime allows it, so this is a warning, not an error.
[[nodiscard]] bool opcodeIsBlocking(Opcode opcode) noexcept;

// True when the opcode carries values across a thread boundary to a worker
// (TaskSpawn, ThreadStart). Captures must be thread-safe; the verifier checks
// the closure body's capture types and the runtime gate backstops it.
[[nodiscard]] bool opcodeIsThreadBoundary(Opcode opcode) noexcept;

// True for scoped synchronisation (Mutex/RwLock/Shared withLock): the closure
// runs on the current thread while the lock is held, so it must not suspend
// or transfer (no await/spawn/start inside the closure body).
[[nodiscard]] bool opcodeIsScopedLock(Opcode opcode) noexcept;

// True for the FFI boundary opcodes (FfiCall plus handle/callback lifecycle).
// These are the native-codegen boundary; the VM backend fails closed on them.
[[nodiscard]] bool opcodeIsFfi(Opcode opcode) noexcept;

// True when the opcode performs a runtime-checked transition whose failure
// raises: capture checks, type/capacity/arity checks, handle/callback validity,
// lock acquisition, and every blocking wait. Optimisers must not delete or
// reorder these checks; backends must preserve their failure modes.
[[nodiscard]] bool opcodeRequiresRuntimeCheck(Opcode opcode) noexcept;

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
    // Ownership at the native boundary, mirroring the catalog's
    // paramOwnership/returnOwnership. The audited catalog entries currently
    // carry NONE/default throughout, so CallNative sites record None here;
    // the fields exist so a future ownership audit has somewhere to land
    // without changing the instruction shape, and so FFI-adjacent natives can
    // already state Borrowed/Owned/Consumed where the runtime enforces it.
    std::vector<NativeOwnership> nativeParamOwnership;
    NativeOwnership nativeReturnOwnership{NativeOwnership::None};
    // FFI calls (@ffi): the dynamic-library path (empty for the default
    // process) plus the exported symbol. Only meaningful for FfiCall.
    std::string ffiLibrary;
    std::string ffiSymbol;
    // FFI ABI signature: one tag per operand plus the result. The MIR operand
    // types (operands[i].type, resultType) are the ZL-level types; these tags
    // are the ZlNative ABI marshaling contract. A backend must check both.
    std::vector<NativeAbiTag> ffiParamTags;
    NativeAbiTag ffiReturnTag{NativeAbiTag::I64};
    // FFI ownership: one entry per operand plus the result. Buffer/Struct
    // views may only be Borrowed; only an Owned handle may be returned; views
    // cannot be returned at all (the runtime rejects them).
    std::vector<NativeOwnership> ffiParamOwnership;
    NativeOwnership ffiReturnOwnership{NativeOwnership::None};
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

    // Block-parameter arguments, one entry per normal successor, in the same
    // order `successors()` reports them:
    //
    //   Jump   -> [target]
    //   Branch -> [target, elseBlock]
    //   Switch -> [case 0, case 1, ..., default]
    //
    // Entry i must have exactly `successors()[i]->parameters.size()` operands,
    // each with the matching type. An absent entry (fewer entries than
    // successors) means "no arguments", which is only legal when the target has
    // no parameters. This lives on the terminator rather than on the edge
    // because the terminator is what actually evaluates the arguments.
    std::vector<std::vector<Operand>> edgeArguments;

    SourceLocation location;

    // Every block this terminator can transfer control to, in a stable order.
    [[nodiscard]] std::vector<BlockId> successors() const;

    // The arguments handed to successor `index`, or an empty list when that
    // successor has none. Never index out of range.
    [[nodiscard]] const std::vector<Operand>& argumentsFor(std::size_t index) const;
};

} // namespace zl::mir
