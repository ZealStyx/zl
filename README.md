# ZL Language

ZL is a small interpreted language implemented in C++17. This repository contains the lexer, parser, type checker, bytecode compiler, virtual machine, native runtime helpers, and a set of runnable `.zl` example programs.

## What this repository includes

- A lexer, parser, type checker, compiler, and VM pipeline
- Native helpers for I/O, math, strings, collections, filesystem, time, and environment access
- A Java-style package/import system, with an auto-imported `zl.lang` and an explicit-import-required stdlib beyond it (see "Packages and imports" below)
- `zlpkg`, a git/local-path dependency manager CLI built alongside `zl_language` (see "External dependencies" below)
- A small curated set of runnable example programs under `examples/`
- Permanent ZL regression fixtures are kept under `tests/zl/`; `examples/` is intentionally small and user-facing

## Project layout

```text
include/zl/    Public headers for the language runtime and compiler
src/           C++ implementation of the compiler, VM, and natives
zlpkg/         C++ implementation of the zlpkg dependency-manager CLI (separate binary/target)
stdlib/        Standard-library packages beyond auto-imported zl.lang (e.g. zl/util/Queue.zl) - copied next to the built binary
examples/      Small curated showcase programs for users and documentation
tests/zl/       Permanent ZL valid/invalid regression fixtures
tests/         C++ test files for lexer, parser, compiler, and VM
docs/          Organized architecture, design, guide, reference, status, and archive documentation
docs/status/IMPLEMENTED_ONLY_SCORECARD.md  Current implemented-only maturity scorecard and verified test baseline
scripts/       Build, run, regression, native-gate, and binding helper scripts
build/         Generated build directory created locally by CMake / scripts/build.bat (not committed)
```

## Requirements

- CMake 3.10 or newer
- A C++17-capable compiler
- Windows if you want to use the provided `.bat` helper scripts

## Memory-model direction

The planned memory model combines ownership with tracing GC. Unannotated managed/reference values default to GC-managed, thread-confined semantics. `shared` is explicit rather than the default. See `docs/architecture/ZL_MEMORY_MODEL.md`.

## Build

The simplest way to build on Windows is:

```bat
scripts/build.bat
```

That will configure a Release build in `build/` and compile the project (both `zl_language` and `zlpkg`).

Useful variants:

```bat
scripts/build.bat --debug
scripts/build.bat --clean
scripts/build.bat --target zl_language
scripts/build.bat --jobs 8
```

If you prefer CMake directly:

```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Install the toolchain

CMake can install the runtime, package manager, public headers, and bundled standard library into a normal prefix:

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --prefix <install-prefix>
```

The install contains `bin/zl` (a launcher for the runtime), `bin/zl_language` (the runtime binary), `bin/zlpkg`, `lib/zl/stdlib/`, and the public headers. The installed runtime can find the bundled stdlib without `ZL_STDLIB_ROOT`. Development builds continue to produce `zl_language` directly.

The runtime also supports `zl --version`, `zl --help`, `ZL_STDLIB_ROOT`, `ZL_HOME`, and `ZL_EXTRA_ROOTS`.

Stdlib lookup precedence is: an explicit `ZL_STDLIB_ROOT`, then `ZL_HOME/lib/zl/stdlib`, then the stdlib beside the executable, then the installed `<prefix>/lib/zl/stdlib` layout. `ZL_EXTRA_ROOTS` is an ordered dependency/search list inserted before the stdlib root; on Windows it uses `;` as the path-list separator, while POSIX platforms use `:`. The runtime validates `lib/zl/stdlib/VERSION` against its own version before loading a program. `zlpkg run` also verifies that the runtime next to `zlpkg` reports the package manager's matching version.

## Run a program

Use `scripts/run.bat` to build and run a `.zl` file in one step:

```bat
scripts/run.bat examples\Hello.zl
```

The underlying executable also accepts extra program arguments:

