#include "zl/mir/vm_backend.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <sstream>

#include "zl/vm/value.hpp"
#include "zl/vm/native.hpp"
#include "zl/vm/runtime_type.hpp"

namespace zl::mir {
namespace {

// Both MIR and the bytecode backend define a struct named `Instruction`;
// inside this file, unqualified `Instruction` is the bytecode one.
using BcInstruction = ::zl::Instruction;

// ---------------------------------------------------------------------------
// Value / type helpers
// ---------------------------------------------------------------------------

Value toVmConstant(const Constant& c) {
    switch (c.kind) {
        case ConstKind::Bool: return Value{c.boolValue};
        case ConstKind::Int: return Value{c.intValue};
        case ConstKind::Double: return Value{c.doubleValue};
        case ConstKind::String: return Value{c.stringValue};
        case ConstKind::Nil: return Value{};
        case ConstKind::EnumMember: return Value{c.stringValue};
    }
    return Value{};
}

// Runtime type-assertion spelling for a MIR type. These strings are handed to
// the VM's RuntimeTypeCheck at field boundaries; primitives and plain object
// names are the well-trodden path. The canonical render is a safe fallback.
const char* primitiveTypeName(TypeKind kind) {
    switch (kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Nil: return "nil";
        case TypeKind::Bool: return "bool";
        case TypeKind::Int: return "int";
        case TypeKind::Double: return "double";
        case TypeKind::String: return "string";
        default: return nullptr;
    }
}

std::string runtimeTypeName(const TypeArena& types, TypeId id) {
    const Type* type = types.find(id);
    if (!type) return "unknown";
    if (const char* p = primitiveTypeName(type->kind)) return p;
    if (type->kind == TypeKind::Object && !type->name.empty()) return type->name;
    return types.render(id);
}

// Dispatch key for a method / constructor, consistent across classes so that
// an override and its base share a slot. Includes the declaring names after
// the receiver (this), matching how overload resolution distinguishes them.
std::string dispatchKeyFor(const Function& fn, const TypeArena& types) {
    const bool hasThis = fn.hasThisParameter;
    std::ostringstream out;
    out << (fn.isConstructor && !fn.ownerClass.empty() ? fn.ownerClass : fn.simpleName) << '(';
    for (std::size_t i = (hasThis ? 1 : 0); i < fn.parameters.size(); ++i) {
        if (i > (hasThis ? 1 : 0)) out << ',';
        out << runtimeTypeName(types, fn.parameters[i].type);
    }
    out << ')';
    return out.str();
}

// ---------------------------------------------------------------------------
// The translator
// ---------------------------------------------------------------------------

struct PerFunctionInfo {
    bool supported{false};
    std::string unsupportedReason;
    // Register-local names are function scoped and unique per slot / temp.
};

class MirBytecodeTranslator {
public:
    explicit MirBytecodeTranslator(const Module& module) : module_(module) {}

    BytecodeResult translate() {
        BytecodeResult result;
        try {
            if (!module_.entryPoint || module_.entryPoint > module_.functions.size()) {
                result.errors.push_back("module has no valid entry point");
                return result;
            }
            registerFunctions();
            if (!result.errors.empty()) return result;
            buildDispatchers();
            buildClassReflection();
            buildConstants();
            buildStatics();

            // Skip over function bodies (and the unsupported stub) to the real
            // program start, mirroring the reference compiler.
            const std::size_t skipJump = chunk_.code.size();
            emitGlobal(OpCode::Jump, 0);

            // Compile every function this backend can, catching the ones whose
            // bodies turn out not to be translatable (a construct missed by the
            // cheap opcode gate, e.g. a method whose overload cannot be pinned
            // down). Those become stubs; because the gate already rejects the
            // obviously-unsupported constructs, a stub is only ever *reached*
            // by a program that genuinely needs an untranslatable function, at
            // which point it raises loudly instead of misbehaving silently.
            for (std::size_t i = 0; i < module_.functions.size(); ++i) {
                if (!fnInfo_[i].supported) continue;
                const std::size_t entry = chunk_.code.size();
                try {
                    compileFunctionBody(i);
                    chunk_.functions[i].entryAddress = entry;
                } catch (const std::exception& e) {
                    // Nothing was appended: compileFunctionBody buffers its
                    // whole body and only splices it on success.
                    fnInfo_[i].supported = false;
                    fnInfo_[i].unsupportedReason = e.what();
                }
            }

            // A single shared stub for every function this backend could not
            // translate. Reaching it is a loud runtime error, never silence.
            const std::size_t stubAddress = chunk_.code.size();
            emitStub();
            for (std::size_t i = 0; i < module_.functions.size(); ++i) {
                if (!fnInfo_[i].supported) chunk_.functions[i].entryAddress = stubAddress;
            }

            // Program entry.
            compileEntry(skipJump);

            result.chunk = std::move(chunk_);
            result.stubbed = std::count_if(fnInfo_.begin(), fnInfo_.end(),
                                           [](const PerFunctionInfo& i) { return !i.supported; });
            for (std::size_t i = 0; i < module_.functions.size(); ++i) {
                if (!fnInfo_[i].supported) result.stubbedFunctions.push_back(module_.functions[i].name);
            }
            return result;
        } catch (const std::exception& e) {
            result.errors.push_back(e.what());
            return result;
        }
    }

private:
    const Module& module_;
    Chunk chunk_;
    std::vector<PerFunctionInfo> fnInfo_;

