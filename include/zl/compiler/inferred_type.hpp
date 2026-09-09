#pragma once

#include <string>
#include <vector>

#include "zl/common/dispatch_signature.hpp"
#include "zl/common/ownership.hpp"
#include "zl/compiler/semantic_types.hpp"

namespace zl {

// The resolved static type of one expression, as produced by semantic
// analysis.
//
// This used to be a private nested type of TypeChecker, which meant every
// backend stage that needed "what type is this expression?" had to either
// re-implement inference or work from untyped AST annotations. It is a public
// value type now so the compiler can hand the *same* inference result to
// bytecode generation and to MIR lowering instead of maintaining two.
//
// `type` is the coarse runtime kind; the remaining fields carry everything the
// coarse kind erases - object/collection/task identity, callable signatures,
// ownership, and borrow provenance.
struct InferredType {
    ZlType type{ZlType::UNKNOWN};
    // Full identity for OBJECT/LIST/MAP/SET/ARRAY/TASK/FUNCTION/UNION values:
    // a class name, a generic instantiation key such as "List<int>", or a
    // rendered union such as "int|string".
    std::string className;
    // Callable shape when the value is a func value.
    std::vector<ZlType> functionParamTypes;
    std::vector<std::string> functionParamClassNames;
    ZlType functionReturnType{ZlType::UNKNOWN};
    std::string functionReturnClassName;
    bool functionIsAsync{false};
    // Task payload shape when the value is a Task<T>.
    ZlType taskValueType{ZlType::UNKNOWN};
    std::string taskValueClassName;
    // Closure provenance.
    std::vector<std::string> functionCaptureNames;
    bool functionUsesThis{false};
    bool functionHasSignature{false};
    // Storage contract chosen by semantic analysis, not an inferred property
    // of the right-hand side.
    OwnershipKind ownership{OwnershipKind::GC};
    // Non-empty when this value is a borrow: the owning region it borrows from.
    std::string borrowSource;
    // True when the func value is a reference to a named function rather than
    // a freshly built closure; identifies the referenced declaration.
    bool functionIsNamedReference{false};
    std::string functionReferenceOwner;
    DispatchSignature functionReferenceDispatch;

    InferredType() = default;
    InferredType(ZlType value, std::string cls = {}) : type(value), className(std::move(cls)) {}
    operator ZlType() const { return type; }
};

} // namespace zl
