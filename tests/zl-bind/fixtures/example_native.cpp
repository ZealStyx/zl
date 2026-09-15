// Fixture implementation for tools/zl-bind/test_zl_bind.sh.
// Compiled together with the generated Demo_bindings.cpp by the test script.

#include "example_native.h"

#include <stdexcept>

extern "C" int zl_add(int a, int b) {
    return a + b;
}

Counter::Counter(int64_t start) : value_(start) {}

Counter::~Counter() = default;

int64_t Counter::value() const {
    return value_;
}

void Counter::increment(int64_t by) {
    value_ += by;
}

int64_t Counter::fail() const {
    throw std::runtime_error("counter failure");
}
