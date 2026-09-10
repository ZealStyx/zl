#include "zl/mir/builder.hpp"

#include <cassert>
#include <utility>

#include "zl/mir/analysis.hpp"

namespace zl::mir {

ModuleBuilder::ModuleBuilder(std::string name) {
    module_.name = std::move(name);
}

FunctionBuilder ModuleBuilder::addFunction(const std::string& qualifiedName) {
    Function function;
    function.id = static_cast<FunctionId>(module_.functions.size() + 1);
    function.name = qualifiedName;
    // Split the qualified name into owner and simple name so backends can key
    // dispatch on either without re-parsing.
    const auto dot = qualifiedName.rfind('.');
    if (dot == std::string::npos) {
        function.simpleName = qualifiedName;
    } else {
        function.ownerClass = qualifiedName.substr(0, dot);
        function.simpleName = qualifiedName.substr(dot + 1);
    }
    function.returnType = module_.types.voidType();
    module_.functions.push_back(std::move(function));
    return FunctionBuilder(module_, module_.functions.back());
}

FunctionBuilder ModuleBuilder::functionBuilder(FunctionId id) {
    Function* function = module_.function(id);
    assert(function && "functionBuilder called with an unknown function id");
    return FunctionBuilder(module_, *function);
}

FunctionId ModuleBuilder::findFunction(const std::string& qualifiedName) const {
    for (const auto& function : module_.functions) {
        if (function.name == qualifiedName) return function.id;
    }
    return kNoFunction;
}

StaticId ModuleBuilder::addStatic(const std::string& className, const std::string& fieldName,
                                  std::uint32_t type, FunctionId initializer, SourceLocation location) {
    StaticField field;
    field.id = static_cast<StaticId>(module_.statics.size() + 1);
    field.className = className;
    field.name = fieldName;
    field.type = type;
    field.initializer = initializer;
    field.location = std::move(location);
    module_.statics.push_back(std::move(field));
    return module_.statics.back().id;
}

InterfaceInfo& ModuleBuilder::addInterface(const std::string& name) {
    InterfaceInfo info;
    info.name = name;
    module_.interfaces.push_back(std::move(info));
    return module_.interfaces.back();
}

ClassLayout& ModuleBuilder::addClassLayout(const std::string& name) {
    ClassLayout layout;
    layout.name = name;
    module_.classes.push_back(std::move(layout));
    return module_.classes.back();
}

ConstId ModuleBuilder::addConstant(const Constant& value) {
    return module_.internConstant(value);
}

ConstId ModuleBuilder::constantInt(std::int64_t value) {
    Constant c;
    c.kind = ConstKind::Int;
    c.intValue = value;
    return addConstant(c);
}

ConstId ModuleBuilder::constantDouble(double value) {
    Constant c;
    c.kind = ConstKind::Double;
    c.doubleValue = value;
    return addConstant(c);
}

ConstId ModuleBuilder::constantBool(bool value) {
    Constant c;
    c.kind = ConstKind::Bool;
    c.boolValue = value;
    return addConstant(c);
}

ConstId ModuleBuilder::constantString(std::string value) {
    Constant c;
    c.kind = ConstKind::String;
    c.stringValue = std::move(value);
    return addConstant(c);
}

ConstId ModuleBuilder::constantNil() {
    Constant c;
    c.kind = ConstKind::Nil;
    return addConstant(c);
}

ConstId ModuleBuilder::constantEnumMember(std::string enumTypeName, std::string member) {
    Constant c;
    c.kind = ConstKind::EnumMember;
    c.enumTypeName = std::move(enumTypeName);
    c.stringValue = std::move(member);
    return addConstant(c);
}

Module ModuleBuilder::take() {
    return std::move(module_);
}

// ---------------------------------------------------------------------------

FunctionBuilder::FunctionBuilder(Module& module, Function& function)
    : module_(module), function_(function) {}

ParamId FunctionBuilder::addParameter(const std::string& name, std::uint32_t type,
                                      zl::OwnershipKind ownership, std::string borrowSource,
                                      SourceLocation location) {
    Parameter parameter;
    parameter.name = name;
    parameter.type = type;
    parameter.ownership = ownership;
    parameter.borrowSource = std::move(borrowSource);
    parameter.location = std::move(location);
    function_.parameters.push_back(std::move(parameter));
    return static_cast<ParamId>(function_.parameters.size() - 1);
}

Operand FunctionBuilder::parameterOperand(ParamId id) const {
    const Parameter* parameter = function_.parameter(id);
    return parameter ? Operand::param(id, parameter->type) : Operand::none();
}

void FunctionBuilder::setReturnType(std::uint32_t type, zl::OwnershipKind ownership) {
    function_.returnType = type;
    function_.returnOwnership = ownership;
}

void FunctionBuilder::setGenericTemplate(std::vector<std::string> typeParameters) {
    function_.typeParameters = std::move(typeParameters);
    function_.isGenericTemplate = !function_.typeParameters.empty();
}

void FunctionBuilder::setGenericArguments(std::vector<std::uint32_t> arguments) {
    function_.genericArguments = std::move(arguments);
}

void FunctionBuilder::addCapture(const std::string& name, std::uint32_t type, bool usesThis) {
    function_.captures.push_back(CaptureSpec{name, type, usesThis});
}

void FunctionBuilder::markIncomplete(std::string reason) {
    function_.incomplete = true;
    function_.incompleteReasons.push_back(std::move(reason));
}

SlotId FunctionBuilder::addSlot(const std::string& name, std::uint32_t type, bool isMutable,
                                zl::OwnershipKind ownership, std::string borrowSource,
                                SourceLocation location) {
    Slot slot;
    slot.name = name;
    slot.type = type;
    slot.isMutable = isMutable;
    slot.ownership = ownership;
    slot.borrowSource = std::move(borrowSource);
    slot.location = std::move(location);
    function_.slots.push_back(std::move(slot));
    return static_cast<SlotId>(function_.slots.size());
}

BlockId FunctionBuilder::addBlock(BlockKind kind, SourceLocation location) {
    BasicBlock block;
    block.id = static_cast<BlockId>(function_.blocks.size() + 1);
    block.kind = kind;
    block.location = std::move(location);
    function_.blocks.push_back(std::move(block));
    if (function_.blocks.size() == 1) {
        // The first block is the entry by construction, which is what makes
        // "entry block == blocks[0]" an invariant rather than a convention.
        function_.entryBlock = block.id;
        current_ = block.id;
    }
    return block.id;
}

BlockParamId FunctionBuilder::addBlockParameter(BlockId block, const std::string& name,
                                               std::uint32_t type, SourceLocation location) {
    // Parameters are numbered in one function-wide space, so a parameter is a
    // value with a unique identity rather than "the third parameter of b4".
    BlockParamId next = 1;
    for (const auto& existing : function_.blocks) {
        for (const auto& parameter : existing.parameters) {
            if (parameter.id >= next) next = parameter.id + 1;
        }
    }
    BasicBlock& target = this->block(block);
    BlockParameter parameter;
    parameter.id = next;
    parameter.name = name;
    parameter.type = type;
    parameter.location = std::move(location);
    target.parameters.push_back(parameter);
    return parameter.id;
}

void FunctionBuilder::setEdgeArguments(BlockId from, std::size_t successorIndex,
                                       std::vector<Operand> arguments) {
    BasicBlock& source = block(from);
    const std::size_t successorCount = source.terminator.successors().size();
    if (source.terminator.edgeArguments.size() < successorCount) {
        source.terminator.edgeArguments.resize(successorCount);
    }
    assert(successorIndex < source.terminator.edgeArguments.size() &&
           "edge argument index is not a successor of that block");
    source.terminator.edgeArguments[successorIndex] = std::move(arguments);
}

void FunctionBuilder::emitJumpWithArguments(BlockId target, std::vector<Operand> arguments,
                                            SourceLocation location) {
    emitJump(target, std::move(location));
    setEdgeArguments(current_, 0, std::move(arguments));
}

void FunctionBuilder::emitBranchWithArguments(Operand condition, BlockId thenBlock,
                                              std::vector<Operand> thenArguments, BlockId elseBlock,
                                              std::vector<Operand> elseArguments,
                                              SourceLocation location) {
    emitBranch(condition, thenBlock, elseBlock, std::move(location));
    setEdgeArguments(current_, 0, std::move(thenArguments));
    setEdgeArguments(current_, 1, std::move(elseArguments));
}

void FunctionBuilder::pushExceptionHandler(std::uint32_t catchType, BlockId target, SlotId catchSlot) {
    assert(current_ != kNoBlock && "pushExceptionHandler with no current block");
    block(current_).exceptionHandlers.push_back(ExceptionHandler{catchType, target, catchSlot});
}

Instruction& FunctionBuilder::append(Opcode opcode, SourceLocation location) {
    assert(current_ != kNoBlock && "emitting an instruction with no current block");
    Instruction instruction;
    instruction.opcode = opcode;
    instruction.location = std::move(location);
    block(current_).instructions.push_back(std::move(instruction));
    return block(current_).instructions.back();
}

void FunctionBuilder::setResult(Instruction& instruction, std::uint32_t resultType) {
    instruction.result = nextTemp();
    instruction.resultType = resultType;
}

TempId FunctionBuilder::emitNop(SourceLocation location) {
    (void)append(Opcode::Nop, std::move(location));
    return kNoTemp;
}

TempId FunctionBuilder::emitBinary(Opcode opcode, Operand left, Operand right, std::uint32_t resultType,
                                   SourceLocation location) {
    Instruction& instruction = append(opcode, std::move(location));
    instruction.operands = {left, right};
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitUnary(Opcode opcode, Operand operand, std::uint32_t resultType,
                                  SourceLocation location) {
    Instruction& instruction = append(opcode, std::move(location));
    instruction.operands = {operand};
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitWiden(Operand operand, SourceLocation location) {
    Instruction& instruction = append(Opcode::Widen, std::move(location));
    instruction.operands = {operand};
    setResult(instruction, types().doubleType());
    return instruction.result;
}

TempId FunctionBuilder::emitRefine(Operand operand, std::uint32_t resultType, SourceLocation location) {
    Instruction& instruction = append(Opcode::Refine, std::move(location));
    instruction.operands = {operand};
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitTypeTest(Operand operand, std::uint32_t testedType, SourceLocation location) {
    Instruction& instruction = append(Opcode::TypeTest, std::move(location));
    instruction.operands = {operand};
    instruction.testedType = testedType;
    setResult(instruction, types().boolType());
    return instruction.result;
}

TempId FunctionBuilder::emitNullCheck(Operand operand, SourceLocation location) {
    Instruction& instruction = append(Opcode::NullCheck, std::move(location));
    instruction.operands = {operand};
    setResult(instruction, operand.type);
    return instruction.result;
}

TempId FunctionBuilder::emitIsNull(Operand operand, SourceLocation location) {
    Instruction& instruction = append(Opcode::IsNull, std::move(location));
    instruction.operands = {operand};
    setResult(instruction, types().boolType());
    return instruction.result;
}

TempId FunctionBuilder::emitLoad(SlotId slot, SourceLocation location) {
    Instruction& instruction = append(Opcode::Load, std::move(location));
    instruction.slot = slot;
    const Slot* s = function_.slot(slot);
    setResult(instruction, s ? s->type : 0);
    return instruction.result;
}

void FunctionBuilder::emitStore(SlotId slot, Operand value, SourceLocation location) {
    Instruction& instruction = append(Opcode::Store, std::move(location));
    instruction.slot = slot;
    instruction.operands = {value};
}

TempId FunctionBuilder::emitFieldLoad(Operand base, const std::string& fieldName, std::uint32_t resultType,
                                      SourceLocation location) {
    Instruction& instruction = append(Opcode::FieldLoad, std::move(location));
    instruction.operands = {base};
    instruction.name = fieldName;
    setResult(instruction, resultType);
    return instruction.result;
}

void FunctionBuilder::emitFieldStore(Operand base, const std::string& fieldName, Operand value,
                                     SourceLocation location) {
    Instruction& instruction = append(Opcode::FieldStore, std::move(location));
    instruction.operands = {base, value};
    instruction.name = fieldName;
}

TempId FunctionBuilder::emitIndexLoad(Operand base, Operand index, std::uint32_t resultType,
                                      SourceLocation location) {
    Instruction& instruction = append(Opcode::IndexLoad, std::move(location));
    instruction.operands = {base, index};
    setResult(instruction, resultType);
    return instruction.result;
}

void FunctionBuilder::emitIndexStore(Operand base, Operand index, Operand value, SourceLocation location) {
    Instruction& instruction = append(Opcode::IndexStore, std::move(location));
    instruction.operands = {base, index, value};
}

TempId FunctionBuilder::emitStaticLoad(const std::string& className, const std::string& fieldName,
                                       std::uint32_t resultType, SourceLocation location) {
    Instruction& instruction = append(Opcode::StaticLoad, std::move(location));
    instruction.target.className = className;
    instruction.name = fieldName;
    setResult(instruction, resultType);
    return instruction.result;
}

void FunctionBuilder::emitStaticStore(const std::string& className, const std::string& fieldName, Operand value,
                                      SourceLocation location) {
    Instruction& instruction = append(Opcode::StaticStore, std::move(location));
    instruction.target.className = className;
    instruction.name = fieldName;
    instruction.operands = {value};
}

TempId FunctionBuilder::emitAlloc(const std::string& className, std::vector<std::uint32_t> typeArguments,
                                  std::uint32_t resultType, SourceLocation location) {
    Instruction& instruction = append(Opcode::Alloc, std::move(location));
    instruction.className = className;
    instruction.typeArguments = std::move(typeArguments);
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitNewCollection(std::uint32_t resultType, SourceLocation location) {
    Instruction& instruction = append(Opcode::NewCollection, std::move(location));
    setResult(instruction, resultType);
    return instruction.result;
}

namespace {

// Records the argument/result types on a call so a backend never has to
// reconstruct the callee's signature from the operands.
void describeCall(Instruction& instruction, const std::vector<Operand>& arguments, std::uint32_t resultType,
                  std::size_t receiverCount) {
    instruction.target.argumentTypes.clear();
    for (std::size_t i = receiverCount; i < instruction.operands.size(); ++i) {
        instruction.target.argumentTypes.push_back(instruction.operands[i].type);
    }
    instruction.target.resultType = resultType;
    (void)arguments;
}

} // namespace

TempId FunctionBuilder::emitCall(FunctionId callee, std::vector<Operand> arguments, std::uint32_t resultType,
                                 SourceLocation location, std::vector<std::uint32_t> typeArguments) {
    Instruction& instruction = append(Opcode::Call, std::move(location));
    instruction.operands = std::move(arguments);
    instruction.target.function = callee;
    instruction.typeArguments = std::move(typeArguments);
    describeCall(instruction, instruction.operands, resultType, 0);
    if (resultType == 0) return kNoTemp;
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitInvokeMethod(Operand receiver, const std::string& className,
                                         const std::string& methodName, std::vector<Operand> arguments,
                                         std::uint32_t resultType, SourceLocation location,
                                         std::vector<std::uint32_t> typeArguments) {
    Instruction& instruction = append(Opcode::InvokeMethod, std::move(location));
    instruction.operands.push_back(receiver);
    for (auto& argument : arguments) instruction.operands.push_back(std::move(argument));
    instruction.target.className = className;
    instruction.target.methodName = methodName;
    instruction.typeArguments = std::move(typeArguments);
    describeCall(instruction, instruction.operands, resultType, 1);
    if (resultType == 0) return kNoTemp;
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitInvokeSuper(Operand receiver, FunctionId target, std::vector<Operand> arguments,
                                        std::uint32_t resultType, SourceLocation location,
                                        std::vector<std::uint32_t> typeArguments) {
    Instruction& instruction = append(Opcode::InvokeSuper, std::move(location));
    instruction.operands.push_back(receiver);
    instruction.typeArguments = std::move(typeArguments);
    for (auto& argument : arguments) instruction.operands.push_back(std::move(argument));
    instruction.target.function = target;
    if (const Function* callee = module_.function(target)) {
        instruction.target.className = callee->ownerClass;
        instruction.target.methodName = callee->simpleName;
    }
    describeCall(instruction, instruction.operands, resultType, 1);
    if (resultType == 0) return kNoTemp;
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitInvokeStatic(FunctionId target, std::vector<Operand> arguments,
                                         std::uint32_t resultType, SourceLocation location,
                                         std::vector<std::uint32_t> typeArguments) {
    Instruction& instruction = append(Opcode::InvokeStatic, std::move(location));
    instruction.operands = std::move(arguments);
    instruction.target.function = target;
    instruction.typeArguments = std::move(typeArguments);
    if (const Function* callee = module_.function(target)) {
        instruction.target.className = callee->ownerClass;
        instruction.target.methodName = callee->simpleName;
    }
    describeCall(instruction, instruction.operands, resultType, 0);
    if (resultType == 0) return kNoTemp;
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitCallIndirect(Operand callee, std::vector<Operand> arguments,
                                         std::uint32_t resultType, SourceLocation location) {
    Instruction& instruction = append(Opcode::CallIndirect, std::move(location));
    instruction.operands.push_back(callee);
    for (auto& argument : arguments) instruction.operands.push_back(std::move(argument));
    describeCall(instruction, instruction.operands, resultType, 1);
    if (resultType == 0) return kNoTemp;
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitCallNative(const std::string& qualifiedName, std::int32_t nativeId,
                                       std::vector<Operand> arguments, std::uint32_t resultType, bool isAsync,
                                       SourceLocation location) {
    Instruction& instruction = append(Opcode::CallNative, std::move(location));
    instruction.operands = std::move(arguments);
    instruction.target.nativeName = qualifiedName;
    instruction.target.nativeId = nativeId;
    instruction.target.isAsync = isAsync;
    describeCall(instruction, instruction.operands, resultType, 0);
    if (resultType == 0) return kNoTemp;
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitMakeClosure(FunctionId body, std::vector<Operand> captures,
                                        std::uint32_t resultType, SourceLocation location) {
    Instruction& instruction = append(Opcode::MakeClosure, std::move(location));
    instruction.operands = std::move(captures);
    instruction.target.function = body;
    if (const Function* callee = module_.function(body)) {
        instruction.target.argumentTypes.clear();
        for (const auto& parameter : callee->parameters) {
            instruction.target.argumentTypes.push_back(parameter.type);
        }
    }
    instruction.target.resultType = resultType;
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitAwait(Operand task, std::uint32_t resultType, SourceLocation location) {
    Instruction& instruction = append(Opcode::Await, std::move(location));
    instruction.operands = {task};
    if (resultType == 0) return kNoTemp;
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitTaskCreate(Operand value, std::uint32_t resultType, SourceLocation location) {
    Instruction& instruction = append(Opcode::TaskCreate, std::move(location));
    instruction.operands = {value};
    setResult(instruction, resultType);
    return instruction.result;
}

TempId FunctionBuilder::emitMove(SlotId slot, SourceLocation location) {
    Instruction& instruction = append(Opcode::Move, std::move(location));
    instruction.slot = slot;
    const Slot* s = function_.slot(slot);
    setResult(instruction, s ? s->type : 0);
    return instruction.result;
}

void FunctionBuilder::emitBorrow(SlotId borrowSlot, Operand owner, SourceLocation location) {
    Instruction& instruction = append(Opcode::Borrow, std::move(location));
    instruction.slot = borrowSlot;
    instruction.operands = {owner};
}

void FunctionBuilder::emitEndBorrow(SlotId borrowSlot, SourceLocation location) {
    Instruction& instruction = append(Opcode::EndBorrow, std::move(location));
    instruction.slot = borrowSlot;
}

void FunctionBuilder::emitDrop(Operand value, SourceLocation location) {
    Instruction& instruction = append(Opcode::Drop, std::move(location));
    instruction.operands = {value};
}

void FunctionBuilder::emitDrop(SlotId slot, SourceLocation location) {
    // The storage-release form: drop the value held by `slot`. Lowering uses
    // this for the deterministic end of an owned local's lifetime - the same
    // event the reference compiler spells `DropVar` - so the slot travels on
    // the instruction and backends can name the local they release without
    // re-deriving provenance from the operand.
    Instruction& instruction = append(Opcode::Drop, std::move(location));
    instruction.slot = slot;
}

TempId FunctionBuilder::emitRangeInBounds(Operand current, Operand end, Operand step, SourceLocation location) {
    Instruction& instruction = append(Opcode::RangeInBounds, std::move(location));
    instruction.operands = {current, end, step};
    setResult(instruction, types().boolType());
    return instruction.result;
}

void FunctionBuilder::emitLog(Operand value, SourceLocation location) {
    Instruction& instruction = append(Opcode::Log, std::move(location));
    instruction.operands = {value};
}

void FunctionBuilder::emitReturn(Operand value, SourceLocation location) {
    Terminator& terminator = block(current_).terminator;
    terminator = Terminator{};
    terminator.kind = TerminatorKind::Return;
    terminator.value = value;
    terminator.location = std::move(location);
}

void FunctionBuilder::emitJump(BlockId target, SourceLocation location) {
    Terminator& terminator = block(current_).terminator;
    terminator = Terminator{};
    terminator.kind = TerminatorKind::Jump;
    terminator.target = target;
    terminator.location = std::move(location);
}

void FunctionBuilder::emitBranch(Operand condition, BlockId thenBlock, BlockId elseBlock, SourceLocation location) {
    Terminator& terminator = block(current_).terminator;
    terminator = Terminator{};
    terminator.kind = TerminatorKind::Branch;
    terminator.value = condition;
    terminator.target = thenBlock;
    terminator.elseBlock = elseBlock;
    terminator.location = std::move(location);
}

void FunctionBuilder::emitSwitch(Operand subject, std::vector<SwitchTarget> cases, BlockId defaultBlock,
                                 SourceLocation location) {
    Terminator& terminator = block(current_).terminator;
    terminator = Terminator{};
    terminator.kind = TerminatorKind::Switch;
    terminator.value = subject;
    terminator.cases = std::move(cases);
    terminator.target = defaultBlock;
    terminator.location = std::move(location);
}

void FunctionBuilder::emitThrow(Operand value, SourceLocation location) {
    Terminator& terminator = block(current_).terminator;
    terminator = Terminator{};
    terminator.kind = TerminatorKind::Throw;
    terminator.value = value;
    terminator.location = std::move(location);
}

void FunctionBuilder::emitUnreachable(SourceLocation location) {
    Terminator& terminator = block(current_).terminator;
    terminator = Terminator{};
    terminator.kind = TerminatorKind::Unreachable;
    terminator.location = std::move(location);
}

void FunctionBuilder::finish() {
    // Two structural invariants are the builder's job, not the caller's:
    // every block ends in exactly one terminator, and no block is unreachable.
    //
    // A lowering pass that gives up partway through a function leaves blocks it
    // created but never wired up. Terminating them with `unreachable` and
    // pruning them keeps the module structurally valid MIR even when a function
    // is only a partial translation - which is what Function::incomplete is for.
    for (auto& block : function_.blocks) {
        if (block.terminator.kind == TerminatorKind::None) {
            block.terminator = Terminator{};
            block.terminator.kind = TerminatorKind::Unreachable;
            block.terminator.location = block.location;
        }
    }
    pruneUnreachableBlocks(function_);
    function_.rebuildEdges();
    current_ = kNoBlock;
}

} // namespace zl::mir
