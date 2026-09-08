# Tooling

ZL ships editor- and test-runner-oriented tooling alongside the compiler/runtime.
The first tools are small, zero-dependency Python commands so they can run anywhere
a developer can edit or test ZL source.

## `zl-lsp`

`zl-lsp` provides fast feedback that is safe to run on every keystroke:

- lexical diagnostics, including unterminated string and regex literals;
- delimiter matching for `{}`, `()`, and `[]`;
- enforcement of ZL's file/class-name rule (`Example.zl` must declare
  `class Example`);
- document symbols for top-level `class`, `interface`, `data`, and `enum`
  declarations plus class/data fields and methods.

It is intentionally conservative. Semantic checks still belong to the C++ compiler and
can be run with `zl --check <file.zl>`. The LSP server is meant to make editors useful
before the full compiler has been invoked.

### Running as a language server

```bash
python3 tools/zl-lsp/zl_lsp.py --stdio
```

Editors that support custom LSP commands can use that command for files matching
`*.zl`. When installed through CMake packaging, the script is installed as `zl-lsp`.

### Command-line diagnostics

The same script can be used in CI or editor integrations without speaking LSP:

```bash
python3 tools/zl-lsp/zl_lsp.py --check examples/basics/HelloWorld.zl
python3 tools/zl-lsp/zl_lsp.py --check examples/basics/HelloWorld.zl --json
```

The process exits with status `0` when no diagnostics are produced and `1` when any
file has an error.

### Document symbols

```bash
python3 tools/zl-lsp/zl_lsp.py --symbols examples/basics/HelloWorld.zl
python3 tools/zl-lsp/zl_lsp.py --symbols examples/basics/HelloWorld.zl --json
```

The JSON output uses LSP-compatible `DocumentSymbol` objects, which lets editor plugins
reuse the command-line mode while a full LSP integration is being wired up.

## `zl-test`

`zl-test` is a cross-platform discovery and runner frontend for examples and the
permanent regression corpus. It is designed for editors and CI systems that need a
machine-readable list of runnable cases without reimplementing the repository's shell
scripts.

### Listing tests

```bash
python3 tools/zl-test/zl_test.py list
python3 tools/zl-test/zl_test.py list --scope examples --json
python3 tools/zl-test/zl_test.py list --scope regressions --case generics
```

Each discovered case has a stable `id`, source `path`, `scope`, `category`, `kind`, and
expected exit code. Example cases also include their embedded expected-output block when
one is present.

### Running tests

```bash
python3 tools/zl-test/zl_test.py run --scope examples --runtime build/zl_language
python3 tools/zl-test/zl_test.py run --scope all --case HelloWorld --json
```

When `--runtime` is omitted, the tool checks `ZL_COMPILER_PATH`, common local build
locations, and then `zl_language`/`zl` on `PATH`. Package-manager regression cases use a
`zlpkg` binary found next to the runtime; if it is missing, those cases are skipped with
a structured result.

Filtering with `--case` accepts either a substring or a shell-style glob and may be
repeated. The filter is matched against the case id, name, category, and path.

A convenience wrapper is available on POSIX systems:

```bash
scripts/zl-test list --scope examples
```

Installed packages expose the command as `zl-test`.

## Testing the tools

Run the focused Python test suites with:

```bash
python3 tests/zl_lsp_tests.py
python3 tests/zl_test_runner_tests.py
```
