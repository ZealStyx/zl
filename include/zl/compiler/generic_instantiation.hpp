#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "zl/compiler/semantic_types.hpp"

namespace zl {

// Structured identity for a resolved generic type argument. The semantic
// model still exposes class names as strings for compatibility, but generic
// caching and substitution no longer need to derive identity from formatted
// strings.
struct ResolvedTypeArg {
    ZlType type{ZlType::UNKNOWN};
    std::string className;
    // When a generic argument is a function type, preserve its complete
    // callable shape so Box<func(A): B> and Box<func(C): D> remain distinct
    // generic instantiations and can substitute callable fields correctly.
    std::vector<ResolvedTypeArg> functionParamTypes;
    std::shared_ptr<ResolvedTypeArg> functionReturnType;
    bool functionHasSignature{false};

    bool operator==(const ResolvedTypeArg& other) const noexcept {
        if (type != other.type || className != other.className ||
            functionHasSignature != other.functionHasSignature ||
            functionParamTypes != other.functionParamTypes) return false;
        if (functionReturnType == nullptr || other.functionReturnType == nullptr)
            return functionReturnType == nullptr && other.functionReturnType == nullptr;
        return *functionReturnType == *other.functionReturnType;
    }
};

// Identity of a concrete generic instantiation, e.g. Box<int> or
// Pair<Box<int>,string>. This is the semantic value; describe() is only for
// diagnostics and compatibility with the existing class-name registry.
struct GenericInstantiation {
    std::string base;
    std::vector<ResolvedTypeArg> args;

    bool operator==(const GenericInstantiation& other) const noexcept {
        return base == other.base && args == other.args;
    }

    [[nodiscard]] std::string describe() const;
};

struct GenericInstantiationHash {
    [[nodiscard]] std::size_t operator()(const GenericInstantiation& value) const noexcept;
};

} // namespace zl
