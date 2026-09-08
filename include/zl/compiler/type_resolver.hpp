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
    [[nodiscard]] ClassFieldInfo fieldInContext(const ClassFieldInfo& field,
        const std::string& receiverClass, const std::string& declaringClass,
        const std::string& currentClass, const std::vector<std::string>& currentTypeParams);
    [[nodiscard]] const std::vector<ResolvedTypeArg>& unionMembers(const std::string& name) const;
    [[nodiscard]] ResolvedTypeArg makeUnion(std::vector<ResolvedTypeArg> members);

    [[nodiscard]] ZlType resolveType(const TypeAnnotation& annotation,
                                     const std::string& currentClassName,
                                     const std::vector<std::string>& currentClassTypeParams,
                                     std::string* outClassName = nullptr);

    [[nodiscard]] std::string instantiateGenericClass(const std::string& genericName,
                                                        const std::vector<ResolvedTypeArg>& typeArgs,
                                                        std::size_t line);

    // Rebuilds every generic instantiation created so far. Shapes are filled
    // in declaration order, so an instantiation requested while resolving an
    // early class's signature can be built from a template whose own members
    // are not registered yet. Calling this once all shapes exist makes those
    // instantiations independent of declaration order.
    void refreshInstantiations();

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
    // Set only while refreshInstantiations() runs, so an already-registered
    // instantiated shape is rebuilt rather than reused.
    bool rebuilding_{false};
};

} // namespace zl
