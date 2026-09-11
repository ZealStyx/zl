#include "zl/mir/effects.hpp"

#include <limits>

#include "zl/mir/folding.hpp"

namespace zl::mir {

const char* effectName(Effect effect) noexcept {
    switch (effect) {
        case Effect::None: return "none";
        case Effect::Pure: return "pure";
        case Effect::ReadsMemory: return "reads-memory";
        case Effect::WritesMemory: return "writes-memory";
        case Effect::Allocation: return "allocation";
        case Effect::RuntimeCheck: return "runtime-check";
        case Effect::MayThrow: return "may-throw";
        case Effect::ObservableIO: return "observable-io";
        case Effect::Synchronization: return "synchronization";
        case Effect::VolatileRead: return "volatile-read";
        case Effect::Calls: return "calls";
        case Effect::TaskEffect: return "task-effect";
        case Effect::ThreadBoundary: return "thread-boundary";
        case Effect::Suspension: return "suspension";
        case Effect::NativeResource: return "native-resource";
        case Effect::Ownership: return "ownership";
        case Effect::UnwindBarrier: return "unwind-barrier";
    }
    return "<invalid-effect>";
}

std::string describeEffects(EffectSet effects) {
    if (effects == 0) return effectName(Effect::None);
    std::string out;
    for (std::uint32_t bit = 0; bit < 32; ++bit) {
        const EffectSet mask = 1u << bit;
        if ((effects & mask) == 0) continue;
        if (!out.empty()) out += " ";
        out += effectName(static_cast<Effect>(mask));
    }
    return out;
}

// ---------------------------------------------------------------------------
// The opcode table
// ---------------------------------------------------------------------------
// One row per opcode, no `default`: adding an opcode without classifying it is
// a compile error (-Wswitch) rather than a silent "no effects, delete freely".
EffectSet classifyOpcode(Opcode opcode) noexcept {
    using E = Effect;
    switch (opcode) {
        // --- arithmetic ----------------------------------------------------
        // Pure computations that raise on the overflow / zero / range cases the
        // VM checks (see VM::binaryArith). `Pure` is what makes them foldable;
        // `MayThrow` is what stops them being deleted until folding has proved
        // they cannot raise.
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::Div:
        case Opcode::Mod:
        case Opcode::Pow:
        case Opcode::Neg:
            return E::Pure | E::MayThrow;
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::BitXor:
        case Opcode::BitNot:
            return effectsOf(E::Pure);
        case Opcode::Shl:
        case Opcode::Shr:
        case Opcode::Ushr:
            return E::Pure | E::MayThrow; // the shift count is range-checked
        case Opcode::Eq:
        case Opcode::Ne:
            // Equality never raises for the primitive shapes; an object's `==`
            // may dispatch to a user `equals` in the general case, so the
            // throw flag is kept and only discharged for constant operands.
            return E::Pure | E::MayThrow;
        case Opcode::Lt:
        case Opcode::Le:
        case Opcode::Gt:
        case Opcode::Ge:
            return E::Pure | E::MayThrow; // "comparison operators require numbers"
        case Opcode::Not:
        case Opcode::And:
        case Opcode::Or:
            return effectsOf(E::Pure);

        // --- conversion / type refinement ----------------------------------
        case Opcode::Widen:
            return effectsOf(E::Pure);
        case Opcode::Refine:
        case Opcode::NullCheck:
            // The whole point of the instruction is the check: it raises when
            // the value does not match. Deleting one deletes an error.
            return E::Pure | E::RuntimeCheck | E::MayThrow;
        case Opcode::TypeTest:
            return effectsOf(E::Pure); // the non-raising question, not the assertion
        case Opcode::IsNull:
            return effectsOf(E::Pure);

        // --- memory: locals -------------------------------------------------
        case Opcode::Load:
            return effectsOf(E::ReadsMemory);
        case Opcode::Store:
            return effectsOf(E::WritesMemory);

        // --- memory: aggregates ---------------------------------------------
        // Each of these both reads heap state and checks the receiver, so each
        // can raise (null receiver, missing field, out-of-range index).
        case Opcode::FieldLoad:
        case Opcode::IndexLoad:
            return E::ReadsMemory | E::RuntimeCheck | E::MayThrow;
        case Opcode::FieldStore:
        case Opcode::IndexStore:
            return E::WritesMemory | E::RuntimeCheck | E::MayThrow;
        // A static is lazily initialised: reading or writing one can run the
        // class's initialiser, which is a call with everything a call implies.
        case Opcode::StaticLoad:
            return E::ReadsMemory | E::RuntimeCheck | E::MayThrow | E::Calls;
        case Opcode::StaticStore:
            return E::WritesMemory | E::RuntimeCheck | E::MayThrow | E::Calls;

        // --- object construction --------------------------------------------
        case Opcode::Alloc:
        case Opcode::NewCollection:
        case Opcode::MakeClosure:
            return effectsOf(E::Allocation);

        // --- calls -----------------------------------------------------------
        // A call is the least we can say about any callee: it may read and
        // write anything reachable, and it may raise.
        case Opcode::Call:
        case Opcode::InvokeMethod:
        case Opcode::InvokeSuper:
        case Opcode::InvokeStatic:
        case Opcode::CallIndirect:
        case Opcode::CallNative:
            return E::Calls | E::MayThrow | E::ReadsMemory | E::WritesMemory;

        // --- async / tasks ---------------------------------------------------
        case Opcode::Await:
            return E::Suspension | E::TaskEffect | E::MayThrow | E::ReadsMemory | E::WritesMemory;
        case Opcode::TaskCreate:
            return E::Allocation | E::TaskEffect;
        case Opcode::TaskSpawn:
            return E::TaskEffect | E::ThreadBoundary | E::MayThrow | E::WritesMemory;
        case Opcode::TaskBlock:
            return E::TaskEffect | E::Synchronization | E::MayThrow | E::ReadsMemory | E::WritesMemory;
        case Opcode::TaskIgnore:
        case Opcode::TaskCancel:
            return E::TaskEffect | E::MayThrow | E::WritesMemory;

        // --- threads ----------------------------------------------------------
        case Opcode::ThreadStart:
            return E::ThreadBoundary | E::MayThrow | E::WritesMemory;
        case Opcode::ThreadJoin:
            return E::ThreadBoundary | E::Synchronization | E::MayThrow | E::ReadsMemory | E::WritesMemory;
        case Opcode::ThreadIsAlive:
            return E::Synchronization | E::VolatileRead | E::MayThrow;

        // --- channels ---------------------------------------------------------
        case Opcode::ChannelCreate:
            return E::Allocation | E::MayThrow;
        case Opcode::ChannelSend:
        case Opcode::ChannelReceive:
            return E::Synchronization | E::MayThrow | E::ReadsMemory | E::WritesMemory;
        case Opcode::ChannelSendAsync:
        case Opcode::ChannelReceiveAsync:
            return E::Synchronization | E::TaskEffect | E::MayThrow | E::ReadsMemory | E::WritesMemory;
        case Opcode::ChannelSize:
            return E::Synchronization | E::VolatileRead | E::MayThrow;

        // --- scoped synchronisation ------------------------------------------
        // The closure runs on this thread while the capability is held, and the
        // release happens even when the body raises: both facts are what stop
        // these being moved, merged or deleted.
        case Opcode::MutexWithLock:
        case Opcode::RwLockWithRead:
        case Opcode::RwLockWithWrite:
        case Opcode::SharedWithLock:
            return E::Synchronization | E::UnwindBarrier | E::Calls | E::MayThrow | E::ReadsMemory |
                   E::WritesMemory;

        // --- atomics ----------------------------------------------------------
        // Sequentially consistent: the order is part of the program's meaning,
        // and a load is a volatile read (two loads may differ).
        case Opcode::AtomicLoad:
        case Opcode::AtomicLoadBool:
        case Opcode::AtomicLoadDouble:
        case Opcode::AtomicLoadRef:
            return E::Synchronization | E::VolatileRead | E::MayThrow;
        case Opcode::AtomicStore:
        case Opcode::AtomicStoreBool:
        case Opcode::AtomicStoreDouble:
        case Opcode::AtomicStoreRef:
            return E::Synchronization | E::MayThrow | E::WritesMemory;
        case Opcode::AtomicAdd:
            return E::Synchronization | E::VolatileRead | E::MayThrow | E::WritesMemory;

        // --- semaphores --------------------------------------------------------
        case Opcode::SemaphoreAcquire:
        case Opcode::SemaphoreRelease:
        case Opcode::SemaphoreSetPermits:
        case Opcode::SemaphoreReleaseMany:
            return E::Synchronization | E::MayThrow | E::WritesMemory;
        case Opcode::SemaphoreAvailable:
        case Opcode::SemaphoreTryAcquire:
            return E::Synchronization | E::VolatileRead | E::MayThrow;

        // --- conditions ---------------------------------------------------------
        case Opcode::ConditionWait:
        case Opcode::ConditionWaitFor:
        case Opcode::ConditionNotifyOne:
        case Opcode::ConditionNotifyAll:
            return E::Synchronization | E::MayThrow | E::ReadsMemory | E::WritesMemory;

        // --- shared state --------------------------------------------------------
        case Opcode::SharedCreate:
            return effectsOf(E::Allocation);
        case Opcode::SharedGet:
            return E::Synchronization | E::VolatileRead | E::MayThrow;
        case Opcode::SharedSet:
            return E::Synchronization | E::MayThrow | E::WritesMemory;

        // --- FFI and native resources ---------------------------------------------
        case Opcode::FfiCall:
            return E::NativeResource | E::Calls | E::MayThrow | E::ReadsMemory | E::WritesMemory;
        case Opcode::HandleBorrow:
            return E::NativeResource | E::RuntimeCheck | E::MayThrow;
        case Opcode::HandleConsume:
        case Opcode::HandleClose:
        case Opcode::CallbackRegister:
        case Opcode::CallbackClose:
            return E::NativeResource | E::RuntimeCheck | E::MayThrow | E::WritesMemory;
        case Opcode::CallbackInvoke:
            return E::NativeResource | E::Calls | E::MayThrow | E::ReadsMemory | E::WritesMemory;

        // --- ownership and resource lifetime ---------------------------------------
        // Each of these is an event in the language's lifetime discipline: a
        // move makes a slot unusable, a borrow claims a region, a drop releases
        // deterministically. Reordering them changes what the verifier is
        // entitled to assume, and removing one changes when a resource dies.
        case Opcode::Move:
            return E::Ownership | E::ReadsMemory | E::WritesMemory | E::MayThrow;
        case Opcode::Borrow:
            return E::Ownership | E::WritesMemory | E::MayThrow;
        case Opcode::EndBorrow:
            return E::Ownership | E::ReadsMemory | E::WritesMemory | E::MayThrow;
        case Opcode::Drop:
            return E::Ownership | E::ReadsMemory | E::WritesMemory | E::MayThrow;

        // --- loop support and output ------------------------------------------------
        case Opcode::RangeInBounds:
            return E::Pure | E::MayThrow; // a zero step raises
        case Opcode::Log:
            return E::ObservableIO | E::MayThrow;

        case Opcode::Nop:
            return effectsOf(E::Pure);
    }
    // Unreachable: the switch is exhaustive over the enum. A missing case is a
    // -Wswitch error, not a classification.
    return EffectSet{0};
}

// Can *this* operation raise, given what is known about these operands?
//
// The opcode table says "arithmetic may throw", which is true and useless on
// its own: it would make every `add` undeletable. This is the per-instance
// discharge, in three tiers, each a proof rather than a heuristic:
//
//   1. every operand is a constant, so the shared evaluator can decide. It
//      refuses exactly the operations that raise, so a successful fold is
//      proof that no unwind edge exists.
//   2. the operation cannot raise whatever the remaining operands are, because
//      of a constant it does have: `x + 0` cannot overflow, `x / 1` cannot
//      divide by zero, `x << 3` cannot be out of range.
//   3. the operand *types* exclude the failure: an ordered comparison of two
//      numbers never raises, because the rule it can break ("comparison
//      operators require numbers") is not breakable by a number.
//
// Anything not covered stays flagged. A dead `x * y` is not deleted, because
// it can overflow, and deleting it would delete the program's error.
bool operationCanRaise(const Module& module, const Instruction& instruction) {
    if (instruction.result == kNoTemp) return true;

    // Tier 1: the evaluator decides for all-constant operands.
    if (foldInstruction(module, instruction).has_value()) return false;

    const std::uint32_t intType = module.types.intType();
    const std::uint32_t doubleType = module.types.doubleType();
    const std::uint32_t stringType = module.types.stringType();

    auto constantAt = [&](std::size_t index) -> const Constant* {
        if (index >= instruction.operands.size()) return nullptr;
        const Operand& operand = instruction.operands[index];
        if (operand.kind != OperandKind::Const) return nullptr;
        return module.constant(operand.index);
    };
    auto intAt = [&](std::size_t index, std::int64_t& out) -> bool {
        const Constant* constant = constantAt(index);
        if (!constant || constant->kind != ConstKind::Int) return false;
        out = constant->intValue;
        return true;
    };

    const std::size_t arity = instruction.operands.size();
    const bool allNumeric = [&] {
        for (const auto& operand : instruction.operands) {
            if (operand.type != intType && operand.type != doubleType) return false;
        }
        return arity != 0;
    }();

    switch (instruction.opcode) {
        case Opcode::Add:
            // A string `+` is concatenation, which cannot fail.
            if (arity == 2 &&
                (instruction.operands[0].type == stringType ||
                 instruction.operands[1].type == stringType))
                return false;
            // x + 0 and 0 + x cannot overflow for any x.
            {
                std::int64_t value = 0;
                if (intAt(0, value) && value == 0) return false;
                if (intAt(1, value) && value == 0) return false;
            }
            return true;
        case Opcode::Sub:
            // x - 0 cannot overflow; 0 - x can (0 - INT64_MIN), so only the
            // right-hand side counts.
            {
                std::int64_t value = 0;
                if (intAt(1, value) && value == 0) return false;
            }
            return true;
        case Opcode::Mul:
            // x * 0 is 0 and x * 1 is x, for every x, including INT64_MIN.
            {
                std::int64_t value = 0;
                if (intAt(0, value) && (value == 0 || value == 1)) return false;
                if (intAt(1, value) && (value == 0 || value == 1)) return false;
            }
            return true;
        case Opcode::Div:
        case Opcode::Mod: {
            // A constant non-zero divisor removes the divide-by-zero raise. The
            // only other raise is INT64_MIN / -1, which needs a constant left
            // operand to rule out.
            std::int64_t divisor = 0;
            if (!intAt(1, divisor) || divisor == 0) return true;
            if (divisor == -1) {
                std::int64_t dividend = 0;
                if (intAt(0, dividend) && dividend != std::numeric_limits<std::int64_t>::min())
                    return false;
                return true;
            }
            return false;
        }
        case Opcode::Shl:
        case Opcode::Shr:
        case Opcode::Ushr: {
            // The range check is the only raise, and a constant count settles
            // it for every value being shifted.
            std::int64_t count = 0;
            if (intAt(1, count) && count >= 0 && count < 64) return false;
            return true;
        }
        case Opcode::Eq:
        case Opcode::Ne:
            // Equality compares by value in the VM and never raises, whatever
            // the operand types are.
            return false;
        case Opcode::Lt:
        case Opcode::Le:
        case Opcode::Gt:
        case Opcode::Ge:
            // Ordering requires numbers; two numeric operands satisfy that by
            // construction, and NaN is a result rather than an error.
            return !allNumeric;
        default:
            return true;
    }
}

EffectSet classifyInstruction(const Module& module, const Instruction& instruction) {
    EffectSet effects = classifyOpcode(instruction.opcode);
    if (hasEffect(effects, Effect::MayThrow) && hasEffect(effects, Effect::Pure)) {
        if (!operationCanRaise(module, instruction)) effects &= ~effectsOf(Effect::MayThrow);
    }
    return effects;
}

bool instructionMayThrow(const Module& module, const Instruction& instruction) {
    return hasEffect(classifyInstruction(module, instruction), Effect::MayThrow);
}

bool isRemovableIfResultUnused(EffectSet effects) noexcept {
    // Only "computes" and "reads" are compatible with deletion. Note that
    // `MayThrow` is *not* in the accepted set: an instruction that can raise
    // is doing something even when nobody reads its result.
    constexpr EffectSet kAllowed = effectsOf(Effect::Pure) | effectsOf(Effect::ReadsMemory);
    return (effects & ~kAllowed) == 0;
}

bool isRemovableIfResultUnused(const Module& module, const Instruction& instruction) {
    if (instruction.result == kNoTemp) return false;
    return isRemovableIfResultUnused(classifyInstruction(module, instruction));
}

bool isObservableEffect(EffectSet effects) noexcept {
    constexpr EffectSet kObservable = effectsOf(Effect::ObservableIO) | effectsOf(Effect::Calls) |
                                      effectsOf(Effect::TaskEffect) | effectsOf(Effect::ThreadBoundary) |
                                      effectsOf(Effect::Suspension) | effectsOf(Effect::NativeResource) |
                                      effectsOf(Effect::Ownership) | effectsOf(Effect::Synchronization) |
                                      effectsOf(Effect::VolatileRead) | effectsOf(Effect::WritesMemory);
    return (effects & kObservable) != 0;
}

} // namespace zl::mir
