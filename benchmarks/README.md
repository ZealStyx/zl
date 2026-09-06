# Phase 10 benchmark gate

`native_numeric_benchmark.cpp` compares the generated native numeric kernel against a direct C++ reference implementation over the same workload. It is a semantic/performance sanity gate, not a promise of C++-class performance.

Typical flow from a built checkout:

1. Generate native C++ for `tests/zl/valid/native/NumericKernel.zl` with `zl --emit-native`.
2. Compile the generated source and `benchmarks/native_numeric_benchmark.cpp` together with `-O3`.
3. Run the benchmark and require the native and reference results to match exactly.
4. Run `tests/zl/valid/native/benchmarks/Benchmark.zl` through the VM and record its wall time separately.

Performance thresholds are intentionally not hard-coded because they are host-dependent. A regression is semantic failure or an unexpected optimizer result, not a fixed machine-time number.
