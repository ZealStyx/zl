#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zl/common/ownership.hpp"
#include "zl/mir/source_location.hpp"
#include "zl/mir/type.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// Values and operands
// ---------------------------------------------------------------------------
//
// MIR is SSA-shaped: a `TempId` names a value produced by exactly one
// instruction and never reassigned, and a `BlockParamId` names a value defined
// on entry to a block (the phi-node / block-argument form of SSA). Mutable ZL
// `var`/`let` locals are *not* values - they are `SlotId`s, explicitly loaded
// and stored, which is what keeps SSA and ZL's reassignment semantics from
// fighting each other. Function parameters are SSA values in their own right;
// lowering copies a parameter into a slot only when the body actually assigns
// to it.
//
// Both spellings of "value defined on two paths" exist on purpose:
//
//   * a block parameter is the *merged* value - `if c { x = 10 } else { x = 20 }`
//     followed by a use of `x` is a block parameter at the join, taking 10 on
//     one incoming edge and 20 on the other;
//   * a slot is the memory form - store on each path, load after the join.
//
// They are interchangeable in meaning and both are verified; a pass may convert
// the second into the first (`promoteSlotsToBlockParameters`), and a backend
// must handle either.
//
// Because every value carries its type, an operand is self-describing: no
// consumer of MIR ever has to re-derive "what type is this?" from context.

using TempId = std::uint32_t;
using SlotId = std::uint32_t;
using ParamId = std::uint32_t;
using BlockId = std::uint32_t;
using ConstId = std::uint32_t;
using FunctionId = std::uint32_t;
using StaticId = std::uint32_t;
// A block parameter: the MIR's phi node. Parameters are numbered in one
// function-wide space (not per block) so a parameter *is* a value: it has one
// definition, many uses, and a def-use chain exactly like a temp does.
using BlockParamId = std::uint32_t;

constexpr TempId kNoTemp = 0;
constexpr BlockId kNoBlock = 0;
constexpr ConstId kNoConst = 0;
constexpr FunctionId kNoFunction = 0;
constexpr StaticId kNoStatic = 0;
constexpr BlockParamId kNoBlockParam = 0;

// A compile-time constant. Constants live in a module-level pool and are
// referred to by index, so an operand stays a fixed-size value type.
enum class ConstKind : std::uint8_t {
    Bool,
    Int,
    Double,
    String,
    // The `null` literal. Its type is Nil on its own; a null assigned to a
    // reference type is still this constant, with the operand typed as that
    // reference type.
    Nil,
    // An enum member, e.g. `Color.RED`. `enumTypeName` is the enum's name and
    // `stringValue` the member's; the operand is typed as that enum, NOT as
    // string. The runtime happens to represent a member as its name, but that
    // is a representation detail - the static type the source states is the
    // enum, and erasing it here would make every comparison and call taking an
    // enum member look like a string/type mismatch.
    EnumMember,
};

[[nodiscard]] const char* constKindName(ConstKind kind) noexcept;

struct Constant {
    ConstKind kind{ConstKind::Nil};
    bool boolValue{false};
    std::int64_t intValue{0};
    double doubleValue{0.0};
    std::string stringValue;
    // EnumMember only: the declaring enum's name.
    std::string enumTypeName;

    friend bool operator==(const Constant& a, const Constant& b) noexcept {
        if (a.kind != b.kind) return false;
        switch (a.kind) {
            case ConstKind::Bool: return a.boolValue == b.boolValue;
            case ConstKind::Int: return a.intValue == b.intValue;
            case ConstKind::Double: return a.doubleValue == b.doubleValue;
            case ConstKind::String: return a.stringValue == b.stringValue;
            case ConstKind::Nil: return true;
            case ConstKind::EnumMember:
                return a.enumTypeName == b.enumTypeName && a.stringValue == b.stringValue;
        }
        return false;
    }
};

enum class OperandKind : std::uint8_t {
    // Absent operand. Used for the value-less half of optional slots, e.g. the
    // operand of a `return` in a void function.
    None,
    // Index into Module::constants.
    Const,
    // SSA temporary produced by an instruction in this function.
    Temp,
    // Function parameter.
    Param,
    // Block parameter (phi): a `BlockParamId` into the function's block
    // parameters. Usable in the block that owns it and in blocks that block
    // dominates, exactly like a temp whose definition is the block's entry.
    BlockParam,
    // Module-level static field (ZL `static` class fields).
    Static,
};

[[nodiscard]] const char* operandKindName(OperandKind kind) noexcept;

// One typed reference to a value. `type` is always a valid TypeId for every
// kind except None.
struct Operand {
    OperandKind kind{OperandKind::None};
    std::uint32_t type{0};
    std::uint32_t index{0}; // ConstId / TempId / ParamId / StaticId

    [[nodiscard]] bool isNone() const noexcept { return kind == OperandKind::None; }

