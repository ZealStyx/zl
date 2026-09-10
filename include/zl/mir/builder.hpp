#pragma once

#include <string>
#include <vector>

#include "zl/mir/function.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// Module and function builders
// ---------------------------------------------------------------------------
//
// The MIR has invariants that are easy to violate by hand: block ids must be
// dense and positional, every block must end in exactly one terminator, the
// edge list must match the terminators, function ids must match positions, and
// every temp must be defined exactly once.
//
// The builder owns those invariants so a lowering pass cannot get them wrong by
// accident. It is the only supported way to construct MIR; hand-editing a
// Module is allowed for tests that deliberately build *invalid* MIR, which is
// exactly what the verifier's negative tests need.

class FunctionBuilder;

class ModuleBuilder {
public:
    explicit ModuleBuilder(std::string name = {});

    [[nodiscard]] TypeArena& types() { return module_.types; }
    [[nodiscard]] const TypeArena& types() const { return module_.types; }

    // Appends a function and returns a builder bound to it. The builder holds a
    // reference into the module's function vector, so it must not outlive the
    // next call that appends another function. Declare every function first and
    // only then lower bodies through functionBuilder(), which is safe to hold
    // while new functions are appended.
    [[nodiscard]] FunctionBuilder addFunction(const std::string& qualifiedName);

    // A builder bound to an already-declared function.
    [[nodiscard]] FunctionBuilder functionBuilder(FunctionId id);
    [[nodiscard]] FunctionId functionCount() const {
        return static_cast<FunctionId>(module_.functions.size());
    }
    // Id of the function with this qualified name, or kNoFunction.
    [[nodiscard]] FunctionId findFunction(const std::string& qualifiedName) const;

    [[nodiscard]] StaticId addStatic(const std::string& className, const std::string& fieldName,
                                     std::uint32_t type, FunctionId initializer = kNoFunction,
                                     SourceLocation location = {});
    [[nodiscard]] ClassLayout& addClassLayout(const std::string& name);
    // An interface declaration's hierarchy entry. Interfaces are not classes
    // (see InterfaceInfo), so this is a separate table rather than a layout.
    [[nodiscard]] InterfaceInfo& addInterface(const std::string& name);
    [[nodiscard]] ConstId addConstant(const Constant& value);

    // Constant constructors, deduplicated.
    [[nodiscard]] ConstId constantInt(std::int64_t value);
    [[nodiscard]] ConstId constantDouble(double value);
    [[nodiscard]] ConstId constantBool(bool value);
    [[nodiscard]] ConstId constantString(std::string value);
    [[nodiscard]] ConstId constantNil();
    // An enum member such as `Color.RED`: `enumTypeName` is "Color", `member`
    // is "RED".
    [[nodiscard]] ConstId constantEnumMember(std::string enumTypeName, std::string member);

    void setEntryPoint(FunctionId id) { module_.entryPoint = id; }

    // Releases the module. Call once, at the end.
    [[nodiscard]] Module take();
    [[nodiscard]] const Module& module() const { return module_; }
    [[nodiscard]] Module& mutableModule() { return module_; }

private:
    Module module_;
};

// Builds one function: slots, blocks, instructions, terminators.
//
// The current block is explicit - `setCurrentBlock` - rather than implicit, so
// lowering a control-flow construct reads as "make these blocks, then wire
// them", with no hidden cursor state to lose track of.
class FunctionBuilder {
public:
    FunctionBuilder(Module& module, Function& function);

    [[nodiscard]] Module& module() { return module_; }
    [[nodiscard]] Function& function() { return function_; }
    [[nodiscard]] TypeArena& types() { return module_.types; }

    // --- signature --------------------------------------------------------
    [[nodiscard]] ParamId addParameter(const std::string& name, std::uint32_t type,
                                       zl::OwnershipKind ownership = zl::OwnershipKind::GC,
                                       std::string borrowSource = {}, SourceLocation location = {});
    // The operand that refers to a parameter.
    [[nodiscard]] Operand parameterOperand(ParamId id) const;

    void setReturnType(std::uint32_t type, zl::OwnershipKind ownership = zl::OwnershipKind::GC);
    void setAsync(bool value) { function_.isAsync = value; }
    void setNative(bool value) { function_.isNative = value; }
    void setConstructor(bool value) { function_.isConstructor = value; }
    void setStatic(bool value) { function_.isStatic = value; }
    void setLambda(bool value) { function_.isLambda = value; }
    void setOperator(bool value) { function_.isOperator = value; }
    void setAccess(MemberAccess value) { function_.access = value; }
    void setGenericTemplate(std::vector<std::string> typeParameters);
    void setGenericArguments(std::vector<std::uint32_t> arguments);
    void setExceptionBehavior(ExceptionBehavior behavior) { function_.exceptionBehavior = behavior; }
    void setLocation(SourceLocation location) { function_.location = std::move(location); }
    void addCapture(const std::string& name, std::uint32_t type, bool usesThis = false);
    void markIncomplete(std::string reason);

