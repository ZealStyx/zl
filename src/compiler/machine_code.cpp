#include "zl/compiler/machine_code.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "zl/lexer/token.hpp"

namespace zl::machine {
namespace {

struct Encoder {
    std::vector<std::uint8_t> code;

    void u8(std::uint8_t v) { code.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
    }

    void pushRbp() { u8(0x55); }
    void movRbpRsp() { u8(0x48); u8(0x89); u8(0xe5); }
    void popRbp() { u8(0x5d); }
    void ret() { u8(0xc3); }

    void subRsp(std::size_t n) {
        if (n == 0) return;
        if (n <= 127) {
            u8(0x48); u8(0x83); u8(0xec); u8(static_cast<std::uint8_t>(n));
        } else {
            u8(0x48); u8(0x81); u8(0xec); u32(static_cast<std::uint32_t>(n));
        }
    }

    void addRsp(std::size_t n) {
        if (n == 0) return;
        if (n <= 127) {
            u8(0x48); u8(0x83); u8(0xc4); u8(static_cast<std::uint8_t>(n));
        } else {
            u8(0x48); u8(0x81); u8(0xc4); u32(static_cast<std::uint32_t>(n));
        }
    }

    void movR64Imm64(unsigned reg, std::uint64_t value) {
        u8(static_cast<std::uint8_t>(0x48 | ((reg >> 3) & 1)));
        u8(static_cast<std::uint8_t>(0xb8 + (reg & 7)));
        u64(value);
    }

    // mov r64, [rbp+disp8]
    void movR64Frame8(unsigned reg, std::int8_t disp) {
        u8(static_cast<std::uint8_t>(0x48 | ((reg >> 3) & 1)));
        u8(0x8b);
        u8(static_cast<std::uint8_t>(0x45 | ((reg & 7) << 3)));
        u8(static_cast<std::uint8_t>(disp));
    }

    // mov [rbp+disp8], r64
    void movFrame8R64(std::int8_t disp, unsigned reg) {
        u8(static_cast<std::uint8_t>(0x48 | ((reg >> 3) & 1)));
        u8(0x89);
        u8(static_cast<std::uint8_t>(0x45 | ((reg & 7) << 3)));
        u8(static_cast<std::uint8_t>(disp));
    }

    void addRaxRcx()  { u8(0x48); u8(0x01); u8(0xc8); }
    void subRaxRcx()  { u8(0x48); u8(0x29); u8(0xc8); }
    void imulRaxRcx() { u8(0x48); u8(0x0f); u8(0xaf); u8(0xc1); }
    void negRax()     { u8(0x48); u8(0xf7); u8(0xd8); }
};

bool parseInt64(const std::string& text, std::int64_t& out) {
    const char* first = text.data();
    const char* last = first + text.size();
    auto r = std::from_chars(first, last, out, 10);
    return r.ec == std::errc{} && r.ptr == last;
}

bool isInt(const std::string& t) {
    return t == "int" || t == "i64";
}

bool supportedOwnership(OwnershipKind k) {
    // This first machine-code slice is a primitive integer backend. GC/borrow
    // metadata is still carried by MIR, but no object ownership operation is
    // lowered here.
    return k == OwnershipKind::GC;
}

struct FnEmitter {
    const ir::Function& fn;
    Encoder e;
    std::unordered_map<ir::ValueId, std::int8_t> slots;
    std::size_t frameSize{0};
    std::string error;

    explicit FnEmitter(const ir::Function& function) : fn(function) {}

    bool fail(const std::string& msg) {
        error = fn.name + ": " + msg;
        return false;
    }

    bool validateType() {
        if (!isInt(fn.returnType)) return fail("machine backend currently supports only int returns");
        for (const auto& t : fn.parameterTypes)
            if (!isInt(t)) return fail("machine backend currently supports only int parameters");
        if (fn.isAsync) return fail("async functions are not supported by the synchronous machine backend");
        return true;
    }

    bool collectSlots() {
        std::unordered_set<ir::ValueId> ids;
        for (const auto& b : fn.blocks)
            for (const auto& ins : b.instructions) {
                if (ins.result) ids.insert(ins.result);
                if (ins.operand0) ids.insert(ins.operand0);
                if (ins.operand1) ids.insert(ins.operand1);
                for (auto v : ins.operands) if (v) ids.insert(v);
            }

        // RBP-relative frame slots are negative offsets. Keep them within the
        // disp8 encoding for this first backend slice; larger frames are a
        // deliberate unsupported case rather than silently emitting bad code.
        std::size_t i = 0;
        for (auto id : ids) {
            const std::size_t off = 8 * (++i);
            if (off > 120) return fail("function needs more than 15 machine-code frame slots");
            slots[id] = static_cast<std::int8_t>(-static_cast<int>(off));
        }
        frameSize = ((i * 8 + 15) / 16) * 16;
        return true;
    }

