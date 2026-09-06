#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

extern "C" std::int64_t zl_mir_native_NumericKernel_sumSquares(std::int64_t n);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static std::int64_t reference_kernel(std::int64_t n) {
    std::int64_t total = 0;
    for (std::int64_t i = 0; i < n; ++i) total += i * i;
    return total;
}

template <typename Fn>
static std::pair<std::int64_t, double> measure(Fn&& fn) {
    constexpr std::int64_t rounds = 50000;
    volatile std::int64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < rounds; ++i) sink += fn(100);
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    return {sink, seconds};
}

int main() {
    const auto expected = reference_kernel(100) * 50000;
    const auto [refValue, refSeconds] = measure(reference_kernel);
    const auto [nativeValue, nativeSeconds] = measure(zl_mir_native_NumericKernel_sumSquares);
    if (refValue != expected || nativeValue != expected) {
        std::cerr << "native benchmark semantic mismatch\n";
        return 1;
    }
    std::cout << "reference=" << refSeconds << "s native=" << nativeSeconds << "s\n";
    std::cout << "expected=" << expected << "\n";
    return 0;
}