    // ConstId (1-based MIR) -> Chunk constant index.
    std::vector<std::size_t> constIndex_;
    std::unordered_map<std::string, std::size_t> slotByDispatchKey_;
    // class name -> vector<functionIndex-or-INVALID> indexed by slot.
    std::unordered_map<std::string, std::vector<std::size_t>> vtables_;
    std::vector<std::string> slotKeyByIndex_;

    // --- chunk emission (global code) -------------------------------------
    std::size_t emitGlobal(OpCode op, std::size_t operand = 0, std::size_t line = 0,
                           std::size_t operand2 = 0, std::size_t operand3 = 0) {
        chunk_.code.push_back(BcInstruction{op, operand, line, operand2, operand3});
        return chunk_.code.size() - 1;
    }
    std::size_t addName(const std::string& name) { return chunk_.addName(name); }
    std::size_t addConst(const Value& value) { return chunk_.addConstant(value); }

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------
    void registerFunctions() {
        fnInfo_.assign(module_.functions.size(), PerFunctionInfo{});
        for (std::size_t i = 0; i < module_.functions.size(); ++i) {
            const Function& fn = module_.functions[i];
            const bool hasThis = fn.hasThisParameter;
            FunctionInfo info;
            info.name = fn.name;
            info.ownerClassName = fn.ownerClass;
            // A function with no `this` parameter is called without a receiver.
            // Native functions are dispatched through the native catalog
            // (CallNative), never through a bytecode `Call`, so they need no
            // runnable body here.
            info.isStatic = fn.isStatic;
            info.isAsync = fn.isAsync;
            info.isNative = fn.isNative;
            info.capturesEvaluationScope = false;
            if (hasThis) info.isStatic = false; // instance methods get `this` from the receiver
            for (std::size_t p = (hasThis ? 1 : 0); p < fn.parameters.size(); ++p) {
                info.paramNames.push_back(fn.parameters[p].name);
                // Keep runtime parameter assertions off ("" skips them); the MIR
                // static checks already guarded the program, and this backend
                // must not invent spurious runtime mismatches.
                info.parameterTypeNames.push_back("");
            }
            info.returnTypeName = ""; // "" skips the return assertion
            info.dispatchSignature.name = fn.simpleName;
            if (fn.isNative) {
                fnInfo_[i].supported = false;
                fnInfo_[i].unsupportedReason = "native function (dispatched via catalog)";
            } else {
                fnInfo_[i].supported = isSupported(fn, fnInfo_[i].unsupportedReason);
            }
            chunk_.functions.push_back(std::move(info));
        }
    }

    // A function is translatable when every block is a normal block with no
    // dynamic exception chain, and every opcode/terminator has a faithful
    // bytecode spelling.
    bool isSupported(const Function& fn, std::string& reason) {
        for (const BasicBlock& block : fn.blocks) {
            if (block.kind != BlockKind::Normal) {
                reason = "block kind " + std::string(blockKindName(block.kind));
                return false;
            }
            if (!block.exceptionHandlers.empty()) {
                reason = "exception handler chain";
                return false;
            }
            for (const Instruction& ins : block.instructions) {
                if (!isSupportedOpcode(ins.opcode)) {
                    reason = "opcode " + std::string(opcodeName(ins.opcode));
                    return false;
                }
            }
            if (!isSupportedTerminator(block.terminator.kind)) {
                reason = "terminator " + std::string(terminatorKindName(block.terminator.kind));
                return false;
            }
        }
        if (fn.isNative) return true; // body never runs; calls go through CallNative
        return true;
    }

    bool isSupportedOpcode(Opcode op) {
        switch (op) {
            case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
            case Opcode::Div: case Opcode::Mod: case Opcode::Pow: case Opcode::Neg:
            case Opcode::BitAnd: case Opcode::BitOr: case Opcode::BitXor:
            case Opcode::BitNot: case Opcode::Shl: case Opcode::Shr: case Opcode::Ushr:
            case Opcode::Eq: case Opcode::Ne: case Opcode::Lt: case Opcode::Le:
            case Opcode::Gt: case Opcode::Ge:
            case Opcode::Not:
            case Opcode::Widen: case Opcode::Refine:
            case Opcode::Load: case Opcode::Store:
            case Opcode::FieldLoad: case Opcode::FieldStore:
            case Opcode::IndexLoad: case Opcode::IndexStore:
            case Opcode::NewCollection:
            case Opcode::Alloc:
            case Opcode::Call: case Opcode::InvokeMethod: case Opcode::InvokeSuper:
            case Opcode::InvokeStatic: case Opcode::CallIndirect: case Opcode::CallNative:
            case Opcode::RangeInBounds: case Opcode::Log:
                return true;
            default:
                return false;
        }
    }

    bool isSupportedTerminator(TerminatorKind kind) {
        switch (kind) {
            case TerminatorKind::Return: case TerminatorKind::Jump:
            case TerminatorKind::Branch: case TerminatorKind::Throw:
                return true;
            default:
                return false;
        }
    }

