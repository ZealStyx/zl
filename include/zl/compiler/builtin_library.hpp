#pragma once

#include <string_view>
#include <vector>

namespace zl {

// Source strings for ZL classes that are implicitly available to every program.
// The compiler still parses and type-checks these classes normally; this module
// only owns their source text so ModuleLoader can focus on import graph mechanics.
[[nodiscard]] const std::vector<std::string_view>& builtinLibrarySources();

} // namespace zl
