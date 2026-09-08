#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "zl/parser/ast.hpp"
#include "zl/compiler/generic_instantiation.hpp"
#include "zl/compiler/semantic_model.hpp"
#include "zl/compiler/semantic_types.hpp"

namespace zl {

// Resolves source-level type annotations and owns concrete generic-class
// instantiation. This module is intentionally independent of statement and
// expression checking; it owns type-name policy and generic shape materialization.
class TypeResolver {
public:
    explicit TypeResolver(SemanticModel& semanticModel) : semanticModel_(semanticModel) {}

    void reset();
    [[nodiscard]] const std::vector<ResolvedTypeArg>& unionMembers(const std::string& name) const;
    [[nodiscard]] ResolvedTypeArg makeUnion(std::vector<ResolvedTypeArg> members);

    [[nodiscard]] ZlType resolveType(const TypeAnnotation& annotation,
                                     const std::string& currentClassName,
                                     const std::vector<std::string>& currentClassTypeParams,
                                     std::string* outClassName = nullptr);

    [[nodiscard]] std::string instantiateGenericClass(const std::string& genericName,
                                                        const std::vector<ResolvedTypeArg>& typeArgs,
                                                        std::size_t line);

    [[nodiscard]] bool isCurrentGenericTypeParam(const std::string& name,
                                                  const std::vector<std::string>& currentClassTypeParams) const;

    void requireNumericGenericTypeParam(const std::string& name,
                                        const std::string& currentClassName,
                                        const std::vector<std::string>& currentClassTypeParams);

private:
    SemanticModel& semanticModel_;
    std::unordered_map<std::string, std::vector<ResolvedTypeArg>> unions_;
    std::unordered_map<GenericInstantiation, std::string, GenericInstantiationHash> genericInstantiationCache_;
    std::unordered_map<std::string, GenericInstantiation> genericInstantiationByName_;
};

} // namespace zl
