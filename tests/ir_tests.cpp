// Legacy IR regressions: the flow verifier's acceptance and rejection of
// malformed modules (block graph errors, value lifecycle errors, ownership
// violations, branch joins), and the optimizer's guaranteed-preserving
// transforms (constant folding, dead-branch elimination, dead-block removal,
// ownership instructions untouched, result still verifies).
#include "zl/compiler/ir.hpp"
#include "zl/compiler/ir_optimizer.hpp"
#include "zl/lexer/token.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace zl::ir;
using zl::OwnershipKind;

const std::string PLUS = std::to_string(static_cast<int>(zl::TokenType::PLUS));

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "IR regression: " << message << '\n';
    ++failures;
}

// ---------------------------------------------------------------------------
// Fixture builders
// ---------------------------------------------------------------------------

Function oneBlockFunction(const std::string& name = "f") {
    Function fn;
    fn.name = name;
    fn.returnType = "int";
    BasicBlock block;
    block.id = 0;
    fn.blocks.push_back(std::move(block));
    return fn;
}

Instruction define(ValueId result, OwnershipKind ownership = OwnershipKind::GC) {
    return Instruction{Opcode::DefineLocal, result, 0, 0, {}, 0, ownership};
}
Instruction constant(ValueId result, const std::string& text) {
    return Instruction{Opcode::Const, result, 0, 0, text, 0, OwnershipKind::GC};
}
Instruction ret(ValueId operand) {
    return Instruction{Opcode::Return, 0, operand, 0, {}, 0, OwnershipKind::GC};
}

// A minimal valid module: define two params, return their sum.
Module validModule() {
    Module module;
    Function fn = oneBlockFunction();
    fn.blocks[0].instructions.push_back(define(1));
    fn.blocks[0].instructions.push_back(define(2));
    fn.blocks[0].instructions.push_back(Instruction{Opcode::Binary, 3, 1, 2, PLUS, 0, OwnershipKind::GC});
    fn.blocks[0].instructions.push_back(ret(3));
    module.functions.push_back(std::move(fn));
    return module;
}

// Runs verify and asserts the module is rejected with `needle` in the error.
void expectRejected(const Module& module, const std::string& label, const std::string& needle) {
    std::string error;
    require(!module.functions.empty(), label + " fixture must contain the function");
    require(!verify(module, &error), label + " must be rejected");
    require(error.find(needle) != std::string::npos,
            label + " rejected with the right diagnostic (got: " + error + ")");
}

void expectAccepted(const Module& module, const std::string& label) {
    std::string error;
    require(verify(module, &error), label + " must verify: " + error);
}

// ---------------------------------------------------------------------------
// Verifier
// ---------------------------------------------------------------------------

void testVerifierAccepts() {
    expectAccepted(validModule(), "a straight-line function");

    // The shape the lowerer actually emits: the result slot is defined before
    // the branch, each arm assigns it a path-specific value, and the join
    // reads the slot - legal because the slot is live on every path.
    Module module;
    Function fn = oneBlockFunction("branchy");
    BasicBlock entry;
    entry.id = 0;
    entry.instructions.push_back(define(1));
    entry.instructions.push_back(define(2)); // the result slot
    entry.successors = {1, 2};
    entry.instructions.push_back(Instruction{Opcode::Branch, 0, 1, 0, {}, 0, OwnershipKind::GC});
    BasicBlock thenBlock;
    thenBlock.id = 1;
    thenBlock.instructions.push_back(constant(5, "5"));
    thenBlock.instructions.push_back(Instruction{Opcode::AssignLocal, 2, 5, 0, {}, 0, OwnershipKind::GC});
    thenBlock.successors = {3};
    BasicBlock elseBlock;
    elseBlock.id = 2;
    elseBlock.instructions.push_back(constant(6, "6"));
    elseBlock.instructions.push_back(Instruction{Opcode::AssignLocal, 2, 6, 0, {}, 0, OwnershipKind::GC});
    elseBlock.successors = {3};
    BasicBlock join;
    join.id = 3;
    join.instructions.push_back(ret(2));
    fn.blocks = {std::move(entry), std::move(thenBlock), std::move(elseBlock), std::move(join)};
    module.functions.push_back(std::move(fn));
    expectAccepted(module, "a branch whose join reads a slot live on every path");
}

