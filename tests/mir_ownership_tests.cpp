// MIR ownership and lifetime regressions.
//
// ZL's ownership model does not evaporate after type checking: the MIR keeps
// the storage contracts (gc / owned / borrow / shared) and the ownership
// events (move, borrow, end_borrow, drop), and the verifier runs a
// flow-sensitive analysis over the CFG so the invalid states the language
// forbids are invalid here too - use after move, use after drop, a resource
// released twice, a borrow outliving its owner, a release of storage that has
// no lifetime to end, and moves or drops that reach a use only across a
// control-flow join.
//
// Two halves, mirroring the other MIR suites:
//   * hand-built MIR modules that name one invalid state each (plus the legal
//     shapes that must stay legal, notably the end-of-lifetime release
//     emitted at function exit while a function-scoped borrow is live);
//   * pipeline tests that lower real ZL source using `owned` / `move` /
//     `borrow` and require the result to verify as emitted.
#include "zl/compiler/module_loader.hpp"
#include "zl/compiler/type_checker.hpp"
#include "zl/mir/builder.hpp"
#include "zl/mir/lowering.hpp"
#include "zl/mir/verifier.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "mir ownership regression: " << message << '\n';
    ++failures;
}

std::string reportText(const zl::mir::VerificationReport& report) {
    return report.describe();
}

using namespace zl::mir;

// A one-function module builder: an owned class slot named `res` in an entry
// block that ends with whatever the test appends.
struct OwnedModule {
    ModuleBuilder builder{"own"};
    FunctionBuilder fb;
    SlotId res{0};

