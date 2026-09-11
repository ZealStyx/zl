#include <cstdlib>
#include <iostream>
#include <string>
#include "zl/mir/builder.hpp"
#include "zl/mir/verifier.hpp"

using namespace zl::mir;
namespace {
int checks = 0;
void require(bool ok, const std::string& message) {
    ++checks;
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
bool has(const VerificationReport& r, SafetyProperty p) {
    for (const auto& d : r.diagnostics) if (d.property == p && d.severity == DiagnosticSeverity::Error) return true;
    return false;
}
struct Fixture {
    ModuleBuilder mb{"safety"};
    FunctionBuilder f{mb.addFunction("Safety.main()")};
    SourceLocation loc{"imported/Safety.zl", 17, 0};
    Fixture() { f.setReturnType(mb.types().voidType()); f.setLocation(loc); f.setCurrentBlock(f.addBlock()); }
    Operand integer() {
        Constant c; c.kind = ConstKind::Int; c.intValue = 1;
        return Operand::constant(mb.mutableModule().internConstant(c), mb.types().intType());
    }
    Operand condition() { return f.parameterOperand(f.addParameter("condition", mb.types().boolType())); }
    SlotId owned() {
        auto type = mb.types().objectType("Resource");
        auto slot = f.addSlot("resource", type, true, zl::OwnershipKind::OWNED);
        f.emitStore(slot, Operand::temp(f.emitAlloc("Resource", {}, type), type));
        return slot;
    }
    VerificationReport verify(bool audit = false) {
        f.finish();
        VerifierOptions o; o.auditBoundaries = audit;
        return verifyModule(mb.module(), o);
    }
};
void definiteAssignment() {
    for (bool both : {false, true}) {
        Fixture m;
        auto s = m.f.addSlot("x", m.mb.types().intType());
        auto a = m.f.addBlock(), b = m.f.addBlock(), join = m.f.addBlock();
        m.f.emitBranch(m.condition(), a, b);
        m.f.setCurrentBlock(a); m.f.emitStore(s, m.integer()); m.f.emitJump(join);
        m.f.setCurrentBlock(b); if (both) m.f.emitStore(s, m.integer()); m.f.emitJump(join);
        m.f.setCurrentBlock(join); (void)m.f.emitLoad(s, m.loc); m.f.emitReturn();
        auto r = m.verify();
        require(both ? r.ok() : has(r, SafetyProperty::Definition), r.describe());
        if (!both) {
            require(r.errorCount() == 1, "fixed-point diagnostics must not repeat: " + r.describe());
            require(r.diagnostics[0].location == m.loc, "source provenance lost");
            require(r.toJson().find("\"property\":\"definition\"") != std::string::npos, "JSON property missing");
        }
    }
}
void loopAssignment() {
    Fixture m;
    auto s = m.f.addSlot("x", m.mb.types().intType());
    auto loop = m.f.addBlock(), exit = m.f.addBlock();
    m.f.emitStore(s, m.integer()); m.f.emitJump(loop);
    m.f.setCurrentBlock(loop); (void)m.f.emitLoad(s); m.f.emitBranch(m.condition(), loop, exit);
    m.f.setCurrentBlock(exit); m.f.emitReturn();
    require(m.verify().ok(), "entry initialization must survive a backedge");
}
void exceptionalAssignment() {
    for (bool before : {false, true}) {
        Fixture m;
        auto s = m.f.addSlot("x", m.mb.types().intType());
        auto catchSlot = m.f.addSlot("error", m.mb.types().stringType());
        m.f.function().slots[catchSlot - 1].isCatchBinding = true;
        auto handler = m.f.addBlock(BlockKind::Catch);
        if (before) m.f.emitStore(s, m.integer());
        m.f.pushExceptionHandler(0, handler, catchSlot);
        (void)m.f.emitCallNative("example.mayThrow", 0, {}, 0);
        if (!before) m.f.emitStore(s, m.integer());
        m.f.emitReturn();
        m.f.setCurrentBlock(handler); (void)m.f.emitLoad(catchSlot); (void)m.f.emitLoad(s); m.f.emitReturn();
        auto r = m.verify();
        require(before ? r.ok() : has(r, SafetyProperty::Definition), r.describe());
    }
}
void conditionalBorrow() {
    for (bool ended : {false, true}) {
        Fixture m;
        auto s = m.owned();
        auto view = m.f.addSlot("view", m.mb.types().objectType("Resource"), true, zl::OwnershipKind::BORROW, "resource");
        auto a = m.f.addBlock(), b = m.f.addBlock(), join = m.f.addBlock();
        m.f.emitBranch(m.condition(), a, b);
        m.f.setCurrentBlock(a);
        m.f.emitBorrow(view, Operand::temp(m.f.emitLoad(s), m.mb.types().objectType("Resource")));
        if (ended) m.f.emitEndBorrow(view);
        m.f.emitJump(join);
        m.f.setCurrentBlock(b); m.f.emitJump(join);
        m.f.setCurrentBlock(join); (void)m.f.emitMove(s); m.f.emitReturn();
        auto r = m.verify();
        require(ended ? r.ok() : !r.ok(), r.describe());
    }
}
void inactiveBorrowAndEscape() {
    Fixture m;
    auto s = m.owned(); auto type = m.mb.types().objectType("Resource");
    auto b = m.f.addSlot("view", type, true, zl::OwnershipKind::BORROW, "resource");
    m.f.emitBorrow(b, Operand::temp(m.f.emitLoad(s), type));
    m.f.emitEndBorrow(b);
    auto v = Operand::temp(m.f.emitLoad(b), type);
    m.f.setReturnType(type); m.f.emitReturn(v);
    require(has(m.verify(), SafetyProperty::Borrow), "use/return after end_borrow was accepted");
}
void nativeLifetime() {
    for (bool close : {false, true}) {
        Fixture m;
        auto type = m.mb.types().nativeHandleType();
        auto handle = m.f.parameterOperand(m.f.addParameter("handle", type));
        auto a = m.f.addBlock(), b = m.f.addBlock(), join = m.f.addBlock();
        m.f.emitBranch(m.condition(), a, b);
        m.f.setCurrentBlock(a); if (close) m.f.emitHandleClose(handle); m.f.emitJump(join);
        m.f.setCurrentBlock(b); m.f.emitJump(join);
        m.f.setCurrentBlock(join); m.f.emitHandleClose(handle, m.loc); m.f.emitReturn();
        auto r = m.verify();
        require(close ? has(r, SafetyProperty::ResourceLifetime) : r.ok(), r.describe());
    }
    Fixture m;
    auto type = m.mb.types().nativeHandleType();
    auto handle = m.f.parameterOperand(m.f.addParameter("handle", type));
    auto alias = Operand::temp(m.f.emitRefine(handle, type), type);
    m.f.emitHandleClose(handle); m.f.emitHandleClose(alias); m.f.emitReturn();
    require(has(m.verify(), SafetyProperty::ResourceLifetime), "SSA alias hid a closed handle");
}
void suspensionBorrowRoots() {
    for (auto ownership : {zl::OwnershipKind::OWNED, zl::OwnershipKind::GC}) {
        Fixture m;
        m.f.setAsync(true);
        auto type = m.mb.types().objectType("Resource");
        auto owner = m.f.parameterOperand(m.f.addParameter("owner", type, ownership));
        auto taskType = m.mb.types().taskType(m.mb.types().intType());
        auto task = m.f.parameterOperand(m.f.addParameter("task", taskType));
        auto view = m.f.addSlot("view", type, true, zl::OwnershipKind::BORROW);
        m.f.emitBorrow(view, owner);
        (void)m.f.emitAwait(task, m.mb.types().intType(), m.loc);
        m.f.emitEndBorrow(view); m.f.emitReturn();
        auto r = m.verify();
        require(ownership == zl::OwnershipKind::OWNED ? r.ok() : has(r, SafetyProperty::Concurrency), r.describe());
        if (ownership == zl::OwnershipKind::GC) require(r.errorCount() == 1, r.describe());
    }
}
void borrowedNativeHandle() {
    for (bool releaseBorrow : {false, true}) {
        Fixture m;
        auto type = m.mb.types().nativeHandleType();
        auto owner = m.f.parameterOperand(m.f.addParameter("handle", type));
        auto borrowed = Operand::temp(m.f.emitHandleBorrow(owner, type), type);
        if (!releaseBorrow) m.f.emitHandleClose(owner);
        m.f.emitHandleClose(borrowed);
        m.f.emitReturn();
        auto r = m.verify();
        require(has(r, releaseBorrow ? SafetyProperty::Ownership : SafetyProperty::ResourceLifetime), r.describe());
    }
    Fixture m;
    auto type = m.mb.types().nativeHandleType();
    auto owner = m.f.parameterOperand(m.f.addParameter("handle", type));
    (void)m.f.emitHandleConsume(owner, type);
    m.f.emitHandleClose(owner); m.f.emitReturn();
    require(has(m.verify(), SafetyProperty::ResourceLifetime), "consume then close accepted");
}
void nativeContract() {
    Fixture m;
    (void)m.f.emitCallNative("test.native", 0, {m.integer()}, m.mb.types().intType());
    m.f.block(m.f.currentBlock()).instructions.back().target.argumentTypes = {m.mb.types().boolType()};
    m.f.emitReturn();
    require(has(m.verify(), SafetyProperty::NativeCall), "native type contract mismatch accepted");
}
void auditIsNotRejection() {
    Fixture m;
    auto dynamic = m.f.parameterOperand(m.f.addParameter("dynamic", m.mb.types().unknownType()));
    (void)m.f.emitRefine(dynamic, m.mb.types().intType(), m.loc); m.f.emitReturn();
    const auto report = m.verify(true);
    require(report.ok() && report.warningCount() == 1, report.describe());
    require(report.diagnostics[0].property == SafetyProperty::DynamicBoundary, "wrong audit property");
}
void moveDiagnostics() {
    Fixture m;
    auto s = m.owned(); (void)m.f.emitMove(s); (void)m.f.emitLoad(s); m.f.emitReturn();
    auto r = m.verify();
    require(r.errorCount() == 1 && has(r, SafetyProperty::Move), r.describe());
}
} // namespace
int main() {
    definiteAssignment(); loopAssignment(); exceptionalAssignment(); conditionalBorrow();
    inactiveBorrowAndEscape(); nativeLifetime(); suspensionBorrowRoots(); borrowedNativeHandle(); nativeContract(); auditIsNotRejection(); moveDiagnostics();
    std::cout << "MIR safety: " << checks << " checks passed\n";
}
