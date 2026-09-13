// Native library regressions: dynamic loading of the shared library the
// build produces for these tests. The fixture library is the same kind of
// artifact a ZL @ffi("library", "symbol") declaration would load: a .so with
// extern "C" entry points. Loading is move-only, fail-closed on a bad path or
// missing symbol, and closing is idempotent.
#include "zl/vm/native_library.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "native library regression: " << message << '\n';
    ++failures;
}

#if defined(ZL_TEST_NATIVE_LIBRARY_PATH)
const std::string kLibraryPath = ZL_TEST_NATIVE_LIBRARY_PATH;

void testOpenFixtureLibrary() {
    zl::NativeLibrary library;
    require(!library.valid(), "a default-constructed library is not open");
    library.open(kLibraryPath);
    require(library.valid(), "the build fixture library opens");

    auto addSymbol = library.symbol("zl_fixture_add");
    require(addSymbol != nullptr, "the add symbol resolves");
    auto* add = reinterpret_cast<std::int32_t (*)(int, int)>(addSymbol);
    require(add(40, 2) == 42, "the exported function computes through the ABI");
    require(add(-1, 1) == 0, "and on negative inputs");

    auto nameSymbol = library.symbol("zl_fixture_name");
    require(nameSymbol != nullptr, "the name symbol resolves");
    auto* name = reinterpret_cast<const char* (*)()>(nameSymbol);
    require(std::string(name()) == "zl-test-native-library", "the exported string is intact");

    library.close();
    require(!library.valid(), "close() marks the library closed");
    library.close(); // idempotent
    require(!library.valid(), "close() is idempotent");

    bool closedSymbolRejected = false;
    try {
        library.symbol("zl_fixture_add");
    } catch (const std::runtime_error&) {
        closedSymbolRejected = true;
    }
    require(closedSymbolRejected, "looking up a symbol on a closed library throws");
}

void testRejections() {
    bool missingPath = false;
    try {
        zl::NativeLibrary library("/definitely/not/a/real/library.so");
        (void)library;
    } catch (const std::runtime_error& e) {
        missingPath = std::string(e.what()).find("failed to load native library") != std::string::npos;
    }
    require(missingPath, "a missing library path is a load error with the path in it");

    bool emptyPath = false;
    try {
        zl::NativeLibrary library;
        library.open("");
    } catch (const std::invalid_argument&) {
        emptyPath = true;
    }
    require(emptyPath, "an empty path is a usage error");

    bool missingSymbol = false;
    {
        zl::NativeLibrary library(kLibraryPath);
        try {
            library.symbol("zl_fixture_no_such_symbol");
        } catch (const std::runtime_error& e) {
            missingSymbol = std::string(e.what()) == "native symbol not found: zl_fixture_no_such_symbol";
        }
    }
    require(missingSymbol, "a missing symbol is named in the error");

    bool emptySymbol = false;
    {
        zl::NativeLibrary library(kLibraryPath);
        try {
            library.symbol("");
        } catch (const std::invalid_argument&) {
            emptySymbol = true;
        }
    }
    require(emptySymbol, "an empty symbol name is a usage error");
}

void testMoveSemantics() {
    zl::NativeLibrary source(kLibraryPath);
    require(source.valid(), "the source opens");
    zl::NativeLibrary moved = std::move(source);
    require(moved.valid(), "the handle moves");
    require(!source.valid(), "the source is emptied");
    auto* add = reinterpret_cast<std::int32_t (*)(int, int)>(moved.symbol("zl_fixture_add"));
    require(add(20, 22) == 42, "the moved library still works");

    zl::NativeLibrary target(kLibraryPath);
    target = std::move(moved);
    require(target.valid() && !moved.valid(), "assignment moves the handle");
    auto* again = reinterpret_cast<std::int32_t (*)(int, int)>(target.symbol("zl_fixture_add"));
    require(again(1, 41) == 42, "the reassigned library still works");
}
#else
void testPlatformSkip() {
    std::cerr << "native library regression: ZL_TEST_NATIVE_LIBRARY_PATH undefined; "
                 "the shared-library target did not build on this platform\n";
    require(false, "the fixture library path must be provided by CMake on this platform");
}
#endif
} // namespace

int main() {
#if defined(ZL_TEST_NATIVE_LIBRARY_PATH)
    testOpenFixtureLibrary();
    testRejections();
    testMoveSemantics();
#else
    testPlatformSkip();
#endif

    if (failures != 0) {
        std::cerr << failures << " native library regression(s) failed\n";
        return 1;
    }
    std::cout << "all native library regressions passed\n";
    return 0;
}
