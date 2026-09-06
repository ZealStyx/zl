#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${ZL_BIND_BIN:-$ROOT/build/zl-bind}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$BIN" "$ROOT/tests/zl-bind/fixtures/example_native.h" Demo "$TMP"
grep -q '^version=3$' "$TMP/Demo.zlbind"
grep -q '^namespace=Demo$' "$TMP/Demo.zlbind"
grep -q '^abi=' "$TMP/Demo.zlbind"
grep -q '^pointer_bits=' "$TMP/Demo.zlbind"
grep -q '^int64_bits=64$' "$TMP/Demo.zlbind"
grep -q '^double_bits=64$' "$TMP/Demo.zlbind"
grep -q '^function|zl_add|int|2|' "$TMP/Demo.zlbind"
grep -q '^class|Counter|constructor=1|ownership=unique|errors=exception|handles=integer$' "$TMP/Demo.zlbind"
grep -q '^method|Counter.value|int|1|const=true$' "$TMP/Demo.zlbind"
grep -q '^method|Counter.increment|void|2|const=false$' "$TMP/Demo.zlbind"
grep -q '^method|Counter.fail|int|1|const=true$' "$TMP/Demo.zlbind"
grep -q 'native exception' "$TMP/Demo_bindings.cpp"
grep -q 'Counter_new_binding' "$TMP/Demo_bindings.cpp"
grep -q 'Counter_value_binding' "$TMP/Demo_bindings.cpp"
grep -q 'NativeFunctionRegistrar registrar' "$TMP/Demo_bindings.cpp"
grep -q 'class Counter' "$TMP/Demo.zl"
grep -q 'class Counter' "$TMP/Demo.zl"
grep -q '^# Demo native bindings$' "$TMP/Demo.md"
grep -q '## ABI requirements' "$TMP/Demo.md"
grep -q 'Counter' "$TMP/Demo.md"

cp "$ROOT/tests/zl-bind/fixtures/example_native.h" "$TMP/example_native.h"
cp "$ROOT/tests/zl-bind/fixtures/example_native.cpp" "$TMP/example_native.cpp"
g++ -std=c++17 -I"$ROOT/include" -I"$TMP" -c "$TMP/Demo_bindings.cpp" -o "$TMP/Demo_bindings.o"
g++ -std=c++17 -I"$ROOT/include" -I"$TMP" -c "$TMP/example_native.cpp" -o "$TMP/example_native.o"
g++ -std=c++17 -I"$ROOT/include" -c "$ROOT/src/vm/native.cpp" -o "$TMP/native.o"
g++ -std=c++17 -I"$ROOT/include" -c "$ROOT/src/vm/value.cpp" -o "$TMP/value.o"
g++ -std=c++17 -I"$ROOT/include" -c "$ROOT/src/compiler/native_catalog.cpp" -o "$TMP/native_catalog.o"
cat > "$TMP/registry_test.cpp" <<'CPP'
#include "zl/vm/native.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
int main() {
    const char* names[] = {
        "Demo.zl_add", "Demo.Counter.new", "Demo.Counter.value",
        "Demo.Counter.increment", "Demo.Counter.fail", "Demo.Counter.close"
    };
    for (const char* name : names) if (!zl::findNativeFunction(name)) return 1;
    auto make = zl::findNativeFunction("Demo.Counter.new");
    auto value = zl::findNativeFunction("Demo.Counter.value");
    auto increment = zl::findNativeFunction("Demo.Counter.increment");
    auto fail = zl::findNativeFunction("Demo.Counter.fail");
    auto close = zl::findNativeFunction("Demo.Counter.close");
    auto handle = zl::nativeFunctionTable()[*make].fn({std::int64_t(7)});
    if (std::get<std::int64_t>(zl::nativeFunctionTable()[*value].fn({handle})) != 7) return 2;
    zl::nativeFunctionTable()[*increment].fn({handle, std::int64_t(5)});
    if (std::get<std::int64_t>(zl::nativeFunctionTable()[*value].fn({handle})) != 12) return 3;
    try { zl::nativeFunctionTable()[*fail].fn({handle}); return 4; }
    catch (const std::exception& e) { if (std::string(e.what()).find("Counter native exception: counter failure") == std::string::npos) return 5; }
    zl::nativeFunctionTable()[*close].fn({handle});
    try { zl::nativeFunctionTable()[*value].fn({handle}); return 6; }
    catch (const std::exception&) {}
    std::cout << "zl-bind class/registry tests passed\n";
}
CPP
g++ -std=c++17 -I"$ROOT/include" "$TMP/Demo_bindings.o" "$TMP/example_native.o" "$TMP/native.o" "$TMP/value.o" "$TMP/native_catalog.o" "$TMP/registry_test.cpp" -o "$TMP/registry_test"
"$TMP/registry_test"


cat > "$TMP/shared.h" <<'H'
#include <stdexcept>
// zl: ownership=shared
// zl: errors=exception
class SharedCounter {
public:
    SharedCounter(int start);
    ~SharedCounter();
    int value() const;
};
H
"$BIN" "$TMP/shared.h" Shared "$TMP/shared_out"
grep -q '^class|SharedCounter|constructor=1|ownership=shared|errors=exception|handles=integer|retain=true$' "$TMP/shared_out/Shared.zlbind"
grep -q 'SharedCounter_retain_binding' "$TMP/shared_out/Shared_bindings.cpp"
g++ -std=c++17 -I"$ROOT/include" -I"$TMP" -c "$TMP/shared_out/Shared_bindings.cpp" -o "$TMP/Shared_bindings.o"

cat > "$TMP/bad.h" <<'H'
extern "C" long long unsupported(long long value);
H
if "$BIN" "$TMP/bad.h" Bad "$TMP/out" >/dev/null 2>&1; then
  echo "expected unsupported type rejection" >&2
  exit 1
fi

echo "zl-bind tests passed"
