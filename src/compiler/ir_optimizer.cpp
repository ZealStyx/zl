#include "zl/compiler/ir_optimizer.hpp"
#include "zl/lexer/token.hpp"

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

namespace zl::ir {
namespace {

std::optional<zl::TokenType> parseOperatorToken(const Instruction& ins) {
    if (ins.opcode != Opcode::Unary && ins.opcode != Opcode::Binary) return std::nullopt;
    try {
        std::size_t pos = 0;
        const int raw = std::stoi(ins.symbol, &pos);
        if (pos != ins.symbol.size()) return std::nullopt;
        return static_cast<zl::TokenType>(raw);
    } catch (...) {
        return std::nullopt;
    }
}

struct ConstValue {
    enum class Kind { None, Int, Double, Bool } kind{Kind::None};
    std::int64_t i{0};
    double d{0.0};
    bool b{false};
};

bool checkedAdd(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) return false;
    out = a + b; return true;
}

bool checkedSub(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if ((b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) ||
        (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b)) return false;
    out = a - b; return true;
}

bool checkedMul(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if (a == 0 || b == 0) { out = 0; return true; }
    if (a == -1) { if (b == std::numeric_limits<std::int64_t>::min()) return false; out = -b; return true; }
    if (b == -1) { if (a == std::numeric_limits<std::int64_t>::min()) return false; out = -a; return true; }
    if (a > 0) {
        if (b > 0) { if (a > std::numeric_limits<std::int64_t>::max() / b) return false; }
        else { if (b < std::numeric_limits<std::int64_t>::min() / a) return false; }
    } else {
        if (b > 0) { if (a < std::numeric_limits<std::int64_t>::min() / b) return false; }
        else { if (a != 0 && b < std::numeric_limits<std::int64_t>::max() / a) return false; }
    }
    out = a * b; return true;
}

bool parseConst(const Instruction& ins, const std::unordered_map<ValueId, ConstValue>& values, ConstValue& out) {
    if (ins.opcode == Opcode::Const) {
        std::int64_t i = 0;
        if (!ins.symbol.empty()) {
            try {
                std::size_t pos = 0;
                i = std::stoll(ins.symbol, &pos);
                if (pos == ins.symbol.size()) { out = {ConstValue::Kind::Int, i, static_cast<double>(i), i != 0}; return true; }
            } catch (...) {}
            if (ins.symbol == "true") { out = {ConstValue::Kind::Bool, 0, 0.0, true}; return true; }
            if (ins.symbol == "false") { out = {ConstValue::Kind::Bool, 0, 0.0, false}; return true; }
            try {
                std::size_t pos = 0;
                double d = std::stod(ins.symbol, &pos);
                if (pos == ins.symbol.size()) { out.kind = ConstValue::Kind::Double; out.d = d; return true; }
            } catch (...) {}
        }
        return false;
    }
    auto it = values.find(ins.operand0);
    if (it == values.end()) return false;
    if (ins.opcode == Opcode::Unary) {
        if (ins.symbol == std::to_string(static_cast<int>(zl::TokenType::MINUS)) && it->second.kind == ConstValue::Kind::Int) {
            if (it->second.i == std::numeric_limits<std::int64_t>::min()) return false;
            out = it->second; out.i = -out.i; out.d = static_cast<double>(out.i); out.b = out.i != 0; return true;
        }
        if (ins.symbol == std::to_string(static_cast<int>(zl::TokenType::MINUS)) && it->second.kind == ConstValue::Kind::Double) {
            if (!std::isfinite(it->second.d)) return false;
            out.kind = ConstValue::Kind::Double;
            out.d = -it->second.d;
            return std::isfinite(out.d);
        }
        if (ins.symbol == std::to_string(static_cast<int>(zl::TokenType::NOT)) && it->second.kind == ConstValue::Kind::Bool) {
            out.kind = ConstValue::Kind::Bool; out.b = !it->second.b; return true;
        }
        if (ins.symbol == std::to_string(static_cast<int>(zl::TokenType::BIT_NOT)) && it->second.kind == ConstValue::Kind::Int) {
            out = it->second;
            out.i = ~it->second.i;
            out.d = static_cast<double>(out.i);
            out.b = out.i != 0;
            return true;
        }
        return false;
    }
    if (ins.opcode != Opcode::Binary) return false;
    auto jt = values.find(ins.operand1);
    if (jt == values.end()) return false;
    const auto parsedOp = parseOperatorToken(ins);
    if (!parsedOp) return false;
    const auto op = static_cast<int>(*parsedOp);
    if (it->second.kind == ConstValue::Kind::Int && jt->second.kind == ConstValue::Kind::Int) {
        const auto a = it->second.i, b = jt->second.i;
        std::int64_t r = 0; bool br = false;
        if (static_cast<zl::TokenType>(op) == zl::TokenType::POW && b >= 0) {
            // Large non-negative exponents are still exactly foldable for the
            // trivial integral bases. For |a| > 1, any exponent > 63 cannot
            // fit in int64, so let the runtime/native path preserve the exact
            // overflow semantics instead of guessing.
            if (b > 63) {
                if (a == 0) {
                    out = {ConstValue::Kind::Int, 0, 0.0, false};
                    return true;
                }
                if (a == 1) {
                    out = {ConstValue::Kind::Int, 1, 1.0, true};
                    return true;
                }
                if (a == -1) {
                    const std::int64_t result = (b & 1) ? -1 : 1;
                    out = {ConstValue::Kind::Int, result, static_cast<double>(result), result != 0};
                    return true;
                }
            }
            std::int64_t result = 1;
            std::int64_t factor = a;
            std::uint64_t n = static_cast<std::uint64_t>(b);
            bool exact = true;
            while (n != 0) {
                if (n & 1u) { std::int64_t next = 0; if (!checkedMul(result, factor, next)) { exact = false; break; } result = next; }
                n >>= 1u;
                if (n != 0) { std::int64_t next = 0; if (!checkedMul(factor, factor, next)) { exact = false; break; } factor = next; }
            }
            if (exact) { out = {ConstValue::Kind::Int, result, static_cast<double>(result), result != 0}; return true; }
            const double d = std::pow(static_cast<double>(a), static_cast<double>(b));
            if (std::isfinite(d)) { out.kind = ConstValue::Kind::Double; out.d = d; return true; }
        }
        switch (static_cast<zl::TokenType>(op)) {
            case zl::TokenType::PLUS: if (!checkedAdd(a, b, r)) return false; break;
            case zl::TokenType::MINUS: if (!checkedSub(a, b, r)) return false; break;
            case zl::TokenType::STAR: if (!checkedMul(a, b, r)) return false; break;
            case zl::TokenType::SLASH: if (b == 0 || (a == std::numeric_limits<std::int64_t>::min() && b == -1)) return false; r = a / b; break;
            case zl::TokenType::PERCENT: if (b == 0 || (a == std::numeric_limits<std::int64_t>::min() && b == -1)) return false; r = a % b; break;
            case zl::TokenType::EQ: br = a == b; out = {ConstValue::Kind::Bool,0,0.0,br}; return true;
            case zl::TokenType::NEQ: br = a != b; out = {ConstValue::Kind::Bool,0,0.0,br}; return true;
            case zl::TokenType::LT: br = a < b; out = {ConstValue::Kind::Bool,0,0.0,br}; return true;
            case zl::TokenType::GT: br = a > b; out = {ConstValue::Kind::Bool,0,0.0,br}; return true;
            case zl::TokenType::LTE: br = a <= b; out = {ConstValue::Kind::Bool,0,0.0,br}; return true;
            case zl::TokenType::GTE: br = a >= b; out = {ConstValue::Kind::Bool,0,0.0,br}; return true;
            case zl::TokenType::BIT_AND: r = a & b; break;
            case zl::TokenType::BIT_OR: r = a | b; break;
            case zl::TokenType::BIT_XOR: r = a ^ b; break;
            case zl::TokenType::SHL:
                if (b < 0 || b >= 64 || a < 0 || a > (std::numeric_limits<std::int64_t>::max() >> b)) return false;
                r = a << b; break;
            case zl::TokenType::SHR:
                if (b < 0 || b >= 64 || a < 0) return false;
                r = a >> b; break;
            case zl::TokenType::USHR:
                if (b < 0 || b >= 64) return false;
                r = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) >> b); break;
            default: return false;
        }
        out = {ConstValue::Kind::Int, r, static_cast<double>(r), r != 0}; return true;
    }
    if ((it->second.kind == ConstValue::Kind::Double || it->second.kind == ConstValue::Kind::Int) &&
        (jt->second.kind == ConstValue::Kind::Double || jt->second.kind == ConstValue::Kind::Int)) {
        const double a = it->second.kind == ConstValue::Kind::Double ? it->second.d : static_cast<double>(it->second.i);
        const double b = jt->second.kind == ConstValue::Kind::Double ? jt->second.d : static_cast<double>(jt->second.i);
        const auto tok = static_cast<zl::TokenType>(op);
        if (tok == zl::TokenType::POW) {
            const double d = std::pow(a, b);
            if (std::isfinite(d)) { out.kind = ConstValue::Kind::Double; out.d = d; return true; }
        }
        if (std::isfinite(a) && std::isfinite(b)) {
            double d = 0.0;
            bool fold = true;
            switch (tok) {
                case zl::TokenType::PLUS: d = a + b; break;
                case zl::TokenType::MINUS: d = a - b; break;
                case zl::TokenType::STAR: d = a * b; break;
                case zl::TokenType::SLASH: if (b == 0.0) fold = false; else d = a / b; break;
                default: fold = false; break;
            }
            if (fold && std::isfinite(d)) { out.kind = ConstValue::Kind::Double; out.d = d; return true; }
        }
        switch (tok) {
            case zl::TokenType::EQ: out = {ConstValue::Kind::Bool, 0, 0.0, a == b}; return true;
            case zl::TokenType::NEQ: out = {ConstValue::Kind::Bool, 0, 0.0, a != b}; return true;
            case zl::TokenType::LT: out = {ConstValue::Kind::Bool, 0, 0.0, a < b}; return true;
            case zl::TokenType::GT: out = {ConstValue::Kind::Bool, 0, 0.0, a > b}; return true;
            case zl::TokenType::LTE: out = {ConstValue::Kind::Bool, 0, 0.0, a <= b}; return true;
            case zl::TokenType::GTE: out = {ConstValue::Kind::Bool, 0, 0.0, a >= b}; return true;
            default: break;
        }
    }

    if (it->second.kind == ConstValue::Kind::Bool && jt->second.kind == ConstValue::Kind::Bool) {
        bool r = false;
        switch (static_cast<zl::TokenType>(op)) {
            case zl::TokenType::AND: r = it->second.b && jt->second.b; break;
            case zl::TokenType::OR: r = it->second.b || jt->second.b; break;
            case zl::TokenType::EQ: r = it->second.b == jt->second.b; break;
            case zl::TokenType::NEQ: r = it->second.b != jt->second.b; break;
            default: return false;
        }
        out = {ConstValue::Kind::Bool,0,0.0,r}; return true;
    }
    return false;
}

