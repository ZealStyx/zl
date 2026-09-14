// Native compiler regressions: the restricted @native C++ emitter and the
// MIR-based native emitter, driven through real ZL source parsed by the real
// lexer/parser. Pins the stable ABI surface (export table, tags, invoke
// stubs, abi version), the primitive type mapping, constant folding, bounded
// loop unrolling, the safe-integer helpers, and every documented rejection
// (unsupported types, non-static members, async, missing returns, data
// methods, calls out of the native set).
#include "zl/compiler/native_compiler.hpp"
#include "zl/compiler/native_ffi_declarations.hpp"
#include "zl/lexer/lexer.hpp"
#include "zl/parser/parser.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace zl::native;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "native compiler regression: " << message << '\n';
    ++failures;
}

std::unique_ptr<zl::Program> parse(const std::string& source) {
    std::stringstream buffer(source);
    zl::Lexer lexer(buffer.str());
    zl::Parser parser(lexer.tokenize());
    return parser.parse();
}

CompileResult emitSimple(const std::string& source) {
    return emitSimpleCpp(*parse(source));
}

CompileResult emitMir(const std::string& source) {
    return emitMirCpp(*parse(source));
}

// ---------------------------------------------------------------------------
// emitSimpleCpp
// ---------------------------------------------------------------------------

void testSimpleEmitsStableAbi() {
    const auto result = emitSimple(
        "class Fixture {\n"
        "    @native\n"
        "    static func add(int a, int b): int {\n"
        "        return a + b\n"
        "    }\n"
        "    @native\n"
        "    static func half(float x): float {\n"
        "        return x / 2.0\n"
        "    }\n"
        "    @native\n"
        "    static func isEven(int x): bool {\n"
        "        return x % 2 == 0\n"
        "    }\n"
        "}\n");
    require(result.success, "the @native subset compiles: " + result.error);
    if (!result.success) return;
    const std::string& src = result.source;
    require(src.find("extern \"C\" std::int64_t zl_native_add(") != std::string::npos,
            "int functions are emitted as extern \"C\" std::int64_t");
    require(src.find("extern \"C\" double zl_native_half(") != std::string::npos,
            "float maps to double");
    require(src.find("extern \"C\" bool zl_native_isEven(") != std::string::npos,
            "bool maps to bool");
    require(src.find("ZL_NATIVE_I64") != std::string::npos &&
              src.find("ZL_NATIVE_F64") != std::string::npos &&
              src.find("ZL_NATIVE_BOOL") != std::string::npos,
            "the export table records the primitive tags");
    require(src.find("ZL_NATIVE_OWNERSHIP_NONE") != std::string::npos,
            "primitive exports are ownership-neutral");
    require(src.find("extern \"C\" const ZlNativeExport zl_native_exports[]") != std::string::npos,
            "the stable export table is emitted");
    require(src.find("zl_native_export_count = 3") != std::string::npos,
            "the export count matches the three exports");
    require(src.find("zl_native_abi_version = 2") != std::string::npos,
            "the abi version is pinned");
    require(src.find("if (argc != 2) { const char* m=\"arity mismatch\"") != std::string::npos,
            "the invoke stub rejects the wrong arity");
    require(src.find("argument type mismatch") != std::string::npos,
            "the invoke stub rejects the wrong argument tag");
}

void testSimpleFoldsConstants() {
    const auto result = emitSimple(
        "class Fold {\n"
        "    @native\n"
        "    static func seven(): int {\n"
        "        return 1 + 2 * 3\n"
        "    }\n"
        "}\n");
    require(result.success, result.error);
    if (!result.success) return;
    require(result.source.find("return 7;") != std::string::npos,
            "constant expressions fold at compile time (1 + 2 * 3 == 7)");
    require(result.source.find("1 + 2 * 3") == std::string::npos,
            "the unfolded expression is not emitted");
}

