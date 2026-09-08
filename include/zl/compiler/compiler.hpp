#pragma once

#include <string>
#include <unordered_map>
#include <cstddef>
#include <vector>

#include "bytecode.hpp"
#include "zl/parser/ast.hpp"
#include "zl/vm/native.hpp"
#include "zl/compiler/compilation_plan.hpp"
#include "zl/compiler/dispatch_table.hpp"
#include "zl/common/dispatch_signature.hpp"

namespace zl {

class Compiler {
public:
    // Walks the AST and returns a finished Chunk, ready to hand to the VM.
    // Searches every class's members for a func named "main" and treats
    // it as the entry point (preferring a class literally named "Main" or
    // "Program" if more than one candidate exists).
    [[nodiscard]] Chunk compile(const Program& program);

private:
    void compileStatement(const AstNode* node);
    void compileExpression(const AstNode* node);

    void compileVarDecl(const VarDecl* node);
    void compileLogStmt(const LogStmt* node);
    void compileIfStmt(const IfStmt* node);
    void compileReturnStmt(const ReturnStmt* node);
    void compileForStmt(const ForStmt* node);
    void compileWhileStmt(const WhileStmt* node);
    void compileRepeatStmt(const RepeatStmt* node);
    void compileBreakStmt(const BreakStmt* node);
    void compileContinueStmt(const ContinueStmt* node);
    void compileTryStmt(const TryStmt* node);
    void compileActiveFinallyCleanup(std::size_t firstIndex);
    void compileThrowStmt(const ThrowStmt* node);
    void compileBlock(const BlockStmt* node);
    void compileExprStmt(const ExprStmt* node);
    void emitOwnedLocalCleanup(std::size_t line);

    void compileLiteral(const Literal* node);
    void compileIdentifier(const Identifier* node);
    void compileUnary(const UnaryExpr* node);
    void compileAwait(const AwaitExpr* node);
    void compileBinary(const BinaryExpr* node);
    void compileCall(const CallExpr* node);
    void compileAssign(const AssignExpr* node);
    void compileMove(const MoveExpr* node);
    void compileCollectionLiteral(const CollectionLiteral* node);
    void compileNewExpr(const NewExpr* node);
    void compileDataLiteralExpr(const DataLiteralExpr* node);
    void compileFieldAccess(const FieldAccessExpr* node);
    void compileIndexAccess(const IndexAccessExpr* node);
    void compileFieldAssign(const FieldAssignExpr* node);
    void compileMethodCall(const MethodCallExpr* node);
    void compileThisExpr(const ThisExpr* node);
    void compileSuperCallExpr(const SuperCallExpr* node);
    void compileSuperMethodCallExpr(const SuperMethodCallExpr* node);
    void compileLambdaExpr(const LambdaExpr* node);
    void compileMatchExpr(const MatchExpr* node);

    std::size_t emit(OpCode op, std::size_t operand = 0, std::size_t line = 0, std::size_t operand2 = 0, std::size_t operand3 = 0);
    void patchJump(std::size_t jumpInstructionIndex);

    // break/continue need to know where "the end of the current loop" and
    // "the next iteration" are, but those addresses aren't known until AFTER
    // the loop body is compiled. So: push a fresh context before compiling a
    // loop's body, let compileBreakStmt/compileContinueStmt append placeholder
    // jump indices to it as they're encountered, then patch them all once the
    // loop's end/continue-target addresses are known. Nested loops "break"
    // out of the innermost one because this is a stack, not a single slot.
    struct LoopContext {
        std::vector<std::size_t> breakJumps;
        std::vector<std::size_t> continueJumps;
        // Number of active try-handlers when this loop was entered. A
        // break/continue must pop only handlers created INSIDE the loop;
        // handlers belonging to an enclosing try must remain active.
        std::size_t handlerDepth{0};
        // Number of active finally blocks when this loop was entered.
        // Break/continue must run only finally blocks introduced inside the loop.
        std::size_t finallyDepth{0};
    };
    std::vector<LoopContext> loopStack_;

    // Number of try-handlers lexically active while compiling the current
    // func body. Used to emit handler cleanup for break/continue.
    std::size_t activeHandlerDepth_{0};
    std::size_t nextHandlerGroupId_{1};
    std::vector<const BlockStmt*> activeFinallyBlocks_;

    std::vector<std::string> activeOwnedLocalNames_;

    // for-loops need hidden per-loop temp variable names (to hold the range's
    // end/step) that won't collide across nested loops - e.g. two nested
    // `for i in ...` `for j in ...` each need their own end/step slot.
    std::size_t loopCounter_{0};

    // The owning class of whichever FunctionDecl body is currently being
    // compiled (see compile()'s pass 2). Used to resolve bare same-class
    // calls like `foo(x)` (implicit self-calls, as opposed to `obj.foo(x)`
    // or `Namespace.foo(x)`) against the compile-time func index.
    std::string currentClassName_;

    // currentClassName_'s `extends` parent, or empty if it has none -
    // mirrors classParents_ for the class currently being compiled, so
    // compileSuperCallExpr/compileSuperMethodCallExpr don't need to re-look
    // it up on every use.
    std::string currentParentClassName_;
    std::unordered_map<DispatchSignature, std::size_t, DispatchSignatureHash> methodSlots_;

    // className -> its `extends` parent (or absent, if none), used during
    // compilation for inherited method and super resolution.
    std::unordered_map<std::string, std::string> classParents_;

    struct FunctionKey {
        std::string ownerClassName;
        DispatchSignature signature;

        bool operator==(const FunctionKey& other) const {
            return ownerClassName == other.ownerClassName && signature == other.signature;
        }
    };

    struct FunctionKeyHash {
        std::size_t operator()(const FunctionKey& key) const noexcept {
            std::size_t h = std::hash<std::string>{}(key.ownerClassName);
            h ^= DispatchSignatureHash{}(key.signature) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<FunctionKey, std::size_t, FunctionKeyHash> functionIndices_;

    // Compile-time func identity. Calls resolve to a FunctionInfo index
    // once, then bytecode carries that index directly. Inherited self/super
    // calls walk the already-known class-parent chain, but never scan the
    // complete func table or construct a qualified func-name string.
    [[nodiscard]] std::size_t resolveFunctionIndex(const std::string& className,
                                                    const DispatchSignature& signature) const;

    // Rebuilds, straight from a FunctionDecl's own declared parameter list,
    // EXACTLY the same signature-suffix string the type checker computes
    // from its already-resolved ZlType list (TypeChecker::paramTypesSuffix)
    // - e.g. "(int,string)". This has to match byte-for-byte, or a call
    // site's resolvedDispatch (built by the type checker) directly identifies
    // the overload that the compiler registered in chunk_.functions. The compiler doesn't know about
    // ZlType/TypeChecker at all, so both sides independently derive the same
    // structured dispatch signature from the AST.
    [[nodiscard]] static DispatchSignature dispatchSignature(const FunctionDecl& func, const std::vector<std::string>& genericTypeParams = {});
    [[nodiscard]] std::size_t methodSlot(const DispatchSignature& dispatchKey) const;

    // Gives every lambda body its own unique, unwritable-by-user
    // chunk_.functions entry name (e.g. "$lambda0", "$lambda1", ...) - a
    // lambda has no source-level name to qualify with, and names
    // only need to be unique, not meaningful (they're never looked up by
    // string - OpCode::MakeClosure references the entry by INDEX, computed
    // at compile time - so the exact spelling here is only ever seen in a
    // debugger/disassembly).
    std::size_t lambdaCounter_{0};

    Chunk chunk_;
};

} // namespace zl
