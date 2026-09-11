# ZL Language

ZL is a small, statically typed, interpreted language implemented in C++17.

This repository contains the full toolchain: lexer, parser, type checker, bytecode
compiler, virtual machine, native runtime helpers, a restricted native (AOT) compiler,
a standard library written mostly in ZL itself, and `zlpkg`, a git/path dependency manager.

```zl
class Hello {
    func main(): void {
        var names = new List<string>()
        names.push("Ada")
        names.push("Grace")
        names.push("Alan")
        names.forEach(func(n) => log("Hello, " + n))
    }
}
```

## Contents

- [Requirements](#requirements)
- [Build](#build)
- [Run a program](#run-a-program)
- [Install](#install)
- [Project layout](#project-layout)
- [Language at a glance](#language-at-a-glance)
- [Documentation](#documentation)
- [Current limitations](#current-limitations)
- [License](#license)

## Requirements

- CMake 3.10 or newer
- A C++17-capable compiler (MSVC, GCC, or Clang)
- `git` on `PATH` if you use `zlpkg` git dependencies

## Build

The portable path is plain CMake, on every platform:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

That produces both binaries: `zl_language` (the runtime) and `zlpkg` (the dependency manager).

Convenience wrappers are also provided:

| Task | Linux / macOS | Windows |
| --- | --- | --- |
| Build | `cmake --build build` | `scripts\build.bat` |
| Run a file | `scripts/zl <file.zl>` | `scripts\run.bat <file.zl>` |
| Run examples | `scripts/run_examples.sh` | `scripts\run_examples.bat` |
| Run + verify examples | `examples/run_all.sh` | `examples\run_all.bat` |
| Run regressions | `scripts/run_regressions.sh` | `scripts\run_regressions.bat` |
| Native compiler gate | `scripts/native_gate.sh` | `scripts\native_gate.ps1` |

`scripts/build.bat` accepts `--debug`, `--clean`, `--target <name>`, and `--jobs <n>`.

## Run a program

```bash
build/zl_language examples/basics/HelloWorld.zl arg1 arg2
```

```text
zl <file.zl> [program args...]
```

The runtime also supports `zl --version`, `zl --help`, and repeatable `--root <path>`
flags for extra import roots.

> **File rule:** each `.zl` file must contain a class whose name matches the file stem
> exactly — `Hello.zl` must declare `class Hello`. This is enforced by the compiler.

## Install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --prefix <install-prefix>
```

The install tree contains `bin/zl` (launcher), `bin/zl_language`, `bin/zlpkg`,
`lib/zl/stdlib/`, and the public headers. An installed runtime finds its bundled
stdlib without any environment variables.

Environment variables: `ZL_STDLIB_ROOT`, `ZL_HOME`, `ZL_EXTRA_ROOTS`.
See [docs/development.md](docs/development.md#stdlib-lookup) for the lookup precedence.

## Project layout

```text
include/zl/    Public headers for the runtime and compiler
src/           C++ implementation of the compiler, VM, and natives
               src/mir/ is the mid-level IR (see docs/mir.md)
zlpkg/         C++ implementation of the zlpkg CLI (separate target)
stdlib/        Standard-library packages beyond auto-imported zl.lang
tools/         Developer tooling, including zl-lsp, zl-test, and the zl-bind binding generator
benchmarks/    Native-boundary performance gates
packaging/     CPack release packaging configuration
scripts/       Build, run, regression, native-gate, and binding helpers
docs/          Reference documentation (see below)
build/         Local CMake build directory (not committed)
```

## Language at a glance

- `var` / `let` declarations with type inference or explicit types
- Primitives `int`, `double`, `decimal`, `bool`, `string`, plus union types (`int|string`)
- `class`, `data` records, `enum`, interfaces, inheritance, and `public`/`private`/`protected`
- Generic classes, including `List<T>`, `Map<K,V>`, and `Set<T>` as real compiled generics
- Lambdas — `func(x) => x * 2` and `func(x) { ... }` — with by-value capture
- Operator overloading via `operator +(...)` declarations
- `if` / `elif` / `else`, `for`, `while`, `repeat`, `break`, `continue`
- `async func`, `await`, `Task<T>`, and `Shared<T>` for concurrency
- Java-style dotted imports with an auto-imported `zl.lang`
- Minimal name-based reflection through `Type.*`
- `@native` opt-in AOT compilation and `@ffi` native bindings

Full syntax and semantics: **[docs/language-guide.md](docs/language-guide.md)**.

## Documentation

| Document | Covers |
| --- | --- |
| [docs/language-guide.md](docs/language-guide.md) | Types, lambdas, generics, records, operators, reflection |
| [docs/stdlib.md](docs/stdlib.md) | `zl.lang`, `Math`, `zl.test`, `zl.logging`, `zl.text`, `zl.serialize`, `zl.time`, `zl.crypto` |
| [docs/packages.md](docs/packages.md) | Imports, the stdlib root, and `zlpkg` dependency management |
| [docs/native.md](docs/native.md) | Native boundary, `@native` compilation, `zl-bind`, FFI, async |
| [docs/mir.md](docs/mir.md) | MIR — the typed mid-level IR: structures, invariants, verifier, lowering |
| [docs/mir-optimizer.md](docs/mir-optimizer.md) | The MIR optimiser: pass/analysis managers, effect classification, the safety contract, differential validation |
| [docs/tooling.md](docs/tooling.md) | Editor/test tooling, `zl-lsp`, `zl-test`, fast diagnostics, document symbols |
| [docs/development.md](docs/development.md) | Build details, stdlib lookup, testing, packaging |
| [examples/](examples/README.md) | 46 runnable examples, basics through async, each with verified output |
| [examples/REVIEW.md](examples/REVIEW.md) | Bugs found while writing them, fixed and open |
| [docs/changelog.md](docs/changelog.md) | Dated development checkpoints by phase |

The memory-model direction — ownership combined with tracing GC, GC-managed and
thread-confined by default, with `shared` explicit rather than implicit — is
summarized in [docs/language-guide.md](docs/language-guide.md#memory-model-direction).

## Current limitations

- The standard library is deliberately hybrid: high-level APIs live in ZL, while the
  VM, OS access, parsing engines, and storage primitives stay native.
- `func`-typed slots don't carry full signature types yet, so arity mismatches on a
  closure call are runtime errors rather than compile-time ones.
- Async is partial: `async func`, `Task<T>`, and `await` work; `block()`/`ignore()`,
  cancellation propagation, unobserved-failure reporting, and async lambdas are pending.
- `zlpkg` has no registry — dependencies resolve by `git` URL or local `path` only,
  with no version ranges, workspaces, or dev-dependencies.
- One class per file, matching the file stem.
- `Shared<T>` marks a capture as shareable; it does not itself provide thread safety.
  The top-level `share()` helper and compile-time confinement checks are pending.

## License

Licensed under the **PolyForm Noncommercial License 1.0.0 with Runtime Exception**.
See [LICENSE](LICENSE) for the full text.