bool isNonThrowingDeadPure(const Instruction& ins) {
    if (ins.opcode == Opcode::Unary) {
        const auto parsedTok = parseOperatorToken(ins);
        if (!parsedTok) return false;
        const auto tok = *parsedTok;
        return tok == zl::TokenType::NOT || tok == zl::TokenType::BIT_NOT;
    }
    if (ins.opcode == Opcode::Binary) {
        const auto parsedTok = parseOperatorToken(ins);
        if (!parsedTok) return false;
        const auto tok = *parsedTok;
        switch (tok) {
            case zl::TokenType::EQ:
            case zl::TokenType::NEQ:
            case zl::TokenType::LT:
            case zl::TokenType::GT:
            case zl::TokenType::LTE:
            case zl::TokenType::GTE:
            case zl::TokenType::BIT_AND:
            case zl::TokenType::BIT_OR:
            case zl::TokenType::BIT_XOR:
                return true;
            default:
                return false;
        }
    }
    return false;
}

void eliminateDeadPureExpressions(Function& fn) {
    // Removing one dead expression can make an earlier pure producer dead as
    // well. Iterate to a fixed point so a chain of now-unused non-throwing
    // primitive expressions does not leave dead instructions behind.
    for (;;) {
        std::unordered_map<ValueId, std::size_t> uses;
        for (const auto& block : fn.blocks) {
            for (const auto& ins : block.instructions) {
                auto count = [&](ValueId value) {
                    if (value) ++uses[value];
                };
                count(ins.operand0);
                count(ins.operand1);
                for (const auto value : ins.operands) count(value);
            }
        }

        bool changed = false;
        for (auto& block : fn.blocks) {
            for (auto& ins : block.instructions) {
                if ((ins.opcode != Opcode::Unary && ins.opcode != Opcode::Binary) || !ins.result ||
                    !isNonThrowingDeadPure(ins) || uses[ins.result] != 0) {
                    continue;
                }
                ins.opcode = Opcode::Nop;
                ins.result = 0;
                ins.operand0 = ins.operand1 = 0;
                ins.symbol.clear();
                ins.operands.clear();
                changed = true;
            }
        }
        if (!changed) break;
    }
}