    // --- slots ------------------------------------------------------------
    // Slot ids are 1-based.
    [[nodiscard]] SlotId addSlot(const std::string& name, std::uint32_t type, bool isMutable = true,
                                 zl::OwnershipKind ownership = zl::OwnershipKind::GC,
                                 std::string borrowSource = {}, SourceLocation location = {});

    // --- blocks -----------------------------------------------------------
    // Block ids are 1-based and match position. The first block created is the
    // entry block.
    [[nodiscard]] BlockId addBlock(BlockKind kind = BlockKind::Normal, SourceLocation location = {});
    void setCurrentBlock(BlockId id) { current_ = id; }
    [[nodiscard]] BlockId currentBlock() const { return current_; }

    // --- block parameters (phi nodes) ------------------------------------
    //
    // A block parameter is added to `block` and gets the next function-wide id.
    // Nothing supplies it until the incoming edges are given arguments, so a
    // caller adding one must also call `setEdgeArguments` (or build the
    // arguments through `emitJumpWithArguments` / `emitBranchWithArguments`) for
    // every predecessor - otherwise the verifier reports the missing argument,
    // which is exactly the failure mode worth catching.
    [[nodiscard]] BlockParamId addBlockParameter(BlockId block, const std::string& name,
                                                 std::uint32_t type, SourceLocation location = {});
    // Replaces the arguments handed to successor `successorIndex` of `from`.
    // The index matches the order `Terminator::successors()` reports.
    void setEdgeArguments(BlockId from, std::size_t successorIndex, std::vector<Operand> arguments);
    // Terminators that carry block-parameter arguments, in successor order.
    void emitJumpWithArguments(BlockId target, std::vector<Operand> arguments,
                               SourceLocation location = {});
    void emitBranchWithArguments(Operand condition, BlockId thenBlock, std::vector<Operand> thenArguments,
                                 BlockId elseBlock, std::vector<Operand> elseArguments,
                                 SourceLocation location = {});
    [[nodiscard]] BasicBlock& block(BlockId id) { return function_.blocks[static_cast<std::size_t>(id - 1)]; }

    // Installs a handler on the *current* block, innermost last: the handler
    // added most recently is searched first, matching nested try/catch.
    void pushExceptionHandler(std::uint32_t catchType, BlockId target, SlotId catchSlot = 0);

    // --- instructions -----------------------------------------------------
    // Each emitter appends to the current block and returns the defined temp,
    // or kNoTemp for an instruction with no result. `resultType` must be
    // supplied for value-producing instructions; the verifier rejects a mismatch
    // between the declared and the computed type.

    [[nodiscard]] TempId emitNop(SourceLocation location = {});

    // Binary/unary operators.
    [[nodiscard]] TempId emitBinary(Opcode opcode, Operand left, Operand right, std::uint32_t resultType,
                                    SourceLocation location = {});
    [[nodiscard]] TempId emitUnary(Opcode opcode, Operand operand, std::uint32_t resultType,
                                   SourceLocation location = {});

    [[nodiscard]] TempId emitWiden(Operand operand, SourceLocation location = {});
    [[nodiscard]] TempId emitRefine(Operand operand, std::uint32_t resultType, SourceLocation location = {});
    [[nodiscard]] TempId emitTypeTest(Operand operand, std::uint32_t testedType, SourceLocation location = {});
    [[nodiscard]] TempId emitNullCheck(Operand operand, SourceLocation location = {});
    [[nodiscard]] TempId emitIsNull(Operand operand, SourceLocation location = {});

    [[nodiscard]] TempId emitLoad(SlotId slot, SourceLocation location = {});
    void emitStore(SlotId slot, Operand value, SourceLocation location = {});

    [[nodiscard]] TempId emitFieldLoad(Operand base, const std::string& fieldName, std::uint32_t resultType,
                                       SourceLocation location = {});
    void emitFieldStore(Operand base, const std::string& fieldName, Operand value, SourceLocation location = {});
    [[nodiscard]] TempId emitIndexLoad(Operand base, Operand index, std::uint32_t resultType,
                                       SourceLocation location = {});
    void emitIndexStore(Operand base, Operand index, Operand value, SourceLocation location = {});
    [[nodiscard]] TempId emitStaticLoad(const std::string& className, const std::string& fieldName,
                                        std::uint32_t resultType, SourceLocation location = {});
    void emitStaticStore(const std::string& className, const std::string& fieldName, Operand value,
                         SourceLocation location = {});

