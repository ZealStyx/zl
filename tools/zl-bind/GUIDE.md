# zl-bind Guide

`zl-bind` is ZL's restricted C/C++ binding generator. It converts a supported native header into ZL-facing declarations plus C++ wrappers that connect those declarations to the ZL native runtime.

## Purpose

Use `zl-bind` to expose a controlled subset of C/C++ APIs without making the binding generator depend on unstable compiler internals.

The compiler/runtime remains the authority for:

- ZL syntax and type semantics
- native ABI conventions
- ownership and borrowing rules
- native handle representation
- callback lifetime rules
- exception translation
- runtime registration

`zl-bind` consumes those contracts; it must not invent alternate semantics.

## Current scope

The current generator is intentionally restricted. It supports the native constructs implemented by `tools/zl-bind/header_parser.*` and generates:

- ZL declarations for supported functions
- ZL-facing wrappers for supported native classes/methods
- constructor and method dispatch
- native handle retain/close operations
- runtime registration metadata
- compile-time ABI guards in generated C++
- generated documentation describing the bindings

Unsupported C/C++ constructs should fail clearly or remain outside the generated API rather than being guessed.

## Basic command

```text
zl-bind <header> <namespace> <output-dir>
```

Example:

```text
zl-bind tests/zl-bind/fixtures/example_native.h Demo /tmp/demo_bindings
```

The generated output is target-specific C++/ZL binding material and should be treated as generated code. Do not hand-edit generated files; update the native header or binding generator instead and regenerate.

## Development workflow

1. Start from the current compiler/runtime handoff.
2. Confirm the native ABI and ownership contract before adding a binding feature.
3. Extend `header_parser.*` only for constructs the generator can represent safely.
4. Extend `main.cpp` generation logic to match the parser model.
5. Add or update fixtures under `tests/zl-bind/fixtures/`.
6. Run `tools/zl-bind/test_zl_bind.sh` (or the equivalent platform test script).
7. Compile the generated C++ and verify runtime registration and exception behavior.
8. Record compiler dependencies or new ABI requirements in the project handoff before relying on them.

## Compiler coordination

`zl-bind` is developed in parallel with the compiler, but the two projects should remain contract-driven.

When a binding feature requires a compiler capability that does not exist yet:

```text
BLOCKED: compiler capability <name>
```

Do not introduce temporary ZL syntax or ABI behavior solely to unblock `zl-bind`.

The canonical compiler-side contract should cover:

- native declaration syntax
- primitive/native type mapping
- opaque handles
- borrowed views
- ownership qualifiers
- callback leases
- calling conventions
- exception boundaries
- runtime registration

## Ownership and lifetime

Generated bindings must preserve the native ownership model.

A native pointer must never become an unmanaged ZL value merely because it is convenient for the generator. Prefer opaque handles and explicit retain/close operations where ownership is transferred or shared.

Borrowed pointers/views must remain tied to their documented lifetime owner. Callback pointers must remain valid for the duration represented by their runtime lease.

## Error handling

Generated wrappers must translate native failures consistently with the current ZL/native exception model. Arity errors, invalid handles, closed handles, type mismatches, and native exceptions should produce deterministic ZL-visible failures rather than undefined behavior.

## ABI expectations

Generated C++ contains compile-time checks for the ABI assumptions used by the binding layer, including integer width, floating-point storage, pointer width, and byte order where applicable.

Bindings should be regenerated for each supported target when target ABI assumptions differ.

## Testing expectations

At minimum, every binding feature should have:

- parser coverage
- generated-output coverage
- a compile test for generated C++
- a runtime registration/dispatch test when applicable
- negative coverage for unsupported or unsafe declarations

When compiler semantics change, rerun both the `zl-bind` suite and the relevant compiler/native ABI gates.

## Repository layout

```text
tools/zl-bind/
    GUIDE.md
    main.cpp
    header_parser.cpp
    header_parser.hpp
    test_zl_bind.sh

tests/zl-bind/
    fixtures/
```

The folder names intentionally do not encode a phase number. Historical phase references belong in Markdown documentation only.

## Source-control rules

Keep generated/build products out of the source archive:

- no `build/`
- no `cmake/` build tree
- no compiled binaries
- no generated benchmark/build output
- no duplicate project trees

The project archive should contain the `zl-bind` source and tests, not disposable build output.
