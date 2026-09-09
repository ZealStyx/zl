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
// instruction and never reassigned. Mutable ZL `var`/`let` locals are *not*
// temps - they are `SlotId`s, explicitly loaded and stored, which is what keeps
// SSA and ZL's reassignment semantics from fighting each other. Function
// parameters are SSA values in their own right; lowering copies a parameter
// into a slot only when the body actually assigns to it.
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

constexpr TempId kNoTemp = 0;
constexpr BlockId kNoBlock = 0;
constexpr ConstId kNoConst = 0;
constexpr FunctionId kNoFunction = 0;
constexpr StaticId kNoStatic = 0;

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
    [[nodiscard]] static Operand staticRef(StaticId id, std::uint32_t type) noexcept {
        return Operand{OperandKind::Static, type, id};
    }

    friend bool operator==(const Operand& a, const Operand& b) noexcept {
        return a.kind == b.kind && a.type == b.type && a.index == b.index;
    }
};

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