    [[nodiscard]] TempId emitAlloc(const std::string& className, std::vector<std::uint32_t> typeArguments,
                                   std::uint32_t resultType, SourceLocation location = {});
    [[nodiscard]] TempId emitNewCollection(std::uint32_t resultType, SourceLocation location = {});

    // Calls. `resultType` may be 0 for a void callee, in which case no temp is
    // defined and kNoTemp is returned.
    //
    // `typeArguments` is the instantiation this call site selected when the
    // callee is a generic template: one TypeId per entry in the callee's
    // `typeParameters`, in order. A MIR function stays a template - it is never
    // duplicated per instantiation - so the call is the only place that records
    // which T the arguments were checked against. Leave it empty for a
    // non-generic callee.
    [[nodiscard]] TempId emitCall(FunctionId callee, std::vector<Operand> arguments, std::uint32_t resultType,
                                  SourceLocation location = {}, std::vector<std::uint32_t> typeArguments = {});
    [[nodiscard]] TempId emitInvokeMethod(Operand receiver, const std::string& className,
                                          const std::string& methodName, std::vector<Operand> arguments,
                                          std::uint32_t resultType, SourceLocation location = {},
                                          std::vector<std::uint32_t> typeArguments = {});
    [[nodiscard]] TempId emitInvokeSuper(Operand receiver, FunctionId target, std::vector<Operand> arguments,
                                         std::uint32_t resultType, SourceLocation location = {},
                                         std::vector<std::uint32_t> typeArguments = {});
    [[nodiscard]] TempId emitInvokeStatic(FunctionId target, std::vector<Operand> arguments,
                                          std::uint32_t resultType, SourceLocation location = {},
                                          std::vector<std::uint32_t> typeArguments = {});
    [[nodiscard]] TempId emitCallIndirect(Operand callee, std::vector<Operand> arguments,
                                          std::uint32_t resultType, SourceLocation location = {});
    [[nodiscard]] TempId emitCallNative(const std::string& qualifiedName, std::int32_t nativeId,
                                        std::vector<Operand> arguments, std::uint32_t resultType,
                                        bool isAsync = false, SourceLocation location = {});
    [[nodiscard]] TempId emitMakeClosure(FunctionId body, std::vector<Operand> captures,
                                         std::uint32_t resultType, SourceLocation location = {});

    [[nodiscard]] TempId emitAwait(Operand task, std::uint32_t resultType, SourceLocation location = {});
    [[nodiscard]] TempId emitTaskCreate(Operand value, std::uint32_t resultType, SourceLocation location = {});

    [[nodiscard]] TempId emitMove(SlotId slot, SourceLocation location = {});
    void emitBorrow(SlotId borrowSlot, Operand owner, SourceLocation location = {});
    void emitEndBorrow(SlotId borrowSlot, SourceLocation location = {});
    void emitDrop(Operand value, SourceLocation location = {});

    [[nodiscard]] TempId emitRangeInBounds(Operand current, Operand end, Operand step,
                                           SourceLocation location = {});
    void emitLog(Operand value, SourceLocation location = {});

    // --- terminators ------------------------------------------------------
    void emitReturn(Operand value = Operand::none(), SourceLocation location = {});
    void emitJump(BlockId target, SourceLocation location = {});
    void emitBranch(Operand condition, BlockId thenBlock, BlockId elseBlock, SourceLocation location = {});
    void emitSwitch(Operand subject, std::vector<SwitchTarget> cases, BlockId defaultBlock,
                    SourceLocation location = {});
    void emitThrow(Operand value, SourceLocation location = {});
    void emitUnreachable(SourceLocation location = {});

    // Recomputes the function's control-flow edge list. Called automatically by
    // finish(); exposed for callers that mutate blocks after building.
    void rebuildEdges() { function_.rebuildEdges(); }

    // Finalises the function: rebuilds edges and clears the builder's notion of
    // a current block so a stray emit is a crash in a debug build rather than
    // silently appended code.
    void finish();

private:
    [[nodiscard]] Instruction& append(Opcode opcode, SourceLocation location);
    [[nodiscard]] TempId nextTemp() { return nextTemp_++; }
    void setResult(Instruction& instruction, std::uint32_t resultType);

    Module& module_;
    Function& function_;
    BlockId current_{kNoBlock};
    TempId nextTemp_{1};
};

} // namespace zl::mir