void testVerifierRejectsMalformedModules() {
    {
        Module noBlocks;
        Function fn;
        fn.name = "empty";
        noBlocks.functions.push_back(std::move(fn));
        expectRejected(noBlocks, "a function without blocks", "no basic blocks");
    }

    {
            Module duplicateBlock;
        Function fn = oneBlockFunction();
        BasicBlock twin = fn.blocks[0];
        twin.id = 0; // same id as the first block
        fn.blocks.push_back(std::move(twin));
        duplicateBlock.functions.push_back(std::move(fn));
        expectRejected(duplicateBlock, "duplicate block ids", "duplicate IR block id");
    }

    {
        Module badSuccessor;
        Function fn = oneBlockFunction();
        fn.blocks[0].successors = {99};
        badSuccessor.functions.push_back(std::move(fn));
        expectRejected(badSuccessor, "a successor to a missing block", "invalid IR block successor");
    }

    {
        Module duplicateDefinition;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(define(5));
        fn.blocks[0].instructions.push_back(define(5));
        duplicateDefinition.functions.push_back(std::move(fn));
        expectRejected(duplicateDefinition, "defining one value twice", "duplicate IR value definition");
    }

    {
        Module useBeforeDefine;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(Instruction{Opcode::LoadLocal, 9, 42, 0, {}, 0, OwnershipKind::GC});
        useBeforeDefine.functions.push_back(std::move(fn));
        expectRejected(useBeforeDefine, "loading an undefined local", "not live");
    }

    {
        Module doubleDefine;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(define(1));
        fn.blocks[0].instructions.push_back(define(1));
        doubleDefine.functions.push_back(std::move(fn));
        expectRejected(doubleDefine, "redefining a live local", "duplicate IR value definition");
    }

    {
        Module missingResult;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(constant(0, "1"));
        missingResult.functions.push_back(std::move(fn));
        expectRejected(missingResult, "a Const without a destination", "requires a result");
    }

    {
        Module callWithoutTarget;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(Instruction{Opcode::Call, 3, 0, 0, {}, 0, OwnershipKind::GC});
        callWithoutTarget.functions.push_back(std::move(fn));
        expectRejected(callWithoutTarget, "a Call without a symbol", "target symbol");
    }

    {
        Module callWithGhostArgument;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(Instruction{Opcode::Call, 3, 0, 0, {}, 0, OwnershipKind::GC});
        fn.blocks[0].instructions.back().symbol = "other";
        fn.blocks[0].instructions.back().operands = {77};
        callWithGhostArgument.functions.push_back(std::move(fn));
        expectRejected(callWithGhostArgument, "a Call argument nobody produced", "unavailable argument");
    }
}

