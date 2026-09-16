#include "zl/mir/instruction.hpp"

#include <array>
#include <cstdint>

namespace zl::mir {

const char* opcodeName(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Nop: return "nop";
        case Opcode::Add: return "add";
        case Opcode::Sub: return "sub";
        case Opcode::Mul: return "mul";
        case Opcode::Div: return "div";
        case Opcode::Mod: return "mod";
        case Opcode::Pow: return "pow";
        case Opcode::Neg: return "neg";
        case Opcode::BitAnd: return "bit_and";
        case Opcode::BitOr: return "bit_or";
        case Opcode::BitXor: return "bit_xor";
        case Opcode::BitNot: return "bit_not";
        case Opcode::Shl: return "shl";
        case Opcode::Shr: return "shr";
        case Opcode::Ushr: return "ushr";
        case Opcode::Eq: return "eq";
        case Opcode::Ne: return "ne";
        case Opcode::Lt: return "lt";
        case Opcode::Le: return "le";
        case Opcode::Gt: return "gt";
        case Opcode::Ge: return "ge";
        case Opcode::Not: return "not";
        case Opcode::And: return "and";
        case Opcode::Or: return "or";
        case Opcode::Widen: return "widen";
        case Opcode::Refine: return "refine";
        case Opcode::TypeTest: return "type_test";
        case Opcode::NullCheck: return "null_check";
        case Opcode::IsNull: return "is_null";
        case Opcode::Load: return "load";
        case Opcode::Store: return "store";
        case Opcode::FieldLoad: return "field_load";
        case Opcode::FieldStore: return "field_store";
        case Opcode::IndexLoad: return "index_load";
        case Opcode::IndexStore: return "index_store";
        case Opcode::StaticLoad: return "static_load";
        case Opcode::StaticStore: return "static_store";
        case Opcode::Alloc: return "alloc";
        case Opcode::Call: return "call";
        case Opcode::InvokeMethod: return "invoke_method";
        case Opcode::InvokeSuper: return "invoke_super";
        case Opcode::InvokeStatic: return "invoke_static";
        case Opcode::CallIndirect: return "call_indirect";
        case Opcode::CallNative: return "call_native";
        case Opcode::MakeClosure: return "make_closure";
        case Opcode::Await: return "await";
        case Opcode::TaskCreate: return "task_create";
        case Opcode::TaskSpawn: return "task_spawn";
        case Opcode::TaskBlock: return "task_block";
        case Opcode::TaskIgnore: return "task_ignore";
        case Opcode::TaskCancel: return "task_cancel";
        case Opcode::ThreadStart: return "thread_start";
        case Opcode::ThreadJoin: return "thread_join";
        case Opcode::ThreadIsAlive: return "thread_is_alive";
        case Opcode::ChannelCreate: return "channel_create";
        case Opcode::ChannelSend: return "channel_send";
        case Opcode::ChannelReceive: return "channel_receive";
        case Opcode::ChannelSize: return "channel_size";
        case Opcode::ChannelSendAsync: return "channel_send_async";
        case Opcode::ChannelReceiveAsync: return "channel_receive_async";
        case Opcode::MutexWithLock: return "mutex_with_lock";
        case Opcode::RwLockWithRead: return "rwlock_with_read";
        case Opcode::RwLockWithWrite: return "rwlock_with_write";
        case Opcode::AtomicLoad: return "atomic_load";
        case Opcode::AtomicStore: return "atomic_store";
        case Opcode::AtomicAdd: return "atomic_add";
        case Opcode::AtomicLoadBool: return "atomic_load_bool";
        case Opcode::AtomicStoreBool: return "atomic_store_bool";
        case Opcode::AtomicLoadDouble: return "atomic_load_double";
        case Opcode::AtomicStoreDouble: return "atomic_store_double";
        case Opcode::AtomicLoadRef: return "atomic_load_ref";
        case Opcode::AtomicStoreRef: return "atomic_store_ref";
        case Opcode::SemaphoreAcquire: return "semaphore_acquire";
        case Opcode::SemaphoreRelease: return "semaphore_release";
        case Opcode::SemaphoreAvailable: return "semaphore_available";
        case Opcode::SemaphoreSetPermits: return "semaphore_set_permits";
        case Opcode::SemaphoreTryAcquire: return "semaphore_try_acquire";
        case Opcode::SemaphoreReleaseMany: return "semaphore_release_many";
        case Opcode::ConditionWait: return "condition_wait";
        case Opcode::ConditionWaitFor: return "condition_wait_for";
        case Opcode::ConditionNotifyOne: return "condition_notify_one";
        case Opcode::ConditionNotifyAll: return "condition_notify_all";
        case Opcode::SharedCreate: return "shared_create";
        case Opcode::SharedGet: return "shared_get";
        case Opcode::SharedSet: return "shared_set";
        case Opcode::SharedWithLock: return "shared_with_lock";
        case Opcode::FfiCall: return "ffi_call";
        case Opcode::HandleBorrow: return "handle_borrow";
        case Opcode::HandleConsume: return "handle_consume";
        case Opcode::HandleClose: return "handle_close";
        case Opcode::CallbackRegister: return "callback_register";
        case Opcode::CallbackInvoke: return "callback_invoke";
        case Opcode::CallbackClose: return "callback_close";
        case Opcode::Move: return "move";
        case Opcode::Borrow: return "borrow";
        case Opcode::EndBorrow: return "end_borrow";
        case Opcode::Drop: return "drop";
        case Opcode::NewCollection: return "new_collection";
        case Opcode::RangeInBounds: return "range_in_bounds";
        case Opcode::Log: return "log";
    }
    return "<invalid-opcode>";
}