```bat
build\zl_language.exe examples\Hello.zl arg1 arg2
```

The entry point syntax is:

```text
zl <file.zl> [program args...]
```

## Important file rule

Each `.zl` file must contain a class whose name matches the file stem exactly.

For example:

```text
tests/zl/valid/core_tests/TestProgram.zl -> class TestProgram
tests/zl/invalid/core_tests/type_mismatch.zl -> class type_mismatch
```

This is enforced by the current compiler pipeline.

## Language features currently exercised

The repository’s example programs cover the following areas of the language:

- Variable declarations with inference and explicit types
- `var` and `let`
- Primitive values such as `int`, `double`, `bool`, and `string`
- Union-style values such as `int|string`
- Arrays, lists, maps, and sets through the collection helpers - `map` is insertion-ordered by construction (a vector-backed association list, not a hash table): new keys append at the end, re-setting an existing key updates it in place, and removing a key doesn't reorder the rest. See `tests/zl/valid/core_tests/MapOrderingGuarantee.zl`, which asserts this rather than just demonstrating it.
- Arithmetic, comparison, logical, bitwise, and shift operators
- `if`, `elif`, `else if`, `else`, `for`, `while`, `repeat`, `break`, and `continue`
- `func` declarations, methods, constructors, and return values
- `data` records (including construction via `TypeName { field: value, ... }` literals) and `class` declarations
- `enum` declarations, accessed as `EnumName.MEMBER`, with real static typing (a variable/parameter typed as the enum rejects a raw string at compile time)
- Access modifiers such as `public`, `private`, and `protected`
- String helpers such as trim, substring, split, replace, and conversions
- Native helpers for math, parsing, filesystem, time, and environment access
- Lambda expressions (`func(params) => expr` / `func(params) { block }`) - see "Lambda expressions" below

## Lambda expressions

An anonymous function VALUE - assignable to a variable, passable as a call
argument, or returnable from a function:

```zl
var square = func(x) => x * x        // expression body - `return` is implied
log(square(5))                        // 25

var greet = func(name) {              // block body - explicit `return` required
    log("Hi " + name)
}
greet("Zeal")

func applyTwice(func f, int x): int {  // `func` as a parameter/return type
    return f(f(x))
}
log(applyTwice(func(x) => x * 2, 3))  // 12
```

**Syntax:** `func` (the same keyword as a named `func` declaration,
reused in expression position) followed by a parameter list, then either
`=> expr` (single-expression body, return implied) or `{ statements }`
(block body, an explicit `return` is required the same as any named
function - falling off the end returns `nil`). Parameters may be untyped
(`x`) or typed (`int x`), using the same `Type Name` order as named functions.
`func` alone (no parameter list) is also a valid TYPE name, for a variable
or parameter that holds a function value: `callback: func`.

**Capture semantics: by value.** A lambda snapshots every variable
currently in scope at the moment it's *evaluated* (not declared) - a copy,
not a live reference. Mutating the original variable afterward is never
visible inside the lambda:

```zl
var n = 10
var readN = func() => n
n = 999
log(readN())   // 10 - NOT 999
```

Two lambdas created from the same scope (e.g. on different iterations of a
loop) get independent snapshots - one lambda's own execution never affects
another's. Within a single closure, though, mutating one of its own
captured variables persists across repeated calls to *that same* closure
(the classic "counter" pattern), since a closure privately owns its
snapshot from the moment it's created:

```zl
func makeCounter(): func {
    var count = 0
    var inc = func() { count = count + 1; return count }
    return inc
}
var c = makeCounter()
log(c())  // 1
log(c())  // 2
```

Full function-type signatures (parameter/return types on a `func`-typed
slot) aren't checked yet - calling a `func`-typed value with the wrong
number of arguments is caught at runtime, not at compile time. See
`tests/zl/valid/regression_suites/callable_suite.zl` and `tests/zl/invalid/function_type_tests/`, and
`ROADMAP.md`'s Phase 7 section for the full design writeup.

