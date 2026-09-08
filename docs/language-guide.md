# ZL Language Guide

Reference for the language surface. For the standard library see
[stdlib.md](stdlib.md); for imports and dependencies see [packages.md](packages.md).

- [File and class rule](#file-and-class-rule)
- [Values and declarations](#values-and-declarations)
- [Lambda expressions](#lambda-expressions)
- [Generic collections](#generic-collections)
- [Static methods](#static-methods)
- [Operator overloading](#operator-overloading)
- [Record values (`data`)](#record-values-data)
- [`Option<T>` and `Result<T,E>`](#optiont-and-resultte)
- [Reflection](#reflection)
- [Memory-model direction](#memory-model-direction)

## File and class rule

Each `.zl` file must contain a class whose name matches the file stem exactly:

```text
TestProgram.zl   -> class TestProgram
type_mismatch.zl -> class type_mismatch
```

This is enforced by the compiler pipeline. Errors are reported in three categories:
`syntax error`, `compile error`, and `runtime error`.

## Values and declarations

- `var` (rebindable) and `let` (single assignment) declarations, with inference or
  explicit types
- Primitives: `int`, `double`, `decimal`, `bool`, `string`
- Union-style values such as `int|string`
- Arrays, lists, maps, and sets via the collection helpers
- Arithmetic, comparison, logical, bitwise, and shift operators
- `if`, `elif`, `else if`, `else`, `for`, `while`, `repeat`, `break`, `continue`
- `func` declarations, methods, constructors, and return values
- `class` and `data` declarations, plus `enum`
- Access modifiers: `public`, `private`, `protected`

`enum` members are accessed as `EnumName.MEMBER` and are genuinely statically typed —
a variable or parameter typed as the enum rejects a raw string at compile time.

**Map ordering is a guarantee, not an accident.** `map` is insertion-ordered by
construction: it is a vector-backed association list, not a hash table. New keys append
at the end, re-setting an existing key updates it in place, and removing a key does not
reorder the rest.

**Static fields** support inherited qualified access. A derived class resolves an
inherited static field to its declaring class, so reads and writes share the same
backing storage. Access modifiers are enforced against the declaring owner: protected
inherited access works from subclasses, private inherited access is rejected, and
interface-qualified static-field access is rejected because interfaces own no static
storage.

## Lambda expressions

An anonymous function *value* — assignable to a variable, passable as an argument, or
returnable from a function:

```zl
var square = func(x) => x * x         // expression body - `return` is implied
log(square(5))                        // 25

var greet = func(name) {              // block body - explicit `return` required
    log("Hi " + name)
}
greet("Zeal")

func applyTwice(func f, int x): int { // `func` as a parameter/return type
    return f(f(x))
}
log(applyTwice(func(x) => x * 2, 3))  // 12
```

**Syntax.** `func` — the same keyword as a named declaration, reused in expression
position — followed by a parameter list, an optional `: ReturnType`, then either
`=> expr` (single expression, return implied) or `{ statements }` (block body, explicit
`return` required; falling off the end returns `nil`). Parameters may be untyped (`x`)
or typed (`int x`), using the same `Type Name` order as named functions. `func` alone,
with no parameter list, is also a valid *type* name for a slot holding a function value:
`callback: func`.

**Declared return types.** A lambda may state its result type just like a named
function. The annotation is a checked contract, not documentation — the body's inferred
result must be assignable to it:

```zl
var double = func(int x): int => x * 2          // ok
var isBig  = func(int x): bool => x > 10        // ok
var report = func(int x): void { log(x) }       // ok - no value returned

var wrong  = func(int x): bool => x + 1         // error: body returns 'int'
```

When the annotation is present it overrides whatever the call site expected, so the
lambda's contract stays visible to callers. Omitting it keeps the previous behaviour:
the result type is inferred from the body, and from the expected type at the call site.

**Capture is by value.** A lambda snapshots every variable in scope at the moment it is
*evaluated* (not declared) — a copy, not a live reference:

```zl
var n = 10
var readN = func() => n
n = 999
log(readN())   // 10 - NOT 999
```

Two lambdas created from the same scope, for example on different loop iterations, get
independent snapshots; one closure's execution never affects another's. Within a single
closure, mutating one of its own captured variables persists across repeated calls to
*that same* closure, since a closure privately owns its snapshot from creation — the
classic counter pattern:

```zl
func makeCounter(): func {
    var count = 0
    var inc = func() { count = count + 1
        return count }
    return inc
}
var c = makeCounter()
log(c())  // 1
log(c())  // 2
```

**Known gap.** Full function-type signatures — parameter and return types on a
`func`-typed slot — are not checked yet. Calling a `func`-typed value with the wrong
number of arguments is caught at runtime, not at compile time.

## Union types and narrowing

A union is a set of alternatives, not the dynamic `unknown` type. Every
alternative must fit a typed assignment, argument or return. Use `match` to
refine it before an operation that only accepts one member:

```zl
static func describe(int|string value): string {
    return match value {
        int _ => "number " + (value + 1)
        string text => String.upper(text)
        null => "missing"
    }
}
```

A type arm refines both its binding and the original subject identifier while
that identifier remains unchanged. The catch-all binding carries the remaining
alternatives. Guards do not establish exhaustive coverage. ZL reference types,
including `string`, remain nullable: cover `null` or use a wildcard as well.
Enums and primitive numbers/bools are not nullable.

The subject is evaluated once. If a guard reassigns it, subsequent patterns still
inspect the original snapshot; use the pattern binding to read that snapshot.
Reassignment invalidates read refinements, and does not change the variable's
original declaration or constness. Mutable closure captures and loop back edges
cannot keep a refinement that their writes may invalidate.

Locals, loop counters, catch variables and pattern bindings have distinct lexical
storage, even when they reuse a spelling. Closures capture the selected bindings
by value. Typed reassignment is checked before storing a dynamic value, so a
rejected write leaves the old slot intact (RHS side effects are not rolled back).
The same expected-type handling supports reassigned lambdas and typed literals.

The focused regression can be run with:

```sh
python3 tests/type_boundaries.py /path/to/zl
```

## Heap write contracts

Native `list<T>`, `map<K,V>`, `set<T>` and `array[N]<T>` values carry contracts
on their shared storage, not just on the variable that first names them. Typed
initialization (including inferred locals), calls, returns, fields and successful
type patterns establish those contracts. An erased alias cannot bypass them:

```zl
list<int> numbers = [1, 2]
unknown alias = numbers
Collection.push(alias, "wrong")   // runtime error; numbers is unchanged
```

Nested containers are checked and constrained as one transaction. If a type
check fails, it does not leave partially constrained children behind. New children
inserted later acquire the required contract too. Containers are invariant:
`list<int>` cannot become `list<double>` through an erased alias, while an `int`
can still be stored in a fresh `list<double>`. Bare/`unknown` views do not erase an
existing contract. A native container's first successful typed view establishes
its storage contract; use a fresh container when a different contract is needed.
Fixed-array aliases cannot grow or shrink the array through list, queue, stack
or set operations. These checks do not replace the explicit synchronization
required for shared mutable program state.

Instance/data/static field writes enforce their complete declarations, including
inherited generic parameters. Class and data fields cannot redeclare an inherited
name: the object layout has one storage slot per field name.

Native return inference also preserves element types: `String.split` returns
`list<string>`, `Collection.get/pop` return the source element type, and map
keys/values and set copies preserve the relevant type argument. The rules live in
the native signature catalog rather than in library-specific compiler branches.

## Generic collections

`List<T>`, `Map<K,V>`, and `Set<T>` are real generic classes, not native tags.
`push`/`pop`/`get`/`put`/`length` and `add`/`has`/`remove` are genuinely compiled
methods, so a wrong element, key, or value type is a compile-time error:

```zl
var nums = new List<int>()
nums.push(1)
nums.push(2)
nums.push("oops")   // compile error: no overload of 'push' matches (int), got (string)

int first = nums.get(0)    // get()'s return type narrows to int for THIS instantiation
nums.put(0, 99)            // index-assignment - see the naming note below

var ages = new Map<string, int>()
ages.put("ada", 36)
log(ages.get("ada"))       // 36
log(ages.has("grace"))     // false
ages.remove("ada")

var tags = new Set<string>()
tags.add("x")
tags.add("x")              // no-op, sets don't duplicate
log(tags.length())         // 1
```

These are always available; no `import` is needed. They are backed by the same native
list/map/set storage that the lowercase `list<T>`/`map<K,V>`/`set<T>` type annotations
use — only the compile-time type surface is new, layered on the same generic-class
machinery that user-defined generics (`class Box<T>`) already use. A mismatched type is
caught by ordinary overload resolution, with the same error-message quality as any other
method call.

**Naming note.** The index/key-assignment method is `put`, not `set`, because `set` is a
reserved keyword (the lowercase `set<T>` annotation). This is a keyword-collision
workaround, not a design preference.

### Algorithms

`List<T>` owns the high-level collection algorithms in ZL while C++ provides only the
storage primitives:

```zl
var values = new List<int>()
values.push(4)
values.push(1)
values.push(3)

var doubled = values.transform(func(x) => x * 2)
var total = values.reduce(func(a, b) => a + b, 0)
values.sort(func(a, b) => a < b)

log(values.contains(2))
var evens = values.filter(func(x) => x % 2 == 0)
log(values.any(func(x) => x > 3))
```

Available: `contains`, `indexOf`, `count`, `any`, `all`, `filter`, `forEach`,
`transform`, `reduce`, `sort`, and `reversed`. These are implemented in ZL against the
existing `Collection.*` native storage boundary rather than as algorithm-specific C++.

`count(item)` is the odd one out: unlike `any`/`all`/`filter` it takes a value, not a
predicate, and returns how many times that value occurs.

### Typed collection literals

The expected declared type participates in literal checking and construction. Lists
support `[...]`; map and set literals use `{...}`:

```zl
list<int> nums = [1, 2, 3]
map<string, int> ages = {"ada": 36, "alan": 41}
set<string> names = {"ada", "alan", "ada"}

List<int> genericNums = [4, 5, 6]
Map<string, int> genericAges = {"ada": 36}
Set<int> genericSet = [1, 2, 2]
```

Every element, key, and value is checked against the expected generic type. Typed
`List`/`Map`/`Set` literals construct normal generic ZL collection objects; lowercase
`list`/`map`/`set` annotations keep their native representation.

## Static methods

Classes may declare `static func` members, called through the class name with no `this`
receiver:

```zl
class Helpers {
    public static func clamp(double x): double {
        if (x < 0.0) { return 0.0 }
        return x
    }
}

log(Helpers.clamp(-2.0))
```

Using `this` from a static function is a compile-time error. The standard library uses
static methods to move high-level policy into ZL without replacing the small native
runtime primitives underneath.

## Operator overloading

Classes define operators with the `operator` declaration. Operators use normal method
dispatch and overload resolution, and are public by default but may be marked `private`
or `protected`:

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

Arithmetic, comparison, bitwise, shift, and unary `+`/`-`/`!`/`~` declarations are
supported. Built-in operators remain the fallback for primitives; object operands use a
declared operator when one exists. Overloads are class-aware, so different object
parameter types coexist, while generic operator parameters keep shared runtime dispatch
with concrete compile-time checking. Numeric mixed-type overloads, operator visibility,
inheritance, and interface operator contracts are supported. Unsupported or ambiguous
object operators are compile-time errors.

## Record values (`data`)

A `data` declaration defines a named record with typed public fields:

```zl
data Point {
    x: int
    y: int
}

var a = Point { x: 10, y: 20 }
var b = Point { x: 10, y: 20 }

log(a == b) // true
```

Record literals must provide every declared field exactly once; unknown, missing, or
duplicate fields are compile-time errors. Records of the same `data` type compare
structurally, including nested records, with numeric fields using normal ZL numeric
equality.

Records are value-oriented and immutable after construction. They may declare public
instance `func` methods — including `toString()` — which use ordinary dispatch and
reflection infrastructure, but they may not reassign their own fields. Record
inheritance and destructuring remain roadmap work.

## `Option<T>` and `Result<T,E>`

Two built-in generic sum types, available with no `import`. They model absence and
failure as values rather than as exceptions you might forget to handle.

```zl
var present = new Some<int>(42)
var absent  = new None<int>()

present.isSome()                              // true
present.unwrap()                              // 42 - throws on None
absent.unwrapOr(-1)                           // -1
absent.unwrapOrElse(func() => expensive())    // lazy fallback
present.isSomeAnd(func(v) => v > 40)          // true
```

`Result<T,E>` is the two-parameter sibling: a success value of type `T`, or an error of
type `E`, for when the failure carries information the caller needs.

```zl
var ok  = new Ok<int, string>(42)
var err = new Err<int, string>("division by zero")

ok.unwrap()          // 42
err.unwrapErr()      // "division by zero"
err.unwrapOr(-1)     // -1
err.isErrAnd(func(e) => e == "division by zero")
```

Full member list: `Option` has `isSome`, `isNone`, `unwrap`, `unwrapOr`, `unwrapOrElse`,
`expect`, `contains`, `isSomeAnd`. `Result` adds `isOk`, `isErr`, `unwrapErr`,
`unwrapErrOr`, and `isOkAnd`/`isErrAnd`.

A function may declare either as its return type or a parameter type, and a
subclass instantiation widens to its parameterized parent - `Some<int>` satisfies
`Option<int>`, while `Some<string>` does not:

```zl
static func find(int n): Option<int> {
    if (n > 0) { return new Some<int>(n) }
    return new None<int>()
}

static func label(Option<int> o): string {
    if (o.isSome()) { return "found " + o.unwrap() }
    return "empty"
}
```

See `examples/advanced/OptionType.zl` and `examples/advanced/ResultType.zl`.

## Reflection

A minimal, name-based `Type` API for runtime inspection:

```zl
Type.name(value)       // runtime type/class name
Type.fields(object)    // effective field names, including inherited fields
Type.methods(object)   // effective method names, including inherited methods
Type.base(object)      // direct base class name, or nil for a root class
```

Reflection metadata is intentionally small. Generic type arguments, method signatures,
annotations, and writable reflection are reserved for future phases.

## Runtime lifetimes and GC roots

Active executions, queued/suspended async invocations and reachable closures keep
their programs reachable. The collector follows program constants, current
static values and cached initialization failures at trace time. A parked VM does
not keep a stale copy of a static slot, and an unreachable static/closure cycle
is collectible rather than permanently pinned.

A thrown ZL exception retains its payload while C++ unwinds. A failure cached in
a Task or static field instead stores a traced edge and an owned diagnostic;
rethrowing it establishes a new in-flight root. Destructors do not read reclaimed
exception objects. Failures of an async `main` propagate to the entry point.

Collection separates tracing from destruction. Unreachable allocations leave the
registry while mutators are stopped, but their native owners are destroyed after
reactivation. Implicit `Thread` joins also wait at a stable VM instruction
boundary, not inside a vector/map update or frame unwind. Explicit `Thread.join`
remains synchronous. Values being returned or caught stay rooted while these
waits run. Pending native channel tasks retain their operation owner until a
terminal transition.

C++ embedders participate explicitly: `GCRoots` contains value snapshots and
borrowed program roots whose owners must outlive the root lease. `TracingGC::collect`
returns a move-only `Collection`; reclaim it only after the stop-the-world phase
has ended. The coordinator performs this ordering for VM collections.

The focused `gc_lifetime_tests.cpp` target checks program/static/closure cycles,
exception lifetimes, pending operation roots and blocking reclamation across
consecutive collections. `StaticMembers.zl` exercises the corresponding language
paths under allocation pressure.

## Memory-model direction

The planned memory model combines ownership with tracing GC. Unannotated managed and
reference values default to GC-managed, thread-confined semantics. `shared` is explicit
rather than the default.

Thread confinement is enforced at compile time: a closure passed to `Thread.start` or
`Task.spawn` may only capture values that are safe to carry across the boundary. The
accepted set is `Shared<T>` plus the runtime's own synchronisation primitives —
`Atomic`, `Mutex`, `RwLock`, `Semaphore`, `Channel` and `Condition` — since each of them
guards its state internally. Capturing anything else is a compile error:

```zl
var total = 0
Thread.start(func() { total += 1 })
// compile error: Thread.start cannot capture 'total' across a thread boundary;
//                use Atomic or Mutex for mutable shared state

var counter = new Atomic()
Thread.start(func() { Atomic.add(counter, 1) })   // fine
```

Wrapping a value in `Shared<T>` makes the capture legal, not automatically safe:
`get()`/`setValue()` on their own can still interleave a read-modify-write, so use
`Atomic` for counters and `Mutex` or `RwLock` for larger critical sections.