    explicit OwnedModule(zl::OwnershipKind ownership = zl::OwnershipKind::OWNED)
        : fb(builder.addFunction("Own.go()")) {
        fb.setReturnType(builder.types().voidType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        res = fb.addSlot("res", builder.types().objectType("Res"), true, ownership);
    }

    // A fresh instance flowing into `res`, the way a store from an alloc does.
    void storeRes() {
        const TempId fresh = fb.emitAlloc("Res", {}, builder.types().objectType("Res"));
        fb.emitStore(res, Operand::temp(fresh, builder.types().objectType("Res")));
    }

    [[nodiscard]] Operand loadedRes() {
        return Operand::temp(fb.emitLoad(res), builder.types().objectType("Res"));
    }

    VerificationReport verify() {
        fb.emitReturn();
        fb.finish();
        builder.setEntryPoint(fb.function().id);
        Module module = builder.take();
        return verifyModule(module);
    }
};

// ---------------------------------------------------------------------------
// Invalid states: one test per rule
// ---------------------------------------------------------------------------

// Using a slot's value after its storage was released.
void useAfterDropTest() {
    OwnedModule m;
    m.storeRes();
    m.fb.emitDrop(m.res);
    m.fb.emitLoad(m.res); // the use the release invalidated
    const auto report = m.verify();
    require(!report.ok(), "a load after drop of the same slot must be rejected:\n" + reportText(report));
    require(report.describe().find("dropped") != std::string::npos,
            "the rejection must name the drop:\n" + reportText(report));
}

// A resource releases exactly once.
void doubleDropTest() {
    OwnedModule m;
    m.storeRes();
    m.fb.emitDrop(m.res);
    m.fb.emitDrop(m.res);
    const auto report = m.verify();
    require(!report.ok(), "a second drop of the same slot must be rejected:\n" + reportText(report));
    require(report.describe().find("exactly once") != std::string::npos,
            "the rejection must state the once-only rule:\n" + reportText(report));
}

// Releasing storage whose value was already moved out.
void dropAfterMoveTest() {
    OwnedModule m;
    m.storeRes();
    m.fb.emitMove(m.res);
    m.fb.emitDrop(m.res);
    const auto report = m.verify();
    require(!report.ok(), "a drop after move of the same slot must be rejected:\n" + reportText(report));
    require(report.describe().find("moved") != std::string::npos,
            "the rejection must name the move:\n" + reportText(report));
}

// Dropping an owner while a borrow of it is still live - mid-block, where the
// borrow can still be used afterwards.
void dropWhileBorrowedTest() {
    OwnedModule m;
    m.storeRes();
    const SlotId view = m.fb.addSlot("view", m.builder.types().objectType("Res"), true,
                                     zl::OwnershipKind::BORROW, "res");
    m.fb.emitBorrow(view, m.loadedRes());
    m.fb.emitDrop(m.res);
    m.fb.emitLoad(view); // the borrow still reading a released owner
    const auto report = m.verify();
    require(!report.ok(), "a drop while a local borrow is live must be rejected:\n" + reportText(report));
    require(report.describe().find("borrowed by") != std::string::npos,
            "the rejection must name the borrow:\n" + reportText(report));
}

// Establishing a borrow after the owner was released.
void borrowAfterDropTest() {
    OwnedModule m;
    m.storeRes();
    m.fb.emitDrop(m.res);
    const SlotId view = m.fb.addSlot("view", m.builder.types().objectType("Res"), true,
                                     zl::OwnershipKind::BORROW, "res");
    m.fb.emitBorrow(view, m.loadedRes());
    const auto report = m.verify();
    require(!report.ok(), "a borrow of a dropped owner must be rejected:\n" + reportText(report));
    require(report.describe().find("dropped") != std::string::npos,
            "the rejection must name the drop:\n" + reportText(report));
}

// Only owned storage has a lifetime to end.
void dropOfGcStorageTest() {
    OwnedModule m{zl::OwnershipKind::GC};
    m.storeRes();
    m.fb.emitDrop(m.res);
    const auto report = m.verify();
    require(!report.ok(), "a storage-release drop of gc storage must be rejected:\n" + reportText(report));
    require(report.describe().find("owned") != std::string::npos,
            "the rejection must name the storage contract:\n" + reportText(report));
}

// The two drop spellings are exclusive.
void dropBothFormsTest() {
    OwnedModule m;
    m.storeRes();
    FunctionBuilder& fb = m.fb;
    // Emit the operand form, then set the slot too: both spellings at once.
    fb.emitDrop(m.loadedRes());
    Instruction& drop = fb.function().blocks.back().instructions.back();
    drop.slot = m.res;
    const auto report = m.verify();
    require(!report.ok(), "a drop naming both a slot and a value must be rejected:\n" + reportText(report));
    require(report.describe().find("exclusive") != std::string::npos,
            "the rejection must name the exclusivity rule:\n" + reportText(report));
}

// A drop with neither spelling names nothing.
void dropNamesNothingTest() {
    OwnedModule m;
    m.storeRes();
    FunctionBuilder& fb = m.fb;
    fb.emitDrop(m.loadedRes());
    Instruction& drop = fb.function().blocks.back().instructions.back();
    drop.operands.clear(); // slot stays 0: no storage named at all
    const auto report = m.verify();
    require(!report.ok(), "a drop naming no storage must be rejected:\n" + reportText(report));
    require(report.describe().find("no storage") != std::string::npos,
            "the rejection must say what is missing:\n" + reportText(report));
}

// A value dropped on one path is still dropped at the join: the states union,
// so a later use is rejected even though the other path kept the value alive.
void dropOnOnePathUseAtJoinTest() {
    ModuleBuilder builder("own");
    FunctionBuilder fb = builder.addFunction("Own.go(bool)");
    fb.setReturnType(builder.types().voidType());
    const ParamId flag = fb.addParameter("flag", builder.types().boolType());
    const BlockId entry = fb.addBlock();
    const BlockId thenB = fb.addBlock();
    const BlockId elseB = fb.addBlock();
    const BlockId join = fb.addBlock();

    const SlotId res = fb.addSlot("res", builder.types().objectType("Res"), true,
                                  zl::OwnershipKind::OWNED);
    fb.setCurrentBlock(entry);
    const TempId fresh = fb.emitAlloc("Res", {}, builder.types().objectType("Res"));
    fb.emitStore(res, Operand::temp(fresh, builder.types().objectType("Res")));
    fb.emitBranch(Operand::constant(builder.constantBool(true), builder.types().boolType()),
                  thenB, elseB);

    fb.setCurrentBlock(thenB);
    fb.emitDrop(res); // dropped on this path only
    fb.emitJump(join);

    fb.setCurrentBlock(elseB);
    fb.emitJump(join);

    fb.setCurrentBlock(join);
    fb.emitLoad(res); // dead on one incoming path, therefore dead here
    fb.emitReturn();
    fb.finish();
    builder.setEntryPoint(fb.function().id);
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a use at a join of a value dropped on one path must be rejected:\n" +
                              reportText(report));
    require(report.describe().find("dropped") != std::string::npos,
            "the rejection must name the drop:\n" + reportText(report));
}

