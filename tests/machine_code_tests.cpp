// Machine-code backend regressions: the x86-64 emitter's prologue/epilogue
// structure, a regression for every supported instruction category (each one
// mapped executable and *run*, compared against the value the language
// computes), and the full set of rejected inputs (unsupported types, control
// flow, frame and parameter limits, malformed constants, unterminated
// functions).
#include "zl/compiler/ir.hpp"
#include "zl/compiler/machine_code.hpp"
#include "zl/lexer/token.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#define ZL_MC_CAN_EXECUTE 1
#else
#define ZL_MC_CAN_EXECUTE 0
#endif
#else
#define ZL_MC_CAN_EXECUTE 0
#endif

namespace {

using namespace zl::ir;
using zl::machine::CompileResult;
using zl::machine::emitX64;
using zl::OwnershipKind;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "machine code regression: " << message << '\n';
    ++failures;
}

// Builds a one-block function whose body is exactly `instructions`.
Module oneFunction(const std::vector<Instruction>& instructions,
                   const std::vector<std::string>& parameterTypes = {},
                   const std::string& returnType = "int", bool isAsync = false) {
    Module module;
    zl::ir::Function fn;
    fn.name = "f";
    fn.returnType = returnType;
    fn.parameterTypes = parameterTypes;
    fn.isAsync = isAsync;
    BasicBlock block;
    block.id = 0;
    block.instructions = instructions;
    fn.blocks.push_back(std::move(block));
    module.functions.push_back(std::move(fn));
    return module;
}

Instruction define(ValueId result) { return Instruction{Opcode::DefineLocal, result, 0, 0, {}, 0, OwnershipKind::GC}; }
Instruction constant(ValueId result, const std::string& text) {
    return Instruction{Opcode::Const, result, 0, 0, text, 0, OwnershipKind::GC};
}
Instruction unary(ValueId result, ValueId operand, const std::string& op) {
    return Instruction{Opcode::Unary, result, operand, 0, op, 0, OwnershipKind::GC};
}
Instruction binary(ValueId result, ValueId a, ValueId b, const std::string& op) {
    return Instruction{Opcode::Binary, result, a, b, op, 0, OwnershipKind::GC};
}
Instruction load(ValueId result, ValueId operand) {
    return Instruction{Opcode::LoadLocal, result, operand, 0, {}, 0, OwnershipKind::GC};
}
Instruction store(ValueId result, ValueId operand) {
    return Instruction{Opcode::AssignLocal, result, operand, 0, {}, 0, OwnershipKind::GC};
}
Instruction ret(ValueId operand) { return Instruction{Opcode::Return, 0, operand, 0, {}, 0, OwnershipKind::GC}; }

const std::string PLUS = std::to_string(static_cast<int>(zl::TokenType::PLUS));
const std::string MINUS = std::to_string(static_cast<int>(zl::TokenType::MINUS));
const std::string STAR = std::to_string(static_cast<int>(zl::TokenType::STAR));

#if ZL_MC_CAN_EXECUTE
// Maps one emitted function executable and calls it with integer arguments.
std::int64_t runFunction(const zl::machine::FunctionCode& code, std::vector<std::int64_t> args) {
    if (code.bytes.empty()) return 0;
    auto page = static_cast<std::uint8_t*>(
        ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (page == MAP_FAILED) return -1;
    std::memcpy(page, code.bytes.data(), code.bytes.size());
    const bool exec = ::mprotect(page, 4096, PROT_READ | PROT_EXEC) == 0;
    std::int64_t result = -1;
    if (exec) {
        std::int64_t callArgs[6] = {0, 0, 0, 0, 0, 0};
        for (std::size_t i = 0; i < args.size() && i < 6; ++i) callArgs[i] = args[i];
        result = reinterpret_cast<std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t,
                                                   std::int64_t, std::int64_t, std::int64_t)>(page)(
            callArgs[0], callArgs[1], callArgs[2], callArgs[3], callArgs[4], callArgs[5]);
    }
    ::munmap(page, 4096);
    return result;
}
#endif

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