void testSimpleUnrollsBoundedLoops() {
    const auto result = emitSimple(
        "class Unroll {\n"
        "    @native\n"
        "    static func sumToThree(): int {\n"
        "        var total = 0\n"
        "        for i in 0..3 {\n"
        "            total = total + i\n"
        "        }\n"
        "        return total\n"
        "    }\n"
        "}\n");
    require(result.success, result.error);
    if (!result.success) return;
    // The range end is exclusive: 0..3 unrolls to i = 0, 1, 2.
    require(result.source.find("const std::int64_t i = 0;") != std::string::npos,
            "the loop unrolls its first value");
    require(result.source.find("const std::int64_t i = 1;") != std::string::npos,
            "the loop unrolls its second value");
    require(result.source.find("const std::int64_t i = 2;") != std::string::npos,
            "the loop unrolls its last value (exclusive end)");
    require(result.source.find("const std::int64_t i = 3;") == std::string::npos,
            "the exclusive end is not unrolled");
    require(result.source.find("for (std::int64_t i = 0;") == std::string::npos,
            "a constant loop is not left as a runtime loop");
}

void testSimpleRejections() {
    {
        const auto result = emitSimple(
            "class R1 {\n"
            "    @native\n"
            "    static func name(): string {\n"
            "        return \"x\"\n"
            "    }\n"
            "}\n");
        require(!result.success, "a string return type is rejected");
        require(result.error.find("unsupported return type") != std::string::npos,
                "the rejection names the return type (got: " + result.error + ")");
    }
    {
        const auto result = emitSimple(
            "class R2 {\n"
            "    @native\n"
            "    func greet(): int {\n"
            "        return 1\n"
            "    }\n"
            "}\n");
        require(!result.success, "an instance @native method is rejected");
        require(result.error.find("only free or static functions") != std::string::npos,
                "the rejection says static is required (got: " + result.error + ")");
    }
    {
        const auto result = emitSimple(
            "class R3 {\n"
            "    @native\n"
            "    static async func later(): int {\n"
            "        return 1\n"
            "    }\n"
            "}\n");
        require(!result.success, "an async @native function is rejected");
        require(result.error.find("cannot be async") != std::string::npos,
                "the rejection names async (got: " + result.error + ")");
    }
    {
        const auto result = emitSimple(
            "class R4 {\n"
            "    @native\n"
            "    static func drops(): int {\n"
            "        var x = 1\n"
            "    }\n"
            "}\n");
        require(!result.success, "a function without a terminating return is rejected");
        require(result.error.find("requires a terminating return") != std::string::npos,
                "the rejection names the missing return (got: " + result.error + ")");
    }
    {
        // Annotations are not part of the data-type member grammar, so the
        // boundary is enforced at parse time: an @native data method never
        // reaches the emitter.
        bool parseRejected = false;
        try {
            (void)parse("data Point {\n"
                        "    int x\n"
                        "    @native\n"
                        "    func get(): int {\n"
                        "        return this.x\n"
                        "    }\n"
                        "}\n");
        } catch (const std::exception&) {
            parseRejected = true;
        }
        require(parseRejected, "an @native annotation on a data method is rejected at parse time");
    }
    {
        const auto result = emitSimple(
            "class R6 {\n"
            "    @native\n"
            "    static func talks(): int {\n"
            "        log(1)\n"
            "        return 1\n"
            "    }\n"
            "}\n");
        require(!result.success, "statements outside the numeric subset are rejected");
        require(result.error.find("outside the supported") != std::string::npos,
                "the rejection names the subset (got: " + result.error + ")");
    }
    {
        const auto result = emitSimple(
            "class R7 {\n"
            "    @native\n"
            "    static func badLocal(): int {\n"
            "        var s = \"x\"\n"
            "        return 1\n"
            "    }\n"
            "}\n");
        require(!result.success, "a non-primitive local is rejected");
        require(result.error.find("cannot infer supported local type") != std::string::npos,
                "the rejection names the local (got: " + result.error + ")");
    }
}

// ---------------------------------------------------------------------------
// emitMirCpp
// ---------------------------------------------------------------------------

