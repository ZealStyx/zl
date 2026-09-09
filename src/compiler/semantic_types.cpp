#include "zl/compiler/semantic_types.hpp"

namespace zl {

// The canonical rendering of a coarse type kind.
//
// This used to live in type_checker.cpp, which made a name-rendering utility for
// a public enum part of the semantic analyser's translation unit. Every other
// layer that needed it - and the MIR verifier does - had to link the whole type
// checker to get one switch statement. It belongs next to the header that
// declares it.
std::string zlTypeName(ZlType t) {
    switch (t) {
        case ZlType::INT:     return "int";
        case ZlType::DOUBLE:  return "double";
        case ZlType::STRING:  return "string";
        case ZlType::BOOL:    return "bool";
        case ZlType::VOID_TYPE:    return "void";
        case ZlType::NIL:     return "nil";
        case ZlType::LIST:    return "list";
        case ZlType::MAP:     return "map";
        case ZlType::SET:     return "set";
        case ZlType::ARRAY:   return "array";
        case ZlType::OBJECT:  return "object";
        case ZlType::FUNCTION: return "func";
        case ZlType::TASK: return "Task";
        case ZlType::UNKNOWN: return "unknown";
        case ZlType::UNION: return "union";
    }
    return "unknown";
}

bool mathConstantValue(const std::string& name, double& out) {
    // Single source of truth for Math.PI / Math.E / Math.TAU. The values used to
    // be inlined in the bytecode compiler and the type checker separately, which
    // meant adding a constant required finding every consumer; the MIR lowerer
    // would have been a third copy.
    if (name == "PI")  { out = 3.141592653589793238462643383279502884; return true; }
    if (name == "E")   { out = 2.718281828459045235360287471352662498; return true; }
    if (name == "TAU") { out = 6.283185307179586476925286766559005768; return true; }
    return false;
}

} // namespace zl