const char* terminatorKindName(TerminatorKind kind) noexcept {
    switch (kind) {
        case TerminatorKind::None: return "none";
        case TerminatorKind::Return: return "return";
        case TerminatorKind::Jump: return "jump";
        case TerminatorKind::Branch: return "branch";
        case TerminatorKind::Switch: return "switch";
        case TerminatorKind::Throw: return "throw";
        case TerminatorKind::Unreachable: return "unreachable";
    }
    return "<invalid-terminator>";
}

namespace {

constexpr std::size_t kOpcodeShapeCount = static_cast<std::size_t>(Opcode::Log) + 1;

using OpcodeShapeTable = std::array<OpcodeShape, kOpcodeShapeCount>;

// A plain function, not a lambda held in a local variable. MSVC does not
// deduce the type of a closure declared inside another lambda whose return
// type is itself being deduced, so it parsed the first call to it as a
// function-style cast (C2440) and reported it as used before it was
// initialized (C3536). A named function has no closure type and nothing to
// deduce, so the table is built identically on every compiler.
//
// Field order is producesResult, optionalResult, mayThrow, hasSideEffects:
// optionalResult defaults to false here and is enabled below for the
// void-omission family only. (Passing throws_/effects positionally into
// optionalResult/mayThrow made every throwing opcode accept a missing temp and
// left hasSideEffects false for the whole table.)
void setOpcodeShape(OpcodeShapeTable& shapes, Opcode op, std::uint8_t operands,
                    bool variadic, bool result, bool throws_, bool effects) {
    shapes[static_cast<std::size_t>(op)] =
        OpcodeShape{op, operands, variadic, result, false, throws_, effects};
}

} // namespace