    // -----------------------------------------------------------------------
    // Dispatch slots and vtables
    // -----------------------------------------------------------------------
    void buildDispatchers() {
        // Assign one slot per distinct dispatch key over all instance methods
        // and constructors, in module order.
        for (const Function& fn : module_.functions) {
            if (fn.isLambda || fn.ownerClass.empty()) continue;
            if (!(fn.hasThisParameter)) continue; // static members are plain Call targets
            const std::string key = dispatchKeyFor(fn, module_.types);
            if (!slotByDispatchKey_.count(key)) {
                slotByDispatchKey_[key] = slotKeyByIndex_.size();
                slotKeyByIndex_.push_back(key);
            }
        }
        const std::size_t slotCount = slotKeyByIndex_.size();

        // Per-class declared function index by dispatch key.
        std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> declByClass;
        for (std::size_t i = 0; i < module_.functions.size(); ++i) {
            const Function& fn = module_.functions[i];
            if (fn.ownerClass.empty() || !fn.hasThisParameter) continue;
            declByClass[fn.ownerClass][dispatchKeyFor(fn, module_.types)] = i;
        }

        // Class parent chain (from layouts).
        std::unordered_map<std::string, std::string> parents;
        for (const ClassLayout& cls : module_.classes) parents[cls.name] = cls.parent;

        // Collect every class name that has instance members or appears as a
        // declared receiver.
        std::vector<std::string> classNames;
        std::unordered_map<std::string, bool> seen;
        for (const ClassLayout& cls : module_.classes) {
            if (!seen.count(cls.name)) { classNames.push_back(cls.name); seen[cls.name] = true; }
        }
        for (const Function& fn : module_.functions) {
            if (!fn.ownerClass.empty() && !seen.count(fn.ownerClass)) {
                classNames.push_back(fn.ownerClass);
                seen[fn.ownerClass] = true;
            }
        }

        for (const std::string& name : classNames) {
            std::vector<std::size_t> row(slotCount, Chunk::INVALID_FUNCTION_INDEX);
            // Inherited entries first (base -> derived) so a derived override wins.
            std::vector<std::string> chain;
            std::string cur = name;
            for (std::size_t guard = 0; guard < 64 && !cur.empty(); ++guard) {
                chain.push_back(cur);
                auto it = parents.find(cur);
                cur = (it != parents.end()) ? it->second : std::string();
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const auto found = declByClass.find(*it);
                if (found == declByClass.end()) continue;
                for (const auto& entry : found->second) {
                    const auto slotIt = slotByDispatchKey_.find(entry.first);
                    if (slotIt != slotByDispatchKey_.end()) row[slotIt->second] = entry.second;
                }
            }
            vtables_[name] = std::move(row);
        }
    }

    // -----------------------------------------------------------------------
    // Class reflection (needed by the VM for instance field stores)
    // -----------------------------------------------------------------------
    void buildClassReflection() {
        std::unordered_map<std::string, std::string> parents;
        for (const ClassLayout& cls : module_.classes) parents[cls.name] = cls.parent;
        for (const ClassLayout& cls : module_.classes) {
            ClassReflectionInfo meta;
            meta.baseClassName = cls.parent;
            meta.interfaces = cls.interfaces;
            meta.typeParameters = cls.typeParameters;
            meta.isDataType = cls.isData;
            meta.isEnumType = cls.isEnum;
            meta.enumMembers = cls.enumMembers;

            // Fields: this class plus ancestors (the VM looks fields up on the
            // receiver's runtime class).
            std::vector<const ClassLayout*> chain;
            std::string cur = cls.name;
            for (std::size_t guard = 0; guard < 64 && !cur.empty(); ++guard) {
                const ClassLayout* layout = module_.classLayout(cur);
                if (!layout) break;
                chain.push_back(layout);
                auto pit = parents.find(cur);
                cur = (pit != parents.end()) ? pit->second : std::string();
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                for (const FieldLayout& field : (*it)->fields) {
                    RuntimeFieldInfo info;
                    info.name = field.name;
                    info.ownerClassName = (*it)->name;
                    info.typeName = runtimeTypeName(module_.types, field.type);
                    info.access = "public";
                    info.isStatic = field.isStatic;
                    info.ownership = field.ownership;
                    if (!field.isStatic) meta.fields.push_back(std::move(info));
                }
            }
            RuntimeTypeInfo runtimeType;
            runtimeType.name = cls.name;
            runtimeType.baseClassName = cls.parent;
            runtimeType.interfaces = cls.interfaces;
            runtimeType.typeParameters = cls.typeParameters;
            runtimeType.isDataType = cls.isData;
            runtimeType.isEnumType = cls.isEnum;
            runtimeType.enumMembers = cls.enumMembers;
            runtimeType.fields = meta.fields;
            meta.runtimeType = std::make_shared<RuntimeTypeInfo>(std::move(runtimeType));
            chunk_.classReflection[cls.name] = std::move(meta);
        }
        chunk_.classVTables = vtables_;
    }

    // -----------------------------------------------------------------------
    // Constant pool
    // -----------------------------------------------------------------------
    void buildConstants() {
        constIndex_.assign(module_.constants.size() + 1,
                           std::numeric_limits<std::size_t>::max());
        for (std::size_t i = 0; i < module_.constants.size(); ++i) {
            constIndex_[i + 1] = addConst(toVmConstant(module_.constants[i]));
        }
    }

    // Statics: unsupported for now; nothing must reference them in a supported
    // function (StaticLoad/Store are not in the supported opcode set).
    void buildStatics() {}

