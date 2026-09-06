# Review: what writing the examples turned up

Writing 46 runnable examples means running every code path a new user runs, in
the order they run it. That surfaced four bugs worth fixing outright - one of
them severe enough to break the language's headline feature - plus a set of
smaller ones that are recorded but left alone.

Everything below was reproduced against a build of this tree. Each entry says
what it is, how to see it, and whether it is fixed here or still open.

## Fixed in this branch

### 1. Every generic in the language was unusable at runtime

**This was the big one.** Any method or constructor with a type-parameter-typed
argument threw on the first call:

```text
var nums = new List<int>()
nums.push(4)
-> runtime error: type assertion failed for argument 1: expected T, got int
```

It was not specific to `List`. The same failure hit `Map.put`, `Set.add`,
`Shared`, `Some`/`None`, `Ok`/`Err`, and any user-defined `class Box<T>`.
`List<int>` and `Map<K,V>` are the documented centrepiece of the language
(`docs/language-guide.md`, "Generic collections" and "Algorithms"), so the
central examples in the reference did not run at all.

**Cause.** The runtime type assertions added in `fe91b12` take their expected
type from `FunctionInfo::parameterTypeNames` and `returnTypeName`, which
`Compiler::compile` filled from `describeTypeAnnotation(...)` - the *source*
annotation. For a generic method that is the bare type parameter, so the VM
compared an `int` argument against the literal name `"T"`. The guard at
`src/vm/vm.cpp:1101` skips `""` and `"unknown"` but not an unsubstituted type
parameter.

**Fix.** `src/compiler/compiler.cpp` now erases type parameters before they
reach an assertion, at all four sites that produce one (pass-1 parameter names,
pass-1 return type, pass-2 `currentReturnTypeName_`, and the shared helper).
Erasure is token-aware, so it covers the bare case (`T`) and the nested ones
(`List<T>`, `func():T`) - the latter mattered because `Shared.withLock` and
`List.extend` take exactly those.

Verified: `examples/basics/Lists.zl`, `Maps.zl`, `Sets.zl`,
`intermediate/Generics.zl`, `advanced/OptionType.zl`, `ResultType.zl`,
`SharedState.zl` all run.

### 2. A block-body lambda was checked against the enclosing function's return type

The counter pattern documented in `docs/language-guide.md` failed on first call:

```zl
static func makeCounter(): func {
    var count = 0
    var inc = func() { count = count + 1
        return count }
    return inc
}
var c = makeCounter()
log(c())          // -> type assertion failed: expected func, got int
```

`Compiler::compileLambdaExpr` never saved or restored `currentReturnTypeName_`,
so an explicit `return` inside the lambda emitted `AssertType("func")` - the
*factory's* return type. Expression-body lambdas were unaffected because they
emit `Return` directly, which is why this looked intermittent.

**Fix.** `compileLambdaExpr` scopes `currentReturnTypeName_` to the lambda's own
inferred return type. `examples/intermediate/Closures.zl` now prints `1 2 3`.

### 3. `await` at the top level was a heap-use-after-free

`async func main` that awaited never resumed, and under AddressSanitizer it was
a write-after-free, reproduced 3/3:

```text
ERROR: AddressSanitizer: heap-use-after-free ... thread T1
  #6 zl::RuntimeScheduler::enqueue(...)   src/vm/runtime_scheduler.cpp:28
  #7 operator()                           src/vm/vm.cpp:1059
  #12 zl::RuntimeTaskState::succeed(...)  src/vm/runtime_task.cpp:86
  #13 operator()                          src/vm/native.cpp:778   (timeSleepAsync)
freed by thread T0 here:
  #7 zl::RuntimeScheduler::~RuntimeScheduler()
  #8 zl::VM::~VM()                        src/vm/vm.cpp:104
  #9 main                                 src/main.cpp:341
```

Two separate causes:

- **Lifetime.** `VM` held its scheduler by value and handed child VMs a raw
  pointer to it. A child created for an async invocation is kept alive by the
  task continuation and so outlives the root VM; when a worker then called
  `enqueue`, the scheduler was already freed. `RuntimeScheduler` is now owned by
  `shared_ptr` and co-owned by every VM that can enqueue into it.