// The move twin of the join test: moved on one path, used at the join.
void moveOnOnePathUseAtJoinTest() {
    ModuleBuilder builder("own");
    FunctionBuilder fb = builder.addFunction("Own.go(bool)");
    fb.setReturnType(builder.types().voidType());
    const ParamId flag = fb.addParameter("flag", builder.types().boolType());
    const BlockId entry = fb.addBlock();
    const BlockId thenB = fb.addBlock();
    const BlockId elseB = fb.addBlock();
    const BlockId join = fb.addBlock();

    const SlotId res = fb.addSlot("res", builder.types().objectType("Res"), true,
                                  zl::OwnershipKind::OWNED);
    fb.setCurrentBlock(entry);
    const TempId fresh = fb.emitAlloc("Res", {}, builder.types().objectType("Res"));
    fb.emitStore(res, Operand::temp(fresh, builder.types().objectType("Res")));
    fb.emitBranch(Operand::constant(builder.constantBool(true), builder.types().boolType()),
                  thenB, elseB);

    fb.setCurrentBlock(thenB);
    fb.emitMove(res); // moved on this path only
    fb.emitJump(join);

    fb.setCurrentBlock(elseB);
    fb.emitJump(join);

    fb.setCurrentBlock(join);
    fb.emitLoad(res);
    fb.emitReturn();
    fb.finish();
    builder.setEntryPoint(fb.function().id);
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a use at a join of a value moved on one path must be rejected:\n" +
                              reportText(report));
    require(report.describe().find("moved") != std::string::npos,
            "the rejection must name the move:\n" + reportText(report));
}

// Ending a borrow that was never taken.
void endBorrowWithoutBorrowTest() {
    OwnedModule m;
    const SlotId view = m.fb.addSlot("view", m.builder.types().objectType("Res"), true,
                                     zl::OwnershipKind::BORROW, "res");
    m.fb.emitEndBorrow(view);
    const auto report = m.verify();
    require(!report.ok(), "an end_borrow without a live borrow must be rejected:\n" + reportText(report));
}

// A parameter's value releases exactly once too (the operand drop form).
void doubleDropOfParameterTest() {
    ModuleBuilder builder("own");
    FunctionBuilder fb = builder.addFunction("Own.go(Res)");
    fb.setReturnType(builder.types().voidType());
    const ParamId r = fb.addParameter("r", builder.types().objectType("Res"),
                                      zl::OwnershipKind::OWNED);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitDrop(fb.parameterOperand(r));
    fb.emitDrop(fb.parameterOperand(r));
    fb.emitReturn();
    fb.finish();
    builder.setEntryPoint(fb.function().id);
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a second drop of an owned parameter must be rejected:\n" + reportText(report));
    require(report.describe().find("exactly once") != std::string::npos,
            "the rejection must state the once-only rule:\n" + reportText(report));
}

// ---------------------------------------------------------------------------
// Legal shapes that must stay legal
// ---------------------------------------------------------------------------

// The exit-cleanup shape lowering emits: a function-scoped borrow is still
// live when the owner's storage is released, because in ZL both end at the
// same point - the function exit. The trailing cleanup region (drops followed
// only by the return) is exactly where that release is legal.
void exitReleaseWithLiveBorrowTest() {
    OwnedModule m;
    m.storeRes();
    const SlotId view = m.fb.addSlot("view", m.builder.types().objectType("Res"), true,
                                     zl::OwnershipKind::BORROW, "res");
    m.fb.emitBorrow(view, m.loadedRes());
    m.fb.emitLoad(view); // the borrow's last use
    m.fb.emitDrop(m.res); // trailing cleanup: nothing after but drops and the return
    const auto report = m.verify();
    require(report.ok(), "an exit release while a function-scoped borrow is live verifies:\n" +
                             reportText(report));
}

