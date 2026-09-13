// FFI declaration regressions: @ffi("symbol") and @ffi("library", "symbol")
// metadata is validated exactly once, at parse time - one argument selects a
// process-local symbol, two arguments select an explicit library plus symbol,
// and every other shape is rejected with a diagnostic that names the function.
#include "zl/compiler/native_ffi_declarations.hpp"
#include "zl/lexer/lexer.hpp"
#include "zl/parser/parser.hpp"

#include <iostream>
#include <sstream>
#include <string>

namespace {

using namespace zl::native;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "ffi declaration regression: " << message << '\n';
    ++failures;
}

// Finds the named member function declaration in a parsed program.
std::unique_ptr<zl::Program> parse(const std::string& source) {
    std::stringstream buffer(source);
    zl::Lexer lexer(buffer.str());
    zl::Parser parser(lexer.tokenize());
    return parser.parse();
}

const zl::FunctionDecl* findFunction(const std::unique_ptr<zl::Program>& program, const std::string& name) {
    for (const auto& decl : program->declarations) {
        if (decl->kind != zl::NodeKind::ClassDecl) continue;
        const auto* cls = static_cast<const zl::ClassDecl*>(decl.get());
        for (const auto& member : cls->members) {
            if (member->kind != zl::NodeKind::FunctionDecl) continue;
            const auto* fn = static_cast<const zl::FunctionDecl*>(member.get());
            if (fn->name == name) return fn;
        }
    }
    return nullptr;
}

void testSymbolOnly() {
    auto program = parse(
        "class Bind {\n"
        "    @ffi(\"zl_fixture_add\")\n"
        "    static func add(int a, int b): int {\n"
        "        return a + b\n"
        "    }\n"
        "}\n");
    const auto* fn = findFunction(program, "add");
    require(fn != nullptr, "the @ffi function parses");
    if (!fn) return;
    std::string error;
    auto decl = parseFfiDeclaration(*fn, error);
    require(decl.has_value(), "one-argument @ffi parses: " + error);
    if (!decl) return;
    require(decl->functionName == "add", "the declaration names the function");
    require(decl->library.empty(), "a one-argument @ffi has no library");
    require(decl->symbol == "zl_fixture_add", "the symbol is recorded verbatim");
}

void testLibraryAndSymbol() {
    auto program = parse(
        "class Bind {\n"
        "    @ffi(\"./libmath.so\", \"vector_dot\")\n"
        "    static func dot(int a, int b): int {\n"
        "        return a\n"
        "    }\n"
        "}\n");
    const auto* fn = findFunction(program, "dot");
    require(fn != nullptr, "the @ffi function parses");
    if (!fn) return;
    std::string error;
    auto decl = parseFfiDeclaration(*fn, error);
    require(decl.has_value(), "two-argument @ffi parses: " + error);
    if (!decl) return;
    require(decl->library == "./libmath.so", "the explicit library is recorded");
    require(decl->symbol == "vector_dot", "the symbol is recorded");
    require(decl->functionName == "dot", "the declaration names the function");
}

void testNoFfiAnnotation() {
    auto program = parse(
        "class Bind {\n"
        "    static func plain(int a): int {\n"
        "        return a\n"
        "    }\n"
        "}\n");
    const auto* fn = findFunction(program, "plain");
    require(fn != nullptr, "the function parses");
    if (!fn) return;
    std::string error;
    auto decl = parseFfiDeclaration(*fn, error);
    require(!decl.has_value(), "a function without @ffi has no declaration");
    require(error.empty(), "and nothing was diagnosed");
}

void testOtherAnnotationsAreIgnored() {
    auto program = parse(
        "class Bind {\n"
        "    @Deprecated\n"
        "    static func old(int a): int {\n"
        "        return a\n"
        "    }\n"
        "}\n");
    const auto* fn = findFunction(program, "old");
    require(fn != nullptr, "the function parses");
    if (!fn) return;
    std::string error;
    auto decl = parseFfiDeclaration(*fn, error);
    require(!decl.has_value(), "a non-ffi annotation is not an ffi declaration");
    require(error.empty(), "no diagnostic for an unrelated annotation");
}

void testWrongArity() {
    for (const char* body : {
             "static func none(): int {\n        return 0\n    }",
             "static func three(int a, int b, int c): int {\n        return a + b + c\n    }",
         }) {
        std::string source = "class Bind {\n    @ffi(\"s\", \"t\", \"u\")\n    ";
        source += body;
        source += "\n}\n";
        auto program = parse(source);
        const auto* fn = findFunction(program, "none");
        if (!fn) fn = findFunction(program, "three");
        require(fn != nullptr, "the @ffi function parses");
        if (!fn) return;
        std::string error;
        auto decl = parseFfiDeclaration(*fn, error);
        require(!decl.has_value(), "three-argument @ffi is rejected");
        require(error.find("expects one argument (symbol) or two arguments (library, symbol)") != std::string::npos,
                "the rejection explains the accepted shapes (got: " + error + ")");
        require(error.find(fn->name) != std::string::npos, "the rejection names the function");
    }
    // Zero arguments.
    {
        auto program = parse(
            "class Bind {\n"
            "    @ffi\n"
            "    static func none(): int {\n        return 0\n    }\n"
            "}\n");
        const auto* fn = findFunction(program, "none");
        require(fn != nullptr, "the @ffi function parses");
        if (!fn) return;
        std::string error;
        auto decl = parseFfiDeclaration(*fn, error);
        require(!decl.has_value(), "a bare @ffi is rejected");
        require(!error.empty(), "with a diagnostic");
    }
}

} // namespace

int main() {
    testSymbolOnly();
    testLibraryAndSymbol();
    testNoFfiAnnotation();
    testOtherAnnotationsAreIgnored();
    testWrongArity();

    if (failures != 0) {
        std::cerr << failures << " ffi declaration regression(s) failed\n";
        return 1;
    }
    std::cout << "all ffi declaration regressions passed\n";
    return 0;
}
