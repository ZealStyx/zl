#include "zl/mir/printer.hpp"

#include <sstream>

namespace zl::mir {
namespace {

std::string renderConstant(const Module& module, ConstId id) {
    const Constant* constant = module.constant(id);
    if (!constant) return "<bad-const:" + std::to_string(id) + ">";
    switch (constant->kind) {
        case ConstKind::Bool: return constant->boolValue ? "true" : "false";
        case ConstKind::Int: return std::to_string(constant->intValue);
        case ConstKind::Double: {
            std::ostringstream out;
            out << constant->doubleValue;
            return out.str();
        }
        case ConstKind::String: return "\"" + constant->stringValue + "\"";
        case ConstKind::Nil: return "nil";
        case ConstKind::EnumMember: return constant->enumTypeName + "." + constant->stringValue;
    }
    return "<bad-const>";
}

std::string locationSuffix(const SourceLocation& location) {
    if (!location.valid()) return {};
    return "  ; " + location.describe();
}

} // namespace

std::string printOperand(const Module& module, const Operand& operand) {
    std::ostringstream out;
    switch (operand.kind) {
        case OperandKind::None:
            out << "_";
            break;
        case OperandKind::Const:
            out << renderConstant(module, operand.index);
            break;
        case OperandKind::Temp:
            out << "%" << operand.index;
            break;
        case OperandKind::Param:
            out << "$" << operand.index;
            break;
        case OperandKind::BlockParam:
            // `^` marks a block parameter, the phi of the IR.
            out << "^" << operand.index;
            break;
        case OperandKind::Static: {
            const StaticField* field = module.staticField(operand.index);
            out << (field ? "@" + field->className + "." + field->name
                          : "@<bad-static:" + std::to_string(operand.index) + ">");
            break;
        }
    }
    out << ":" << module.types.render(operand.type);
    return out.str();
}

std::string printInstruction(const Module& module, const Function& function, const Instruction& instruction) {
    std::ostringstream out;
    out << "    ";
    if (instruction.result != kNoTemp) {
        out << "%" << instruction.result << ":" << module.types.render(instruction.resultType) << " = ";
    }
    out << opcodeName(instruction.opcode);

    bool first = true;
    auto separator = [&] {
        if (first) { first = false; return std::string(" "); }
        return std::string(", ");
    };

    if (opcodeUsesSlot(instruction.opcode)) {
        const Slot* slot = function.slot(instruction.slot);
        out << separator() << "slot " << instruction.slot << "('" << (slot ? slot->name : "?") << "')";
    }
    if (!instruction.className.empty()) {
        out << separator() << "class '" << instruction.className << "'";
    }
    if (!instruction.target.className.empty()) {
        out << separator() << "on '" << instruction.target.className << "'";
    }
    if (!instruction.target.methodName.empty()) {
        out << separator() << "method '" << instruction.target.methodName << "'";
    }
    if (instruction.target.function != kNoFunction) {
        const Function* callee = module.function(instruction.target.function);
        out << separator() << "fn " << instruction.target.function
            << "('" << (callee ? callee->name : "?") << "')";
    }
    if (!instruction.target.nativeName.empty()) {
        out << separator() << "native '" << instruction.target.nativeName << "'";
    }
    if (!instruction.name.empty()) {
        out << separator() << "'" << instruction.name << "'";
    }
    if (instruction.testedType != kNoType) {
        out << separator() << "as " << module.types.render(instruction.testedType);
    }

    for (const auto& operand : instruction.operands) {
        out << separator() << printOperand(module, operand);
    }
    return out.str();
}

namespace {

// `b7(%3:int, ^2:int)` - a successor plus the block-parameter arguments handed
// to it on this edge. Plain `b7` when the edge carries no arguments, so the
// common case stays uncluttered.
std::string edgeWithArguments(const Module& module, BlockId target, const std::vector<Operand>& arguments) {
    std::ostringstream out;
    out << "b" << target;
    if (arguments.empty()) return out.str();
    out << "(";
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i) out << ", ";
        out << printOperand(module, arguments[i]);
    }
    out << ")";
    return out.str();
}

} // namespace