    // -----------------------------------------------------------------------
    // Function body compilation
    // -----------------------------------------------------------------------
    // Per-function emission buffer. Local addresses until finalised (offset by
    // the global base at append time).
    struct Body {
        std::vector<BcInstruction> code;
        std::unordered_map<BlockId, std::size_t> label;
        // Instruction index in `code` whose Jump/Branch operand needs patching.
        std::vector<std::pair<std::size_t, BlockId>> patches;
        std::size_t base{0};
        std::size_t emit(::zl::OpCode op, std::size_t operand = 0, std::size_t line = 0,
                         std::size_t operand2 = 0, std::size_t operand3 = 0) {
            code.push_back(BcInstruction{op, operand, line, operand2, operand3});
            return code.size() - 1;
        }
    };

    void compileFunctionBody(std::size_t fIndex) {
        const Function& fn = module_.functions[fIndex];
        if (fn.isNative) return; // entry set; body is never run
        Body body;
        body.base = chunk_.code.size();

        // Reserve locals: slot and temp names are just indexed names; a slot is
        // materialised on first store (its defining Store), a temp on its
        // producing instruction. Nothing needs declaring up front.

        for (const BasicBlock& block : fn.blocks) {
            if (!body.label.count(block.id)) body.label[block.id] = body.code.size();
            // Block parameters are the MIR's phi nodes. The VM has no phi, but
            // block parameters and slots are interchangeable in meaning, so the
            // lowering is exactly the memory form: each predecessor stores the
            // value it would have passed into the parameter's own local (see
            // emitEdgeArguments) and the block simply reads it. Every incoming
            // edge writes that local before the block runs, so a read inside
            // the block sees the value its predecessor supplied, which is what
            // the parameter means. Nothing is emitted here on entry.
            for (const Instruction& ins : block.instructions) {
                emitInstruction(fn, block.id, ins, body);
            }
            emitTerminator(fn, block, body);
        }

        // Patch unresolved forward references.
        for (const auto& patch : body.patches) {
            const auto it = body.label.find(patch.second);
            if (it != body.label.end()) body.code[patch.first].operand = it->second;
        }

        // Append, translating local addresses to global ones.
        for (auto& ins : body.code) {
            if (ins.op == OpCode::Jump || ins.op == OpCode::JumpIfFalse) {
                ins.operand += body.base;
            }
        }
        chunk_.code.insert(chunk_.code.end(), body.code.begin(), body.code.end());
    }

    // ---- local operand / register naming ---------------------------------
    bool hasThis(const Function& fn) const { return fn.hasThisParameter; }
    std::string paramLocal(const Function& fn, ParamId p) const {
        if (hasThis(fn)) {
            if (p == 0) return "this";
            return fn.parameters[p].name;
        }
        return fn.parameters[p].name;
    }
    std::string slotLocal(SlotId s) const { return "@m_slot_" + std::to_string(s); }
    std::string tempLocal(TempId t) const { return "@m_tmp_" + std::to_string(t); }
    std::string blockParamLocal(BlockParamId p) const { return "@m_bparam_" + std::to_string(p); }

    void pushOperand(const Function& fn, Operand op, Body& body, std::size_t line) {
        switch (op.kind) {
            case OperandKind::Const:
                if (op.index >= constIndex_.size() ||
                    constIndex_[op.index] == std::numeric_limits<std::size_t>::max())
                    throw std::runtime_error("MIR backend: missing constant #" + std::to_string(op.index));
                body.emit(OpCode::PushConst, constIndex_[op.index], line);
                return;
            case OperandKind::Param:
                body.emit(OpCode::LoadVar, addName(paramLocal(fn, op.index)), line);
                return;
            case OperandKind::Temp:
                body.emit(OpCode::LoadVar, addName(tempLocal(op.index)), line);
                return;
            case OperandKind::BlockParam:
                // A block parameter's local was written by the predecessor that
                // transferred here; reading it is the phi.
                body.emit(OpCode::LoadVar, addName(blockParamLocal(op.index)), line);
                return;
            case OperandKind::None:
                throw std::runtime_error("MIR backend: cannot push a None operand");
        }
    }

    void defineTemp(TempId temp, Body& body, std::size_t line) {
        if (temp == kNoTemp) return;
        body.emit(OpCode::DefineVar, addName(tempLocal(temp)), line);
    }