void rewriteInstructionConstants(Instruction& ins, const std::unordered_map<ValueId, ConstValue>& values) {
    auto it = values.find(ins.result);
    if (it == values.end()) return;
    if (ins.opcode != Opcode::Const && ins.opcode != Opcode::Unary && ins.opcode != Opcode::Binary) return;
    const auto& v = it->second;
    ins.opcode = Opcode::Const;
    ins.operand0 = ins.operand1 = 0;
    ins.operands.clear();
    if (v.kind == ConstValue::Kind::Int) ins.symbol = std::to_string(v.i);
    else if (v.kind == ConstValue::Kind::Double) ins.symbol = std::to_string(v.d);
    else if (v.kind == ConstValue::Kind::Bool) ins.symbol = v.b ? "true" : "false";
}

void trimInstructionsAfterTerminator(BasicBlock& block) {
    // Instructions after an unconditional terminator can never execute.
    // Branch remains conditional, so it is intentionally not treated here.
    for (std::size_t i = 0; i < block.instructions.size(); ++i) {
        const auto op = block.instructions[i].opcode;
        if (op == Opcode::Return || op == Opcode::Throw || op == Opcode::Jump) {
            block.instructions.resize(i + 1);
            return;
        }
    }
}

BasicBlock optimizeBlock(const BasicBlock& input) {
    BasicBlock out = input;
    std::unordered_map<ValueId, ConstValue> constants;
    std::unordered_map<ValueId, ConstValue> localConstants;
    std::unordered_map<ValueId, bool> localConstEligible;
    std::unordered_map<ValueId, ValueId> localCopies;
    std::unordered_map<ValueId, ValueId> valueAliases;
    std::unordered_map<std::string, ValueId> pureExprs;

    auto resolveAlias = [&](ValueId value) {
        std::size_t guard = 0;
        while (value) {
            auto it = valueAliases.find(value);
            if (it == valueAliases.end() || it->second == value) break;
            value = it->second;
            if (++guard > valueAliases.size() + 1) break;
        }
        return value;
    };

    auto rewriteUses = [&](Instruction& ins) {
        switch (ins.opcode) {
            case Opcode::Unary:
            case Opcode::Binary:
            case Opcode::Return:
            case Opcode::Branch:
                ins.operand0 = resolveAlias(ins.operand0);
                ins.operand1 = resolveAlias(ins.operand1);
                break;
            case Opcode::Call:
                for (auto& arg : ins.operands) arg = resolveAlias(arg);
                ins.operand0 = resolveAlias(ins.operand0);
                ins.operand1 = resolveAlias(ins.operand1);
                break;
            case Opcode::DefineLocal:
            case Opcode::AssignLocal:
                ins.operand0 = resolveAlias(ins.operand0);
                ins.operand1 = resolveAlias(ins.operand1);
                break;
            default:
                break;
        }
    };

    auto invalidatePureExpressions = [&]() {
        // Value IDs are immutable, so assignments do not invalidate an
        // expression whose operands are SSA values. The expression table is
        // intentionally limited to value-producing primitive operations.
    };

    for (auto& ins : out.instructions) {
        if (ins.opcode == Opcode::LoadLocal) {
            auto lit = localConstants.find(ins.operand0);
            if (lit != localConstants.end() && localConstEligible[ins.operand0]) {
                ins.opcode = Opcode::Const;
                ins.operand0 = ins.operand1 = 0;
                ins.operands.clear();
                if (lit->second.kind == ConstValue::Kind::Int) ins.symbol = std::to_string(lit->second.i);
                else if (lit->second.kind == ConstValue::Kind::Double) ins.symbol = std::to_string(lit->second.d);
                else if (lit->second.kind == ConstValue::Kind::Bool) ins.symbol = lit->second.b ? "true" : "false";
            } else if (ins.result && localConstEligible[ins.operand0]) {
                auto copy = localCopies.find(ins.operand0);
                if (copy != localCopies.end()) {
                    valueAliases[ins.result] = resolveAlias(copy->second);
                }
            }
        }

        rewriteUses(ins);

        // Apply only semantics-preserving integer bitwise identities. These
        // rules are intentionally limited to primitive SSA values and do not
        // touch ownership-sensitive instructions or potentially-throwing
        // arithmetic.
        // Safe redundant-unary elimination. Double logical negation preserves
        // a known boolean value, while double bitwise-not preserves a known
        // integer value. Both rewrites reuse the inner SSA value and therefore
        // do not add/remove evaluation of the operand.
        if (ins.opcode == Opcode::Unary && ins.result) {
            const auto parsedTok = parseOperatorToken(ins);
            if (!parsedTok) continue;
            const auto tok = *parsedTok;
            const ValueId inner = resolveAlias(ins.operand0);
            if (tok == zl::TokenType::NOT || tok == zl::TokenType::BIT_NOT) {
                for (const auto& candidate : out.instructions) {
                    if (candidate.result == inner && candidate.opcode == Opcode::Unary &&
                        candidate.symbol == ins.symbol) {
                        valueAliases[ins.result] = inner;
                        ins.opcode = Opcode::Nop;
                        ins.operand0 = ins.operand1 = 0;
                        ins.symbol.clear();
                        ins.operands.clear();
                        break;
                    }
                }
            }
        }

        if (ins.opcode == Opcode::Binary && ins.result) {
            const auto parsedTok = parseOperatorToken(ins);
            if (!parsedTok) continue;
            const auto tok = *parsedTok;
            const ValueId a = resolveAlias(ins.operand0);
            const ValueId b = resolveAlias(ins.operand1);
            const auto ac = constants.find(a);
            const auto bc = constants.find(b);
            auto makeConst = [&](std::int64_t value) {
                ins.opcode = Opcode::Const;
                ins.operand0 = ins.operand1 = 0;
                ins.operands.clear();
                ins.symbol = std::to_string(value);
            };
            auto aliasResult = [&](ValueId source) {
                if (!source || source == ins.result) return false;
                valueAliases[ins.result] = source;
                ins.opcode = Opcode::Nop;
                ins.operand0 = ins.operand1 = 0;
                ins.symbol.clear();
                ins.operands.clear();
                return true;
            };

            const bool ai = ac != constants.end() && ac->second.kind == ConstValue::Kind::Int;
            const bool bi = bc != constants.end() && bc->second.kind == ConstValue::Kind::Int;
            // Safe boolean identities. These are restricted to a known boolean
            // constant on one side, so removing the logical operation cannot
            // change short-circuit behavior or evaluation of the other operand.
            if (tok == zl::TokenType::AND || tok == zl::TokenType::OR) {
                const bool ab = ac != constants.end() && ac->second.kind == ConstValue::Kind::Bool;
                const bool bb = bc != constants.end() && bc->second.kind == ConstValue::Kind::Bool;
                if (ab && ac->second.b && tok == zl::TokenType::AND) {
                    aliasResult(b);
                } else if (bb && bc->second.b && tok == zl::TokenType::AND) {
                    aliasResult(a);
                } else if (ab && !ac->second.b && tok == zl::TokenType::OR) {
                    aliasResult(b);
                } else if (bb && !bc->second.b && tok == zl::TokenType::OR) {
                    aliasResult(a);
                }
            }

            // Safe integer arithmetic identities. These are limited to integer
            // SSA values and only remove operations whose identity result cannot
            // alter overflow or fault behavior.
            if (ai && tok == zl::TokenType::PLUS && ac->second.i == 0) {
                aliasResult(b);
            } else if (bi && tok == zl::TokenType::PLUS && bc->second.i == 0) {
                aliasResult(a);
            } else if (bi && tok == zl::TokenType::MINUS && bc->second.i == 0) {
                aliasResult(a);
            } else if (ai && tok == zl::TokenType::STAR && ac->second.i == 1) {
                aliasResult(b);
            } else if (bi && tok == zl::TokenType::STAR && bc->second.i == 1) {
                aliasResult(a);
            } else if (ai && tok == zl::TokenType::STAR && ac->second.i == 0) {
                // Multiplication by zero cannot overflow and does not fault for
                // integer operands. Keep the other operand evaluated, but remove
                // the redundant arithmetic operation itself.
                makeConst(0);
            } else if (bi && tok == zl::TokenType::STAR && bc->second.i == 0) {
                makeConst(0);
            } else if (bi && tok == zl::TokenType::SLASH && bc->second.i == 1) {
                aliasResult(a);
            } else if (bi && tok == zl::TokenType::PERCENT && bc->second.i == 1) {
                makeConst(0);
            } else if (bi && (tok == zl::TokenType::SHL || tok == zl::TokenType::SHR ||
                              tok == zl::TokenType::USHR) && bc->second.i == 0) {
                aliasResult(a);
            }

            if (tok == zl::TokenType::BIT_AND || tok == zl::TokenType::BIT_OR || tok == zl::TokenType::BIT_XOR) {
                // Idempotent integer bitwise operators are safe once the
                // type checker has established integer operands.
                if ((tok == zl::TokenType::BIT_AND || tok == zl::TokenType::BIT_OR) && a == b) {
                    aliasResult(a);
                } else if (ai && ac->second.i == 0) {
                    if (tok == zl::TokenType::BIT_AND) makeConst(0);
                    else if (tok == zl::TokenType::BIT_OR || tok == zl::TokenType::BIT_XOR) aliasResult(b);
                } else if (bi && bc->second.i == 0) {
                    if (tok == zl::TokenType::BIT_AND) makeConst(0);
                    else aliasResult(a);
                } else if (ai && ac->second.i == -1 && tok == zl::TokenType::BIT_OR) {
                    makeConst(-1);
                } else if (bi && bc->second.i == -1 && tok == zl::TokenType::BIT_OR) {
                    makeConst(-1);
                } else if (ai && bi && tok == zl::TokenType::BIT_XOR && a == b) {
                    makeConst(0);
                } else if (tok == zl::TokenType::BIT_XOR && a == b) {
                    makeConst(0);
                } else if (ai && ac->second.i == -1 && tok == zl::TokenType::BIT_AND) {
                    aliasResult(b);
                } else if (bi && bc->second.i == -1 && tok == zl::TokenType::BIT_AND) {
                    aliasResult(a);
                }
            }
        }

        // Perform local CSE only for expressions proven non-throwing. Reusing
        // the result of arithmetic such as division or overflowing addition
        // could otherwise remove a second observable fault.
        bool cseEligible = false;
        if (ins.opcode == Opcode::Unary || ins.opcode == Opcode::Binary) {
            cseEligible = isNonThrowingDeadPure(ins);
        }
        if (cseEligible) {
            const ValueId a = resolveAlias(ins.operand0);
            const ValueId b = resolveAlias(ins.operand1);
            ValueId keyA = a;
            ValueId keyB = b;
            if (ins.opcode == Opcode::Binary) {
                const auto parsedTok = parseOperatorToken(ins);
            if (!parsedTok) continue;
            const auto tok = *parsedTok;
                const bool commutative = tok == zl::TokenType::EQ ||
                    tok == zl::TokenType::NEQ ||
                    tok == zl::TokenType::BIT_AND ||
                    tok == zl::TokenType::BIT_OR ||
                    tok == zl::TokenType::BIT_XOR;
                // Canonicalize only the CSE key. The instruction's operand order
                // is never changed, so source evaluation order remains intact.
                if (commutative && keyB < keyA) std::swap(keyA, keyB);
            }
            std::string key = std::to_string(static_cast<int>(ins.opcode)) + ":" + ins.symbol + ":" + std::to_string(keyA);
            if (ins.opcode == Opcode::Binary) key += ":" + std::to_string(keyB);
            auto prior = pureExprs.find(key);
            if (prior != pureExprs.end() && prior->second != ins.result) {
                valueAliases[ins.result] = prior->second;
                ins.opcode = Opcode::Nop;
                ins.operand0 = ins.operand1 = 0;
                ins.symbol.clear();
                ins.operands.clear();
            } else if (ins.result) {
                pureExprs.emplace(std::move(key), ins.result);
            }
        } else if (ins.opcode == Opcode::Call || ins.opcode == Opcode::Throw) {
            // Calls/throws are barriers for future memory-sensitive pure
            // expression extensions; current primitive value IDs remain SSA.
            pureExprs.clear();
        }

        ConstValue value;
        if (ins.opcode != Opcode::Nop && parseConst(ins, constants, value)) constants[ins.result] = value;
        rewriteInstructionConstants(ins, constants);

        if (ins.opcode == Opcode::DefineLocal || ins.opcode == Opcode::AssignLocal) {
            const bool eligible = ins.ownership == OwnershipKind::GC;
            localConstEligible[ins.result] = eligible;
            const ValueId source = resolveAlias(ins.operand0);
            auto c = constants.find(source);
            if (eligible && c != constants.end()) localConstants[ins.result] = c->second;
            else localConstants.erase(ins.result);
            if (eligible && source) localCopies[ins.result] = source;
            else localCopies.erase(ins.result);
        } else if (ins.opcode == Opcode::MoveLocal || ins.opcode == Opcode::BorrowLocal || ins.opcode == Opcode::EndBorrow || ins.opcode == Opcode::DropLocal) {
            localConstants.erase(ins.operand0);
            localConstEligible.erase(ins.operand0);
            localCopies.erase(ins.operand0);
        }

    }
    trimInstructionsAfterTerminator(out);
    return out;
}

} // namespace