- **Nothing drove the scheduler.** `VM::run` called `runUntilIdle()`, which only
  drains frames that are ready *right now*. A native async op completes on a
  worker thread later, so `main` returned and the process exited before the
  continuation was ever enqueued - which is why the failure looked like a silent
  exit on a normal build and only showed as a crash under ASan. `VM::run` now
  remembers the Task produced by a top-level async call and pumps until it
  settles, the same way `TaskBlock` does.

Verified: `async func main` with nested awaits prints its full output 3/3 and is
clean under ASan. Covered by `examples/advanced/AsyncTasks.zl`.

Separately, `docs` lists `block()`/`ignore()` as pending. `block()` works, and
`examples/advanced/TaskBlocking.zl` documents it as the way to collect a task
from a synchronous `main`.

### 4. Enum values could not be passed to a function typed with the enum

```zl
enum Color { Red, Green }
static func describe(Color c): string { ... }
describe(Color.Red)
-> runtime error: type assertion failed for argument 1: expected Color, got string
```

An enum member is stored as its name string, so the runtime type name is
`"string"` while the declared type is `"Color"`. The compile-time check is
correct and still rejects a raw string; only the runtime assertion was wrong.

**Fix.** `runtimeAssignableToType` in `src/vm/vm.cpp` now consults
`chunk.classReflection[...].isEnumType` and validates against `enumMembers`.
Verified by `examples/intermediate/Enums.zl`.

### 5. A thread closure that referenced nothing captured the entire scope

```zl
var n = 5                                    // never used by the closure
var t = Thread.start(func() { log("hi") })
-> runtime error: Thread.start: captured values must be Shared<T> when crossing
   raw threads
```

The compile-time check used the accurate reference list, but `MakeClosure` had a
fallback that snapshotted the whole scope whenever `captureNames` was empty.
Since `TypeChecker::analyzeLambdaCaptures` populates that list for *every*
lambda, empty genuinely means "references nothing" - so the fallback captured
every local in the enclosing function, including the handle of a thread you had
already started. That is why a second `Thread.start` never worked. The fallback
is gone.

### 6. Capture analysis missed the callee of a call

Removing that fallback exposed a second bug: `collectThreadRefs` walked a
`CallExpr`'s arguments but not its callee, because `CallExpr` stores the callee
as a *name* rather than a child node. So in

```zl
static func compose(func f, func g): func {
    return func(x) => f(g(x))
}
```

neither `f` nor `g` was recorded as a capture. The whole-scope snapshot had been
hiding it; with accurate capture the lambda body failed at runtime with
`undefined variable 'g'`. The callee is now included. `Lambdas.zl` covers it.

### 7. `Option<int>` and `Result<T,E>` could not be a declared type

```zl
static func find(): Option<int> { return new Some<int>(5) }
-> runtime error: type assertion failed: expected Option<int>, got Some
```

`reflectiveObjectMatches` deliberately preserves concrete instantiated identity,
so `Base<int>` stays distinguishable from `Base<string>`. The rule was too
strict: it also rejected the ordinary `Some<int>` -> `Option<int>` widening that
`class Some<T> extends Option<T>` exists to provide.

It now accepts that widening when the object's own instantiation carries the
same type arguments as the expected type, so `Some<int>` satisfies `Option<int>`
while `Some<string>` still does not. Mismatched instantiations were already
caught at compile time - `GB<string>` into a `GB<int>` parameter is rejected
before the runtime check runs - and both directions were re-verified.

`OptionType.zl` and `ResultType.zl` now use the idiomatic shape: a function that
returns an Option, passed to a function that takes one.

### 8. A closure nested inside a closure could not see the outer closure's captures

```zl
var n = 7
var outer = func() {
    var inner = func() { return n }
    return inner()
}
log("got " + outer())
-> runtime error: undefined variable 'n'
```

This one is a regression that fix 5 introduced, and it only showed up after fix
6. Both fixes made `captureNames` more accurate, and accuracy removed the
accidental safety net: `MakeClosure` used to snapshot the *entire* enclosing
scope whenever `captureNames` was empty, so an outer lambda carried every local
in `main` whether it named them or not, and the inner lambda could find `n`
among them. Once the outer lambda's capture list became accurate it stopped
carrying `n`, and the inner lambda had nothing to capture it from.