    // ---- instructions -----------------------------------------------------
    void emitInstruction(const Function& fn, BlockId blockId, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        (void)blockId;
        switch (ins.opcode) {
            case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
            case Opcode::Div: case Opcode::Mod: case Opcode::Pow:
            case Opcode::BitAnd: case Opcode::BitOr: case Opcode::BitXor:
            case Opcode::Shl: case Opcode::Shr: case Opcode::Ushr:
            case Opcode::Eq: case Opcode::Ne: case Opcode::Lt:
            case Opcode::Le: case Opcode::Gt: case Opcode::Ge: {
                pushOperand(fn, ins.operands[0], body, line);
                pushOperand(fn, ins.operands[1], body, line);
                body.emit(vmBinaryOp(ins.opcode), 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Neg: case Opcode::Not: case Opcode::BitNot: {
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(vmUnaryOp(ins.opcode), 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Widen: {
                // The VM stores numbers without a static type, so a widening is
                // the identity; the value just passes through.
                pushOperand(fn, ins.operands[0], body, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Refine: {
                // A refine is a runtime no-op for well-typed programs (the MIR
                // checker proved the narrowing). Skipping the check keeps this
                // backend from fabricating a type-assertion it cannot name.
                pushOperand(fn, ins.operands[0], body, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Load:
                body.emit(OpCode::LoadVar, addName(slotLocal(ins.slot)), line);
                defineTemp(ins.result, body, line);
                return;
            case Opcode::Store:
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::DefineVar, addName(slotLocal(ins.slot)), line);
                return;
            case Opcode::FieldLoad:
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::GetField, addName(ins.name), line);
                defineTemp(ins.result, body, line);
                return;
            case Opcode::FieldStore:
                pushOperand(fn, ins.operands[0], body, line);   // base
                pushOperand(fn, ins.operands[1], body, line);   // value
                body.emit(OpCode::SetField, addName(ins.name), line); // leaves value
                body.emit(OpCode::Pop, 0, line);
                return;
            case Opcode::IndexLoad:
                // Raw native collection access: list/array use Collection.get,
                // map uses Collection.mapGet. (A typed List/Map *object* is
                // always accessed through its methods, so it never reaches an
                // Index opcode.)
                emitCollectionIndexLoad(fn, ins, body);
                return;
            case Opcode::IndexStore:
                emitCollectionIndexStore(fn, ins, body);
                return;
            case Opcode::NewCollection:
                emitNewCollection(fn, ins, body);
                return;
            case Opcode::Alloc: {
                body.emit(OpCode::NewObject, addName(ins.className), line, 0, concreteTypeNameIndex(ins));
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Call: emitCall(fn, ins, body); return;
            case Opcode::InvokeMethod: emitInvokeMethod(fn, ins, body); return;
            case Opcode::InvokeSuper: emitInvokeSuper(fn, ins, body); return;
            case Opcode::InvokeStatic: emitInvokeStatic(fn, ins, body); return;
            case Opcode::CallIndirect: emitCallIndirect(fn, ins, body); return;
            case Opcode::CallNative: emitCallNative(fn, ins, body); return;
            case Opcode::RangeInBounds:
                pushOperand(fn, ins.operands[0], body, line); // current
                pushOperand(fn, ins.operands[1], body, line); // end
                pushOperand(fn, ins.operands[2], body, line); // step
                body.emit(OpCode::RangeContinue, 0, line);
                defineTemp(ins.result, body, line);
                return;
            case Opcode::Log:
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::Log, 0, line);
                return;
            default:
                throw std::runtime_error("MIR backend: unsupported opcode reached emission");
        }
    }

    // If an Alloc names a generic class, its Chunk name table index (+1, the
    // VM's NewObject convention) is the concrete instantiated type name (e.g.
    // "List<string>"). The VM parses that to record the object's runtime type
    // bindings, which is what lets a generic method body substitute its type
    // parameter when later type-checked. Plain classes need no type name
    // (operand3 = 0).
    std::size_t concreteTypeNameIndex(const Instruction& ins) {
        if (ins.typeArguments.empty()) return 0;
        const std::string concreteName = module_.types.render(ins.resultType);
        return addName(concreteName) + 1;
    }

    OpCode vmBinaryOp(Opcode op) const {
        switch (op) {
            case Opcode::Add: return OpCode::Add;
            case Opcode::Sub: return OpCode::Sub;
            case Opcode::Mul: return OpCode::Mul;
            case Opcode::Div: return OpCode::Div;
            case Opcode::Mod: return OpCode::Mod;
            case Opcode::Pow: return OpCode::Pow;
            case Opcode::BitAnd: return OpCode::BitAnd;
            case Opcode::BitOr: return OpCode::BitOr;
            case Opcode::BitXor: return OpCode::BitXor;
            case Opcode::Shl: return OpCode::Shl;
            case Opcode::Shr: return OpCode::Shr;
            case Opcode::Ushr: return OpCode::Ushr;
            case Opcode::Eq: return OpCode::Eq;
            case Opcode::Ne: return OpCode::Neq;
            case Opcode::Lt: return OpCode::Lt;
            case Opcode::Le: return OpCode::Lte;
            case Opcode::Gt: return OpCode::Gt;
            case Opcode::Ge: return OpCode::Gte;
            default: throw std::runtime_error("not a binary op");
        }
    }
    OpCode vmUnaryOp(Opcode op) const {
        switch (op) {
            case Opcode::Neg: return OpCode::Neg;
            case Opcode::Not: return OpCode::Not;
            case Opcode::BitNot: return OpCode::BitNot;
            default: throw std::runtime_error("not a unary op");
        }
    }

    // Direct calls: the MIR `Call` carries a receiver as operand 0 when the
    // callee has a `this` parameter.
    void emitCall(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        const FunctionId calleeId = ins.target.function;
        const Function* callee = module_.function(calleeId);
        if (!callee) throw std::runtime_error("MIR backend: bad direct call target");
        const bool calleeHasThis = callee->hasThisParameter;
        const std::size_t argBase = calleeHasThis ? 1 : 0;
        // Constructors are dispatched through the constructor slot on the
        // freshly-allocated receiver object (the VM has no direct-with-receiver
        // opcode).
        if (callee->isConstructor && calleeHasThis) {
            pushOperand(fn, ins.operands[0], body, line); // receiver (new object)
            for (std::size_t i = 1; i < ins.operands.size(); ++i)
                pushOperand(fn, ins.operands[i], body, line);
            const std::string key = dispatchKeyFor(*callee, module_.types);
            const std::size_t slot = requireSlot(key, ins.location);
            body.emit(OpCode::InvokeMethod, slot, line, ins.operands.size() - 1);
        } else if (calleeHasThis) {
            // A self-call: the VM injects `this` from the current frame, and the
            // operand 0 receiver is that same receiver. Assert we are the caller's
            // own receiver would require frame context; instead push the receiver
            // explicitly is not possible for plain Call, so we require operand 0 to
            // be this function's `this` parameter and drop it (the VM re-adds it).
            if (!isSelfReceiver(fn, ins.operands[0])) {
                throw std::runtime_error("MIR backend: direct instance call whose receiver "
                                         "is not `this` is not translatable");
            }
            for (std::size_t i = 1; i < ins.operands.size(); ++i)
                pushOperand(fn, ins.operands[i], body, line);
            body.emit(OpCode::Call, calleeId - 1, line);
        } else {
            for (std::size_t i = 0; i < ins.operands.size(); ++i)
                pushOperand(fn, ins.operands[i], body, line);
            body.emit(OpCode::Call, calleeId - 1, line);
        }
        // Result (the callee's return value is always pushed by the VM).
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    bool isSelfReceiver(const Function& fn, const Operand& op) const {
        if (!fn.hasThisParameter) return false;
        return op.kind == OperandKind::Param && op.index == 0;
    }

    void emitInvokeMethod(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        // operands: [receiver, args...]
        pushOperand(fn, ins.operands[0], body, line);
        for (std::size_t i = 1; i < ins.operands.size(); ++i)
            pushOperand(fn, ins.operands[i], body, line);
        const std::string className = ins.target.className.empty()
                                          ? runtimeTypeName(module_.types, ins.operands[0].type)
                                          : ins.target.className;
        const std::size_t slot =
            resolveMethodSlot(className, ins.target.methodName, ins.operands, ins.location);
        body.emit(OpCode::InvokeMethod, slot, line, ins.operands.size() - 1);
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    void emitInvokeSuper(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        pushOperand(fn, ins.operands[0], body, line); // receiver
        for (std::size_t i = 1; i < ins.operands.size(); ++i)
            pushOperand(fn, ins.operands[i], body, line);
        body.emit(OpCode::InvokeSuper, ins.target.function - 1, line, ins.operands.size() - 1);
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    void emitInvokeStatic(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        for (const Operand& op : ins.operands) pushOperand(fn, op, body, line);
        body.emit(OpCode::Call, ins.target.function - 1, line);
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    void emitCallIndirect(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        const std::size_t argCount = ins.operands.empty() ? 0 : ins.operands.size() - 1;
        for (std::size_t i = 0; i + 1 < ins.operands.size(); ++i)
            pushOperand(fn, ins.operands[i], body, line); // args
        pushOperand(fn, ins.operands.back(), body, line); // callee value on top
        body.emit(OpCode::CallValue, 0, line, argCount);
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    std::size_t nativeIndexByName(const std::string& name) const {
        auto byName = findNativeFunction(name);
        if (!byName) throw std::runtime_error("MIR backend: unknown native '" + name + "'");
        return *byName;
    }

    // list/array vs map vs set for a native collection typed operand.
    enum class NativeCollKind { List, Map, Set };
    NativeCollKind collKind(std::uint32_t typeId) const {
        const std::string render = module_.types.render(typeId);
        if (render.rfind("map", 0) == 0) return NativeCollKind::Map;
        if (render.rfind("set", 0) == 0) return NativeCollKind::Set;
        return NativeCollKind::List;
    }
    void emitNewCollection(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        const char* name = "Collection.newList";
        switch (collKind(ins.resultType)) {
            case NativeCollKind::Map: name = "Collection.newMap"; break;
            case NativeCollKind::Set: name = "Collection.newSet"; break;
            case NativeCollKind::List: name = "Collection.newList"; break;
        }
        body.emit(OpCode::CallNative, nativeIndexByName(name), line, 0);
        defineTemp(ins.result, body, line);
    }

    // IndexLoad: push (collection, key), call the getter, keep the result.
    void emitCollectionIndexLoad(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        const NativeCollKind kind = collKind(ins.operands[0].type);
        const char* getter = (kind == NativeCollKind::Map) ? "Collection.mapGet" : "Collection.get";
        pushOperand(fn, ins.operands[0], body, line);
        pushOperand(fn, ins.operands[1], body, line);
        body.emit(OpCode::CallNative, nativeIndexByName(getter), line, 0);
        defineTemp(ins.result, body, line);
    }

    // IndexStore writes element `key` of `collection`. In the MIR, a list is
    // only ever grown through IndexStore as part of building a literal (ZL has
    // no out-of-range list assignment; that goes through Collection.set
    // explicitly), so a list IndexStore appends with Collection.push. A map's
    // key/value write grows naturally, so it maps to Collection.mapSet.
    void emitCollectionIndexStore(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        const NativeCollKind kind = collKind(ins.operands[0].type);
        if (kind == NativeCollKind::Map) {
            pushOperand(fn, ins.operands[0], body, line);
            pushOperand(fn, ins.operands[1], body, line);
            pushOperand(fn, ins.operands[2], body, line);
            body.emit(OpCode::CallNative, nativeIndexByName("Collection.mapSet"), line, 0);
        } else {
            // list/array: append value (ignore the growing index).
            pushOperand(fn, ins.operands[0], body, line);
            pushOperand(fn, ins.operands[2], body, line);
            body.emit(OpCode::CallNative, nativeIndexByName("Collection.push"), line, 0);
        }
        body.emit(OpCode::Pop, 0, line);
    }

    void emitCallNative(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        for (const Operand& op : ins.operands) pushOperand(fn, op, body, line);
        std::size_t nativeIndex = 0;
        if (ins.target.nativeId >= 0) {
            auto byId = findNativeFunction(static_cast<NativeId>(ins.target.nativeId));
            if (byId) nativeIndex = *byId;
            else {
                auto byName = findNativeFunction(ins.target.nativeName);
                if (!byName)
                    throw std::runtime_error("MIR backend: unknown native '" + ins.target.nativeName + "'");
                nativeIndex = *byName;
            }
        } else {
            auto byName = findNativeFunction(ins.target.nativeName);
            if (!byName)
                throw std::runtime_error("MIR backend: unknown native '" + ins.target.nativeName + "'");
            nativeIndex = *byName;
        }
        body.emit(OpCode::CallNative, nativeIndex, line, 0);
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    // ---- terminators ------------------------------------------------------
    //
    // Block-parameter arguments are stored into the parameter's own local before
    // control transfers, one store per argument, in successor order. The VM has
    // no phi node, so the parameter's value is materialised as a local the
    // predecessor writes and the block reads - the memory form of the same
    // merge. Storing before the branch keeps the values the parameters name
    // visible in the target block no matter which edge is taken; the arguments
    // are already-computed operands, so evaluating them ahead of the branch
    // cannot reorder any effect the block itself performs.
    void emitEdgeArguments(const Function& fn, const Terminator& term, std::size_t successorIndex,
                           Body& body, std::size_t line) {
        const auto successors = term.successors();
        if (successorIndex >= successors.size()) return;
        const BasicBlock* target = fn.block(successors[successorIndex]);
        if (!target || target->parameters.empty()) return;
        const std::vector<Operand>& arguments = term.argumentsFor(successorIndex);
        if (arguments.size() != target->parameters.size())
            throw std::runtime_error("MIR backend: block b" + std::to_string(target->id) +
                                     " has " + std::to_string(target->parameters.size()) +
                                     " block parameter(s) but its incoming edge supplies " +
                                     std::to_string(arguments.size()));
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            pushOperand(fn, arguments[i], body, line);
            body.emit(OpCode::DefineVar, addName(blockParamLocal(target->parameters[i].id)), line);
        }
    }

    void emitTerminator(const Function& fn, const BasicBlock& block, Body& body) {
        const Terminator& term = block.terminator;
        const std::size_t line = term.location.line;
        switch (term.kind) {
            case TerminatorKind::Return: {
                if (term.value.isNone()) {
                    const std::size_t nilIdx = addConst(Value{});
                    body.emit(OpCode::PushConst, nilIdx, line);
                } else {
                    pushOperand(fn, term.value, body, line);
                }
                body.emit(OpCode::Return, 0, line);
                return;
            }
            case TerminatorKind::Jump: {
                emitEdgeArguments(fn, term, 0, body, line);
                emitJumpTo(term.target, body, line);
                return;
            }
            case TerminatorKind::Branch: {
                // Arguments first, then the condition: the condition has to end
                // up on top of the stack for the jump, and the arguments are
                // pure reads so their order relative to it does not matter.
                emitEdgeArguments(fn, term, 0, body, line);
                emitEdgeArguments(fn, term, 1, body, line);
                pushOperand(fn, term.value, body, line);
                emitJumpIfFalse(term.elseBlock, body, line);
                emitJumpTo(term.target, body, line);
                return;
            }
            case TerminatorKind::Switch: {
                // A multi-way terminator is not translatable to this VM's
                // single jump-if-false; the opcode gate rejects it before we
                // get here, so reaching this is a translator bug, not user
                // input. Fail loudly rather than emit a wrong transfer.
                throw std::runtime_error(
                    "MIR backend: switch terminator is not translatable to this bytecode");
            }
            case TerminatorKind::Throw: {
                pushOperand(fn, term.value, body, line);
                body.emit(OpCode::Throw, 0, line);
                return;
            }
            default:
                // Unreachable / switch: no bytecode transfer; a block that is
                // statically unreachable needs no emitted transfer.
                return;
        }
    }

    void emitJumpTo(BlockId target, Body& body, std::size_t line) {
        const auto it = body.label.find(target);
        if (it != body.label.end()) {
            body.emit(OpCode::Jump, it->second, line);
        } else {
            const std::size_t index = body.emit(OpCode::Jump, 0, line);
            body.patches.emplace_back(index, target);
        }
    }
    void emitJumpIfFalse(BlockId target, Body& body, std::size_t line) {
        const auto it = body.label.find(target);
        if (it != body.label.end()) {
            body.emit(OpCode::JumpIfFalse, it->second, line);
        } else {
            const std::size_t index = body.emit(OpCode::JumpIfFalse, 0, line);
            body.patches.emplace_back(index, target);
        }
    }

    // ---- slot helpers -----------------------------------------------------
    std::size_t requireSlot(const std::string& key, const SourceLocation&) const {
        const auto it = slotByDispatchKey_.find(key);
        if (it == slotByDispatchKey_.end())
            throw std::runtime_error("MIR backend: no dispatch slot for '" + key + "'");
        return it->second;
    }

    // MIR records an instance method's full declared signature as its
    // `simpleName` (e.g. "push(object)") but the InvokeMethod site only names
    // the unqualified token ("push"). Extract that token for matching.
    static std::string methodToken(const std::string& simpleName) {
        const std::size_t paren = simpleName.find('(');
        return paren == std::string::npos ? simpleName : simpleName.substr(0, paren);
    }

    // Resolves the method's dispatch slot by finding the method declared on
    // `className` (or an ancestor) whose name-token and parameter count match
    // the call site, then returns its slot. Overloads differing only in the
    // static types of the arguments are disambiguated by comparing the MIR
    // parameter type ids to the actual operand type ids when that is decisive.
    std::size_t resolveMethodSlot(const std::string& className, const std::string& methodName,
                                  const std::vector<Operand>& args, const SourceLocation& loc) const {
        const std::size_t argCount = args.empty() ? 0 : args.size() - 1; // args[0] is the receiver
        std::string cur = className;
        std::unordered_map<std::string, std::string> parents;
        for (const ClassLayout& cls : module_.classes) parents[cls.name] = cls.parent;
        for (std::size_t guard = 0; guard < 64 && !cur.empty(); ++guard) {
            const Function* found = findDeclaredMethod(cur, methodName, argCount, args);
            if (found) return requireSlot(dispatchKeyFor(*found, module_.types), loc);
            const auto pit = parents.find(cur);
            cur = (pit != parents.end()) ? pit->second : std::string();
        }
        throw std::runtime_error("MIR backend: cannot resolve method '" + className + "." +
                                 methodName + "' to a dispatch slot");
    }

    // Candidates on `className` with this method-token and `argCount` explicit
    // parameters. When several share the token (genuine overloads) but only one
    // also agrees on the static operand types, that one is chosen.
    const Function* findDeclaredMethod(const std::string& className, const std::string& methodName,
                                       std::size_t argCount,
                                       const std::vector<Operand>& args) const {
        const Function* chosen = nullptr;
        std::size_t arityMatch = 0;
        for (const Function& fn : module_.functions) {
            if (fn.ownerClass != className || !fn.hasThisParameter || fn.isConstructor) continue;
            if (methodToken(fn.simpleName) != methodName) continue;
            const std::size_t explicitParams = fn.parameters.size() - 1; // drop `this`
            if (explicitParams != argCount) continue;                    // arity must agree
            ++arityMatch;
            if (arityMatch == 1) chosen = &fn; // first arity match is provisional
        }
        if (arityMatch == 0) return nullptr;
        if (arityMatch == 1) return chosen;

        // Several overloads share the arity. Narrow by static operand types:
        // a parameter matches the corresponding call operand when their MIR
        // type ids are equal (operands carry their static type).
        for (const Function& fn : module_.functions) {
            if (fn.ownerClass != className || !fn.hasThisParameter || fn.isConstructor) continue;
            if (methodToken(fn.simpleName) != methodName) continue;
            if (fn.parameters.size() - 1 != argCount) continue;
            bool allMatch = true;
            for (std::size_t i = 0; i < argCount; ++i) {
                const std::uint32_t paramType = fn.parameters[i + 1].type;
                const std::uint32_t argType = args[i + 1].type;
                if (paramType != argType) { allMatch = false; break; }
            }
            if (allMatch) return &fn;
        }
        return nullptr; // ambiguous overload -> caller fails closed
    }

    // ---- stub + entry -----------------------------------------------------
    void emitStub() {
        const std::size_t line = 0;
        const std::size_t msg = addConst(Value(std::string(
            "MIR backend: function not translatable by this backend (unsupported construct)")));
        emitGlobal(OpCode::PushConst, msg, line);
        // Throwing a non-object is a loud runtime error, which is what we want
        // if a stubbed function is ever actually reached.
        emitGlobal(OpCode::Throw, 0, line);
    }

    void compileEntry(std::size_t skipJump) {
        const Function& main = module_.functions[module_.entryPoint - 1];
        const std::size_t mainIndex = module_.entryPoint - 1;
        const std::size_t line = main.location.line;
        const bool hasThis = main.hasThisParameter;
        const std::size_t declared = main.parameters.size() - (hasThis ? 1 : 0);
        if (declared > 1)
            throw std::runtime_error("MIR backend: entry function can take at most one parameter");
        const std::size_t entryStart = chunk_.code.size();
        if (declared == 1) emitGlobal(OpCode::PushProgramArgs, 0, line);
        emitGlobal(OpCode::Call, mainIndex, line);
        emitGlobal(OpCode::Pop, 0, line);
        emitGlobal(OpCode::Halt);
        // Patch the leading skip jump to land here.
        chunk_.code[skipJump].operand = entryStart;
    }
};

} // namespace

BytecodeResult compileModuleToBytecode(const Module& module) {
    MirBytecodeTranslator translator(module);
    return translator.translate();
}

} // namespace zl::mir