const OpcodeShape& opcodeShape(Opcode opcode) noexcept {
    // Built once. The table is indexed by the numeric opcode, so every
    // enumerator must appear here; the default entry (Nop's shape) makes a
    // missing row detectable as "claims zero operands and no result", which the
    // verifier rejects for any opcode that is used with operands.
    static const OpcodeShapeTable table = [] {
        OpcodeShapeTable shapes{};

        setOpcodeShape(shapes, Opcode::Nop, 0, false, false, false, false);

        setOpcodeShape(shapes, Opcode::Add, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Sub, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Mul, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Div, 2, false, true, true, false);
        setOpcodeShape(shapes, Opcode::Mod, 2, false, true, true, false);
        setOpcodeShape(shapes, Opcode::Pow, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Neg, 1, false, true, false, false);

        setOpcodeShape(shapes, Opcode::BitAnd, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::BitOr, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::BitXor, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::BitNot, 1, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Shl, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Shr, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Ushr, 2, false, true, false, false);

        setOpcodeShape(shapes, Opcode::Eq, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Ne, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Lt, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Le, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Gt, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Ge, 2, false, true, false, false);

        setOpcodeShape(shapes, Opcode::Not, 1, false, true, false, false);
        setOpcodeShape(shapes, Opcode::And, 2, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Or, 2, false, true, false, false);

        setOpcodeShape(shapes, Opcode::Widen, 1, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Refine, 1, false, true, true, false);
        setOpcodeShape(shapes, Opcode::TypeTest, 1, false, true, false, false);
        setOpcodeShape(shapes, Opcode::NullCheck, 1, false, true, true, false);
        setOpcodeShape(shapes, Opcode::IsNull, 1, false, true, false, false);

        setOpcodeShape(shapes, Opcode::Load, 0, false, true, false, false);
        setOpcodeShape(shapes, Opcode::Store, 1, false, false, false, true);

        setOpcodeShape(shapes, Opcode::FieldLoad, 1, false, true, true, false);
        setOpcodeShape(shapes, Opcode::FieldStore, 2, false, false, true, true);
        setOpcodeShape(shapes, Opcode::IndexLoad, 2, false, true, true, false);
        setOpcodeShape(shapes, Opcode::IndexStore, 3, false, false, true, true);
        setOpcodeShape(shapes, Opcode::StaticLoad, 0, false, true, false, false);
        setOpcodeShape(shapes, Opcode::StaticStore, 1, false, false, false, true);

        setOpcodeShape(shapes, Opcode::Alloc, 0, false, true, false, false);

        setOpcodeShape(shapes, Opcode::Call, 0, true, true, true, true);
        setOpcodeShape(shapes, Opcode::InvokeMethod, 1, true, true, true, true);
        setOpcodeShape(shapes, Opcode::InvokeSuper, 1, true, true, true, true);
        setOpcodeShape(shapes, Opcode::InvokeStatic, 0, true, true, true, true);
        setOpcodeShape(shapes, Opcode::CallIndirect, 1, true, true, true, true);
        setOpcodeShape(shapes, Opcode::CallNative, 0, true, true, true, true);
        setOpcodeShape(shapes, Opcode::MakeClosure, 0, true, true, false, false);

        setOpcodeShape(shapes, Opcode::Await, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::TaskCreate, 1, false, true, false, false);

        setOpcodeShape(shapes, Opcode::TaskSpawn, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::TaskBlock, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::TaskIgnore, 1, false, false, true, true);
        setOpcodeShape(shapes, Opcode::TaskCancel, 1, false, false, true, true);

        setOpcodeShape(shapes, Opcode::ThreadStart, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::ThreadJoin, 1, false, false, true, true);
        // Volatile read of the atomic done flag: marked as having side effects
        // so it is never CSE'd, DCE'd, or reordered across a join/start.
        setOpcodeShape(shapes, Opcode::ThreadIsAlive, 1, false, true, true, true);

        setOpcodeShape(shapes, Opcode::ChannelCreate, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::ChannelSend, 2, false, false, true, true);
        setOpcodeShape(shapes, Opcode::ChannelReceive, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::ChannelSize, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::ChannelSendAsync, 2, false, true, true, true);
        setOpcodeShape(shapes, Opcode::ChannelReceiveAsync, 1, false, true, true, true);

        setOpcodeShape(shapes, Opcode::MutexWithLock, 2, false, true, true, true);
        setOpcodeShape(shapes, Opcode::RwLockWithRead, 2, false, true, true, true);
        setOpcodeShape(shapes, Opcode::RwLockWithWrite, 2, false, true, true, true);

        // Atomic loads are volatile reads: side effects keep them ordered and
        // undeleted, matching the seq_cst contract.
        setOpcodeShape(shapes, Opcode::AtomicLoad, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::AtomicStore, 2, false, false, true, true);
        setOpcodeShape(shapes, Opcode::AtomicAdd, 2, false, true, true, true);
        setOpcodeShape(shapes, Opcode::AtomicLoadBool, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::AtomicStoreBool, 2, false, false, true, true);
        setOpcodeShape(shapes, Opcode::AtomicLoadDouble, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::AtomicStoreDouble, 2, false, false, true, true);
        setOpcodeShape(shapes, Opcode::AtomicLoadRef, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::AtomicStoreRef, 2, false, false, true, true);

        setOpcodeShape(shapes, Opcode::SemaphoreAcquire, 1, false, false, true, true);
        setOpcodeShape(shapes, Opcode::SemaphoreRelease, 1, false, false, true, true);
        setOpcodeShape(shapes, Opcode::SemaphoreAvailable, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::SemaphoreSetPermits, 2, false, false, true, true);
        setOpcodeShape(shapes, Opcode::SemaphoreTryAcquire, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::SemaphoreReleaseMany, 2, false, false, true, true);

        setOpcodeShape(shapes, Opcode::ConditionWait, 1, false, false, true, true);
        setOpcodeShape(shapes, Opcode::ConditionWaitFor, 2, false, true, true, true);
        setOpcodeShape(shapes, Opcode::ConditionNotifyOne, 1, false, false, true, true);
        setOpcodeShape(shapes, Opcode::ConditionNotifyAll, 1, false, false, true, true);

        setOpcodeShape(shapes, Opcode::SharedCreate, 1, false, true, false, false);
        setOpcodeShape(shapes, Opcode::SharedGet, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::SharedSet, 2, false, false, true, true);
        setOpcodeShape(shapes, Opcode::SharedWithLock, 2, false, true, true, true);

        setOpcodeShape(shapes, Opcode::FfiCall, 0, true, true, true, true);
        // A borrow validates liveness and pins nothing: like Refine, it throws
        // but has no side effect of its own.
        setOpcodeShape(shapes, Opcode::HandleBorrow, 1, false, true, true, false);
        setOpcodeShape(shapes, Opcode::HandleConsume, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::HandleClose, 1, false, false, true, true);
        setOpcodeShape(shapes, Opcode::CallbackRegister, 1, false, true, true, true);
        setOpcodeShape(shapes, Opcode::CallbackInvoke, 1, true, true, true, true);
        setOpcodeShape(shapes, Opcode::CallbackClose, 1, false, false, true, true);

        setOpcodeShape(shapes, Opcode::Move, 0, false, true, false, true);
        setOpcodeShape(shapes, Opcode::Borrow, 1, false, false, false, true);
        setOpcodeShape(shapes, Opcode::EndBorrow, 0, false, false, false, true);
        // Drop has two spellings with the same meaning: a value operand (drop
        // this value) or no operand plus a slot (release this slot's storage -
        // the end-of-lifetime form lowering emits for owned locals). The
        // minimum is therefore 0; the verifier's Drop rule decides which shape
        // it is looking at and checks each form's own requirements.
        setOpcodeShape(shapes, Opcode::Drop, 0, true, false, false, true);

        setOpcodeShape(shapes, Opcode::NewCollection, 0, false, true, false, false);
        setOpcodeShape(shapes, Opcode::RangeInBounds, 3, false, true, true, false);
        setOpcodeShape(shapes, Opcode::Log, 1, false, false, false, true);

        // A call defines a temp unless the callee returns void; an `await`
        // defines one unless the task's payload is void. The same void-omission
        // applies to TaskBlock (payload may be void), the scoped-lock helpers
        // (the closure body may return void), and the FFI call opcodes (a bound
        // export or callback may be void-typed). Marked here rather than
        // special-cased in the verifier so the table stays the single answer to
        // "does this opcode produce a value?".
        for (const Opcode opcode : {Opcode::Call, Opcode::InvokeMethod, Opcode::InvokeSuper,
                                    Opcode::InvokeStatic, Opcode::CallIndirect, Opcode::CallNative,
                                    Opcode::Await, Opcode::TaskBlock, Opcode::MutexWithLock,
                                    Opcode::RwLockWithRead, Opcode::RwLockWithWrite,
                                    Opcode::SharedWithLock, Opcode::FfiCall,
                                    Opcode::CallbackInvoke}) {
            shapes[static_cast<std::size_t>(opcode)].optionalResult = true;
        }
        return shapes;
    }();

    const auto index = static_cast<std::size_t>(opcode);
    if (index >= table.size()) {
        static const OpcodeShape invalid{Opcode::Nop, 0, false, false, false, false, false};
        return invalid;
    }
    return table[index];
}

