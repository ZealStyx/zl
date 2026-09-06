#include "zl/compiler/bytecode.hpp"

#include <algorithm>

namespace zl {

std::size_t Chunk::addConstant(const Value& value) {
    // std::variant supports == out of the box as long as every alternative
    // type does (int64_t, double, string, bool, monostate all do).
    auto it = std::find(constants.begin(), constants.end(), value);
    if (it != constants.end()) {
        return static_cast<std::size_t>(it - constants.begin());
    }
    constants.push_back(value);
    return constants.size() - 1;
}

std::size_t Chunk::addName(const std::string& name) {
    auto it = std::find(names.begin(), names.end(), name);
    if (it != names.end()) {
        return static_cast<std::size_t>(it - names.begin());
    }
    names.push_back(name);
    return names.size() - 1;
}

} // namespace zl