std::string printTerminator(const Module& module, const Terminator& terminator) {
    std::ostringstream out;
    out << "    ";
    switch (terminator.kind) {
        case TerminatorKind::None:
            out << "<no terminator>";
            break;
        case TerminatorKind::Return:
            out << "return";
            if (!terminator.value.isNone()) out << " " << printOperand(module, terminator.value);
            break;
        case TerminatorKind::Jump:
            out << "jump " << edgeWithArguments(module, terminator.target,
                                                terminator.argumentsFor(0));
            break;
        case TerminatorKind::Branch:
            out << "branch " << printOperand(module, terminator.value) << ", "
                << edgeWithArguments(module, terminator.target, terminator.argumentsFor(0)) << ", "
                << edgeWithArguments(module, terminator.elseBlock, terminator.argumentsFor(1));
            break;
        case TerminatorKind::Switch: {
            out << "switch " << printOperand(module, terminator.value);
            for (std::size_t i = 0; i < terminator.cases.size(); ++i) {
                out << ", " << terminator.cases[i].value << " => "
                    << edgeWithArguments(module, terminator.cases[i].block,
                                         terminator.argumentsFor(i));
            }
            out << ", default => "
                << edgeWithArguments(module, terminator.target,
                                     terminator.argumentsFor(terminator.cases.size()));
            break;
        }
        case TerminatorKind::Throw:
            out << "throw " << printOperand(module, terminator.value);
            break;
        case TerminatorKind::Unreachable:
            out << "unreachable";
            break;
    }
    return out.str();
}

std::string printFunction(const Module& module, const Function& function, const PrinterOptions& options) {
    std::ostringstream out;

    out << "func " << function.name << "(";
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        if (i) out << ", ";
        out << "$" << i << " " << function.parameters[i].name << ":"
            << module.types.render(function.parameters[i].type);
        if (function.parameters[i].ownership != zl::OwnershipKind::GC) {
            out << " [" << zl::ownershipName(function.parameters[i].ownership) << "]";
        }
    }
    out << ")";
    if (function.returnOwnership != zl::OwnershipKind::GC) {
        out << " [" << zl::ownershipName(function.returnOwnership) << "]";
    }
    out << ": " << module.types.render(function.returnType);

    std::vector<std::string> flags;
    if (function.isAsync) flags.push_back("async");
    if (function.isNative) flags.push_back("native");
    if (function.isConstructor) flags.push_back("constructor");
    if (function.isStatic) flags.push_back("static");
    if (function.isLambda) flags.push_back("lambda");
    if (function.isOperator) flags.push_back("operator");
    if (function.isGenericTemplate) flags.push_back("generic-template");
    if (function.incomplete) flags.push_back("incomplete");
    if (!flags.empty()) {
        out << "  ;";
        for (const auto& flag : flags) out << " " << flag;
    }
    out << "\n";

    if (function.isGenericTemplate) {
        out << "  type-params:";
        for (const auto& parameter : function.typeParameters) out << " " << parameter;
        out << "\n";
    }
    if (function.exceptionBehavior != ExceptionBehavior::Unknown) {
        out << "  exceptions: " << exceptionBehaviorName(function.exceptionBehavior) << "\n";
    }
    if (options.locations && function.location.valid()) {
        out << "  at " << function.location.describe() << "\n";
    }
    for (const auto& reason : function.incompleteReasons) {
        out << "  incomplete: " << reason << "\n";
    }

    if (options.declarations && !function.slots.empty()) {
        out << "  slots:\n";
        for (std::size_t i = 0; i < function.slots.size(); ++i) {
            const auto& slot = function.slots[i];
            out << "    s" << (i + 1) << " " << slot.name << ":" << module.types.render(slot.type);
            if (!slot.isMutable) out << " let";
            if (slot.ownership != zl::OwnershipKind::GC) out << " [" << zl::ownershipName(slot.ownership) << "]";
            if (slot.isCatchBinding) out << " catch";
            out << "\n";
        }
    }

    for (const auto& block : function.blocks) {
        out << "  b" << block.id;
        if (block.kind != BlockKind::Normal) out << " [" << blockKindName(block.kind) << "]";
        if (function.entryBlock == block.id) out << " (entry)";
        // Block parameters: the merged values this block is entered with.
        if (!block.parameters.empty()) {
            out << "(";
            for (std::size_t i = 0; i < block.parameters.size(); ++i) {
                if (i) out << ", ";
                const auto& parameter = block.parameters[i];
                out << "^" << parameter.id << " " << parameter.name << ":"
                    << module.types.render(parameter.type);
            }
            out << ")";
        }
        out << ":\n";
        for (const auto& handler : block.exceptionHandlers) {
            out << "    handler ";
            out << (handler.catchType ? module.types.render(handler.catchType) : std::string("any"));
            out << " => b" << handler.block;
            if (handler.catchSlot) out << " into s" << handler.catchSlot;
            out << "\n";
        }
        for (const auto& instruction : block.instructions) {
            out << printInstruction(module, function, instruction);
            if (options.locations) out << locationSuffix(instruction.location);
            out << "\n";
        }
        out << printTerminator(module, block.terminator);
        if (options.locations) out << locationSuffix(block.terminator.location);
        out << "\n";
    }

    if (options.edges && !function.edges.empty()) {
        out << "  edges:";
        for (const auto& edge : function.edges) {
            out << " b" << edge.from << "->b" << edge.to;
            if (edge.kind == EdgeKind::Unwind) out << "(unwind)";
        }
        out << "\n";
    }
    return out.str();
}

