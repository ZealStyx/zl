#include "zl/mir/instruction.hpp"

#include <array>

namespace zl::mir {

const char* opcodeName(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Nop: return "nop";
        case Opcode::Add: return "add";
        case Opcode::Sub: return "sub";
        case Opcode::Mul: return "mul";
        case Opcode::Div: return "div";
        case Opcode::Mod: return "mod";
        case Opcode::Pow: return "pow";
        case Opcode::Neg: return "neg";
        case Opcode::BitAnd: return "bit_and";
        case Opcode::BitOr: return "bit_or";
        case Opcode::BitXor: return "bit_xor";
        case Opcode::BitNot: return "bit_not";
        case Opcode::Shl: return "shl";
        case Opcode::Shr: return "shr";
        case Opcode::Ushr: return "ushr";
        case Opcode::Eq: return "eq";
        case Opcode::Ne: return "ne";
        case Opcode::Lt: return "lt";
        case Opcode::Le: return "le";
        case Opcode::Gt: return "gt";
        case Opcode::Ge: return "ge";
        case Opcode::Not: return "not";
        case Opcode::And: return "and";
        case Opcode::Or: return "or";
        case Opcode::Widen: return "widen";
        case Opcode::Refine: return "refine";
        case Opcode::TypeTest: return "type_test";
        case Opcode::NullCheck: return "null_check";
        case Opcode::IsNull: return "is_null";
        case Opcode::Load: return "load";
        case Opcode::Store: return "store";
        case Opcode::FieldLoad: return "field_load";
        case Opcode::FieldStore: return "field_store";
        case Opcode::IndexLoad: return "index_load";
        case Opcode::IndexStore: return "index_store";
        case Opcode::StaticLoad: return "static_load";
        case Opcode::StaticStore: return "static_store";
        case Opcode::Alloc: return "alloc";
        case Opcode::Call: return "call";
        case Opcode::InvokeMethod: return "invoke_method";
        case Opcode::InvokeSuper: return "invoke_super";
        case Opcode::InvokeStatic: return "invoke_static";
        case Opcode::CallIndirect: return "call_indirect";
        case Opcode::CallNative: return "call_native";
        case Opcode::MakeClosure: return "make_closure";
        case Opcode::Await: return "await";
        case Opcode::TaskCreate: return "task_create";
        case Opcode::Move: return "move";
        case Opcode::Borrow: return "borrow";
        case Opcode::EndBorrow: return "end_borrow";
        case Opcode::Drop: return "drop";
        case Opcode::NewCollection: return "new_collection";
        case Opcode::RangeInBounds: return "range_in_bounds";
        case Opcode::Log: return "log";
    }
    return "<invalid-opcode>";
}

const char* terminatorKindName(TerminatorKind kind) noexcept {
    switch (kind) {
        case TerminatorKind::None: return "none";
        case TerminatorKind::Return: return "return";
        case TerminatorKind::Jump: return "jump";
        case TerminatorKind::Branch: return "branch";
        case TerminatorKind::Switch: return "switch";
        case TerminatorKind::Throw: return "throw";
        case TerminatorKind::Unreachable: return "unreachable";
    }
    return "<invalid-terminator>";
}