**Cause.** `collectThreadRefs` (`src/compiler/type_checker.cpp`) walks a lambda
body to build its capture list, but its `switch` had no `LambdaExpr` case, so it
hit `default: break` and never descended into a nested lambda. Names referenced
only inside the inner lambda were therefore never recorded as captures of the
outer one.

**Fix.** `collectThreadRefs` now recurses into a nested `LambdaExpr`, threading
the inner lambda's own parameter names through the shadowing set so an inner
parameter is not recorded as an outer capture.

Verified against upstream `fe91b12` built side by side: the snippet above prints
`got 7` on both. `MutexLocks.zl` and `Channels.zl` exercise the threaded form.

### 9. Nested generic type arguments could not be written

```zl
var grid = new List<List<int>>()
-> syntax error: Expected '>' to close type argument list -- got ">>"
```

The same happened for `Map<string, List<int>>`, `Box<List<int>>`, and anything
else that closed two type argument lists in a row. Adding a space -
`new List<List<int> >` - parsed fine, which is what gave the cause away.

**Cause.** The lexer runs without context, so the trailing `>>` becomes a single
`SHR` token, and all ten places in the parser that close a type construct did a
plain `expect(TokenType::GT, ...)`. This is the classic C++ `>>` problem.

**Fix.** `Parser::expectTypeAngleClose` (`src/parser/parser.cpp`) accepts `GT`,
or splits a fused `SHR`/`USHR` back into the `>` characters it was made of,
consuming exactly one and leaving the rest in the stream for the enclosing list.
All ten call sites in `parser.cpp` and `expression_parser.cpp` now go through it.

Shift expressions are untouched, because they are never parsed by these paths -
`32 >> 2` is `8`, `64 >> 2 >> 1` is `8`, `256 >>> 4` is `16`.
`intermediate/NestedGenerics.zl` covers both halves.

### 10. Any zero-argument method named `get` was treated as a `Shared` payload read

```zl
class Holder<T> {
    public T item
    public func get(): T { return this.item }
}
var h = new Holder<List<int>>(new List<int>())
log("len " + h.get().length())
-> compile error: protected Shared payload access requires Shared.withLock on 'h'
```

`h` is not a `Shared` and there is no payload to protect. Renaming the method to
`fetch` or `value` made the identical program compile, which pinned it to the
name.

**Cause.** `sharedPayloadOwnerForExpr` recognised a payload read purely by shape
- a zero-argument method called `get` - and then resolved its receiver through
`resolveSharedCellAlias`, which answers for *any* identifier by returning it
unchanged. So every `x.get()` counted as a protected `Shared` cell.

Fixing that alone made the guard fire on nothing at all, which exposed a second,
opposite bug: a cell built directly by `new Shared<T>(...)` was never registered.
Registration only happened for `var alias = existingCell`, because
`resolveSharedCellAlias` returns `nullopt` for a `NewExpr`. The guard therefore
only ever worked by accident, on copies.

**Fix.** Three changes in `src/compiler/type_checker.cpp`:

- `trackedSharedCellForExpr` only treats `x.get()` as a payload read when `x` is
  actually a tracked cell.
- `var c = new Shared<T>(...)` now registers `c` as its own canonical cell.
- `resolveSharedCellAlias` treats a self-edge as canonical rather than as a
  cycle, so the registration above resolves instead of failing the loop guard.

Verified both directions. Still rejected, as intended:

| Guarded | |
| --- | --- |
| `c.get().length()` on a direct cell | error |
| `alias.get().length()` on a copy | error |
| `var items = c.get()` then `items.length()` | error |
| `var items = c.get()` then `items[0]` | error |

Allowed: `c.get() + 1` on a `Shared<int>` (no member access), any payload access
inside `withLock`, a plain class's `get()`, and `Map.get(key)` (takes an
argument, so it was never matched).

### 11. `Task<T>` lost its type argument when read out of a `List` by index

```zl
var ts = new List<Task<int>>()
ts.push(inst.work(3))
log("v " + ts[0].block())
-> compile error: type error: type 'Task' has no method 'block'

log("v " + ts.get(0).block())   // the same thing, and this one worked
```

So the two equivalent reads of the same element disagreed, which made it
impossible to fan tasks out into a collection and collect the results - the
natural way to write concurrent code.

