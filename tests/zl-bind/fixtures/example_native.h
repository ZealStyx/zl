#pragma once

#include <cstdint>

// Fixture header for tools/zl-bind/test_zl_bind.sh.
//
// Exercises the restricted binding surface: a top-level borrowed function and
// a unique-ownership native class with a constructor, destructor, const and
// mutating methods, and a method that throws.
//
// Annotations use the `// zl:` comment protocol understood by the parser. The
// class needs no annotation: the parser's default (borrowed) maps a class to
// unique ownership with exception error translation.

// zl: ownership=borrowed
// zl: errors=exception
extern "C" int zl_add(int a, int b);

class Counter {
public:
    Counter(int64_t start);
    ~Counter();

    int64_t value() const;
    void increment(int64_t by);
    int64_t fail() const;

private:
    // Data members are skipped by the binding generator (methods only).
    int64_t value_;
};
