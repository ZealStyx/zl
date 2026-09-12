#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Target abstraction for the native backend
// ---------------------------------------------------------------------------
//
// Everything above this header is target independent. The native pipeline is
//
//     ZL -> MIR (verified) -> Native IR (LIR) -> machine code
//
// and only the last arrow knows what an x86-64 instruction looks like. This
// header is the contract between the two halves: it states, in target-neutral
// terms, the facts a lowering pass legitimately needs to know before machine
// encoding exists - how values are classified, how wide they are, which
// registers arguments and results live in, and what a frame must look like.
//
// The rule this header exists to enforce: *no target-specific constant appears
// anywhere outside a TargetMachine.* Selection asks the target which class a
// value belongs to and how many argument registers of that class exist; it
// never spells `rdi` or `xmm0`.

namespace zl::native {

// --- value classes ---------------------------------------------------------
//
// A value class is "which machine bank does this live in, and what does the
// ABI do with it". It is deliberately coarser than the ZL type lattice and
// coarser than MIR types: MIR answers "what does the language mean by this
// value", a value class answers "where does the machine keep it". The mapping
// from MIR type to value class is the one place the two meet
// (see classifyType in lir.hpp).
enum class ValueClass : std::uint8_t {
    // No value. Only legal as a function result or an instruction with no
    // definition.
    Void,
    // Integral scalar in a general-purpose register. ZL `int` is a 64-bit
    // two's-complement value; ZL `bool` is also this class, represented as 0
    // or 1 in a full-width register, because the ABI has no narrower place to
    // put it and because a bool is only ever produced by a compare or a
    // constant, both of which already normalise to 0/1.
    Integer,
    // IEEE-754 binary64 in a floating-point register. ZL `double`/`decimal`.
    Float,
    // A managed object reference: pointer-width, points at a GC-visible heap
    // object, and therefore must be reported to the collector at any safepoint
    // it is live across. The class exists now, and is deliberately *rejected*
    // by the current selection subset rather than quietly treated as an
    // integer: an untracked reference in a register is exactly the bug that is
    // impossible to find later, so the backend refuses the function instead.
    Reference,
};

[[nodiscard]] const char* valueClassName(ValueClass cls) noexcept;

// True when a value of this class occupies a general-purpose register.
[[nodiscard]] bool isGprClass(ValueClass cls) noexcept;

// --- registers -------------------------------------------------------------
//
// A physical register is an (id, class) pair plus a name. Ids are the target's
// own encoding numbers, so the encoder can use them directly, but nothing
// above the encoder interprets them: allocation only ever compares them and
// looks them up in the lists below.
struct PhysReg {
    std::uint8_t id{0};
    ValueClass cls{ValueClass::Integer};
    const char* name{"?"};

    friend bool operator==(const PhysReg& a, const PhysReg& b) noexcept {
        return a.id == b.id && a.cls == b.cls;
    }
    friend bool operator!=(const PhysReg& a, const PhysReg& b) noexcept { return !(a == b); }
};

// --- calling convention ----------------------------------------------------
//
// One convention, described as data rather than as code, so that adding a
// target means adding a table and an encoder instead of editing the lowering.
//
// The description covers exactly what the backend needs to place a call and to
// receive one:
//
//   * where the first N arguments of each class go, and what happens after N;
//   * where the result comes back;
//   * which registers a call destroys and which it preserves - the fact that
//     decides whether a value live across a call may stay in a register at all;
//   * the stack discipline: alignment at the call instruction, any mandated
//     shadow space, and whether a leaf may use a red zone below the stack
//     pointer.
struct CallingConvention {
    std::string name;

    // Argument registers, in order, per class. When they run out, remaining
    // arguments are passed on the stack in left-to-right order at increasing
    // addresses (the only discipline the backend implements; a convention that
    // needs another one must say so with a new field rather than by having the
    // emitter improvise).
    std::vector<PhysReg> intArgRegs;
    std::vector<PhysReg> floatArgRegs;

    // Where a result of each class is returned.
    PhysReg intReturnReg{};
    PhysReg floatReturnReg{};

    // Destroyed by a call. A vreg live across a call may not be allocated to
    // one of these.
    std::vector<PhysReg> callerSaved;
    // Preserved across a call; the callee must save and restore any it uses.
    std::vector<PhysReg> calleeSaved;

    // Scratch registers the encoder may always use inside one instruction's
    // expansion without asking the allocator. They are a subset of
    // callerSaved and are never handed to the allocator.
    std::vector<PhysReg> intScratch;
    std::vector<PhysReg> floatScratch;

    // Required stack alignment (bytes) at the point a `call` executes.
    std::uint32_t stackAlignment{16};
    // Bytes the caller must reserve above the return address for the callee
    // to spill its register arguments into (Win64's 32-byte shadow space); 0
    // where the convention has none.
    std::uint32_t shadowSpace{0};
    // Bytes below the stack pointer a leaf function may use without adjusting
    // rsp. The backend does not currently exploit it; it is recorded because
    // "may I?" is a property of the convention, not of the emitter's mood.
    std::uint32_t redZone{0};
    // True when the callee, not the caller, pops argument stack space.
    bool calleePopsArguments{false};
};

enum class Arch : std::uint8_t { X86_64, AArch64, Unknown };
enum class Platform : std::uint8_t { SysV, Windows, Unknown };

[[nodiscard]] const char* archName(Arch arch) noexcept;

// A target the backend can be asked about. Selection consults this; only the
// encoder consumes register ids.
struct TargetMachine {
    Arch arch{Arch::Unknown};
    Platform platform{Platform::Unknown};
    std::string triple;

    std::uint32_t pointerSize{8};
    std::uint32_t stackSlotSize{8};
    // Frame growth direction: every target the backend supports grows down,
    // recorded so a future one cannot silently inherit the assumption.
    bool stackGrowsDown{true};

    CallingConvention cc;

    // Registers the allocator may hand out, per class. Deliberately a subset
    // of the callee-saved set for the integer class: this backend allocates
    // across calls, and a callee-saved register is the only place a value can
    // survive one without a spill.
    std::vector<PhysReg> allocatableInt;
    std::vector<PhysReg> allocatableFloat;

    // Bytes a value of this class occupies in a frame slot.
    [[nodiscard]] std::uint32_t slotSizeFor(ValueClass cls) const noexcept;
    // Can this backend actually encode for the target?
    [[nodiscard]] bool encoderAvailable() const noexcept;
};

// The x86-64 System V convention (Linux, macOS, the BSDs).
[[nodiscard]] const TargetMachine& x64SysVTarget();
// The x86-64 Windows convention. Described so calls can be *reasoned* about
// and so the difference is visible in one table; the encoder for it is not
// written yet, and `encoderAvailable()` says so rather than emitting SysV code
// under a Windows name.
[[nodiscard]] const TargetMachine& x64WindowsTarget();

// The target matching the host this build runs on, or a target with
// `arch == Unknown` when the host is not one the backend describes.
[[nodiscard]] const TargetMachine& hostTarget();

} // namespace zl::native