    bool slot(ir::ValueId id, std::int8_t& out) {
        auto it = slots.find(id);
        if (it == slots.end()) return fail("instruction references unknown value " + std::to_string(id));
        out = it->second;
        return true;
    }

    bool emitLoad(ir::ValueId id, unsigned reg) {
        std::int8_t off{};
        if (!slot(id, off)) return false;
        e.movR64Frame8(reg, off);
        return true;
    }

    bool emitStore(ir::ValueId id, unsigned reg) {
        std::int8_t off{};
        if (!slot(id, off)) return false;
        e.movFrame8R64(off, reg);
        return true;
    }

    bool emitBinary(const ir::Instruction& ins) {
        if (!emitLoad(ins.operand0, 0) || !emitLoad(ins.operand1, 1)) return false;
        // rcx is encoded as register 1.
        if (ins.symbol == std::to_string(static_cast<int>(zl::TokenType::PLUS))) e.addRaxRcx();
        else if (ins.symbol == std::to_string(static_cast<int>(zl::TokenType::MINUS))) e.subRaxRcx();
        else if (ins.symbol == std::to_string(static_cast<int>(zl::TokenType::STAR))) e.imulRaxRcx();
        else return fail("unsupported binary operator '" + ins.symbol + "'");
        return emitStore(ins.result, 0);
    }

    bool emit() {
        if (!validateType() || !collectSlots()) return false;

        e.pushRbp();
        e.movRbpRsp();
        e.subRsp(frameSize);

        std::size_t parameterIndex = 0;
        bool terminated = false;

        // This first slice intentionally accepts one straight-line block.
        if (fn.blocks.size() != 1) return fail("control flow is not yet supported by the machine backend");

        for (const auto& ins : fn.blocks[0].instructions) {
            if (terminated) return fail("instructions appear after a terminating Return");

            switch (ins.opcode) {
                case ir::Opcode::Const: {
                    std::int64_t value{};
                    if (!parseInt64(ins.symbol, value)) return fail("invalid integer constant '" + ins.symbol + "'");
                    e.movR64Imm64(0, static_cast<std::uint64_t>(value));
                    if (!emitStore(ins.result, 0)) return false;
                    break;
                }
                case ir::Opcode::DefineLocal: {
                    if (!supportedOwnership(ins.ownership)) return fail("ownership mode is not supported in this machine-code slice");
                    // Function parameters are represented by the initial DefineLocal
                    // instructions emitted by IR lowering.
                    if (parameterIndex < fn.parameterTypes.size()) {
#ifdef _WIN32
                        static constexpr unsigned argRegs[] = {1, 2, 8, 9}; // rcx, rdx, r8, r9
#else
                        static constexpr unsigned argRegs[] = {7, 6, 2, 1, 8, 9}; // rdi, rsi, rdx, rcx, r8, r9
#endif
                        if (parameterIndex >= sizeof(argRegs) / sizeof(argRegs[0]))
                            return fail("too many parameters for current ABI slice");
                        // Store the incoming ABI register directly.
                        std::int8_t off{};
                        if (!slot(ins.result, off)) return false;
                        e.movFrame8R64(off, argRegs[parameterIndex]);
                        ++parameterIndex;
                    } else if (ins.operand0) {
                        if (!emitLoad(ins.operand0, 0) || !emitStore(ins.result, 0)) return false;
                    }
                    break;
                }
                case ir::Opcode::LoadLocal:
                    if (!emitLoad(ins.operand0, 0) || !emitStore(ins.result, 0)) return false;
                    break;
                case ir::Opcode::AssignLocal:
                    if (!emitLoad(ins.operand0, 0) || !emitStore(ins.result, 0)) return false;
                    break;
                case ir::Opcode::Unary:
                    if (ins.symbol != std::to_string(static_cast<int>(zl::TokenType::MINUS)))
                        return fail("unsupported unary operator '" + ins.symbol + "'");
                    if (!emitLoad(ins.operand0, 0)) return false;
                    e.negRax();
                    if (!emitStore(ins.result, 0)) return false;
                    break;
                case ir::Opcode::Binary:
                    if (!emitBinary(ins)) return false;
                    break;
                case ir::Opcode::Return:
                    if (ins.operand0 && !emitLoad(ins.operand0, 0)) return false;
                    e.addRsp(frameSize);
                    e.popRbp();
                    e.ret();
                    terminated = true;
                    break;
                case ir::Opcode::Nop:
                    break;
                default:
                    return fail("unsupported MIR opcode in straight-line machine backend");
            }
        }

        if (!terminated) return fail("function does not end in Return");
        return true;
    }
};

} // namespace

CompileResult emitX64(const ir::Module& module) {
    CompileResult out;
    for (const auto& fn : module.functions) {
        FnEmitter emitter(fn);
        if (!emitter.emit()) {
            out.error = emitter.error;
            return out;
        }
        out.functions.push_back(FunctionCode{fn.name, std::move(emitter.e.code),
                                             emitter.frameSize, fn.parameterTypes.size()});
    }
    out.success = true;
    return out;
}

} // namespace zl::machine