void testMirEmitsNativeFunctions() {
    const auto result = emitMir(
        "class Mir {\n"
        "    @native\n"
        "    static func triple(int n): int {\n"
        "        return n * 3\n"
        "    }\n"
        "    static func hidden(int n): int {\n"
        "        return n\n"
        "    }\n"
        "}\n");
    require(result.success, "the MIR native path compiles the @native function: " + result.error);
    if (!result.success) return;
    require(result.source.find("zl_mir_native_Mir_triple") != std::string::npos,
            "the @native function is emitted under its mangled name");
    require(result.source.find("zl_mir_native_Mir_hidden") == std::string::npos,
            "a non-native function is not emitted");
    require(result.source.find("zl_mir_native_exports[]") != std::string::npos,
            "the MIR export table is emitted");
    require(result.source.find("zl_safe_mul_i64") != std::string::npos,
            "integer multiplication uses the overflow-checked helper");
}

void testMirKeepsOverflowingPowChecked() {
    const auto result = emitMir(
        "class Pow {\n"
        "    @native\n"
        "    static func big(int n): int {\n"
        "        return (2 ^^ 63) + n\n"
        "    }\n"
        "    @native\n"
        "    static func small(int n): int {\n"
        "        return (2 ^^ 10) + n\n"
        "    }\n"
        "}\n");
    require(result.success, "the pow cases compile: " + result.error);
    if (!result.success) return;
    require(result.source.find("1024") != std::string::npos,
            "an exact integer power still folds at compile time (2 ^^ 10 == 1024)");
    // NOTE: match "= zl_safe_pow_i64(v" (a call site), not the bare helper
    // name: the helper's own definition is always emitted into the preamble.
    require(result.source.find("= zl_safe_pow_i64(v") != std::string::npos,
            "an overflowing integer power stays a checked runtime call (2 ^^ 63 "
            "must raise, not fold to a double)");
}

void testMirRejectsCallsOutOfTheNativeSet() {
    const auto result = emitMir(
        "class Caller {\n"
        "    @native\n"
        "    static func plain(int n): int {\n"
        "        return n + 1\n"
        "    }\n"
        "    @native\n"
        "    static func callsPlain(int n): int {\n"
        "        return Caller.plain(n)\n"
        "    }\n"
        "}\n");
    require(result.success, "a call between @native functions compiles: " + result.error);
    if (result.success) {
        require(result.source.find("zl_mir_native_Caller_plain(") != std::string::npos,
                "the call targets the emitted native function");
    }

    const auto bad = emitMir(
        "class Mixed {\n"
        "    static func notNative(int n): int {\n"
        "        return n\n"
        "    }\n"
        "    @native\n"
        "    static func callsForeign(int n): int {\n"
        "        return Mixed.notNative(n)\n"
        "    }\n"
        "}\n");
    require(!bad.success, "a @native function calling a non-native function is rejected");
    require(bad.error.find("cannot call non-native or unresolved function") != std::string::npos,
            "the rejection names the callee (got: " + bad.error + ")");
}

void testMirRejectsEmptyModule() {
    const auto result = emitMir(
        "class Empty {\n"
        "    static func nothing(): int {\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    require(!result.success, "a module with no @native functions is rejected");
    require(result.error.find("found no functions") != std::string::npos,
            "the rejection says the native set is empty (got: " + result.error + ")");
}

void testSimpleIgnoresNonNativeFunctions() {
    const auto result = emitSimple(
        "class Quiet {\n"
        "    static func regular(int n): int {\n"
        "        return n\n"
        "    }\n"
        "}\n");
    require(result.success, "a program with no @native functions emits cleanly");
    require(result.source.find("zl_native_exports") == std::string::npos,
            "no export table is emitted for an empty native set");
    require(result.source.find("zl_native_regular") == std::string::npos,
            "ordinary functions are not native exports");
}

} // namespace

int main() {
    testSimpleEmitsStableAbi();
    testSimpleFoldsConstants();
    testSimpleUnrollsBoundedLoops();
    testSimpleRejections();
    testMirEmitsNativeFunctions();
    testMirKeepsOverflowingPowChecked();
    testMirRejectsCallsOutOfTheNativeSet();
    testMirRejectsEmptyModule();
    testSimpleIgnoresNonNativeFunctions();

    if (failures != 0) {
        std::cerr << failures << " native compiler regression(s) failed\n";
        return 1;
    }
    std::cout << "all native compiler regressions passed\n";
    return 0;
}
