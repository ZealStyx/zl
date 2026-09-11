#include "zl/native/target.hpp"

namespace zl::native {
namespace {

// x86-64 register encodings. These numbers are the ModRM/REX register fields,
// so the encoder can use them directly; nothing above the encoder reads them.
constexpr PhysReg gpr(std::uint8_t id, const char* name) { return PhysReg{id, ValueClass::Integer, name}; }
constexpr PhysReg xmm(std::uint8_t id, const char* name) { return PhysReg{id, ValueClass::Float, name}; }

const PhysReg kRax = gpr(0, "rax");
const PhysReg kRcx = gpr(1, "rcx");
const PhysReg kRdx = gpr(2, "rdx");
const PhysReg kRbx = gpr(3, "rbx");
const PhysReg kRsi = gpr(6, "rsi");
const PhysReg kRdi = gpr(7, "rdi");
const PhysReg kR8 = gpr(8, "r8");
const PhysReg kR9 = gpr(9, "r9");
const PhysReg kR10 = gpr(10, "r10");
const PhysReg kR11 = gpr(11, "r11");
const PhysReg kR12 = gpr(12, "r12");
const PhysReg kR13 = gpr(13, "r13");
const PhysReg kR14 = gpr(14, "r14");
const PhysReg kR15 = gpr(15, "r15");

const PhysReg kXmm0 = xmm(0, "xmm0");
const PhysReg kXmm1 = xmm(1, "xmm1");
const PhysReg kXmm2 = xmm(2, "xmm2");
const PhysReg kXmm3 = xmm(3, "xmm3");
const PhysReg kXmm4 = xmm(4, "xmm4");
const PhysReg kXmm5 = xmm(5, "xmm5");
const PhysReg kXmm6 = xmm(6, "xmm6");
const PhysReg kXmm7 = xmm(7, "xmm7");
const PhysReg kXmm8 = xmm(8, "xmm8");
const PhysReg kXmm9 = xmm(9, "xmm9");

TargetMachine makeSysV() {
    TargetMachine t;
    t.arch = Arch::X86_64;
    t.platform = Platform::SysV;
    t.triple = "x86_64-unknown-linux-gnu";

    CallingConvention& cc = t.cc;
    cc.name = "x86-64 System V";
    cc.intArgRegs = {kRdi, kRsi, kRdx, kRcx, kR8, kR9};
    cc.floatArgRegs = {kXmm0, kXmm1, kXmm2, kXmm3, kXmm4, kXmm5, kXmm6, kXmm7};
    cc.intReturnReg = kRax;
    cc.floatReturnReg = kXmm0;
    cc.callerSaved = {kRax, kRcx, kRdx, kRsi, kRdi, kR8, kR9, kR10, kR11};
    cc.calleeSaved = {kRbx, kR12, kR13, kR14, kR15};
    // The emitter computes into rax/rcx (and rdx, which idiv clobbers) and
    // into xmm8/xmm9, none of which the allocator hands out.
    cc.intScratch = {kRax, kRcx, kRdx};
    cc.floatScratch = {kXmm8, kXmm9};
    cc.stackAlignment = 16;
    cc.shadowSpace = 0;
    cc.redZone = 128;
    cc.calleePopsArguments = false;

    // Allocatable sets are the callee-saved registers: this backend's baseline
    // policy keeps every value in a frame slot, and the sets are what a future
    // allocator would draw from without touching the scratch registers the
    // encoder relies on.
    t.allocatableInt = cc.calleeSaved;
    t.allocatableFloat = {xmm(10, "xmm10"), xmm(11, "xmm11"), xmm(12, "xmm12")};
    return t;
}

TargetMachine makeWin64() {
    TargetMachine t;
    t.arch = Arch::X86_64;
    t.platform = Platform::Windows;
    t.triple = "x86_64-pc-windows-msvc";

    CallingConvention& cc = t.cc;
    cc.name = "x86-64 Windows";
    // Win64 passes the first four arguments positionally: argument i goes in
    // the i-th integer OR the i-th float register depending on its class, and
    // the other register of that position is not reusable. The backend records
    // the register lists here; the positional rule is honoured by the emitter,
    // which is why the two lists are the same length.
    cc.intArgRegs = {kRcx, kRdx, kR8, kR9};
    cc.floatArgRegs = {kXmm0, kXmm1, kXmm2, kXmm3};
    cc.intReturnReg = kRax;
    cc.floatReturnReg = kXmm0;
    cc.callerSaved = {kRax, kRcx, kRdx, kR8, kR9, kR10, kR11};
    cc.calleeSaved = {kRbx, kRsi, kRdi, kR12, kR13, kR14, kR15};
    cc.intScratch = {kRax, kRcx, kRdx};
    cc.floatScratch = {kXmm4, kXmm5};
    cc.stackAlignment = 16;
    cc.shadowSpace = 32;
    cc.redZone = 0;
    cc.calleePopsArguments = false;

    t.allocatableInt = {kRbx, kR12, kR13, kR14, kR15};
    t.allocatableFloat = {kXmm6, kXmm7};
    return t;
}

TargetMachine makeUnknown() {
    TargetMachine t;
    t.arch = Arch::Unknown;
    t.platform = Platform::Unknown;
    t.triple = "unknown";
    return t;
}

} // namespace

const char* valueClassName(ValueClass cls) noexcept {
    switch (cls) {
        case ValueClass::Void: return "void";
        case ValueClass::Integer: return "int";
        case ValueClass::Float: return "float";
        case ValueClass::Reference: return "ref";
    }
    return "?";
}

bool isGprClass(ValueClass cls) noexcept {
    return cls == ValueClass::Integer || cls == ValueClass::Reference;
}

const char* archName(Arch arch) noexcept {
    switch (arch) {
        case Arch::X86_64: return "x86_64";
        case Arch::AArch64: return "aarch64";
        case Arch::Unknown: return "unknown";
    }
    return "unknown";
}

std::uint32_t TargetMachine::slotSizeFor(ValueClass cls) const noexcept {
    switch (cls) {
        case ValueClass::Void: return 0;
        case ValueClass::Integer:
        case ValueClass::Reference: return stackSlotSize;
        case ValueClass::Float: return 8;
    }
    return stackSlotSize;
}

bool TargetMachine::encoderAvailable() const noexcept {
    // Only the System V encoder exists. Saying so explicitly is the point:
    // emitting SysV bytes for a Windows target would be a silent miscompile of
    // every call.
    return arch == Arch::X86_64 && platform == Platform::SysV;
}

const TargetMachine& x64SysVTarget() {
    static const TargetMachine target = makeSysV();
    return target;
}

const TargetMachine& x64WindowsTarget() {
    static const TargetMachine target = makeWin64();
    return target;
}

const TargetMachine& hostTarget() {
#if defined(_WIN32)
    return x64WindowsTarget();
#elif defined(__x86_64__) || defined(_M_X64)
    return x64SysVTarget();
#else
    static const TargetMachine unknown = makeUnknown();
    return unknown;
#endif
}

} // namespace zl::native