**Cause.** `TypeChecker::inferIndexAccess` derived the element type from the
`List<...>` class name with a hardcoded list that only covered the four
primitives (`int`, `double`, `string`, `bool`) and sent everything else to
`ZlType::OBJECT`. `Task` is its own type kind, not an object whose class name
happens to be `"Task<int>"`, so `block` could not be resolved on it. The other
element-type mappers in the same file already handled `Task`, `func`, `nil`,
`list`, `map`, `set` and `array`; this one had drifted.

**Fix.** `inferIndexAccess` now uses the same canonical mapping. A subtlety the
first attempt got wrong: the mapping matches on the *base* name (`list`, not
`List`), so a class element must keep its full instantiation as its class name -
otherwise `List<List<int>>` indexed once became plain `List` and the next index
failed with "indexed access requires a List<T>". `intermediate/NestedGenerics.zl`
caught that immediately.

Verified: `List<Task<int>>[i].block()`, `List<Task<string>>[i].block()`, a 50-task
fan-out summed through indices (`sum 1225`), and no change to `List<int>`,
`List<List<int>>` or `List<Shared<int>>`.

## Still open

Ranked by how quickly a new user hits them. Entries marked **(fixed)** no longer
reproduce; they are kept here because the workaround is still baked into an
example or a doc, and the context explains why.

### O2 - A `func`-typed lambda parameter cannot be called

```zl
var apply = func(f) => f(21)
-> compile error: type error (line 3): undefined func 'f'
```

The same thing written as a *named* function works fine, so the gap is specific
to lambda parameters:

```zl
static func namedCall(func f): int { return f(21) }
namedCall(func(x) => x * 2)      // 42
```

Verified to be independent of the capture fixes above - it reproduces with and
without them. This is the "`func`-typed slots don't carry full signature types"
limitation already listed in the README, showing up in a second place.

### O3 - `Atomic`, `Mutex` and `Channel` could not be captured by a thread **(fixed)**

The rejection message said "use Atomic or Mutex for mutable shared state", but
the capture check whitelisted `Shared<T>` only, so it rejected the very things it
recommended:

```zl
var a = new Atomic()
var t = Thread.start(func() { Atomic.add(a, 1) })
-> compile error: Thread.start cannot capture 'a' across a thread boundary;
   use Atomic or Mutex for mutable shared state
```

The whitelist existed in three places that had drifted: `isExplicitlySharedValue`
in `src/vm/native.cpp`, `TypeChecker::validateThreadLambda`, and a further inline
`Shared`-only test at each of the two call sites in `type_checker.cpp`
(`Task.spawn` and `Thread.start`). Fixing only the first two let `Atomic` and
`Channel` through while `Mutex` was still rejected by the third with a different
message. All three now share one list - `Shared<T>`, `Atomic`, `Mutex`, `RwLock`,
`Semaphore`, `Channel`, `Condition` - kept in step between
`isThreadSafeClassName` in `native.cpp` and
`capturedValueCrossesThreadBoundary` in `type_checker.cpp`.

Each was then verified end to end from a worker thread:

| Captured value | Verified |
| --- | --- |
| `Atomic` | `atomic total 2000`, 3/3 runs (`Atomics.zl`) |
| `Channel` | `received from-worker` (`Channels.zl`) |
| `Semaphore` | `available 2` / `count 2` |
| `Mutex` | `guarded total 100`, 5/5 runs at 2x50 (`MutexLocks.zl`) |
| `RwLock` | `read 1000` under `withWrite`/`withRead` |

`Condition` is on the list but has not been exercised from a worker thread -
unchecked.

`Atomics.zl`, `Channels.zl` and `MutexLocks.zl` no longer need the
`new Shared<Atomic>(...)` / `new Shared<Channel>(...)` handle wrappers they used
as a workaround.

Also worth noting: `stdlib/zl/lang/Atomic.zl` and `Mutex.zl` are empty class
bodies. Their entire API is native statics, so there is no instance surface at
all.

### O4 - Locked critical sections deadlock under contention

2 x 50 locked increments completes and prints `total=100`. 2 x 100 hangs
forever with no output, 4/4 runs. Both threads are joined by `main`, so it
presents as a hang rather than a crash.

It is not specific to `Shared<T>`. `Mutex.withLock` deadlocks at the same
threshold (2 x 100 hangs, exit 124) while `Shared.withLock` at 2 x 50 is stable
5/5, so the fault is in the locked-closure path they share, not in either
wrapper.

