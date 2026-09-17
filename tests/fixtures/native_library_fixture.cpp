// Shared-library fixture for zl-native-library-tests: a real loadable object
// with known symbols, so the tests exercise open/symbol/unload against a
// library the build controls instead of a system path it cannot assume.
#if defined(_WIN32)
#define ZL_FIXTURE_EXPORT __declspec(dllexport)
#else
#define ZL_FIXTURE_EXPORT
#endif

extern "C" ZL_FIXTURE_EXPORT int zl_fixture_add(int a, int b) { return a + b; }
extern "C" ZL_FIXTURE_EXPORT const char* zl_fixture_name(void) { return "zl-test-native-library"; }