## Generic collection types: List<T>, Map<K,V>, Set<T>

Real generic classes, not native tags - `push`/`pop`/`get`/`put`/`length`
(and `add`/`has`/`remove`) are genuinely compiled methods, so a wrong
element/key/value type is a compile-time error, not a runtime one:

```zl
var nums = new List<int>()
nums.push(1)
nums.push(2)
nums.push("oops")   // compile error: no overload of 'push' matches (int), got (string)

int first = nums.get(0)   // get()'s return type is narrowed to int for THIS instantiation
nums.put(0, 99)            // index-assignment - see naming note below

var ages = new Map<string, int>()
ages.put("ada", 36)
log(ages.get("ada"))       // 36
log(ages.has("grace"))     // false
ages.remove("ada")

var tags = new Set<string>()
tags.add("x")
tags.add("x")               // no-op, sets don't duplicate
log(tags.length())          // 1
```

`List<T>` also owns the high-level collection algorithms in ZL, while C++
continues to provide only the underlying storage primitives. Available
algorithms include `contains`, `indexOf`, `count`, `any`, `all`, `filter`,
`forEach`, and `reversed`:

```zl
var nums = new List<int>()
nums.push(1)
nums.push(2)
nums.push(4)

log(nums.contains(2))
var evens = nums.filter(func(x) => x % 2 == 0)
log(evens.length())
log(nums.any(func(x) => x > 3))
```

These algorithms are implemented in ZL and call the small native storage
boundary underneath.