### O5 - An exception escaping `Mutex.withLock` corrupts the continuation

```zl
try {
    Mutex.withLock(m, func() => throwSomething())
} catch Exception e { log("caught") }
```

prints the catch, then runs the rest of `main` **twice**, then dies with
`'return' used outside of a func`. A plain `1 / 0` in the same position escapes
the `try` entirely. The lock's value is in the closure, so keep bodies
non-throwing until this is fixed.

### O6 - A generic collection loses its instantiated identity

A list built inside a generic method - the result of `transform`, `filter`, or
`reversed` - is erased to `List`, so it cannot be passed to a `List<int>`
parameter:

```zl
static func total(List<int> xs): int { ... }
total(nums)                          // fine
total(nums.transform(func(x) => x * 2))
-> runtime error: type assertion failed for argument 1: expected List<int>, got List
```

This one is *not* fixed by the widening above, and the reason is worth stating:
a list built inside a generic method body does not know its own instantiation.
`transform` runs as `List<T>`, so the object it constructs records `T` as its
type argument, not `int` - there is nothing to compare against `List<int>`.
Fixing it means threading the concrete instantiation through generic method
bodies, which is a real piece of work rather than a check to relax.

`CollectionAlgorithms.zl` reads such lists through their own methods instead of
passing them on.

The same erasure also breaks the **typed collection literals** documented in
`docs/language-guide.md`:

```zl
List<int> nums = [4, 5, 6]
-> runtime error: type assertion failed: expected List<int>, got List

List<string> names = ["Ada", "Grace"]
-> runtime error: type assertion failed: expected List<string>, got List
```

The literal builds an erased `List`, which the typed declaration then asserts
against `List<int>`. The lowercase native form is unaffected and works:

```zl
list<int> nums = [1, 2, 3]                 // fine
var nums = new List<int>()                 // fine
```

This is why the README's headline snippet did not run, and why `Arrays.zl` and
`Lists.zl` use `new List<T>()` plus `push` rather than a typed literal.

### O8 - A static method taking an interface-typed parameter does not compile

```zl
static func describe(Shape s): string { return s.name() }
-> runtime error: Compiler: unknown library func 'R3.describe'
```

An interface-typed *variable* works, so `Interfaces.zl` uses one.

Re-confirmed on a minimal case: `static func describe(Shape s): double { return
s.area() }` with `Circle implements Shape`. Calling `describe` directly on the
instance works (`new Circle(2.0).area()` is fine); only the static entry point
fails.

The failure is late - it is a `Compiler:` error at emit time, not a type error.
`Compiler::compileCall` resolves `Namespace.callee` by scanning
`chunk_.functions` for a static whose `dispatchSignature` equals the call site's
`node->resolvedDispatch`, and only falls through to the native table if nothing
matches (`src/compiler/compiler.cpp:1336-1351`). So the two signatures disagree
for an interface-typed parameter, the static is never found, and the lookup ends
up asking the native table for `R3.describe`. Which side of the comparison is
wrong is not yet pinned down.

### O9 - `Time.nowMillis()` recursed until the stack ran out (fixed)

`stdlib/zl/time/Time.zl` declared
`public static func nowMillis(): int { return Time.nowMillis() }`, which shadowed
the native of the same name and called itself:

```text
runtime error: stack overflow: maximum call depth (100000) exceeded
```

The wrapper was redundant - the native is already reachable as `Time.nowMillis` -
so it has been deleted and the call now resolves to the native.

### O10 - `Time.format` uses its own tokens, and nothing documents them

I originally recorded this as "format does not expand its specifiers". That was
wrong, and worth keeping as a caution: `Time.format(now, "%Y-%m-%d")` returns the
format string unchanged because it recognises `YYYY`, `MM`, `DD`, `HH`, `mm`,
`ss` - not strftime. Given the right tokens it works:

```zl
Time.format(now, "YYYY-MM-DD HH:mm:ss")   // 2026-09-06 16:32:44
```

The real problem is that no document mentions the token set, so `%Y` is the
obvious thing to reach for and fails silently - an unrecognised token is left in
place rather than reported. `Time.format` is now documented in
`docs/stdlib.md`, and `TimeLib.zl` uses the correct tokens.

