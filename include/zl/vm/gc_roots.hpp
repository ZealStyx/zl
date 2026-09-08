#pragma once

#include <exception>
#include <initializer_list>
#include <utility>
#include <vector>

#include "value.hpp"

namespace zl {
struct Chunk;

// Values are snapshots of execution state. Programs are graph roots: constants
// and shared static state must be read at trace time, not flattened when a VM
// parks (another participant may update them before the next collection).
// Program pointers are borrowed; their execution/closure owner must outlive the
// root lease. Reachable closures also trace their own shared Chunk ownership.
struct GCRoots {
    std::vector<Value> values;
    std::vector<const Chunk*> programs;

    GCRoots() = default;
    GCRoots(std::initializer_list<Value> roots) : values(roots) {}
    GCRoots(std::vector<Value> roots) : values(std::move(roots)) {}

    void append(const GCRoots& other) {
        values.insert(values.end(), other.values.begin(), other.values.end());
        programs.insert(programs.end(), other.programs.begin(), other.programs.end());
    }
};

void appendExceptionRoots(const std::exception_ptr& error, std::vector<Value>& roots);
} // namespace zl