bool opcodeUsesSlot(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Load:
        case Opcode::Store:
        case Opcode::Move:
        case Opcode::Borrow:
        case Opcode::EndBorrow:
        // Drop's storage-release form names the slot whose value is released;
        // the operand form leaves the slot empty.
        case Opcode::Drop:
            return true;
        default:
            return false;
    }
}

bool opcodeIsCall(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Call:
        case Opcode::InvokeMethod:
        case Opcode::InvokeSuper:
        case Opcode::InvokeStatic:
        case Opcode::CallIndirect:
        case Opcode::CallNative:
            return true;
        default:
            return false;
    }
}

const char* nativeAbiTagName(NativeAbiTag tag) noexcept {
    switch (tag) {
        case NativeAbiTag::I64: return "i64";
        case NativeAbiTag::F64: return "f64";
        case NativeAbiTag::Bool: return "bool";
        case NativeAbiTag::Handle: return "handle";
        case NativeAbiTag::BufferView: return "buffer";
        case NativeAbiTag::StructView: return "struct";
        case NativeAbiTag::Callback: return "callback";
    }
    return "<invalid-abi-tag>";
}

const char* nativeOwnershipName(NativeOwnership ownership) noexcept {
    switch (ownership) {
        case NativeOwnership::None: return "none";
        case NativeOwnership::Borrowed: return "borrowed";
        case NativeOwnership::Owned: return "owned";
        case NativeOwnership::Consumed: return "consumed";
    }
    return "<invalid-ownership>";
}

