#include "zl/compiler/bytecode.hpp"

namespace zl {

std::size_t Chunk::addConstant(const Value& value) {
    // Indexed: a linear scan here made compiling a large literal O(n^2) -
    // each of the 200k distinct ints in one list literal re-scanned the
    // whole pool. First occurrence still wins, so ids are unchanged.
    const auto it = constantIndex.find(value);
    if (it != constantIndex.end()) {
        return it->second;
    }
    constants.push_back(value);
    const std::size_t id = constants.size() - 1;
    constantIndex.emplace(constants.back(), id);
    return id;
}

std::size_t Chunk::addName(const std::string& name) {
    const auto it = nameIndex.find(name);
    if (it != nameIndex.end()) {
        return it->second;
    }
    names.push_back(name);
    const std::size_t id = names.size() - 1;
    nameIndex.emplace(names.back(), id);
    return id;
}

} // namespace zl
