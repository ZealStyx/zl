#pragma once

#include "value.hpp"
#include "zl/common/type_name.hpp"

namespace zl {
struct Chunk;

// Runtime substitutions are lexical: only bindings from the declaring class
// or captured closure may replace a parameter token. Unknown names are never
// guessed to be type parameters and silently exempted from checking.
RuntimeTypeBindings receiverTypeBindings(const ObjectBox& receiver, const Chunk& chunk,
                                        const std::string& declaringClass);

bool reflectiveTypeMatchesName(const Value& value, const std::string& typeName, const Chunk* chunk = nullptr);
bool runtimeAssignableToType(const Value& value, const std::string& typeName, const Chunk* chunk);
std::string runtimeValueTypeName(const Value& value);
// Attach the compiler-declared identity to a newly allocated object only.
void initializeObjectType(Value& value, const std::string& typeName, const Chunk& chunk);
} // namespace zl
