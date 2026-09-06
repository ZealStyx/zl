# Native Boundary, Compilation, and FFI

- [The boundary rule](#the-boundary-rule)
- [Native compilation (`@native`)](#native-compilation-native)
- [Native bindings (`zl-bind`)](#native-bindings-zl-bind)
- [FFI](#ffi)
- [Native export registry](#native-export-registry)

## The boundary rule

A permanent design rule for the project:

> **C++ is the small, stable native foundation; ZL owns high-level library policy and
> composition.**

Future C++ → ZL migrations must be benchmarked before a performance-sensitive native
implementation is removed. The `benchmarks/` directory holds the gate used for that.

Deliberately native: the VM, OS access, parsing engines (JSON, regex), collection
storage primitives, cryptographic primitives, and low-level numeric operations.

## Native compilation (`@native`)

Selected functions opt into the restricted native compiler with `@native`. The compiler
preserves the normal VM path as a fallback and can emit MIR-native portable C++:

```text
zl --emit-native output.cpp source.zl
```

The current native tier supports:

- Primitive `int`, `double`/`decimal`, and `bool` values
- Initialized primitive locals and assignments
- Integer ranges and boolean branches
- Arithmetic and bitwise expressions
- Bounded constant-loop unrolling
- Safe integer constant folding

Generated modules expose the stable primitive ABI in
`include/zl/compiler/native_abi.hpp`.

`Task<T>`, closures, heap objects, and VM reflection objects do **not** cross the native
ABI. Unsupported native signatures are rejected deterministically; the ordinary VM
remains authoritative for those cases.

Run `scripts/native_gate.sh` (Linux/macOS) or `scripts/native_gate.ps1` (Windows) to
exercise the native compiler, ABI regression tests, generated C++ compilation, and the
benchmark semantic gate.

### IR optimization

The compiler IR has a backend-neutral optimization pass. It removes unreachable blocks
and folds safe primitive integer/boolean constant expressions while leaving
ownership/borrow instructions untouched. MIR native emission runs this pass before
lowering, so optimized IR feeds both native output and future VM/backend consumers.

## Native bindings (`zl-bind`)

The `zl-bind` tool generates controlled native bindings, ZL facades, manifests, and API
documentation from a restricted C/C++ declaration subset. Generated manifests carry
target ABI metadata, and generated C++ contains compile-time ABI guards. Unsupported C++
constructs are rejected rather than guessed.

Class bindings use smart-pointer-backed native ownership and translate C++ exceptions
into deterministic ZL runtime errors. Shared bindings can retain an opaque handle
without exposing C++ pointer types to ZL.

The tool is deliberately conservative: C++ stays a small native foundation, while
high-level library behavior stays in ZL.

## FFI

The ownership-aware FFI foundation provides opaque handles, borrowed buffers and struct
views, callback lifetime contracts, native resource adapters, dynamic library loading,
and validated `@ffi(symbol)` / `@ffi(library, symbol)` metadata.

Typed field-by-field C struct schemas remain a later ABI extension.

## Native export registry

A process-local `NativeExportRegistry` accepts generated export tables, rejects
malformed or duplicate registrations, resolves exports by name, and routes name-based
calls through the `NativeError` boundary. This is the runtime registration bridge for
generated native modules; it avoids embedding raw function pointers in ZL values.
