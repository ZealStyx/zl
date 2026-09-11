#include "zl/mir/folding.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include "zl/mir/effects.hpp"

namespace zl::mir {
namespace {

#if defined(__GNUC__) || defined(__clang__)
bool addChecked(std::int64_t a, std::int64_t b, std::int64_t& out) { return !__builtin_add_overflow(a, b, &out); }
bool subChecked(std::int64_t a, std::int64_t b, std::int64_t& out) { return !__builtin_sub_overflow(a, b, &out); }
bool mulChecked(std::int64_t a, std::int64_t b, std::int64_t& out) { return !__builtin_mul_overflow(a, b, &out); }
#else
// The VM carries the same fallback (see VM::binaryArith); these mirror it so a
// compiler without the builtins still refuses to fold a raising operation
// instead of wrapping around.
bool addChecked(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
        return false;
    out = a + b;
    return true;
}
bool subChecked(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if ((b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) ||
        (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b))
        return false;
    out = a - b;
    return true;
}
bool mulChecked(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if (a == 0 || b == 0) { out = 0; return true; }
    if (a == -1) {
        if (b == std::numeric_limits<std::int64_t>::min()) return false;
        out = -b;
        return true;
    }
    if (b == -1) {
        if (a == std::numeric_limits<std::int64_t>::min()) return false;
        out = -a;
        return true;
    }
    if (a > 0) {
        if (b > 0) { if (a > std::numeric_limits<std::int64_t>::max() / b) return false; }
        else { if (b < std::numeric_limits<std::int64_t>::min() / a) return false; }
    } else {
        if (b > 0) { if (a < std::numeric_limits<std::int64_t>::min() / b) return false; }
        else { if (a != 0 && b < std::numeric_limits<std::int64_t>::max() / a) return false; }
    }
    out = a * b;
    return true;
}
#endif

// Integer exponentiation, exactly as the VM computes it (see VM::binaryArith):
// exponentiation by squaring, and an overflow anywhere in the chain is the
// runtime's `integer overflow in exponentiation` error rather than a wrapped
// result. `false` therefore means "this raises at runtime; do not fold".
bool powChecked(std::int64_t base, std::int64_t exponent, std::int64_t& out) {
    std::int64_t result = 1;
    std::int64_t factor = base;
    std::uint64_t n = static_cast<std::uint64_t>(exponent);
    while (n != 0) {
        if (n & 1u) {
            std::int64_t next = 0;
            if (!mulChecked(result, factor, next)) return false;
            result = next;
        }
        n >>= 1u;
        if (n != 0) {
            std::int64_t next = 0;
            if (!mulChecked(factor, factor, next)) return false;
            factor = next;
        }
    }
    out = result;
    return true;
}

Constant makeInt(std::int64_t value) {
    Constant c;
    c.kind = ConstKind::Int;
    c.intValue = value;
    return c;
}
Constant makeDouble(double value) {
    Constant c;
    c.kind = ConstKind::Double;
    c.doubleValue = value;
    return c;
}
Constant makeBool(bool value) {
    Constant c;
    c.kind = ConstKind::Bool;
    c.boolValue = value;
    return c;
}

bool isInt(const Constant& c) { return c.kind == ConstKind::Int; }
bool isDouble(const Constant& c) { return c.kind == ConstKind::Double; }
bool isBool(const Constant& c) { return c.kind == ConstKind::Bool; }
bool isString(const Constant& c) { return c.kind == ConstKind::String; }
bool isNil(const Constant& c) { return c.kind == ConstKind::Nil; }

// The shift the VM performs for `Shr`: arithmetic, sign-extending
// (VM::binaryBitwise spells it out because C++ leaves it unspecified).
std::int64_t shiftRightArithmetic(std::int64_t value, std::int64_t amount) {
    const auto shift = static_cast<unsigned>(amount);
    const auto ux = static_cast<std::uint64_t>(value);
    std::uint64_t shifted = ux >> shift;
    if (value < 0 && shift != 0) shifted |= (~std::uint64_t{0}) << (64 - shift);
    return static_cast<std::int64_t>(shifted);
}

// An operand is only foldable when it names a constant the module's pool
// actually holds. Anything else - a temp, a parameter, a static, an out of
// range id - is "not known", never a guess.
const Constant* constantOperand(const Module& module, const Operand& operand) {
    if (operand.kind != OperandKind::Const) return nullptr;
    return module.constant(operand.index);
}

} // namespace

std::uint32_t constantTypeOf(const Module& module, const Constant& constant) {
    switch (constant.kind) {
        case ConstKind::Bool: return module.types.boolType();
        case ConstKind::Int: return module.types.intType();
        case ConstKind::Double: return module.types.doubleType();
        case ConstKind::String: return module.types.stringType();
        // A nil constant or an enum member can stand for more than one type
        // (any nullable type; the enum's own type), so neither pins one down.
        case ConstKind::Nil:
        case ConstKind::EnumMember: return 0;
    }
    return 0;
}

bool constantMatchesType(const Module& module, const Constant& constant, std::uint32_t type) {
    if (type == 0) return false;
    const std::uint32_t natural = constantTypeOf(module, constant);
    if (natural != 0) return natural == type;
    // Nil is a legal value of any nullable type; an enum member is only a
    // legal value of its own enum, which the type table spells by name.
    if (constant.kind == ConstKind::Nil) return module.types.isNullable(type);
    if (constant.kind == ConstKind::EnumMember) {
        const Type* declared = module.types.find(type);
        return declared != nullptr && declared->kind == TypeKind::Object &&
               declared->name == constant.enumTypeName;
    }
    return false;
}

std::optional<Constant> foldInstruction(const Module& module, const Instruction& instruction) {
    // Never fold anything the effect table says is not a pure computation.
    // This is the single guard that keeps calls, drops, locks, atomics,
    // allocations and I/O out of the folder whatever their operands are.
    const EffectSet effects = classifyOpcode(instruction.opcode);
    if (!hasEffect(effects, Effect::Pure)) return std::nullopt;
    if (instruction.result == kNoTemp) return std::nullopt;

    const bool unary = instruction.operands.size() == 1;
    const bool binary = instruction.operands.size() == 2;
    if (!unary && !binary) return std::nullopt;

    auto operandAt = [&](std::size_t index) -> const Constant* {
        if (index >= instruction.operands.size()) return nullptr;
        return constantOperand(module, instruction.operands[index]);
    };
    const Constant* a = operandAt(0);
    const Constant* b = operandAt(1);
    if (a == nullptr) return std::nullopt;
    if (binary && b == nullptr) return std::nullopt;

    // The result must be exactly what the instruction declares, so a folded
    // operand can be substituted without changing any operand's type.
    auto typed = [&](const Constant& value) -> std::optional<Constant> {
        if (instruction.resultType == 0) return std::nullopt;
        if (!constantMatchesType(module, value, instruction.resultType)) return std::nullopt;
        return value;
    };

    switch (instruction.opcode) {
        case Opcode::Add:
            if (!binary) return std::nullopt;
            if (isInt(*a) && isInt(*b)) {
                std::int64_t result = 0;
                if (!addChecked(a->intValue, b->intValue, result)) return std::nullopt; // raises at runtime
                return typed(makeInt(result));
            }
            if (isDouble(*a) && isDouble(*b)) return typed(makeDouble(a->doubleValue + b->doubleValue));
            // String concatenation. `+` means concat as soon as either side is
            // a string, so both must be strings here (a mixed pair would need
            // the VM's valueToString, which is a formatting contract this
            // folder does not own).
            if (isString(*a) && isString(*b)) {
                Constant c;
                c.kind = ConstKind::String;
                c.stringValue = a->stringValue + b->stringValue;
                return typed(c);
            }
            return std::nullopt;
        case Opcode::Sub:
            if (!binary) return std::nullopt;
            if (isInt(*a) && isInt(*b)) {
                std::int64_t result = 0;
                if (!subChecked(a->intValue, b->intValue, result)) return std::nullopt;
                return typed(makeInt(result));
            }
            if (isDouble(*a) && isDouble(*b)) return typed(makeDouble(a->doubleValue - b->doubleValue));
            return std::nullopt;
        case Opcode::Mul:
            if (!binary) return std::nullopt;
            if (isInt(*a) && isInt(*b)) {
                std::int64_t result = 0;
                if (!mulChecked(a->intValue, b->intValue, result)) return std::nullopt;
                return typed(makeInt(result));
            }
            if (isDouble(*a) && isDouble(*b)) return typed(makeDouble(a->doubleValue * b->doubleValue));
            return std::nullopt;
        case Opcode::Div:
            if (!binary) return std::nullopt;
            if (isInt(*a) && isInt(*b)) {
                if (b->intValue == 0) return std::nullopt;                                  // raises
                if (a->intValue == std::numeric_limits<std::int64_t>::min() && b->intValue == -1)
                    return std::nullopt;                                                    // raises
                return typed(makeInt(a->intValue / b->intValue));
            }
            if (isDouble(*a) && isDouble(*b)) {
                if (b->doubleValue == 0.0) return std::nullopt; // raises
                return typed(makeDouble(a->doubleValue / b->doubleValue));
            }
            return std::nullopt;
        case Opcode::Mod:
            if (!binary) return std::nullopt;
            if (isInt(*a) && isInt(*b)) {
                if (b->intValue == 0) return std::nullopt; // raises
                // The VM defines the one case C++ leaves undefined.
                if (a->intValue == std::numeric_limits<std::int64_t>::min() && b->intValue == -1)
                    return typed(makeInt(0));
                return typed(makeInt(a->intValue % b->intValue));
            }
            if (isDouble(*a) && isDouble(*b)) {
                if (b->doubleValue == 0.0) return std::nullopt; // raises
                return typed(makeDouble(std::fmod(a->doubleValue, b->doubleValue)));
            }
            return std::nullopt;
        case Opcode::Pow:
            if (!binary) return std::nullopt;
            if (isInt(*a) && isInt(*b)) {
                // A negative integer exponent is a runtime error in ZL, not a
                // value; an inexact one raises too. Both stay unfolded.
                if (b->intValue < 0) return std::nullopt;
                std::int64_t result = 0;
                if (!powChecked(a->intValue, b->intValue, result)) return std::nullopt;
                return typed(makeInt(result));
            }
            if (isDouble(*a) && isDouble(*b)) return typed(makeDouble(std::pow(a->doubleValue, b->doubleValue)));
            return std::nullopt;
        case Opcode::Neg:
            if (!unary) return std::nullopt;
            if (isInt(*a)) {
                if (a->intValue == std::numeric_limits<std::int64_t>::min()) return std::nullopt; // raises
                return typed(makeInt(-a->intValue));
            }
            if (isDouble(*a)) return typed(makeDouble(-a->doubleValue));
            return std::nullopt;
        case Opcode::BitNot:
            if (!unary || !isInt(*a)) return std::nullopt;
            return typed(makeInt(~a->intValue));
        case Opcode::BitAnd:
            if (!binary || !isInt(*a) || !isInt(*b)) return std::nullopt;
            return typed(makeInt(a->intValue & b->intValue));
        case Opcode::BitOr:
            if (!binary || !isInt(*a) || !isInt(*b)) return std::nullopt;
            return typed(makeInt(a->intValue | b->intValue));
        case Opcode::BitXor:
            if (!binary || !isInt(*a) || !isInt(*b)) return std::nullopt;
            return typed(makeInt(a->intValue ^ b->intValue));
        case Opcode::Shl:
            if (!binary || !isInt(*a) || !isInt(*b)) return std::nullopt;
            if (b->intValue < 0 || b->intValue >= 64) return std::nullopt; // raises
            return typed(makeInt(static_cast<std::int64_t>(static_cast<std::uint64_t>(a->intValue)
                                                           << b->intValue)));
        case Opcode::Shr:
            if (!binary || !isInt(*a) || !isInt(*b)) return std::nullopt;
            if (b->intValue < 0 || b->intValue >= 64) return std::nullopt; // raises
            return typed(makeInt(shiftRightArithmetic(a->intValue, b->intValue)));
        case Opcode::Ushr:
            if (!binary || !isInt(*a) || !isInt(*b)) return std::nullopt;
            if (b->intValue < 0 || b->intValue >= 64) return std::nullopt; // raises
            return typed(makeInt(static_cast<std::int64_t>(static_cast<std::uint64_t>(a->intValue)
                                                           >> b->intValue)));
        case Opcode::Not:
            // `Not` is only folded for a bool operand: the VM's unary Not is
            // `!isTruthy(a)`, and truthiness of a non-bool is a VM rule this
            // folder does not restate.
            if (!unary || !isBool(*a)) return std::nullopt;
            return typed(makeBool(!a->boolValue));
        case Opcode::And:
            if (!binary || !isBool(*a) || !isBool(*b)) return std::nullopt;
            return typed(makeBool(a->boolValue && b->boolValue));
        case Opcode::Or:
            if (!binary || !isBool(*a) || !isBool(*b)) return std::nullopt;
            return typed(makeBool(a->boolValue || b->boolValue));
        case Opcode::Widen:
            if (!unary || !isInt(*a)) return std::nullopt;
            return typed(makeDouble(static_cast<double>(a->intValue)));
        case Opcode::Eq:
        case Opcode::Ne: {
            if (!binary) return std::nullopt;
            std::optional<bool> equal;
            if (isInt(*a) && isInt(*b)) equal = a->intValue == b->intValue;
            else if (isBool(*a) && isBool(*b)) equal = a->boolValue == b->boolValue;
            else if (isString(*a) && isString(*b)) equal = a->stringValue == b->stringValue;
            else if (isNil(*a) && isNil(*b)) equal = true;
            else if ((isInt(*a) || isDouble(*a)) && (isInt(*b) || isDouble(*b))) {
                // Mixed int/double: the VM compares numerically, and a NaN is
                // not equal to anything, itself included.
                const double x = isDouble(*a) ? a->doubleValue : static_cast<double>(a->intValue);
                const double y = isDouble(*b) ? b->doubleValue : static_cast<double>(b->intValue);
                if (std::isnan(x) || std::isnan(y)) equal = false;
                else equal = x == y;
            }
            if (!equal) return std::nullopt;
            return typed(makeBool(instruction.opcode == Opcode::Eq ? *equal : !*equal));
        }
        case Opcode::Lt:
        case Opcode::Le:
        case Opcode::Gt:
        case Opcode::Ge: {
            if (!binary) return std::nullopt;
            const bool numeric = (isInt(*a) || isDouble(*a)) && (isInt(*b) || isDouble(*b));
            if (!numeric) return std::nullopt;
            const double x = isDouble(*a) ? a->doubleValue : static_cast<double>(a->intValue);
            const double y = isDouble(*b) ? b->doubleValue : static_cast<double>(b->intValue);
            // Every ordered comparison against a NaN is false in ZL.
            if (std::isnan(x) || std::isnan(y)) return typed(makeBool(false));
            switch (instruction.opcode) {
                case Opcode::Lt: return typed(makeBool(x < y));
                case Opcode::Le: return typed(makeBool(x <= y));
                case Opcode::Gt: return typed(makeBool(x > y));
                case Opcode::Ge: return typed(makeBool(x >= y));
                default: return std::nullopt;
            }
        }
        default:
            return std::nullopt;
    }
}

} // namespace zl::mir