Always available, no `import` needed. `List`/`Map`/`Set` are internally
backed by the same native list/map/set storage the older lowercase
`list<T>`/`map<K,V>`/`set<T>` type-annotation syntax uses (see "Current
limitations" below) - only the compile-time type surface is new, layered
on top via the same generic-class machinery user-defined generic classes
(`class Box<T>`) already use, so a mismatched element/key/value type is
caught by ordinary overload resolution, with the same quality of error
message as any other method call.

**Naming note:** the index/key-assignment method is `put`, not `set` -
`set` is a reserved keyword (the lowercase `set<T>` type annotation), so
it can't also be a method name. This is a keyword-collision workaround,
not a design preference.

**Typed collection literals:** the expected declared type now participates
in literal checking and construction. Lists support `[...]`, while map/set
literals continue to use `{...}`:

```zl
list<int> nums = [1, 2, 3]
map<string, int> ages = {"ada": 36, "alan": 41}
set<string> names = {"ada", "alan", "ada"}

List<int> genericNums = [4, 5, 6]
Map<string, int> genericAges = {"ada": 36}
Set<int> genericSet = [1, 2, 2]
```

Every element/key/value is checked against the expected generic type. Typed
`List`/`Map`/`Set` literals construct the normal generic ZL collection
objects; lowercase `list`/`map`/`set` annotations continue to use their
native collection representation.

## Example programs

Broad coverage examples:

- `tests/zl/valid/core_tests/CoreLanguage.zl`
- `tests/zl/valid/core_tests/OopAndAccess.zl`
- `tests/zl/valid/core_tests/CollectionsCoverage.zl`
- `tests/zl/valid/core_tests/StringCoverage.zl`
- `tests/zl/valid/core_tests/NativeCoverage.zl`

Negative examples:

- `tests/zl/invalid/core_tests/return_type.zl`
- `tests/zl/invalid/core_tests/type_mismatch.zl`
- `tests/zl/invalid/core_tests/argument_count.zl`
- `tests/zl/invalid/core_tests/syntax.zl`
- `tests/zl/invalid/core_tests/undefined_variable.zl` currently fails at runtime rather than compile time, so it is less useful as a compiler-only negative case

All regression fixtures live under `tests/zl/valid/` and `tests/zl/invalid/` (grouped by test category, e.g. `core_tests/`, `import_tests/`, `language_hardening_tests/`) and `tests/zl/invalid/` (same category grouping, for cases that are expected to fail). User-facing demonstrations are kept outside the regression corpus. Run `scripts/run_examples.bat` / `scripts/run_examples.sh` to discover and run all of them - see "Running the examples" below.

## Packages and imports

Java-style dotted imports are implemented: `import io.github.test.Helpers` resolves to `<sourceRoot>/io/github/test/Helpers.zl`, where `sourceRoot` is the nearest ancestor directory literally named `src` (or the entry file's own directory, for flat single-file programs with no imports). Transitive imports, diamond imports, and circular-import detection are all handled - see `tests/zl/valid/import_tests/` and `tests/zl/invalid/import_tests/`.

On top of your own project's imports, the interpreter also searches a **stdlib root** - `$ZL_STDLIB_ROOT` if set, otherwise `<the zl_language executable's own directory>/stdlib` - for standard-library packages:

- **`zl.lang`** is auto-imported everywhere, no `import` statement needed - this is everything that existed as a flat native before packages did: `Math`, `String`, `IO`, `Collection`, `FileSystem`, `Time`, `System`, `Int`/`Double`/`Bool` parsing.
- **Everything else** requires an explicit `import`, and the compiler rejects a call to a package that wasn't imported in that file, e.g. `import zl.util.Queue` before `Queue.newQueue()` is usable. `zl.util.Queue`/`zl.util.Stack` are the first (and so far only) packages built this way - see `tests/zl/valid/package_tests/` and `tests/zl/invalid/package_tests/`.

Stdlib packages aren't special-cased at the resolver level: a stdlib package is just another `.zl` file the loader happens to find via the second root, resolved with the exact same code path as a project's own `import pkg.Helpers`.

### External dependencies (`zlpkg`)

Beyond the project root and the stdlib root, `zl_language` will search any number of additional roots passed as repeatable `--root <path>` flags (or listed in the `ZL_EXTRA_ROOTS` env var, OS-path-list separated) - tried in the order given, before the stdlib root. `zlpkg`, a separate CLI built alongside `zl_language` from the same `CMakeLists.txt`, is what turns a dependency graph into that list of roots automatically:

```bash
# in a project directory containing zlpkg.toml:
zlpkg install            # resolve dependencies, write zlpkg.lock
zlpkg install --update   # force a fresh resolve, ignoring any existing lock
zlpkg run src/Main.zl    # install if needed, then run with dependencies wired in
```

`zlpkg.toml`:

```toml
[package]
name = "myproject"
version = "0.1.0"

[dependencies]
geom = { path = "../geom" }                                    # local, for dev/fixtures
ecs  = { git = "https://example.com/zl-ecs.git", ref = "v1.0.0" } # ref is a tag/branch/commit; omit for the default branch
```

Package API/versioning convention:

- Every installable package declares `[package] name` and an exact `version` (currently `MAJOR.MINOR.PATCH` is the project convention).
- A dependency may request an exact package version with `version = "..."` inside its inline table. zlpkg verifies that the resolved package manifest declares exactly that version; there is intentionally no version-range or registry resolution yet.
- Package names must agree between the dependency key and the dependency package's own manifest.
- `zlpkg.lock` records the resolved package version in addition to its source/ref, so the dependency API identity is visible in the lockfile.

Each dependency's own `zlpkg.toml` (if it has one) is resolved transitively. If the same dependency name is reached through two different paths in the graph and they resolve to different content (a different commit, or a different local path), `zlpkg` reports a version-conflict error naming both requesters rather than silently picking one. `zlpkg.lock` records the exact resolved commit/path for each dependency and is meant to be committed (like `Cargo.lock`); only the `.zlpkg/` cache directory it populates is gitignored. See `ROADMAP.md`'s Phase 6 section for the full design writeup, and `tests/zl/valid/package_manager_tests/` / `tests/zl/invalid/package_manager_tests/` for worked examples (including a diamond-dependency case).

Dependencies are fetched over git by shelling out to the system `git` CLI - no registry/index exists yet, so every dependency has to say exactly where it comes from (`path` or `git`, not a bare version number).

## Current limitations

- The standard library is intentionally hybrid: high-level APIs live in ZL packages, while the VM, OS, parsing engines, storage primitives, and other performance/security-sensitive operations remain native.
- `zlpkg` has no package registry/index - dependencies must be fetched by `git` URL or local `path`, and `zlpkg.toml` has no workspaces, dev-dependencies, or version-range syntax yet.
- The language currently uses a single-class-per-file convention.
- Lambda expressions (`func(...) => ...` / `func(...) { ... }`) don't have full function-type signature checking yet - a `func`-typed slot's parameter/return types aren't tracked, so calling a closure with the wrong argument count is a runtime error, not a compile-time one. See "Lambda expressions" above.
- Async execution is partially implemented: `async func`, `Task<T>`, and `await` suspension/resumption work; `Task<T>.block()` / `ignore()`, cancellation propagation, unobserved-failure reporting, and async lambda values remain roadmap work.

## Development notes

- The source tree is organized as a standard CMake executable target defined in `CMakeLists.txt`.
- The main executable loads a `.zl` file, tokenizes it, parses it, checks types, compiles bytecode, and runs it in the VM.
- Errors are reported in three categories: `syntax error`, `compile error`, and `runtime error`.

## Testing

There are C++ test files in `tests/` for the lexer, parser, compiler, and VM. If you extend the language, add tests for the affected layer and keep the runnable `.zl` examples in sync.

### Running the examples

`scripts/run_examples.sh` (Linux/macOS) and `scripts/run_examples.bat` (Windows) run the eight small showcase programs under `examples/`. The full permanent regression corpus is under `tests/zl/valid/` and `tests/zl/invalid/`; use `scripts/run_regressions.sh` or `scripts/run_regressions.bat` to discover and run it, including package-manager cases.

```bash
# interactive: pick 1) Test All  2) Invalid  3) Valid, then which categories to run
./scripts/run_examples.sh build/zl_language

# non-interactive
./scripts/run_examples.sh build/zl_language all            # everything
./scripts/run_examples.sh build/zl_language invalid         # every invalid category
./scripts/run_examples.sh build/zl_language valid 1,3       # only categories 1 and 3 under "valid"
./scripts/run_examples.sh build/zl_language --list          # list discovered categories/cases and exit
```

`scripts/run_examples.bat` takes the same arguments (`scripts/run_examples.bat build\zl_language.exe [all|invalid|valid] [selection]`).

## License

No license file is present in the repository yet.


## Math library

The built-in `Math` namespace provides numeric constants and common mathematical
operations. Constants are accessed without parentheses:

```zl
log(Math.PI)
log(Math.E)
log(Math.TAU)
```

Available operations include:

- `sqrt`, `abs`, `pow`, `floor`, `ceil`, `round`, `min`, `max`
- `clamp`, `sign`, `cbrt`, `trunc`
- `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`
- `log`, `log10`, `exp`
- `lerp`, `degrees`, `radians`
- `random`, `randomInt(min, max)`, `randomFloat(min, max)`

Domain errors are reported for `asin`/`acos` outside `[-1, 1]` and `log`/`log10`
for non-positive inputs. `Math.exp` reports finite-input overflow. `Math.clamp`,
`Math.randomInt`, and `Math.randomFloat` reject a minimum greater than the maximum.
The random functions use a process-local pseudorandom generator and are not
cryptographically secure.

The math library uses a hybrid implementation. High-level helpers such as
`clamp`, `sign`, `lerp`, `degrees`, and `radians` are ZL-owned static methods,
while genuinely primitive numeric operations such as `sqrt`, `sin`, `cbrt`, and
random-number generation remain native for performance and platform access.
The public `Math.*` API stays stable across that implementation boundary.


## Static library methods

Classes may declare `static func` members. Static functions are called through
the class name and do not have a `this` receiver:

```zl
class Helpers {
    public static func clamp(double x): double {
        if (x < 0.0) { return 0.0 }
        return x
    }
}

log(Helpers.clamp(-2.0))
```

Static methods are used by the standard library to move high-level policy into
ZL without replacing the small native runtime primitives underneath it. Using
`this` from a static function is a compile-time error.

## Operator overloading

Classes can define operators using the `operator` declaration. Operators use the
normal method dispatch and overload-resolution system. Operators are public by
default, but may be explicitly marked `private` or `protected`.

```zl
class Number {
    public int value

    func Number(int value) {
        this.value = value
    }

    operator +(Number other): Number {
        return new Number(this.value + other.value)
    }

    operator -(): Number {
        return new Number(-this.value)
    }
}
```

The current implementation supports arithmetic, comparison, bitwise, shift,
and unary `+`/`-`/`!`/`~` operator declarations. Built-in operators remain the
fallback for primitive values; object operands use a declared operator when one
is available. Operator overloads are class-aware, so different object parameter
types can coexist, while generic operator parameters retain shared runtime
dispatch and concrete compile-time type checking. Numeric mixed-type overloads,
operator visibility, inheritance, and interface operator contracts are supported.
Unsupported or ambiguous object operators are reported as compile-time errors.

## Record values (`data`)

ZL's `data` declaration defines a named, mutable record with typed public fields:

```zl
data Point {
    x: int
    y: int
}

var a = Point { x: 10, y: 20 }
var b = Point { x: 10, y: 20 }

log(a == b) // true
```

Record literals must provide every declared field exactly once. Unknown, missing, or duplicate fields are compile-time errors. Records of the same `data` type compare structurally, including nested records; numeric fields use the normal ZL numeric equality rules.

`data` records remain value-oriented and immutable after construction. They may define ordinary methods, including `toString()`, but may not reassign their own fields. Record inheritance and destructuring are defined by the current roadmap.

## Reflection / Type metadata

ZL provides a minimal `Type` API for runtime inspection:

```zl
Type.name(value)       // runtime type/class name
Type.fields(object)    // effective field names, including inherited fields
Type.methods(object)   // effective method names, including inherited methods
Type.base(object)      // direct base class name, or nil for a root class
```

Reflection metadata is intentionally small and name-based. Generic type arguments,
method signatures, annotations, and writable reflection are reserved for future
phases.


### ZL-owned collection algorithms

`List<T>` keeps its VM storage primitives native but implements higher-level algorithms in ZL:

```zl
var values = new List<int>()
values.push(4)
values.push(1)
values.push(3)

var doubled = values.transform(func(x) => x * 2)
var total = values.reduce(func(a, b) => a + b, 0)
values.sort(func(a, b) => a < b)
```

Also available are `contains`, `indexOf`, `count`, `any`, `all`, `filter`,
`forEach`, and `reversed`. The implementation deliberately uses the existing
`Collection.*` native storage boundary rather than adding algorithm-specific C++ code.

## Standard library expansion

Phase 15 adds user-facing packages for testing, logging, text processing, serialization, time, and hashing. The public APIs live in ZL packages while low-level operations remain small native primitives.

### `zl.test`

```zl
import zl.test.Test
import zl.test.Runner

class ExampleTests {
    func main(): void {
        var tester = new Test()
        tester.assertThrows(func() { throw "expected" })

        var runner = new Runner("example")
        runner.run("truth", func() { Test.assertTrue(true) })
        runner.run("math", func() { Test.assertEqual(2 + 2, 4) })
        Test.assertEqual(runner.report(), 0)
    }
}
```

Available assertions:

- `Test.assertTrue(value)`
- `Test.assertFalse(value)`
- `Test.assertEqual(actual, expected)`
- `Test.assertNotEqual(actual, expected)`
- `Test.assertNear(actual, expected, epsilon)`
- `Test.fail(message)`
- `Test.assertThrows(action)`

`Runner` provides ZL-native grouping, pass/fail accounting, and reporting.

### `zl.logging`

```zl
import zl.logging.Log

Log.info("server started")
Log.warn("cache miss")
Log.error("request failed")
```

`zl.logging` is used instead of `zl.log` because `log` is a reserved language keyword.

### `zl.text`

```zl
import zl.text.Text

Text.upper("zl")
Text.startsWith("hello", "he")
Text.endsWith("hello", "lo")
Text.join(new List<string>(), ",")

Text.format("hello {0}", {"ZL"})
Text.regexMatches("abc123", "[0-9]+")
Text.regexFindAll("a12 b34", "[0-9]+")
Text.regexReplace("a1b2", "[0-9]", "X")
```

The convenience/composition methods are ZL-owned. `String.*` remains the low-level native string primitive boundary, while regex and formatting engines remain native where appropriate.

### `zl.serialize`

```zl
import zl.serialize.Serialize

var json = Serialize.stringify({"name": "ZL", "version": 15})
var value = Serialize.parse(json)
var answer = Serialize.number(42)
```

`encode`/`decode` and the low-level typed conversion primitives remain available
behind the facade. The JSON parser/encoder itself remains native.

The JSON implementation supports null, booleans, numbers, strings, arrays/lists, and string-keyed objects/maps.

### `zl.time`

The package provides `Date`, `DateTime`, `TimeOfDay`, and `Duration` value objects over the native clock primitives. Higher-level operations such as `Time.nowDateTime()`, `Time.today()`, `Date.addDays()`, `DateTime.addHours()`, and `DateTime.difference()` are implemented in ZL.

### `zl.crypto`

```zl
import zl.crypto.Crypto

Crypto.sha256("abc")
Crypto.crc32("123456789")
```

SHA-256 and CRC32 are native primitives. ZL owns the public package API; cryptographic primitives are deliberately kept native for correctness and performance.

## Native boundary and Phase 16

Phase 16 establishes a permanent design rule: **C++ is the small, stable native foundation; ZL owns high-level library policy and composition.** See `docs/architecture/NATIVE_BOUNDARY.md` for the current inventory, benchmark gate, and the list of deliberately native primitives.

Future C++ -> ZL migrations must be benchmarked before a performance-sensitive native implementation is removed.

### Phase 17 binding safety

`zl-bind` class bindings now use smart-pointer-backed native ownership and translate C++ exceptions into deterministic ZL runtime errors. Shared bindings can retain an opaque handle without exposing C++ pointer types to ZL.


## Native compilation (Phase 10)

Selected functions can opt into the restricted native compiler with `@native`. The compiler preserves the normal VM path as the fallback and can emit MIR-native portable C++ with:

```text
zl --emit-native output.cpp source.zl
```

The current native tier supports primitive `int`, `double`/`decimal`, and `bool` values, initialized primitive locals, assignments, integer ranges, boolean branches, arithmetic/bitwise expressions, bounded constant-loop unrolling, and safe integer constant folding. Generated modules expose the stable primitive ABI in `include/zl/compiler/native_abi.hpp`.

`Task<T>`, closures, heap objects, and VM reflection objects do not cross the native ABI. Unsupported native signatures are rejected deterministically; the ordinary VM remains authoritative for those cases. Run `scripts/native_gate.sh` on Linux/macOS or `scripts/native_gate.ps1` on Windows to exercise the native compiler, ABI regression tests, generated C++ compilation, and benchmark semantic gate.

## Native bindings (`zl-bind`)

Phase 17's `zl-bind` tool generates controlled native bindings, ZL facades, manifests, and API documentation from a restricted C/C++ declaration subset. Generated manifests carry target ABI metadata and generated C++ contains compile-time ABI guards. Unsupported C++ constructs are rejected rather than guessed.

The binding tool is deliberately conservative: C++ remains a small native foundation, while high-level library behavior stays in ZL.


## Distribution builds

Release packaging is driven by CMake/CPack. See `packaging/README.md`.

On Linux, the CMake/CPack release configuration produces native TGZ/ZIP/DEB packages. Windows and macOS releases should be built on native runners using the same CMake/CPack configuration; the repository includes a GitHub Actions release matrix for those builds.


### Async native operations

The runtime supports native asynchronous operations that return `Task<T>` and complete from worker threads. The reference operation is `Time.sleepAsync(milliseconds)`, which can be awaited from `async func` without blocking the VM scheduler.

### Concurrency
`Shared<T>` is now available as an explicit generic wrapper. Construct with `new Shared<T>(value)` and access with `get()` / `setValue()`. CPU-worker and raw-thread closures may carry only `Shared`-wrapped object captures; ordinary captures remain rejected. `Shared<T>` does not imply thread safety. The top-level `share()` helper and compile-time confinement checks remain pending.

## Current development checkpoint — 2026-08-30

Phase 8 record-method slice is implemented. `data` declarations support public instance `func` methods, record fields are immutable after construction, and record methods use the ordinary dispatch and reflection infrastructure. Focused valid/invalid regressions are under `tests/zl/valid/records/` and `tests/zl/invalid/records/`.


## 2026-08-30 Phase 9 continuation

Static fields now support inherited qualified access. A derived class resolves an inherited static field to its declaring class, so reads and writes use the same backing storage. Access modifiers are enforced against the declaring owner; protected inherited access works from subclasses and private inherited access is rejected. Interface-qualified static-field access is explicitly rejected because interfaces do not own static field storage.

Verification: inherited public/protected access, shared storage, interface/class coexistence, private inherited rejection, and interface-qualified rejection were exercised. Existing lazy/re-entrant static behavior remains intact; the concurrent static fixture passes under a 20-second direct run.


### Standard library and package ecosystem

Phase 11 is complete. The standard library includes reference queue/stack utilities, text/time helpers, task/channel/thread facades, serialization, filesystem, and a small portable DNS network facade. `zlpkg` supports version-checked path dependencies and records resolved package versions in `zlpkg.lock`.

## Design documents

The long-term architecture is documented under `docs/design/`. Start with `docs/design/ZL_VISION.md` and `docs/architecture/ZL_MEMORY_MODEL.md`.

## 2026-09-04 Phase 17 native runtime integration and optimization groundwork

Completed the next two Phase 17 objectives. A process-local NativeExportRegistry now
accepts generated export tables, rejects malformed/duplicate registrations, resolves exports
by name, and routes name-based calls through the existing NativeError boundary. This provides
the runtime registration bridge required by generated native modules without embedding raw
function pointers in ZL values.

The compiler IR also has a backend-neutral optimization pass. It removes unreachable blocks
and folds safe primitive integer/boolean constant expressions while leaving ownership/borrow
instructions untouched. MIR native emission runs this pass before lowering, so optimized IR
feeds both native output and future VM/backend consumers.

Validation: `zl-ir-tests`, `zl-native-runtime-registry-tests`, and `zl-native-compiler-tests`
pass. The emitted MIR-native constant-folding path is covered, as is direct C++ compilation of
the generated native source.


### Phase 16 completion (2026-09-04)
The ownership-aware FFI foundation is complete: opaque handles, borrowed buffers and struct views, callback lifetime contracts, native resource adapters, dynamic library loading, and validated @ffi(symbol)/@ffi(library, symbol) metadata are implemented and covered by focused tests. Typed field-by-field C struct schemas remain a later ABI extension.