bool opcodeIsSuspension(Opcode opcode) noexcept {
    return opcode == Opcode::Await;
}

bool opcodeIsBlocking(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::TaskBlock:
        case Opcode::ThreadJoin:
        case Opcode::ChannelSend:
        case Opcode::ChannelReceive:
        case Opcode::MutexWithLock:
        case Opcode::RwLockWithRead:
        case Opcode::RwLockWithWrite:
        case Opcode::SemaphoreAcquire:
        case Opcode::ConditionWait:
        case Opcode::ConditionWaitFor:
        case Opcode::SharedWithLock:
            return true;
        default:
            return false;
    }
}

bool opcodeIsThreadBoundary(Opcode opcode) noexcept {
    return opcode == Opcode::TaskSpawn || opcode == Opcode::ThreadStart;
}

bool opcodeIsScopedLock(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::MutexWithLock:
        case Opcode::RwLockWithRead:
        case Opcode::RwLockWithWrite:
        case Opcode::SharedWithLock:
            return true;
        default:
            return false;
    }
}

bool opcodeIsFfi(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::FfiCall:
        case Opcode::HandleBorrow:
        case Opcode::HandleConsume:
        case Opcode::HandleClose:
        case Opcode::CallbackRegister:
        case Opcode::CallbackInvoke:
        case Opcode::CallbackClose:
            return true;
        default:
            return false;
    }
}

bool opcodeRequiresRuntimeCheck(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Await:
        case Opcode::TaskSpawn:
        case Opcode::TaskBlock:
        case Opcode::TaskIgnore:
        case Opcode::TaskCancel:
        case Opcode::ThreadStart:
        case Opcode::ThreadJoin:
        case Opcode::ThreadIsAlive:
        case Opcode::ChannelCreate:
        case Opcode::ChannelSend:
        case Opcode::ChannelReceive:
        case Opcode::ChannelSize:
        case Opcode::ChannelSendAsync:
        case Opcode::ChannelReceiveAsync:
        case Opcode::MutexWithLock:
        case Opcode::RwLockWithRead:
        case Opcode::RwLockWithWrite:
        case Opcode::AtomicLoad:
        case Opcode::AtomicStore:
        case Opcode::AtomicAdd:
        case Opcode::AtomicLoadBool:
        case Opcode::AtomicStoreBool:
        case Opcode::AtomicLoadDouble:
        case Opcode::AtomicStoreDouble:
        case Opcode::AtomicLoadRef:
        case Opcode::AtomicStoreRef:
        case Opcode::SemaphoreAcquire:
        case Opcode::SemaphoreRelease:
        case Opcode::SemaphoreAvailable:
        case Opcode::SemaphoreSetPermits:
        case Opcode::SemaphoreTryAcquire:
        case Opcode::SemaphoreReleaseMany:
        case Opcode::ConditionWait:
        case Opcode::ConditionWaitFor:
        case Opcode::ConditionNotifyOne:
        case Opcode::ConditionNotifyAll:
        case Opcode::SharedGet:
        case Opcode::SharedSet:
        case Opcode::SharedWithLock:
        case Opcode::FfiCall:
        case Opcode::HandleBorrow:
        case Opcode::HandleConsume:
        case Opcode::HandleClose:
        case Opcode::CallbackRegister:
        case Opcode::CallbackInvoke:
        case Opcode::CallbackClose:
            return true;
        default:
            // Refine/NullCheck/Div/... document their own checks in the enum;
            // this predicate covers the concurrency/FFI family, where every op
            // performs at least one runtime-checked transition.
            return false;
    }
}

std::vector<BlockId> Terminator::successors() const {
    std::vector<BlockId> out;
    switch (kind) {
        case TerminatorKind::Jump:
            if (target != kNoBlock) out.push_back(target);
            break;
        case TerminatorKind::Branch:
            if (target != kNoBlock) out.push_back(target);
            if (elseBlock != kNoBlock) out.push_back(elseBlock);
            break;
        case TerminatorKind::Switch:
            for (const auto& entry : cases) {
                if (entry.block != kNoBlock) out.push_back(entry.block);
            }
            if (target != kNoBlock) out.push_back(target);
            break;
        case TerminatorKind::None:
        case TerminatorKind::Return:
        case TerminatorKind::Throw:
        case TerminatorKind::Unreachable:
            break;
    }
    return out;
}

const std::vector<Operand>& Terminator::argumentsFor(std::size_t index) const {
    static const std::vector<Operand> empty;
    return index < edgeArguments.size() ? edgeArguments[index] : empty;
}

} // namespace zl::mir