// Borrow, end the borrow, then move the owner: the normal borrow-then-release
// sequence every `borrow` declaration lowers to.
void borrowEndMoveTest() {
    OwnedModule m;
    m.storeRes();
    const SlotId view = m.fb.addSlot("view", m.builder.types().objectType("Res"), true,
                                     zl::OwnershipKind::BORROW, "res");
    m.fb.emitBorrow(view, m.loadedRes());
    m.fb.emitEndBorrow(view);
    m.fb.emitMove(m.res);
    const auto report = m.verify();
    require(report.ok(), "borrow, end_borrow, then move must verify:\n" + reportText(report));
}

// A move is final, as in the language itself: the checker rejects assigning
// to a moved variable, so a store cannot resurrect the slot here either.
void storeAfterMoveRejectedTest() {
    OwnedModule m;
    m.storeRes();
    m.fb.emitMove(m.res);
    m.storeRes(); // assignment to a moved variable
    const auto report = m.verify();
    require(!report.ok(), "a store to a moved slot must be rejected, as in the checker:\n" +
                              reportText(report));
    require(report.describe().find("after it was moved") != std::string::npos,
            "the rejection must name the move:\n" + reportText(report));
}

// ---------------------------------------------------------------------------
// Pipeline: lowering real owned/move/borrow source must verify as emitted
// ---------------------------------------------------------------------------

void loweredOwnedProgramVerifiesTest() {
    const std::string source = R"(class Pip {
    public string tag
    public func Pip(string t): void {
        this.tag = t
    }
}
class PipeMain {
    static func read(borrow Pip p): string {
        return p.tag
    }
    static func consume(owned Pip p): string {
        return p.tag
    }
    func main(): void {
        owned Pip a = new Pip("one")
        log(PipeMain.read(a))
        log(PipeMain.consume(move a))
        owned Pip b = new Pip("two")
        if true {
            borrow Pip view = b
            log(PipeMain.read(view))
        }
        log(b.tag)
        log(PipeMain.consume(move b))
    }
}
)";

    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "zl_mir_own_tests";
    fs::create_directories(directory);
    const fs::path file = directory / "PipeMain.zl";
    {
        std::ofstream out(file);
        out << source;
    }

    try {
        zl::ModuleLoader loader(file, std::vector<fs::path>{});
        auto program = loader.load();
        zl::TypeChecker checker;
        checker.check(*program, /*requireMain=*/false);
        auto lowered = zl::mir::lowerProgram(*program, checker);
        const auto report = zl::mir::verifyModule(lowered.module);
        require(report.ok(), "lowered owned/move/borrow program must verify as emitted:\n" +
                                 reportText(report));
    } catch (const std::exception& e) {
        require(false, std::string("pipeline fixture raised: ") + e.what());
    }
    fs::remove_all(directory);
}

} // namespace

int main() {
    useAfterDropTest();
    doubleDropTest();
    dropAfterMoveTest();
    dropWhileBorrowedTest();
    borrowAfterDropTest();
    dropOfGcStorageTest();
    dropBothFormsTest();
    dropNamesNothingTest();
    dropOnOnePathUseAtJoinTest();
    moveOnOnePathUseAtJoinTest();
    endBorrowWithoutBorrowTest();
    doubleDropOfParameterTest();
    exitReleaseWithLiveBorrowTest();
    borrowEndMoveTest();
    storeAfterMoveRejectedTest();
    loweredOwnedProgramVerifiesTest();

    if (failures != 0) {
        std::cerr << failures << " mir ownership test(s) FAILED\n";
        return 1;
    }
    std::cout << "mir ownership tests: PASS (16)\n";
    return 0;
}