### O11 - `Serialize.stringify` on a generic `Map` encoded internal storage (fixed)

```zl
var m = new Map<string, string>()
m.put("a", "b")
Serialize.stringify(m)      // was: {"__native":{"a":"b"}}
```

The generic collection classes are thin wrappers whose only field is the native
storage they delegate to, and the encoder walked object fields blindly. It now
looks through `List`/`Map`/`Set` wrappers and encodes the payload, so both
collection spellings produce the same JSON. See `JsonLib.zl`.

### O12 - `Math.sqrt(-1.0)` returns `nan` instead of throwing

`docs/stdlib.md` says domain errors are reported. They are for `log`/`log10`,
`asin`/`acos`, and out-of-order `clamp`/`random` ranges - but `sqrt` returns
`nan`. `MathLib.zl` shows both behaviours.

### O13 - The README's headline example did not run (fixed here)

```zl
var names = ["Ada", "Grace", "Alan"]
names.forEach(func(n) => log("Hello, " + n))
-> compile error: cannot call method 'forEach' on value of type list
```

A list literal infers the native `list`, which has no methods; `forEach` lives on
the generic `List<T>`. Annotating does not rescue it either, because of the typed
literal problem in O6 - `List<string> names = [...]` fails at runtime.

The README snippet now uses `new List<string>()` with `push`, which runs, and the
stale `examples/Hello.zl` path in the same file now points at
`examples/basics/HelloWorld.zl`.

### O14 - `docs/language-guide.md` shows a semicolon that is not valid syntax

The closures section contains `func() { count = count + 1; return count }`.
`;` is not accepted anywhere - not as a separator, not even as a trailing
terminator:

```text
var a = 1;
-> syntax error: Expected expression -- got ";" at line 3
```

### O15 - `docs/language-guide.md` implies `count` is a predicate

`count` is listed among the lambda-taking algorithms. It is
`count(T item)` and returns the number of occurrences of a value; the predicate
versions are `any`, `all`, and `filter`.

### O16 - Generic methods are not supported

`static func firstOf<T>(List<T> items, T fallback): T` is a syntax error
(`Expected '(' after func name -- got "<"`). Only generic *classes* exist, so
`Generics.zl` writes the helper per element type.

### O17 - The repository's `tests/` directory is missing

`CMakeLists.txt` declares 15+ test executables whose sources are not in the
tree, so `cmake -S . -B build` fails at the generate step:

```text
CMake Error at CMakeLists.txt:259 (add_executable):
  No SOURCES given to target: zl-native-struct-tests
```

The examples were therefore built by compiling `src/**/*.cpp` directly with the
same flags the `zl_language` target uses, and the project's own regression suite
could not be run. `examples/run_all.sh` is the only executable check available
here.

### O18 - String operations count and index bytes, not characters

Every `String.*` / `Text.*` operation works on UTF-8 bytes rather than code
points. `String.length` is a plain `.size()` (`src/vm/native.cpp:466`). Nothing
under `docs/` mentions Unicode, UTF-8, or code points at all, so the behaviour is
undocumented either way.

```zl
Text.length("héllo")            // 6, not 5      ("é" is two bytes)
Text.length("日本語")            // 9, not 3
Text.charAt("héllo", 1)         // the single byte 0xC3, half of "é"
Text.substring("héllo wörld", 0, 3)   // "hé"  - three bytes, two characters
Text.indexOf("héllo wörld", "w")      // 7, not 6
Text.upper("héllo")             // "HéLLO" - only ASCII is cased
```

The one that is clearly wrong rather than merely surprising is `Text.reverse`,
which walks the string by byte index and concatenates - so it emits the bytes of
a multi-byte character in the wrong order. Confirmed with `od -c` rather than
terminal rendering:

```text
Text.reverse("héllo")  ->  o l l \251 \303 h
```

`é` went in as `\303\251` and came out as `\251\303`, which is not valid UTF-8.
ASCII-only input is unaffected (`Text.reverse("ab")` is `ba`), so this only bites
once someone stores a non-ASCII name, and then it corrupts the value silently.

The same byte/character confusion would affect `Text.repeatText`, `padLeft` /
`padRight` widths, and `startsWith` / `endsWith` when the prefix ends mid-character,
though only `length`, `charAt`, `substring`, `indexOf` and `reverse` were
directly verified.

