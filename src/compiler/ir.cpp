#include "zl/compiler/ir.hpp"
#include <algorithm>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace zl::ir {
namespace {

struct FlowState {
    std::unordered_set<ValueId> live;
    std::unordered_set<ValueId> values;
    std::unordered_set<ValueId> moved;
    std::unordered_map<ValueId, ValueId> borrowSources; // borrow local -> owner
    std::unordered_map<ValueId, OwnershipKind> ownership;
};

bool sameState(const FlowState& a, const FlowState& b) {
    if (a.live != b.live || a.values != b.values || a.moved != b.moved || a.borrowSources != b.borrowSources || a.ownership != b.ownership)
        return false;
    return true;
}

FlowState joinStates(const std::vector<FlowState>& inputs) {
    if (inputs.empty()) return {};
    FlowState out = inputs.front();
    for (std::size_t i = 1; i < inputs.size(); ++i) {
        std::unordered_set<ValueId> live;
        for (ValueId v : out.live) if (inputs[i].live.count(v)) live.insert(v);
        out.live.swap(live);
        std::unordered_set<ValueId> values;
        for (ValueId v : out.values) if (inputs[i].values.count(v)) values.insert(v);
        out.values.swap(values);
        out.moved.insert(inputs[i].moved.begin(), inputs[i].moved.end());

        // A borrow that remains active on any path keeps its owner borrowed at
        // the join. Conflicting path owners are represented as 0 and are
        // treated conservatively by move validation.
        std::unordered_map<ValueId, ValueId> mergedBorrows = out.borrowSources;
        for (const auto& [borrow, source] : inputs[i].borrowSources) {
            auto it = mergedBorrows.find(borrow);
            if (it == mergedBorrows.end()) mergedBorrows.emplace(borrow, source);
            else if (it->second != source) it->second = 0;
        }
        out.borrowSources.swap(mergedBorrows);

        // Ownership metadata must be identical for locals that survive the
        // join. Values defined on only one path are not live after the join.
        for (auto it = out.ownership.begin(); it != out.ownership.end();) {
            auto jt = inputs[i].ownership.find(it->first);
            if (jt == inputs[i].ownership.end() || jt->second != it->second || !out.live.count(it->first))
                it = out.ownership.erase(it);
            else
                ++it;
        }
    }
    return out;
}

bool requireLive(const FlowState& s, ValueId v, const char* op, std::string* error) {
    if (!v || !s.live.count(v)) {
        if (error) *error = std::string(op) + " references a local that is not live on this path";
        return false;
    }
    return true;
}

bool requireNotMoved(const FlowState& s, ValueId v, const char* op, std::string* error) {
    if (s.moved.count(v)) {
        if (error) *error = std::string(op) + " uses a moved local";
        return false;
    }
    return true;
}

bool transfer(const BasicBlock& block, const FlowState& input, FlowState& state, std::string* error) {
    state = input;
    for (const auto& ins : block.instructions) {
        switch (ins.opcode) {
            case Opcode::DefineLocal:
                if (!ins.result) { if (error) *error = "DefineLocal requires a destination"; return false; }
                if (state.live.count(ins.result) || state.ownership.count(ins.result)) {
                    if (error) *error = "local is defined more than once on the same path";
                    return false;
                }
                state.live.insert(ins.result);
                state.values.insert(ins.result);
                state.ownership[ins.result] = ins.ownership;
                break;
            case Opcode::AssignLocal:
                if (!requireLive(state, ins.result, "AssignLocal", error)) return false;
                if (!requireNotMoved(state, ins.result, "AssignLocal", error)) return false;
                if (ins.operand0) {
                    if (!state.values.count(ins.operand0) && !state.live.count(ins.operand0)) {
                        if (error) *error = "AssignLocal references a value that is not available on this path";
                        return false;
                    }
                    if (state.live.count(ins.operand0) && !requireNotMoved(state, ins.operand0, "AssignLocal", error)) return false;
                    auto it = state.ownership.find(ins.operand0);
                    if (it != state.ownership.end() && it->second == OwnershipKind::BORROW && state.ownership[ins.result] != OwnershipKind::BORROW) {
                        if (error) *error = "borrowed value assigned to non-borrow local";
                        return false;
                    }
                }
                break;
            case Opcode::MoveLocal: {
                if (!requireLive(state, ins.operand0, "MoveLocal", error)) return false;
                if (!requireNotMoved(state, ins.operand0, "MoveLocal", error)) return false;
                auto it = state.ownership.find(ins.operand0);
                if (it == state.ownership.end() || it->second != OwnershipKind::OWNED) {
                    if (error) *error = "MoveLocal requires an owned source";
                    return false;
                }
                for (const auto& [borrow, source] : state.borrowSources) {
                    if (source == ins.operand0 || source == 0) {
                        if (error) *error = "MoveLocal cannot move an actively borrowed owner";
                        return false;
                    }
                }
                state.moved.insert(ins.operand0);
                if (ins.result) {
                    state.live.insert(ins.result);
                    state.values.insert(ins.result);
                    state.ownership[ins.result] = OwnershipKind::OWNED;
                }
                break;
            }
            case Opcode::BorrowLocal:
                if (!requireLive(state, ins.operand0, "BorrowLocal", error)) return false;
                if (!requireNotMoved(state, ins.operand0, "BorrowLocal", error)) return false;
                if (!ins.result) { if (error) *error = "BorrowLocal requires a destination"; return false; }
                if (state.live.count(ins.result) || state.ownership.count(ins.result)) {
                    if (error) *error = "BorrowLocal destination is already live";
                    return false;
                }
                state.live.insert(ins.result);
                state.values.insert(ins.result);
                state.ownership[ins.result] = OwnershipKind::BORROW;
                state.borrowSources[ins.result] = ins.operand0;
                break;
            case Opcode::EndBorrow:
                if (!requireLive(state, ins.operand0, "EndBorrow", error)) return false;
                if (state.ownership[ins.operand0] != OwnershipKind::BORROW) {
                    if (error) *error = "EndBorrow requires a borrow local";
                    return false;
                }
                state.borrowSources.erase(ins.operand0);
                state.live.erase(ins.operand0);
                state.values.erase(ins.operand0);
                state.ownership.erase(ins.operand0);
                break;
            case Opcode::DropLocal:
                if (!requireLive(state, ins.operand0, "DropLocal", error)) return false;
                if (state.ownership[ins.operand0] == OwnershipKind::BORROW) {
                    state.borrowSources.erase(ins.operand0);
                } else {
                    for (auto it = state.borrowSources.begin(); it != state.borrowSources.end();) {
                        if (it->second == ins.operand0 || it->second == 0) it = state.borrowSources.erase(it);
                        else ++it;
                    }
                }
                state.live.erase(ins.operand0);
                state.values.erase(ins.operand0);
                state.ownership.erase(ins.operand0);
                state.moved.erase(ins.operand0);
                break;
            case Opcode::Const:
            case Opcode::Unary:
            case Opcode::Binary:
                if (!ins.result) { if (error) *error = "value-producing instruction requires a result"; return false; }
                state.values.insert(ins.result);
                break;
            case Opcode::Call:
                if (!ins.result) { if (error) *error = "Call requires a result"; return false; }
                if (ins.symbol.empty()) { if (error) *error = "Call requires a target symbol"; return false; }
                for (ValueId arg : ins.operands) {
                    if (!arg || !state.values.count(arg)) { if (error) *error = "Call references an unavailable argument value"; return false; }
                }
                state.values.insert(ins.result);
                break;
            case Opcode::LoadLocal:
                if (!requireLive(state, ins.operand0, "LoadLocal", error)) return false;
                if (!requireNotMoved(state, ins.operand0, "LoadLocal", error)) return false;
                if (!ins.result) { if (error) *error = "LoadLocal requires a result"; return false; }
                state.values.insert(ins.result);
                break;
            case Opcode::Return:
                if (ins.operand0) {
                    if (state.live.count(ins.operand0)) {
                        if (!requireNotMoved(state, ins.operand0, "Return", error)) return false;
                        if (state.ownership[ins.operand0] == OwnershipKind::BORROW) {
                            if (error) *error = "Return cannot return a borrowed local";
                            return false;
                        }
                    } else if (!state.values.count(ins.operand0)) {
                        if (error) *error = "Return references a value that is not live on this path";
                        return false;
                    }
                }
                break;
            default:
                break;
        }
    }
    return true;
}

}

