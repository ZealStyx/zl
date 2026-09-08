# Development

- [Build](#build)
- [Stdlib lookup](#stdlib-lookup)
- [Source organization](#source-organization)
- [Testing](#testing)
- [Distribution builds](#distribution-builds)

## Build

Portable, on every platform:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Targets: `zl_language` (runtime), `zlpkg` (dependency manager), and `zl-bind`
(native binding generator), plus the C++ test executables. The Python-based `zl-lsp`
and `zl-test` tools do not need compilation.

On Windows, `scripts/build.bat` wraps the same configuration and accepts:

```bat
scripts\build.bat --debug
scripts\build.bat --clean
scripts\build.bat --target zl_language
scripts\build.bat --jobs 8
```

### Install

```bash
cmake --install build --prefix <install-prefix>
```

Produces `bin/zl` (launcher), `bin/zl_language`, `bin/zlpkg`, `lib/zl/stdlib/`, and the
public headers. Development builds continue to produce `zl_language` directly.

## Stdlib lookup

The runtime resolves its standard library in this order:

1. An explicit `ZL_STDLIB_ROOT`
2. `ZL_HOME/lib/zl/stdlib`
3. The `stdlib` directory beside the executable
4. The installed `<prefix>/lib/zl/stdlib` layout

`ZL_EXTRA_ROOTS` is an ordered dependency/search list inserted *before* the stdlib root.
It uses `;` as the path-list separator on Windows and `:` on POSIX platforms.

The runtime validates `lib/zl/stdlib/VERSION` against its own version before loading a
program, and `zlpkg run` verifies that the adjacent runtime reports a matching version.

## Source organization

- A standard CMake executable target defined in `CMakeLists.txt`.
- The main executable loads a `.zl` file, tokenizes it, parses it, type-checks it,
  compiles bytecode, and runs it in the VM.
- Errors are reported in three categories: `syntax error`, `compile error`, and
  `runtime error`.
- `stdlib/` is copied next to the built binary so a development build can find it.

## Testing

C++ unit tests cover the lexer, parser, compiler, VM, IR, regex engine, scheduler,
native compiler, and FFI layers; they are declared as separate executables in
`CMakeLists.txt`. If you extend the language, add tests for the affected layer.

The ZL-level corpus is split in two:

- **Regression fixtures** under `tests/zl/valid/` and `tests/zl/invalid/`, grouped by
  category (`core_tests/`, `import_tests/`, `package_tests/`,
  `package_manager_tests/`, `language_hardening_tests/`, `records/`, …). These are
  permanent and are the source of truth for language behavior.
- **Examples** under `examples/` — a small, curated, user-facing set kept deliberately
  separate from the regression corpus.

> Note: the `.zl` corpus and `examples/` are not part of every checkout. The runner
> scripts below discover cases from the filesystem, so they only report what is present.

### Running

```bash
# interactive: pick 1) Test All  2) Invalid  3) Valid, then which categories to run
./scripts/run_examples.sh build/zl_language

# non-interactive
./scripts/run_examples.sh build/zl_language all         # everything
./scripts/run_examples.sh build/zl_language invalid     # every invalid category
./scripts/run_examples.sh build/zl_language valid 1,3   # only categories 1 and 3
./scripts/run_examples.sh build/zl_language --list      # list categories/cases and exit
```

`scripts/run_examples.bat` takes the same arguments:
`scripts\run_examples.bat build\zl_language.exe [all|invalid|valid] [selection]`.

Use `scripts/run_regressions.sh` / `scripts/run_regressions.bat` for the full permanent
regression corpus, including package-manager cases, and `scripts/native_gate.sh` /
`scripts/native_gate.ps1` for the native compiler gate.

## Distribution builds

Release packaging is driven by CMake/CPack — see [`packaging/README.md`](../packaging/README.md).

On Linux the release configuration produces native TGZ, ZIP, and DEB packages. Windows
and macOS releases should be built on native runners using the same CMake/CPack
configuration; the repository includes a GitHub Actions release matrix for those builds.