Two smaller notes found in the same pass:

- `string` has no methods at all. `s.length()` fails with `cannot call method
  'length' on value of type string`; the `Text.length(s)` free-function form is
  the only option. Every other collection type is method-based, so this reads as
  an inconsistency rather than a decision.
- `List.pop()` reports `Collection.pop: cannot pop from an empty list` while
  `List.first()` reports `List.first called on empty list`. Cosmetic.

### O19 - `INT64_MIN` cannot be written as a literal

```zl
var min = -9223372036854775808
-> compile error: integer literal is out of range '9223372036854775808'
```

The lexer hands the type checker the unsigned magnitude, and both
`TypeChecker::inferLiteral` (`type_checker.cpp:3402`) and `Compiler::compileLiteral`
(`compiler.cpp:694`) validate it with `std::stoll`, whose ceiling is `INT64_MAX`.
The unary minus is a separate node applied afterwards, so the one value that needs
the extra slot is rejected before the negation is ever considered.

Everything else at the boundary is correct, and the value is reachable by
arithmetic:

```zl
var max = 9223372036854775807        // fine
(0 - max) - 1                        // -9223372036854775808, prints correctly
((0 - max) - 1) - 1                  // runtime error: integer overflow in subtraction
```

Deliberately **not** fixed. A correct fix has to allow the magnitude only in a
negation context, and the codegen side has no such context at the literal site -
so accepting it outright would also accept `9223372036854775808` as a positive
literal, which is wrong. Left as a documented limitation with the arithmetic
workaround.

### O20 - `Shared<T>` really does lose updates; measured

`SharedState.zl` warns that `Shared<T>` makes a capture *legal* but not *safe*.
That is not a hypothetical. Two threads doing 2000 unsynchronised read-modify-write
increments each, run alongside two threads using `Atomic` for the same work:

```text
plain  (expect <= 4000)  2490     <- 1510 updates lost
atomic (expect 4000)     4000     <- exact
```

So the guidance in that example is load-bearing, and `Atomic` is correct under
real contention rather than merely in the small counts the examples use. Recorded
here as evidence, not as a defect.

## Verdict on the previously reported F6-F9

| | Claim | Verdict |
| --- | --- | --- |
| F6 | `await Time.sleepAsync(...)` is a heap-use-after-free, crashes every run | **Confirmed** as a UAF - ASan trace above, 3/3 - and now **fixed**. The wording needs one correction: a normal build did not reliably segfault; it hung or exited silently, and the crash only showed under ASan. |
| F7 | `Thread.start` unusable inline; an unused `var n = 5` in scope breaks it | **Confirmed**, with a refinement: it only bit closures that reference *no* variable, because those captured the whole scope. A closure that referenced a `Shared` was fine even with unused locals present. Now **fixed** (fix 5). |
| F8 | `withLock` deadlocks; 2x50 fine, 2x100 hangs | **Confirmed** - `total=100` at 2x50 (5/5), 2x100 hangs (exit 124). One correction to the original report: the API is the instance method `counter.withLock(func() { ... })` (`builtin_library.cpp:663`), not a qualified `Shared.withLock(counter, f)` call - the latter does not compile. `Mutex.withLock` hangs at the same threshold, so this is the locked-closure path, not `Shared` (see O4). |
| F9 | `log(3.14)` prints `3.1400000000000001` | **Confirmed.** Cosmetic, but it is the first double a beginner prints. |

F6 and F8 were not reachable at all before fix 1: `Shared<int>` could not be
constructed, so no thread or lock example could even start. Both were re-verified
after the fix. F6 and F7 have since been fixed (fixes 3 and 5); F8 and F9 remain
open.

## Three features that existed but were documented nowhere

`Option<T>`, `Result<T,E>`, and the fluent `Regex` builder are all defined in
`src/compiler/builtin_library.cpp` and appear in no file under `docs/`. They now
have examples: `OptionType.zl`, `ResultType.zl`, `RegexBuilder.zl`.

The builder is worth calling out - `new Regex("").digit().exact(4)` producing
`^\d\d\d\d$` is the kind of thing people would use if they knew it existed, and
`source()` makes it self-documenting.

One small oddity while reading it: `None<T>.unwrapOrElse` carries two stacked
`@Override` annotations. It compiles and behaves correctly, so this is cosmetic.
