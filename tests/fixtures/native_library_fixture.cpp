// Shared-library fixture for zl-native-library-tests: a real loadable object
// with known symbols, so the tests exercise open/symbol/unload against a
// library the build controls instead of a system path it cannot assume.
extern "C" int zl_fixture_add(int a, int b) { return a + b; }
extern "C" const char* zl_fixture_name(void) { return "zl-test-native-library"; }