std::string printModule(const Module& module, const PrinterOptions& options) {
    std::ostringstream out;
    out << "mir module" << (module.name.empty() ? "" : " '" + module.name + "'") << "\n";

    if (options.declarations) {
        out << "  types:\n";
        for (std::uint32_t id = 1; id <= module.types.size() - 1; ++id) {
            out << "    t" << id << " = " << module.types.render(id) << "\n";
        }
        if (!module.constants.empty()) {
            out << "  constants:\n";
            for (std::size_t i = 0; i < module.constants.size(); ++i) {
                out << "    c" << (i + 1) << " = " << renderConstant(module, static_cast<ConstId>(i + 1)) << "\n";
            }
        }
        if (!module.statics.empty()) {
            out << "  statics:\n";
            for (const auto& field : module.statics) {
                out << "    @" << field.className << "." << field.name << ":"
                    << module.types.render(field.type) << "\n";
            }
        }
        if (!module.interfaces.empty()) {
            out << "  interfaces:\n";
            for (const auto& info : module.interfaces) {
                out << "    " << info.name;
                if (!info.bases.empty()) {
                    out << " extends ";
                    for (std::size_t i = 0; i < info.bases.size(); ++i) {
                        if (i) out << ", ";
                        out << info.bases[i];
                    }
                }
                out << "\n";
                for (const auto& method : info.methods) {
                    out << "      " << method.name << "(";
                    for (std::size_t i = 0; i < method.parameterTypes.size(); ++i) {
                        if (i) out << ", ";
                        out << module.types.render(method.parameterTypes[i]);
                    }
                    out << "): " << module.types.render(method.returnType) << "\n";
                }
            }
        }
        if (!module.classes.empty()) {
            out << "  classes:\n";
            for (const auto& layout : module.classes) {
                out << "    " << layout.name;
                if (!layout.typeParameters.empty()) {
                    out << "<";
                    for (std::size_t i = 0; i < layout.typeParameters.size(); ++i) {
                        if (i) out << ",";
                        out << layout.typeParameters[i];
                    }
                    out << ">";
                }
                if (!layout.parent.empty()) out << " extends " << layout.parent;
                if (!layout.interfaces.empty()) {
                    out << " implements ";
                    for (std::size_t i = 0; i < layout.interfaces.size(); ++i) {
                        if (i) out << ", ";
                        out << layout.interfaces[i];
                    }
                }
                if (layout.isData) out << " [data]";
                if (layout.isEnum) out << " [enum]";
                out << "\n";
                for (const auto& field : layout.fields) {
                    out << "      field " << field.name << ":" << module.types.render(field.type);
                    if (field.isStatic) out << " [static]";
                    out << "\n";
                }
            }
        }
    }

    if (module.entryPoint != kNoFunction) {
        const Function* entry = module.function(module.entryPoint);
        out << "  entry: " << (entry ? entry->name : "<invalid>") << "\n";
    }

    out << "\n";
    for (const auto& function : module.functions) {
        out << printFunction(module, function, options) << "\n";
    }
    return out.str();
}

} // namespace zl::mir