const OpcodeShape& opcodeShape(Opcode opcode) noexcept {
    // Built once. The table is indexed by the numeric opcode, so every
    // enumerator must appear here; the default entry (Nop's shape) makes a
    // missing row detectable as "claims zero operands and no result", which the
    // verifier rejects for any opcode that is used with operands.
    static const auto table = [] {
        constexpr std::size_t count = static_cast<std::size_t>(Opcode::Log) + 1;
        std::array<OpcodeShape, count> shapes{};
        auto set = [&](Opcode op, std::uint8_t operands, bool variadic, bool result, bool throws_, bool effects) {
            shapes[static_cast<std::size_t>(op)] = OpcodeShape{op, operands, variadic, result, throws_, effects};
        };

        set(Opcode::Nop, 0, false, false, false, false);

        set(Opcode::Add, 2, false, true, false, false);
        set(Opcode::Sub, 2, false, true, false, false);
        set(Opcode::Mul, 2, false, true, false, false);
        set(Opcode::Div, 2, false, true, true, false);
        set(Opcode::Mod, 2, false, true, true, false);
        set(Opcode::Pow, 2, false, true, false, false);
        set(Opcode::Neg, 1, false, true, false, false);

        set(Opcode::BitAnd, 2, false, true, false, false);
        set(Opcode::BitOr, 2, false, true, false, false);
        set(Opcode::BitXor, 2, false, true, false, false);
        set(Opcode::BitNot, 1, false, true, false, false);
        set(Opcode::Shl, 2, false, true, false, false);
        set(Opcode::Shr, 2, false, true, false, false);
        set(Opcode::Ushr, 2, false, true, false, false);

        set(Opcode::Eq, 2, false, true, false, false);
        set(Opcode::Ne, 2, false, true, false, false);
        set(Opcode::Lt, 2, false, true, false, false);
        set(Opcode::Le, 2, false, true, false, false);
        set(Opcode::Gt, 2, false, true, false, false);
        set(Opcode::Ge, 2, false, true, false, false);

        set(Opcode::Not, 1, false, true, false, false);
        set(Opcode::And, 2, false, true, false, false);
        set(Opcode::Or, 2, false, true, false, false);

        set(Opcode::Widen, 1, false, true, false, false);
        set(Opcode::Refine, 1, false, true, true, false);
        set(Opcode::TypeTest, 1, false, true, false, false);
        set(Opcode::NullCheck, 1, false, true, true, false);
        set(Opcode::IsNull, 1, false, true, false, false);

        set(Opcode::Load, 0, false, true, false, false);
        set(Opcode::Store, 1, false, false, false, true);

        set(Opcode::FieldLoad, 1, false, true, true, false);
        set(Opcode::FieldStore, 2, false, false, true, true);
        set(Opcode::IndexLoad, 2, false, true, true, false);
        set(Opcode::IndexStore, 3, false, false, true, true);
        set(Opcode::StaticLoad, 0, false, true, false, false);
        set(Opcode::StaticStore, 1, false, false, false, true);

        set(Opcode::Alloc, 0, false, true, false, false);

        set(Opcode::Call, 0, true, true, true, true);
        set(Opcode::InvokeMethod, 1, true, true, true, true);
        set(Opcode::InvokeSuper, 1, true, true, true, true);
        set(Opcode::InvokeStatic, 0, true, true, true, true);
        set(Opcode::CallIndirect, 1, true, true, true, true);
        set(Opcode::CallNative, 0, true, true, true, true);
        set(Opcode::MakeClosure, 0, true, true, false, false);

        set(Opcode::Await, 1, false, true, true, true);
        set(Opcode::TaskCreate, 1, false, true, false, false);

        set(Opcode::Move, 0, false, true, false, true);
        set(Opcode::Borrow, 1, false, false, false, true);
        set(Opcode::EndBorrow, 0, false, false, false, true);
        set(Opcode::Drop, 1, false, false, false, true);

        set(Opcode::NewCollection, 0, false, true, false, false);
        set(Opcode::RangeInBounds, 3, false, true, true, false);
        set(Opcode::Log, 1, false, false, false, true);

        // A call defines a temp unless the callee returns void; an `await`
        // defines one unless the task's payload is void. Marked here rather than
        // special-cased in the verifier so the table stays the single answer to
        // "does this opcode produce a value?".
        for (const Opcode opcode : {Opcode::Call, Opcode::InvokeMethod, Opcode::InvokeSuper,
                                    Opcode::InvokeStatic, Opcode::CallIndirect, Opcode::CallNative,
                                    Opcode::Await}) {
            shapes[static_cast<std::size_t>(opcode)].optionalResult = true;
        }
        return shapes;
    }();

    const auto index = static_cast<std::size_t>(opcode);
    if (index >= table.size()) {
        static const OpcodeShape invalid{Opcode::Nop, 0, false, false, false, false, false};
        return invalid;
    }
    return table[index];
}

bool opcodeUsesSlot(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Load:
        case Opcode::Store:
        case Opcode::Move:
        case Opcode::Borrow:
        case Opcode::EndBorrow:
            return true;
        default:
            return false;
    }
}

bool opcodeIsCall(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Call:
        case Opcode::InvokeMethod:
        case Opcode::InvokeSuper:
        case Opcode::InvokeStatic:
        case Opcode::CallIndirect:
        case Opcode::CallNative:
            return true;
        default:
            return false;
    }
}

std::vector<BlockId> Terminator::successors() const {
    std::vector<BlockId> out;
    switch (kind) {
        case TerminatorKind::Jump:
            if (target != kNoBlock) out.push_back(target);
            break;
        case TerminatorKind::Branch:
            if (target != kNoBlock) out.push_back(target);
            if (elseBlock != kNoBlock) out.push_back(elseBlock);
            break;
        case TerminatorKind::Switch:
            for (const auto& entry : cases) {
                if (entry.block != kNoBlock) out.push_back(entry.block);
            }
            if (target != kNoBlock) out.push_back(target);
            break;
        case TerminatorKind::None:
        case TerminatorKind::Return:
        case TerminatorKind::Throw:
        case TerminatorKind::Unreachable:
            break;
    }
    return out;
}

} // namespace zl::mir
