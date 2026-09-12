#include "zl/mir/function.hpp"

#include <algorithm>
#include <sstream>

namespace zl::mir {

const char* blockKindName(BlockKind kind) noexcept {
    switch (kind) {
        case BlockKind::Normal: return "normal";
        case BlockKind::Catch: return "catch";
        case BlockKind::Cleanup: return "cleanup";
    }
    return "<invalid-block-kind>";
}

const char* exceptionBehaviorName(ExceptionBehavior behavior) noexcept {
    switch (behavior) {
        case ExceptionBehavior::Unknown: return "unknown";
        case ExceptionBehavior::NoThrow: return "nothrow";
        case ExceptionBehavior::MayThrow: return "maythrow";
    }
    return "<invalid-exception-behavior>";
}

std::string SourceLocation::describe() const {
    std::ostringstream out;
    if (!file.empty()) out << file << ":";
    out << "line " << line;
    if (column != 0) out << ":" << column;
    return out.str();
}

const char* constKindName(ConstKind kind) noexcept {
    switch (kind) {
        case ConstKind::Bool: return "bool";
        case ConstKind::Int: return "int";
        case ConstKind::Double: return "double";
        case ConstKind::String: return "string";
        case ConstKind::Nil: return "nil";
        case ConstKind::EnumMember: return "enum";
    }
    return "<invalid-const>";
}

const char* operandKindName(OperandKind kind) noexcept {
    switch (kind) {
        case OperandKind::None: return "none";
        case OperandKind::Const: return "const";
        case OperandKind::Temp: return "temp";
        case OperandKind::Param: return "param";
        case OperandKind::BlockParam: return "block-param";
        case OperandKind::Static: return "static";
    }
    return "<invalid-operand>";
}

const BasicBlock* Function::block(BlockId id) const {
    if (id == kNoBlock) return nullptr;
    const auto it = std::find_if(blocks.begin(), blocks.end(),
                                 [id](const BasicBlock& b) { return b.id == id; });
    return it == blocks.end() ? nullptr : &*it;
}

BasicBlock* Function::block(BlockId id) {
    if (id == kNoBlock) return nullptr;
    const auto it = std::find_if(blocks.begin(), blocks.end(),
                                 [id](const BasicBlock& b) { return b.id == id; });
    return it == blocks.end() ? nullptr : &*it;
}

const Parameter* Function::parameter(ParamId id) const {
    return id < parameters.size() ? &parameters[id] : nullptr;
}

const Slot* Function::slot(SlotId id) const {
    // Slot ids are 1-based so that a zero-initialised SlotId is unambiguously
    // "no slot" rather than slot zero.
    if (id == 0 || id > slots.size()) return nullptr;
    return &slots[id - 1];
}

const BlockParameter* Function::blockParameter(BlockParamId id) const {
    if (id == kNoBlockParam) return nullptr;
    for (const auto& block : blocks) {
        for (const auto& parameter : block.parameters) {
            if (parameter.id == id) return &parameter;
        }
    }
    return nullptr;
}

const BasicBlock* Function::blockOfParameter(BlockParamId id) const {
    if (id == kNoBlockParam) return nullptr;
    for (const auto& block : blocks) {
        for (const auto& parameter : block.parameters) {
            if (parameter.id == id) return &block;
        }
    }
    return nullptr;
}

BlockParamId Function::nextBlockParameterId() const {
    BlockParamId next = 1;
    for (const auto& block : blocks) {
        for (const auto& parameter : block.parameters) {
            if (parameter.id >= next) next = parameter.id + 1;
        }
    }
    return next;
}

void Function::rebuildEdges() {
    edges.clear();
    for (const auto& block : blocks) {
        for (BlockId successor : block.terminator.successors()) {
            edges.push_back(ControlFlowEdge{block.id, successor, EdgeKind::Normal});
        }
        for (const auto& handler : block.exceptionHandlers) {
            if (handler.block == kNoBlock) continue;
            edges.push_back(ControlFlowEdge{block.id, handler.block, EdgeKind::Unwind});
        }
    }
    // Deterministic order: by source block, then by kind, then by target. Two
    // builds of the same function must produce a byte-identical edge list, or
    // the verifier's predecessor-consistency check would be order-sensitive.
    std::stable_sort(edges.begin(), edges.end(), [](const ControlFlowEdge& a, const ControlFlowEdge& b) {
        if (a.from != b.from) return a.from < b.from;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.to < b.to;
    });
}

const FieldLayout* ClassLayout::field(const std::string& fieldName) const {
    const auto it = std::find_if(fields.begin(), fields.end(),
                                 [&](const FieldLayout& f) { return f.name == fieldName; });
    return it == fields.end() ? nullptr : &*it;
}

const Function* Module::function(FunctionId id) const {
    if (id == kNoFunction || id > functions.size()) return nullptr;
    return &functions[id - 1];
}

Function* Module::function(FunctionId id) {
    if (id == kNoFunction || id > functions.size()) return nullptr;
    return &functions[id - 1];
}

const ClassLayout* Module::classLayout(const std::string& name) const {
    const auto it = std::find_if(classes.begin(), classes.end(),
                                 [&](const ClassLayout& c) { return c.name == name; });
    return it == classes.end() ? nullptr : &*it;
}

std::string Function::declaredName() const {
    const auto open = simpleName.find('(');
    return open == std::string::npos ? simpleName : simpleName.substr(0, open);
}

const InterfaceMethod* InterfaceInfo::method(const std::string& methodName) const {
    const auto it = std::find_if(methods.begin(), methods.end(),
                                 [&](const InterfaceMethod& m) { return m.name == methodName; });
    return it == methods.end() ? nullptr : &*it;
}

const InterfaceInfo* Module::interfaceInfo(const std::string& name) const {
    const auto it = std::find_if(interfaces.begin(), interfaces.end(),
                                 [&](const InterfaceInfo& i) { return i.name == name; });
    return it == interfaces.end() ? nullptr : &*it;
}

const Constant* Module::constant(ConstId id) const {
    if (id == kNoConst || id > constants.size()) return nullptr;
    return &constants[id - 1];
}

const StaticField* Module::staticField(StaticId id) const {
    if (id == kNoStatic || id > statics.size()) return nullptr;
    return &statics[id - 1];
}

ConstId Module::internConstant(const Constant& value) {
    const auto it = std::find(constants.begin(), constants.end(), value);
    if (it != constants.end()) {
        return static_cast<ConstId>(std::distance(constants.begin(), it) + 1);
    }
    constants.push_back(value);
    return static_cast<ConstId>(constants.size());
}

} // namespace zl::mir