void testVerifierOwnershipRules() {
    {
        Module useAfterMove;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(define(1, OwnershipKind::OWNED));
        fn.blocks[0].instructions.push_back(Instruction{Opcode::MoveLocal, 2, 1, 0, {}, 0, OwnershipKind::OWNED});
        fn.blocks[0].instructions.push_back(Instruction{Opcode::LoadLocal, 3, 1, 0, {}, 0, OwnershipKind::GC});
        useAfterMove.functions.push_back(std::move(fn));
        expectRejected(useAfterMove, "loading a moved local", "uses a moved local");
    }

    {
        Module moveNotOwned;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(define(1, OwnershipKind::GC));
        fn.blocks[0].instructions.push_back(Instruction{Opcode::MoveLocal, 2, 1, 0, {}, 0, OwnershipKind::GC});
        moveNotOwned.functions.push_back(std::move(fn));
        expectRejected(moveNotOwned, "moving a GC local", "owned source");
    }

    {
        Module moveWhileBorrowed;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(define(1, OwnershipKind::OWNED));
        fn.blocks[0].instructions.push_back(Instruction{Opcode::BorrowLocal, 2, 1, 0, {}, 0, OwnershipKind::BORROW});
        fn.blocks[0].instructions.push_back(Instruction{Opcode::MoveLocal, 3, 1, 0, {}, 0, OwnershipKind::OWNED});
        moveWhileBorrowed.functions.push_back(std::move(fn));
        expectRejected(moveWhileBorrowed, "moving a local that is actively borrowed", "actively borrowed");
    }

    {
        Module endBorrowNotBorrow;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(define(1, OwnershipKind::GC));
        fn.blocks[0].instructions.push_back(Instruction{Opcode::EndBorrow, 0, 1, 0, {}, 0, OwnershipKind::GC});
        endBorrowNotBorrow.functions.push_back(std::move(fn));
        expectRejected(endBorrowNotBorrow, "ending a non-borrow local", "borrow local");
    }

    {
        Module borrowIntoNonBorrow;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(define(1, OwnershipKind::OWNED));
        fn.blocks[0].instructions.push_back(Instruction{Opcode::BorrowLocal, 2, 1, 0, {}, 0, OwnershipKind::BORROW});
        fn.blocks[0].instructions.push_back(define(3, OwnershipKind::GC));
        fn.blocks[0].instructions.push_back(Instruction{Opcode::AssignLocal, 3, 2, 0, {}, 0, OwnershipKind::GC});
        borrowIntoNonBorrow.functions.push_back(std::move(fn));
        expectRejected(borrowIntoNonBorrow, "assigning a borrow into a non-borrow local", "non-borrow local");
    }

    {
        Module returnBorrow;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(define(1, OwnershipKind::OWNED));
        fn.blocks[0].instructions.push_back(Instruction{Opcode::BorrowLocal, 2, 1, 0, {}, 0, OwnershipKind::BORROW});
        fn.blocks[0].instructions.push_back(ret(2));
        returnBorrow.functions.push_back(std::move(fn));
        expectRejected(returnBorrow, "returning a borrowed local", "borrowed local");
    }

    // A borrow that is ended before the move is fine: the legal sequence
    // verifies, which pins the rejections above to the actual violation.
    {
        Module legalSequence;
        Function fn = oneBlockFunction();
        fn.blocks[0].instructions.push_back(define(1, OwnershipKind::OWNED));
        fn.blocks[0].instructions.push_back(Instruction{Opcode::BorrowLocal, 2, 1, 0, {}, 0, OwnershipKind::BORROW});
        fn.blocks[0].instructions.push_back(Instruction{Opcode::EndBorrow, 0, 2, 0, {}, 0, OwnershipKind::BORROW});
        fn.blocks[0].instructions.push_back(Instruction{Opcode::MoveLocal, 3, 1, 0, {}, 0, OwnershipKind::OWNED});
        fn.blocks[0].instructions.push_back(ret(3));
        legalSequence.functions.push_back(std::move(fn));
        expectAccepted(legalSequence, "borrow, end, then move the owner");
    }
}

void testVerifierJoinSemantics() {
    // A value defined on only one path of a branch is not live at the join.
    {
        Module join;
        Function fn = oneBlockFunction("joiny");
        BasicBlock entry;
        entry.id = 0;
        entry.successors = {1, 2};
        entry.instructions.push_back(Instruction{Opcode::Branch, 0, 1, 0, {}, 0, OwnershipKind::GC});
        BasicBlock thenBlock;
        thenBlock.id = 1;
        thenBlock.instructions.push_back(define(7));
        thenBlock.successors = {3};
        BasicBlock elseBlock;
        elseBlock.id = 2;
        elseBlock.successors = {3};
        BasicBlock joinBlock;
        joinBlock.id = 3;
        joinBlock.instructions.push_back(Instruction{Opcode::LoadLocal, 8, 7, 0, {}, 0, OwnershipKind::GC});
        fn.blocks = {std::move(entry), std::move(thenBlock), std::move(elseBlock), std::move(joinBlock)};
        Module module;
        module.functions.push_back(std::move(fn));
        expectRejected(module, "a value defined on one path only", "not live");
    }

    // A borrow that is live on every path into the join keeps its owner
    // moved-unsafe after the join: the verifier stays conservative and
    // rejects the move even though no path ends the borrow.
    {
        Module conflict;
        Function fn = oneBlockFunction("conflict");
        BasicBlock entry;
        entry.id = 0;
        entry.instructions.push_back(define(1, OwnershipKind::OWNED));
        entry.instructions.push_back(define(2, OwnershipKind::OWNED));
        entry.instructions.push_back(Instruction{Opcode::BorrowLocal, 3, 1, 0, {}, 0, OwnershipKind::BORROW});
        entry.successors = {1, 2};
        entry.instructions.push_back(Instruction{Opcode::Branch, 0, 1, 0, {}, 0, OwnershipKind::GC});
        BasicBlock thenBlock;
        thenBlock.id = 1;
        thenBlock.successors = {3};
        BasicBlock elseBlock;
        elseBlock.id = 2;
        elseBlock.successors = {3};
        BasicBlock joinBlock;
        joinBlock.id = 3;
        joinBlock.instructions.push_back(Instruction{Opcode::MoveLocal, 5, 1, 0, {}, 0, OwnershipKind::OWNED});
        fn.blocks = {std::move(entry), std::move(thenBlock), std::move(elseBlock), std::move(joinBlock)};
        Module module2;
        module2.functions.push_back(std::move(fn));
        expectRejected(module2, "moving an owner whose borrow is live at the join",
                       "actively borrowed");
    }
}

