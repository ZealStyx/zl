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
