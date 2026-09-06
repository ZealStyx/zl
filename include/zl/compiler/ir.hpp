#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "zl/common/ownership.hpp"

namespace zl::ir {

enum class Opcode : std::uint8_t {
    Nop, Const, LoadLocal, StoreLocal,
    DefineLocal, AssignLocal, MoveLocal, BorrowLocal, EndBorrow, DropLocal,
    Unary, Binary, Call, Return, Jump, Branch, Throw
};
using ValueId = std::uint32_t;
using BlockId = std::uint32_t;
struct Instruction {
    Opcode opcode{Opcode::Nop};
    ValueId result{0};
    ValueId operand0{0};
    ValueId operand1{0};
    std::string symbol;
    std::size_t sourceLine{0};
    OwnershipKind ownership{OwnershipKind::GC};
    std::vector<ValueId> operands;
};
struct BasicBlock {
    BlockId id{0};
    std::vector<Instruction> instructions;
    std::vector<BlockId> successors;
};
struct Function {
    std::string name;
    std::string returnType;
    std::vector<std::string> parameterTypes;
    std::vector<OwnershipKind> parameterOwnership;
    OwnershipKind returnOwnership{OwnershipKind::GC};
    std::vector<BasicBlock> blocks;
    bool isAsync{false};
    bool isNative{false};
};
struct Module { std::vector<Function> functions; };
[[nodiscard]] bool verify(const Module& module, std::string* error = nullptr);
}
