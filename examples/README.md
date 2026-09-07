# ZL Examples

Runnable, tour-style examples covering the language from first `log` to threads
and async. 46 programs, each verified against its own expected output. Every file is a complete program you can run on its own, and every
file ends with an `Expected output` block that the test runner diffs against the
program's real stdout.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
examples/run_all.sh                      # or examples\run_all.bat on Windows
```

```text
46/46 PASS
```

Run one example directly with the runtime:

```bash
build/zl_language examples/basics/HelloWorld.zl
```

Files under a `_lib` directory are **not** examples - they are importable module
sources (an interface, a record, a helper class) that have no `main`. The runner
skips them and passes each `_lib` directory to the runtime with `--root` so the
examples that `import` them resolve.

## Basics

| Example | Covers |
| --- | --- |
| [HelloWorld.zl](basics/HelloWorld.zl) | The one-class-per-file rule, `main`, `log` |
| [Variables.zl](basics/Variables.zl) | `var` vs `let`, inference and explicit types |
| [DataTypes.zl](basics/DataTypes.zl) | `int`, `double`, `bool`, `string`, integer division, double formatting |
| [Operators.zl](basics/Operators.zl) | Arithmetic, comparison, logical, bitwise, shift, concatenation |
| [Strings.zl](basics/Strings.zl) | The `String.*` primitives, end-exclusive `substring` |
| [Conditions.zl](basics/Conditions.zl) | `if` / `elif` / `else` |
| [Loops.zl](basics/Loops.zl) | Range `for`, `step`, `while`, `repeat ... while`, `break`, `continue` |
| [Functions.zl](basics/Functions.zl) | Parameters, returns, `static`, recursion |
| [Arrays.zl](basics/Arrays.zl) | Native `list<T>` and the `Collection.*` primitives |
| [Lists.zl](basics/Lists.zl) | The generic `List<T>` class and its storage API |
| [Maps.zl](basics/Maps.zl) | `Map<K,V>` and its insertion-order guarantee |
| [Sets.zl](basics/Sets.zl) | `Set<T>` and de-duplication |

## Intermediate

| Example | Covers |
| --- | --- |
| [Classes.zl](intermediate/Classes.zl) | Fields, constructors, methods, `this` |
| [Inheritance.zl](intermediate/Inheritance.zl) | `extends`, `super`, `@Override`, virtual dispatch |
| [Interfaces.zl](intermediate/Interfaces.zl) | `interface`, `implements`, interface-typed variables |
| [Encapsulation.zl](intermediate/Encapsulation.zl) | `public` / `private` / `protected`, invariants |
| [StaticMembers.zl](intermediate/StaticMembers.zl) | `static func`, static fields, why `this` is banned |
| [DataRecords.zl](intermediate/DataRecords.zl) | `data` records, structural equality, nesting |
| [Enums.zl](intermediate/Enums.zl) | `enum`, static typing of members |
| [OperatorOverloading.zl](intermediate/OperatorOverloading.zl) | `operator +`, `operator ==`, `toString` |
| [Lambdas.zl](intermediate/Lambdas.zl) | Expression and block bodies, `func` as a type |
| [Closures.zl](intermediate/Closures.zl) | By-value capture, independent snapshots, the counter pattern |
| [Generics.zl](intermediate/Generics.zl) | User-defined `class Box<T>`, instantiation, current limits |
| [NestedGenerics.zl](intermediate/NestedGenerics.zl) | `List<List<int>>`, `Map<K, List<V>>`, three levels deep, `>>` still shifts |
| [MatchExpressions.zl](intermediate/MatchExpressions.zl) | `match` arms, `when` guards, variable bindings, exhaustiveness rules |
| [CollectionAlgorithms.zl](intermediate/CollectionAlgorithms.zl) | `transform`, `filter`, `reduce`, `sort`, `forEach`, `any`/`all` |
| [Exceptions.zl](intermediate/Exceptions.zl) | `throw`, Java-style `catch`, exception subclasses |
| [Modules.zl](intermediate/Modules.zl) | Dotted imports, the auto-imported `zl.lang`, local modules |
| [Reflection.zl](intermediate/Reflection.zl) | `Type.of`, fields, methods, access levels |

## Advanced

| Example | Covers |
| --- | --- |
| [OptionType.zl](advanced/OptionType.zl) | `Option<T>`, `Some` / `None`, `unwrapOr`, `unwrapOrElse` |
| [ResultType.zl](advanced/ResultType.zl) | `Result<T,E>`, `Ok` / `Err`, error-carrying failures |
| [RegexBasics.zl](advanced/RegexBasics.zl) | `Text.regex*`, capture groups, search vs anchored match |
| [RegexBuilder.zl](advanced/RegexBuilder.zl) | The fluent `Regex` builder, `source()`, named groups |
| [AsyncTasks.zl](advanced/AsyncTasks.zl) | `async func main`, `Task<T>`, nested `await` |
| [TaskBlocking.zl](advanced/TaskBlocking.zl) | Collecting a task with `.block()` from sync code |
| [Threads.zl](advanced/Threads.zl) | `Thread.start` / `join`, the capture rule |
| [SharedState.zl](advanced/SharedState.zl) | `Shared<T>`, and why it is not thread safety |
| [MutexLocks.zl](advanced/MutexLocks.zl) | `Mutex.withLock` and its current caveats |
| [Atomics.zl](advanced/Atomics.zl) | `Atomic` counters, a correct 2-thread increment |
| [Channels.zl](advanced/Channels.zl) | Bounded channels, producer/consumer across threads |
| [MathLib.zl](advanced/MathLib.zl) | `Math` constants, functions, domain-error behaviour |
| [TextLib.zl](advanced/TextLib.zl) | `zl.text.Text`, padding, joining, `format` |
| [JsonLib.zl](advanced/JsonLib.zl) | `Serialize.stringify` / `parse`, round-tripping |
| [LoggingLib.zl](advanced/LoggingLib.zl) | `Log.info` / `warn` / `error` |
| [TestingLib.zl](advanced/TestingLib.zl) | `zl.test` assertions and the `Runner` |
| [TimeLib.zl](advanced/TimeLib.zl) | `Time.*` clock primitives, `Duration`, two known traps |
| [CryptoLib.zl](advanced/CryptoLib.zl) | `Crypto.sha256`, `Crypto.crc32` |
| [FileSystemLib.zl](advanced/FileSystemLib.zl) | Reading, writing, appending, deleting files |

## Reading the expected-output blocks

Each example ends with:

```zl
// --- Expected output (verified by examples/run_all.sh) ---
// Hello, World!
// --- End expected output ---
```

`run_all.sh` strips the `// ` prefix, runs the program, and compares the two
byte-for-byte. A mismatch prints a diff and fails the run, so a change anywhere
in the compiler, VM, or standard library that alters observable behaviour shows
up here immediately.

Two formatting details to know before you read a diff:

- **Doubles print at full round-trip precision.** `log(3.14)` prints
  `3.1400000000000001`, and `Math.PI` prints `3.1415926535897931`. This is the
  shortest decimal that reads back as the same binary double, not a rounding
  bug - but it is surprising the first time you see it.
- **`bool` prints lowercase**, as `true` / `false`.
