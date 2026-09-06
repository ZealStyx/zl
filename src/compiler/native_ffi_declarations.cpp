#include "zl/compiler/native_ffi_declarations.hpp"

namespace zl::native {

std::optional<FfiDeclaration> parseFfiDeclaration(const FunctionDecl& fn, std::string& error) {
    const Annotation* ffi = nullptr;
    for (const auto& ann : fn.annotations) {
        if (ann.name == "ffi") { ffi = &ann; break; }
    }
    if (!ffi) return std::nullopt;
    if (ffi->arguments.size() == 1) return FfiDeclaration{fn.name, {}, ffi->arguments[0]};
    if (ffi->arguments.size() == 2) return FfiDeclaration{fn.name, ffi->arguments[0], ffi->arguments[1]};
    error = "@ffi on " + fn.name + " expects one argument (symbol) or two arguments (library, symbol)";
    return std::nullopt;
}

}