    [[nodiscard]] static Operand none() noexcept { return Operand{}; }
    [[nodiscard]] static Operand constant(ConstId id, std::uint32_t type) noexcept {
        return Operand{OperandKind::Const, type, id};
    }
    [[nodiscard]] static Operand temp(TempId id, std::uint32_t type) noexcept {
        return Operand{OperandKind::Temp, type, id};
    }
    [[nodiscard]] static Operand param(ParamId id, std::uint32_t type) noexcept {
        return Operand{OperandKind::Param, type, id};
    }
    [[nodiscard]] static Operand blockParam(BlockParamId id, std::uint32_t type) noexcept {
        return Operand{OperandKind::BlockParam, type, id};
    }
    [[nodiscard]] static Operand staticRef(StaticId id, std::uint32_t type) noexcept {
        return Operand{OperandKind::Static, type, id};
    }

    friend bool operator==(const Operand& a, const Operand& b) noexcept {
        return a.kind == b.kind && a.type == b.type && a.index == b.index;
    }
};

// ---------------------------------------------------------------------------
// The value space
// ---------------------------------------------------------------------------
//
// A *value* is anything in a function that is defined once and used many times:
// a parameter (defined by the caller), a block parameter (defined on entry to a
// block), or a temp (defined by one instruction). Constants and statics are not
// values - they are not defined by anything inside the function.
//
// Analyses (def-use, liveness, constant propagation) work over this one type so
// there is a single implementation of each instead of one per value spelling.

struct ValueId {
    OperandKind kind{OperandKind::None};
    std::uint32_t index{0};

    [[nodiscard]] bool valid() const noexcept { return kind != OperandKind::None; }
    [[nodiscard]] bool isTemp() const noexcept { return kind == OperandKind::Temp; }
    [[nodiscard]] bool isParam() const noexcept { return kind == OperandKind::Param; }
    [[nodiscard]] bool isBlockParam() const noexcept { return kind == OperandKind::BlockParam; }

    friend bool operator==(const ValueId& a, const ValueId& b) noexcept {
        return a.kind == b.kind && a.index == b.index;
    }
    friend bool operator!=(const ValueId& a, const ValueId& b) noexcept { return !(a == b); }
    // Ordered by kind then index so a value set has a deterministic iteration
    // order - diagnostics and dumps must not depend on hash seeding.
    friend bool operator<(const ValueId& a, const ValueId& b) noexcept {
        if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        return a.index < b.index;
    }
};

// Hashing must agree with `operator==`, which it does by combining the same two
// fields; the kind is folded in first so a temp and a block parameter with the
// same index are different keys.
struct ValueIdHash {
    std::size_t operator()(const ValueId& value) const noexcept {
        const std::size_t kind = static_cast<std::size_t>(value.kind);
        return (kind * 0x9e3779b97f4a7c15ULL) ^ (static_cast<std::size_t>(value.index) * 0xbf58476d1ce4e5b9ULL);
    }
};

} // namespace zl::mir

// So that a ValueId can key an unordered container directly.
template <>
struct std::hash<zl::mir::ValueId> {
    std::size_t operator()(const zl::mir::ValueId& value) const noexcept {
        return zl::mir::ValueIdHash{}(value);
    }
};

namespace zl::mir {

[[nodiscard]] inline ValueId tempValue(TempId id) noexcept {
    return ValueId{OperandKind::Temp, id};
}
[[nodiscard]] inline ValueId paramValue(ParamId id) noexcept {
    return ValueId{OperandKind::Param, id};
}
[[nodiscard]] inline ValueId blockParamValue(BlockParamId id) noexcept {
    return ValueId{OperandKind::BlockParam, id};
}

// The value an operand names, or an invalid ValueId for a constant / static /
// absent operand.
[[nodiscard]] inline ValueId valueOf(const Operand& operand) noexcept {
    switch (operand.kind) {
        case OperandKind::Param: return paramValue(operand.index);
        case OperandKind::Temp: return tempValue(operand.index);
        case OperandKind::BlockParam: return blockParamValue(operand.index);
        case OperandKind::None:
        case OperandKind::Const:
        case OperandKind::Static: return ValueId{};
    }
    return ValueId{};
}

// A mutable local. Lowering allocates one per ZL `var`/`let` (and per
// assigned-to parameter, per loop counter, per catch binding).
struct Slot {
    std::string name;         // source name, for diagnostics and debugging
    std::uint32_t type{0};    // TypeId
    bool isMutable{true};     // false for `let`
    bool isCatchBinding{false};
    // Storage contract recorded by semantic analysis. Reuses the language's
    // own `gc`/`owned`/`borrow`/`shared` markers so the MIR keeps exactly the
    // information a later ownership or GC pass needs, rather than inventing a
    // parallel enum that could drift from it.
    zl::OwnershipKind ownership{zl::OwnershipKind::GC};
    // Non-empty when this slot holds a borrow: the owning region it borrows
    // from, as recorded by semantic analysis.
    std::string borrowSource;
    SourceLocation location;
};

} // namespace zl::mir