// ---------------------------------------------------------------------------
// Optimizer
// ---------------------------------------------------------------------------

Module foldingModule() {
    Module module;
    Function fn = oneBlockFunction("fold");
    fn.blocks[0].instructions.push_back(constant(1, "1"));
    fn.blocks[0].instructions.push_back(constant(2, "2"));
    fn.blocks[0].instructions.push_back(Instruction{Opcode::Binary, 3, 1, 2, PLUS, 0, OwnershipKind::GC});
    fn.blocks[0].instructions.push_back(ret(3));
    module.functions.push_back(std::move(fn));
    return module;
}

void testOptimizerFoldsConstants() {
    Module module = foldingModule();
    Module optimized = optimize(module);
    expectAccepted(optimized, "the optimized module still verifies");
    require(optimized.functions.size() == 1 && !optimized.functions[0].blocks.empty(),
            "optimization keeps the function");
    bool folded = false;
    for (const auto& ins : optimized.functions[0].blocks[0].instructions) {
        if (ins.result == 3 && ins.opcode == Opcode::Const && ins.symbol == "3") folded = true;
    }
    require(folded, "1 + 2 folds to a 3 constant");
}

void testOptimizerEliminatesDeadBranchAndBlock() {
    Module module;
    Function fn = oneBlockFunction("branch");
    BasicBlock entry;
    entry.id = 0;
    entry.instructions.push_back(constant(1, "true"));
    entry.instructions.push_back(Instruction{Opcode::Branch, 0, 1, 0, {}, 0, OwnershipKind::GC});
    entry.successors = {1, 2};
    BasicBlock taken;
    taken.id = 1;
    taken.instructions.push_back(ret(0));
    BasicBlock dead;
    dead.id = 2;
    dead.instructions.push_back(ret(0));
    fn.blocks = {std::move(entry), std::move(taken), std::move(dead)};
    module.functions.push_back(std::move(fn));

    Module optimized = optimize(module);
    expectAccepted(optimized, "the branch-optimized module still verifies");
    bool deadGone = true;
    bool branchGone = true;
    bool edgeToDead = false;
    for (const auto& block : optimized.functions[0].blocks) {
        if (block.id == 2) deadGone = false;
        for (const auto succ : block.successors) if (succ == 2) edgeToDead = true;
        for (const auto& ins : block.instructions) if (ins.opcode == Opcode::Branch) branchGone = false;
    }
    require(deadGone && !edgeToDead, "the dead block is removed, and no edge references it");
    require(branchGone, "a constant branch never survives as a conditional branch");
}

void testOptimizerLeavesOwnershipAlone() {
    Module module;
    Function fn = oneBlockFunction("owner");
    fn.blocks[0].instructions.push_back(define(1, OwnershipKind::OWNED));
    fn.blocks[0].instructions.push_back(Instruction{Opcode::MoveLocal, 2, 1, 0, {}, 0, OwnershipKind::OWNED});
    fn.blocks[0].instructions.push_back(ret(2));
    module.functions.push_back(std::move(fn));

    Module optimized = optimize(module);
    expectAccepted(optimized, "the ownership module still verifies");
    int defines = 0, moves = 0;
    for (const auto& ins : optimized.functions[0].blocks[0].instructions) {
        if (ins.opcode == Opcode::DefineLocal && ins.ownership == OwnershipKind::OWNED) ++defines;
        if (ins.opcode == Opcode::MoveLocal) ++moves;
    }
    require(defines == 1 && moves == 1, "ownership/borrow instructions are never rewritten");
}

} // namespace

int main() {
    testVerifierAccepts();
    testVerifierRejectsMalformedModules();
    testVerifierOwnershipRules();
    testVerifierJoinSemantics();
    testOptimizerFoldsConstants();
    testOptimizerEliminatesDeadBranchAndBlock();
    testOptimizerLeavesOwnershipAlone();

    if (failures != 0) {
        std::cerr << failures << " IR regression(s) failed\n";
        return 1;
    }
    std::cout << "all IR regressions passed\n";
    return 0;
}