void testPrologueAndEpilogue() {
    const auto module = oneFunction(
        {define(1), ret(1)},
        {"int"});
    const auto result = emitX64(module);
    require(result.success, "a trivial int function emits: " + result.error);
    require(result.functions.size() == 1, "one function in");
    const auto& code = result.functions[0];
    require(code.parameterCount == 1, "parameter count is recorded");
    require(code.stackSize % 16 == 0, "the frame is 16-byte aligned");
    // push rbp; mov rbp, rsp
    require(code.bytes.size() >= 4 && code.bytes[0] == 0x55 && code.bytes[1] == 0x48 &&
              code.bytes[2] == 0x89 && code.bytes[3] == 0xe5,
            "the prologue is push rbp / mov rbp,rsp");
    // ... pop rbp; ret at the very end
    require(code.bytes.size() >= 2 && code.bytes.back() == 0xc3, "the function ends with ret");
    require(code.bytes[code.bytes.size() - 2] == 0x5d, "ret is preceded by pop rbp");
}

// ---------------------------------------------------------------------------
// One executed regression per instruction category
// ---------------------------------------------------------------------------

void testExecutedCategories() {
#if ZL_MC_CAN_EXECUTE
    // Const: returns 42.
    {
        const auto module = oneFunction({constant(1, "42"), ret(1)});
        const auto result = emitX64(module);
        require(result.success, result.error);
        require(runFunction(result.functions[0], {}) == 42, "a Const expression evaluates");
    }
    // Parameters: identity.
    {
        const auto module = oneFunction({define(1), ret(1)}, {"int"});
        const auto result = emitX64(module);
        require(result.success, result.error);
        require(runFunction(result.functions[0], {7}) == 7, "a parameter reaches the return");
    }
    // Two parameters + Binary: a + b.
    {
        const auto module = oneFunction(
            {define(1), define(2), binary(3, 1, 2, PLUS), ret(3)}, {"int", "int"});
        const auto result = emitX64(module);
        require(result.success, result.error);
        require(runFunction(result.functions[0], {20, 22}) == 42, "Binary + executes");
    }
    {
        const auto module = oneFunction(
            {define(1), define(2), binary(3, 1, 2, MINUS), ret(3)}, {"int", "int"});
        const auto result = emitX64(module);
        require(result.success, result.error);
        require(runFunction(result.functions[0], {50, 8}) == 42, "Binary - executes");
    }
    {
        const auto module = oneFunction(
            {define(1), define(2), binary(3, 1, 2, STAR), ret(3)}, {"int", "int"});
        const auto result = emitX64(module);
        require(result.success, result.error);
        require(runFunction(result.functions[0], {6, 7}) == 42, "Binary * executes");
    }
    // Unary minus.
    {
        const auto module = oneFunction({define(1), unary(2, 1, MINUS), ret(2)}, {"int"});
        const auto result = emitX64(module);
        require(result.success, result.error);
        require(runFunction(result.functions[0], {42}) == -42, "Unary - executes");
    }
    // LoadLocal / AssignLocal: a := a; b := a; return b.
    {
        const auto module = oneFunction({define(1), load(2, 1), store(3, 2), ret(3)}, {"int"});
        const auto result = emitX64(module);
        require(result.success, result.error);
        require(runFunction(result.functions[0], {9}) == 9, "Load/AssignLocal move values through the frame");
    }
    // Chained constants: (10 * 4) - 2 + 14 == 52, pinning store/reload order.
    {
        const auto module = oneFunction(
            {constant(1, "10"), constant(2, "4"), binary(3, 1, 2, STAR), constant(4, "2"),
             binary(5, 3, 4, MINUS), constant(6, "14"), binary(7, 5, 6, PLUS), ret(7)});
        const auto result = emitX64(module);
        require(result.success, result.error);
        require(runFunction(result.functions[0], {}) == 52, "a chained expression keeps its order");
    }
#else
    (void)emitX64;
    std::cerr << "machine code regression: skipped execution on a non-x86-64 host\n";
#endif
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

void testRejections() {
    const auto expectFailure = [](const std::vector<Instruction>& instructions,
                                  const std::string& label, const std::string& needle,
                                  const std::vector<std::string>& params = {},
                                  const std::string& returnType = "int", bool isAsync = false) {
        const auto result = emitX64(oneFunction(instructions, params, returnType, isAsync));
        require(!result.success, label + " must be rejected");
        require(result.error.find(needle) != std::string::npos,
                label + " rejected with the right diagnostic (got: " + result.error + ")");
    };

    expectFailure({define(1), ret(1)}, "a double return", "only int returns", {}, "double");
    expectFailure({define(1), ret(1)}, "a string parameter", "only int parameters", {"string"});
    expectFailure({define(1), ret(1)}, "an async function", "async functions are not supported", {}, "int", true);

    // Control flow: two blocks.
    {
        Module module;
        zl::ir::Function fn;
        fn.name = "cf";
        fn.returnType = "int";
        BasicBlock a;
        a.id = 0;
        a.successors = {1};
        a.instructions.push_back(Instruction{Opcode::Jump, 0, 0, 0, {}, 0, OwnershipKind::GC});
        BasicBlock b;
        b.id = 1;
        b.instructions.push_back(ret(0));
        fn.blocks = {std::move(a), std::move(b)};
        module.functions.push_back(std::move(fn));
        const auto result = emitX64(module);
        require(!result.success, "control flow must be rejected");
        require(result.error.find("control flow is not yet supported") != std::string::npos,
                "control flow rejection is diagnosed");
    }

    // Frame limit: 16 distinct values exceed the disp8 slot budget.
    {
        std::vector<Instruction> instructions;
        for (ValueId i = 1; i <= 16; ++i) instructions.push_back(constant(i, "1"));
        instructions.push_back(ret(1));
        expectFailure(instructions, "16 live values", "more than 15 machine-code frame slots");
    }

    // Parameter limit: seven exceeds the register slice.
    expectFailure({define(1), define(2), define(3), define(4), define(5), define(6), define(7), ret(1)},
                  "seven parameters", "too many parameters",
                  {"int", "int", "int", "int", "int", "int", "int"});

    expectFailure({constant(1, "abc"), ret(1)}, "a non-numeric constant", "invalid integer constant");
    expectFailure({constant(1, "1"), ret(1), constant(2, "2")}, "instructions after Return",
                  "after a terminating Return");
    expectFailure({constant(1, "1")}, "a function without a Return", "does not end in Return");
    // Note: value liveness is not this slice's contract - collectSlots
    // assigns a frame slot to any referenced id, and undefined locals are
    // rejected upstream by the IR verifier (see ir_tests) and by the
    // pipeline's verify-before-codegen gate (see pipeline_tests).
    expectFailure({define(1), define(2), binary(3, 1, 2, std::to_string(static_cast<int>(zl::TokenType::SLASH))), ret(3)},
                  "an unsupported binary operator", "unsupported binary operator", {"int", "int"});
    expectFailure({define(1), unary(2, 1, std::to_string(static_cast<int>(zl::TokenType::NOT))), ret(2)},
                  "an unsupported unary operator", "unsupported unary operator", {"int"});
    {
        Module module;
        zl::ir::Function fn;
        fn.name = "owned";
        fn.returnType = "int";
        BasicBlock block;
        block.id = 0;
        block.instructions.push_back(Instruction{Opcode::DefineLocal, 1, 0, 0, {}, 0, OwnershipKind::OWNED});
        block.instructions.push_back(ret(0));
        fn.blocks.push_back(std::move(block));
        module.functions.push_back(std::move(fn));
        const auto result = emitX64(module);
        require(!result.success, "owned locals are outside this backend slice");
        require(result.error.find("ownership mode is not supported") != std::string::npos,
                "ownership refusal is diagnosed");
    }
    expectFailure({Instruction{Opcode::Jump, 0, 0, 0, {}, 0, OwnershipKind::GC}}, "a Jump in a straight-line slice",
                  "unsupported MIR opcode");
    // An empty body is also unsupported: nothing returns.
    expectFailure({}, "an empty function", "does not end in Return");
}

} // namespace

int main() {
    testPrologueAndEpilogue();
    testExecutedCategories();
    testRejections();

    if (failures != 0) {
        std::cerr << failures << " machine code regression(s) failed\n";
        return 1;
    }
    std::cout << "all machine code regressions passed\n";
    return 0;
}
