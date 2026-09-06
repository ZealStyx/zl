#pragma once

#include <string>
#include <utility>
#include <vector>

#include "zl/compiler/semantic_model.hpp"

namespace zl {

class TypeChecker;

// Chooses a single method candidate from the semantic model's visible
// overload set. The resolver owns only overload policy; declaration shapes,
// hierarchy traversal, and type assignability remain owned by their canonical
// modules.
class OverloadResolver {
public:
    explicit OverloadResolver(const TypeChecker& checker) : checker_(checker) {}

    [[nodiscard]] const ClassMethodInfo* resolve(
        const std::vector<std::pair<std::string, ClassMethodInfo>>& candidates,
        const std::vector<ZlType>& argTypes, const std::vector<std::string>& argClassNames,
        const std::string& methodName, std::size_t line, std::string* outOwner = nullptr) const;

private:
    const TypeChecker& checker_;
};

} // namespace zl