bool verify(const Module& module, std::string* error) {
    for (const auto& fn : module.functions) {
        if (fn.blocks.empty()) {
            if (error) *error = "IR function '" + fn.name + "' has no basic blocks";
            return false;
        }

        std::unordered_map<BlockId, std::size_t> index;
        for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
            if (!index.emplace(fn.blocks[i].id, i).second) {
                if (error) *error = "duplicate IR block id";
                return false;
            }
        }
        std::vector<std::vector<std::size_t>> preds(fn.blocks.size());
        for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
            for (BlockId succ : fn.blocks[i].successors) {
                auto it = index.find(succ);
                if (it == index.end()) {
                    if (error) *error = "invalid IR block successor";
                    return false;
                }
                preds[it->second].push_back(i);
            }
        }

        // Defining result IDs must be globally unique. Branch-local values are
        // represented by distinct IDs and therefore naturally disappear at
        // joins unless defined by all incoming paths.
        std::unordered_set<ValueId> definitionIds;
        for (const auto& block : fn.blocks) {
            for (const auto& ins : block.instructions) {
                if (ins.opcode == Opcode::DefineLocal || ins.opcode == Opcode::BorrowLocal ||
                    (ins.opcode == Opcode::MoveLocal && ins.result)) {
                    if (!definitionIds.insert(ins.result).second) {
                        if (error) *error = "duplicate IR value definition";
                        return false;
                    }
                }
            }
        }

        std::vector<FlowState> in(fn.blocks.size()), out(fn.blocks.size());
        std::vector<bool> hasIn(fn.blocks.size(), false), hasOut(fn.blocks.size(), false), reachable(fn.blocks.size(), false);
        std::queue<std::size_t> work;
        hasIn[0] = true; reachable[0] = true; work.push(0);

        while (!work.empty()) {
            const std::size_t bi = work.front(); work.pop();
            FlowState input;
            if (bi == 0) {
                // Parameter DefineLocal operations in block zero establish the
                // initial function ownership state.
                input = {};
            } else {
                std::vector<FlowState> incoming;
                for (std::size_t p : preds[bi]) if (hasOut[p]) incoming.push_back(out[p]);
                if (incoming.empty()) continue;
                input = joinStates(incoming);
            }
            if (!hasIn[bi] || !sameState(in[bi], input)) in[bi] = input;
            hasIn[bi] = true;

            FlowState next;
            if (!transfer(fn.blocks[bi], input, next, error)) return false;
            if (hasOut[bi] && sameState(out[bi], next)) continue;
            out[bi] = next; hasOut[bi] = true;
            for (BlockId succ : fn.blocks[bi].successors) {
                const std::size_t si = index.at(succ);
                reachable[si] = true;
                work.push(si);
            }
        }
    }
    return true;
}
}