Module optimize(const Module& module) {
    Module out = module;
    for (auto& fn : out.functions) {
        if (fn.blocks.empty()) continue;

        std::unordered_map<BlockId, std::size_t> byId;
        for (std::size_t i = 0; i < fn.blocks.size(); ++i) byId[fn.blocks[i].id] = i;

        // First optimize each block independently. Ownership/borrow instructions are
        // never removed or rewritten by this optimizer.
        for (auto& block : fn.blocks) {
            block = optimizeBlock(block);
        }

        // Propagate only immutable SSA constants across CFG edges. At a join, a
        // value is considered constant only when every reachable predecessor
        // agrees on the exact same primitive value. This deliberately ignores
        // mutable locals and ownership state.
        byId.clear();
        for (std::size_t i = 0; i < fn.blocks.size(); ++i) byId[fn.blocks[i].id] = i;

        std::unordered_map<BlockId, std::vector<BlockId>> predecessors;
        for (const auto& block : fn.blocks) {
            for (const auto succ : block.successors) predecessors[succ].push_back(block.id);
        }

        std::unordered_map<BlockId, std::unordered_map<ValueId, ConstValue>> inConstants;
        std::unordered_map<BlockId, std::unordered_map<ValueId, ConstValue>> outConstants;

        auto sameConst = [](const ConstValue& a, const ConstValue& b) {
            if (a.kind != b.kind) return false;
            switch (a.kind) {
                case ConstValue::Kind::Int: return a.i == b.i;
                case ConstValue::Kind::Double: return a.d == b.d;
                case ConstValue::Kind::Bool: return a.b == b.b;
                default: return true;
            }
        };

        std::vector<BlockId> worklist;
        worklist.push_back(fn.blocks.front().id);
        std::unordered_set<BlockId> queued{fn.blocks.front().id};

        auto transferConstants = [&](const BasicBlock& block, const std::unordered_map<ValueId, ConstValue>& incoming) {
            auto constants = incoming;
            for (const auto& ins : block.instructions) {
                ConstValue value;
                if (ins.opcode != Opcode::Nop && parseConst(ins, constants, value)) {
                    constants[ins.result] = value;
                } else if (ins.result && (ins.opcode == Opcode::Const || ins.opcode == Opcode::Unary ||
                                          ins.opcode == Opcode::Binary)) {
                    constants.erase(ins.result);
                }
            }
            return constants;
        };

        while (!worklist.empty()) {
            const BlockId id = worklist.back();
            worklist.pop_back();
            queued.erase(id);
            const auto bit = byId.find(id);
            if (bit == byId.end()) continue;
            const auto& block = fn.blocks[bit->second];

            std::unordered_map<ValueId, ConstValue> incoming;
            if (id != fn.blocks.front().id) {
                const auto pit = predecessors.find(id);
                if (pit == predecessors.end() || pit->second.empty()) continue;
                bool first = true;
                for (const auto pred : pit->second) {
                    const auto oit = outConstants.find(pred);
                    if (oit == outConstants.end()) { first = false; break; }
                    if (first) {
                        incoming = oit->second;
                        first = false;
                    } else {
                        for (auto it = incoming.begin(); it != incoming.end();) {
                            const auto other = oit->second.find(it->first);
                            if (other == oit->second.end() || !sameConst(it->second, other->second)) it = incoming.erase(it);
                            else ++it;
                        }
                    }
                }
            }

            const auto previousIn = inConstants.find(id);
            const bool inChanged = previousIn == inConstants.end() || previousIn->second.size() != incoming.size();
            if (inChanged || previousIn == inConstants.end()) inConstants[id] = incoming;

            auto transferred = transferConstants(block, incoming);
            const auto previousOut = outConstants.find(id);
            bool outChanged = previousOut == outConstants.end() || previousOut->second.size() != transferred.size();
            if (!outChanged && previousOut != outConstants.end()) {
                for (const auto& [value, constant] : transferred) {
                    const auto it = previousOut->second.find(value);
                    if (it == previousOut->second.end() || !sameConst(it->second, constant)) { outChanged = true; break; }
                }
            }
            if (outChanged) {
                outConstants[id] = std::move(transferred);
                for (const auto succ : block.successors) {
                    if (!queued.count(succ)) { worklist.push_back(succ); queued.insert(succ); }
                }
            }
        }

        // Rewrite uses with the converged immutable-constant facts. Repeat to a
        // fixed point because folding one expression can expose another.
        for (int iteration = 0; iteration < 16; ++iteration) {
            bool changed = false;
            for (auto& block : fn.blocks) {
                auto constants = inConstants[block.id];
                for (auto& ins : block.instructions) {
                    if (ins.opcode == Opcode::Branch) {
                        const auto it = constants.find(ins.operand0);
                        if (it != constants.end() && it->second.kind == ConstValue::Kind::Bool && block.successors.size() == 2) {
                            const BlockId chosen = block.successors[it->second.b ? 0 : 1];
                            block.successors.assign(1, chosen);
                            ins.opcode = Opcode::Jump;
                            ins.operand0 = ins.operand1 = 0;
                            ins.operands.clear();
                            changed = true;
                        }
                    }
                    ConstValue value;
                    if (ins.opcode != Opcode::Nop && parseConst(ins, constants, value)) {
                        if (ins.opcode != Opcode::Const) {
                            ins.opcode = Opcode::Const;
                            ins.operand0 = ins.operand1 = 0;
                            ins.operands.clear();
                            if (value.kind == ConstValue::Kind::Int) ins.symbol = std::to_string(value.i);
                            else if (value.kind == ConstValue::Kind::Double) ins.symbol = std::to_string(value.d);
                            else if (value.kind == ConstValue::Kind::Bool) ins.symbol = value.b ? "true" : "false";
                            changed = true;
                        }
                        constants[ins.result] = value;
                    } else if (ins.result && (ins.opcode == Opcode::Const || ins.opcode == Opcode::Unary || ins.opcode == Opcode::Binary)) {
                        constants.erase(ins.result);
                    }
                }
            }
            if (!changed) break;
            // The rewrite set is monotonic: each iteration either simplifies an
            // expression/branch or reaches a stable form. Keep a finite guard
            // against malformed future rewrite cycles.
        }

        // If both arms of a conditional branch reach the same target, the
        // condition has no control-flow effect. Keep the condition-producing
        // instructions intact, but replace the Branch terminator with a Jump.
        // This is safe even when the condition itself is dynamic because both
        // outcomes lead to the same successor.
        for (auto& block : fn.blocks) {
            if (block.instructions.empty() || block.instructions.back().opcode != Opcode::Branch ||
                block.successors.size() != 2 || block.successors[0] != block.successors[1]) {
                continue;
            }
            auto& term = block.instructions.back();
            const BlockId target = block.successors[0];
            term.opcode = Opcode::Jump;
            term.operand0 = term.operand1 = 0;
            term.operands.clear();
            block.successors.resize(1);
            block.successors[0] = target;
        }

        // Thread empty unconditional jump blocks. A block containing only a
        // Jump has no observable work of its own, so predecessors can bypass it
        // and target the next real block directly. Keep the entry block intact
        // and stop at cycles/malformed targets rather than inventing CFG edges.
        for (int pass = 0; pass < 16; ++pass) {
            bool changed = false;
            byId.clear();
            for (std::size_t i = 0; i < fn.blocks.size(); ++i) byId[fn.blocks[i].id] = i;

            std::unordered_map<BlockId, BlockId> forwarding;
            for (const auto& block : fn.blocks) {
                if (block.id == fn.blocks.front().id || block.instructions.size() != 1 ||
                    block.instructions.front().opcode != Opcode::Jump || block.successors.size() != 1) {
                    continue;
                }
                BlockId target = block.successors.front();
                std::unordered_set<BlockId> seen{block.id};
                while (seen.insert(target).second) {
                    const auto it = byId.find(target);
                    if (it == byId.end()) break;
                    const auto& next = fn.blocks[it->second];
                    if (next.instructions.size() != 1 || next.instructions.front().opcode != Opcode::Jump ||
                        next.successors.size() != 1) break;
                    target = next.successors.front();
                }
                if (target != block.id && byId.count(target)) forwarding[block.id] = target;
            }

            if (forwarding.empty()) break;
            for (auto& predecessor : fn.blocks) {
                if (predecessor.id == fn.blocks.front().id && predecessor.successors.empty()) continue;
                for (auto& successor : predecessor.successors) {
                    const auto it = forwarding.find(successor);
                    if (it != forwarding.end() && successor != it->second) {
                        successor = it->second;
                        changed = true;
                    }
                }
            }
            if (!changed) break;
        }

        // Merge linear CFG blocks when an unconditional predecessor is the
        // sole predecessor of its target. Concatenating the target instructions
        // preserves exact execution order while reducing branch overhead.
        // Blocks participating in ownership/borrow/lifetime tracking are not
        // treated specially here: their instruction order is unchanged and no
        // instruction is removed or moved across the merge boundary.
        for (int pass = 0; pass < 16; ++pass) {
            std::unordered_map<BlockId, std::vector<BlockId>> preds;
            for (const auto& block : fn.blocks) {
                for (const auto succ : block.successors) preds[succ].push_back(block.id);
            }
            bool merged = false;
            for (std::size_t i = 1; i < fn.blocks.size(); ++i) {
                auto& block = fn.blocks[i];
                if (block.instructions.empty()) continue;
                if (preds[block.id].size() != 1) continue;
                const BlockId predId = preds[block.id].front();
                auto predIt = std::find_if(fn.blocks.begin(), fn.blocks.end(),
                    [&](const BasicBlock& candidate) { return candidate.id == predId; });
                if (predIt == fn.blocks.end()) continue;
                if (predIt->successors.size() != 1 || predIt->successors.front() != block.id) continue;
                if (predIt->instructions.empty() || predIt->instructions.back().opcode != Opcode::Jump) continue;

                predIt->instructions.pop_back();
                predIt->instructions.insert(predIt->instructions.end(),
                    std::make_move_iterator(block.instructions.begin()),
                    std::make_move_iterator(block.instructions.end()));
                predIt->successors = block.successors;
                fn.blocks.erase(fn.blocks.begin() + static_cast<std::ptrdiff_t>(i));
                merged = true;
                break;
            }
            if (!merged) break;
        }

        // Recompute reachability after constant-branch simplification, jump
        // threading, and linear block merging so dead CFG metadata is not retained.
        byId.clear();
        for (std::size_t i = 0; i < fn.blocks.size(); ++i) byId[fn.blocks[i].id] = i;
        std::unordered_set<BlockId> reachable;
        std::vector<BlockId> work{fn.blocks.front().id};
        while (!work.empty()) {
            const auto id = work.back();
            work.pop_back();
            if (!reachable.insert(id).second) continue;
            const auto it = byId.find(id);
            if (it == byId.end()) continue;
            for (const auto succ : fn.blocks[it->second].successors) work.push_back(succ);
        }

        std::vector<BasicBlock> kept;
        kept.reserve(reachable.size());
        for (const auto& block : fn.blocks) {
            if (!reachable.count(block.id)) continue;
            BasicBlock trimmed = block;
            trimmed.successors.erase(std::remove_if(trimmed.successors.begin(), trimmed.successors.end(),
                [&](const BlockId id) { return !reachable.count(id); }), trimmed.successors.end());
            kept.push_back(std::move(trimmed));
        }
        fn.blocks.swap(kept);
        eliminateDeadPureExpressions(fn);

        // Remove optimizer tombstones before the backend sees the final MIR.
        // All uses of an aliased value have already been rewritten, and Nop
        // instructions have no control-flow or ownership semantics, so they
        // can be compacted without changing execution order.
        for (auto& block : fn.blocks) {
            block.instructions.erase(
                std::remove_if(block.instructions.begin(), block.instructions.end(),
                    [](const Instruction& ins) { return ins.opcode == Opcode::Nop; }),
                block.instructions.end());
        }
    }
    return out;
}

} // namespace zl::ir
