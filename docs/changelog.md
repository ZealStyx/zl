# Development Checkpoints

Dated progress notes, newest first. These were previously appended to `README.md`.

## 2026-09-10 — MIR: ownership and lifetime as events, not metadata

ZL's ownership model now survives lowering. The MIR keeps each storage's
contract (`gc`/`owned`/`borrow`/`shared` on slots and parameters) and
represents the lifetime events as instructions: `move` empties a slot and
continues the value; `borrow`/`end_borrow` bracket a function-scoped view;
`drop` releases a resource in two exclusive spellings — a value operand, or
the **storage-release form** (slot named, no operands) that lowering emits
for the end of an owned local's lifetime. Documented in
[`docs/mir.md`](mir.md); regression suite in `tests/mir_ownership_tests.cpp`
(`zl-mir-ownership-tests`).

**Lowering.** Every return site — each `return`, the implicit end of a void
body, each lambda's exit — emits reverse-order releases for owned slots that
were not moved and for owned parameters, mirroring the reference compiler's
`DropVar`-before-`Return` placement. Slots whose value was `move`d are
skipped: the reference clears the local and the frame teardown releases the
rest, and the MIR path does the same. GC slots get no events; closure
captures are always GC values, so `MakeClosure` carries none either.

**Verification.** `checkOwnershipFlow` now tracks
`{moved, dropped, droppedParams, borrows}` across the CFG with union joins —
the same rule the type checker's `joinOwnershipStates` uses, so MIR rejects
exactly the programs the checker does, no stricter and no looser at branch
and loop joins. The invalid states it catches: use after move, use after
drop, double drop ("a resource releases exactly once"), drop after move,
drop while borrowed outside the exit-cleanup region (a ZL borrow is
function-scoped and ends with the function, so the trailing release cannot
conflict with it), borrow of a moved or released owner, storage-release of
non-owned storage, and `end_borrow` without a borrow. A move is final: the
checker rejects assigning to a moved variable, so no store resurrects one
here either.

**Backend.** The bytecode backend translates the events to the reference
runtime's own opcodes — `MoveVar`, `DropVar`, and value rebinding for borrows
(the runtime has no aliasing; lifetimes are the verifier's job) — and
registers owned locals in `ownedLocalNames` so the VM's frame teardown
releases them on early return and exception exactly as the reference path
does. Differential runs (`tools/mir_backend_diff.sh`) stay at 24 matched /
7 known gaps; examples stay 50/50.

## 2026-09-10 — MIR: the ZL type system, end to end

MIR now carries the language's *type semantics*, not just its shapes, and the
boundaries where a dynamic value becomes a typed one are explicit instructions
instead of silent retypes. Documented in [`docs/mir.md`](mir.md); regression
suite in `tests/mir_type_tests.cpp` (`zl-mir-type-tests`).

**Types.** `Option<T>` and `Result<T,E>` are first-class kinds carrying their
payload/[ok, error] type ids; `Some`/`None`/`Ok`/`Err` keep their class spellings
and relate to the sums by a verified assignability rule (`Some<int>` satisfies
`Option<int>`, `Some<string>` does not, and the relation composes under
arguments). Unions are ordinary types everywhere — a function may declare
`int|string` as its return type, which the verifier previously rejected outright.
Nested generics survive as structural ids, so `Map<string,List<int>>` and
`Option<List<int>>` keep their arguments as type ids, not rendered strings.
Native/resource semantics stay where the language actually has them: FFI values
are typed by their declared renders and resource behaviour by slot/parameter
ownership with the move/borrow/drop dataflow.

**Boundaries.** A dynamic (`unknown`) value crossing into typed territory — a
typed local or assignment, a call argument, a return, a field/element write,
a `match` arm — now lowers to an explicit `refine` (the runtime type
assertion), and the verifier rejects any `unknown` operand that reaches a typed
destination without one, so an invalid type assumption fails at the boundary
instead of being trusted downstream. The MIR→bytecode backend translates
`refine` to a real `AssertType`, `type_test` to `MatchType`, and index reads
through `GetIndex`, which fixed `--mir-vm` on every program that indexes a
class-spelled collection (`names[0]` on a `List<string>`). SSA promotion
declines to promote a slot whose stores are retyped relative to the slot's
declared type, so promotion can no longer erase a declared `unknown` (or any
declared contract) out from under a `match` subject.

**Compiler type resolution.** Dispatch signatures erased every concrete generic
instantiation to a bare `object`, so `label(Option<int>)` and `label(List<int>)`
collided in every function table keyed by the rendered name — the second
declaration silently replaced the first and calls dispatched to the wrong body,
caught (when at all) by a runtime assertion. A `GENERIC_OBJECT` parameter now
keeps the generic class's erased base name (`label(Option)`, `label(List)`):
still one body per declaration, never per instantiation, but no declaration can
shadow another.

**Verification:** `zl-mir-type-tests` (new), the three existing MIR suites, the
50-example corpus, `type_boundaries.py`, the LSP/test-runner Python suites, and
differential `--mir-vm` runs against the reference path all pass.

## 2026-09-09 — MIR: a typed mid-level IR with a verifier

ZL now has a real mid-level intermediate representation, in `zl::mir`
(`include/zl/mir/`, `src/mir/`). It is reached with `zl --emit-mir <out|->
<file.zl>` and is documented in [`docs/mir.md`](mir.md).

**Why.** The compiler had one internal graph, `zl::ir`, and it could not carry
what a backend needs: types were `std::string` names, terminators were mixed in
with value-producing instructions, successors were duplicated separately from the
terminator that implied them, and generics, unions, function types, exceptions,
field access and indexing had no representation at all. Rather than retrofit
that, MIR is a new layer beside it. `zl::ir` is unchanged and still feeds
`--emit-native` and `--emit-machine-code`; the bytecode compiler and the VM are
untouched, and no language semantics changed.

**What it is.** Types are interned structural values, so a type *is* its id and
`TypeId` equality is type equality. Nullability is derived from the kind rather
than stored. Generic parameters, unions and function signatures are first-class
kinds — nothing was erased to `object` to make a first version tractable.
Functions are SSA over temps, with mutable locals as explicit slots and
`Load`/`Store`; parameters stay SSA unless the body writes, moves or borrows
them. `Instruction` and `Terminator` are separate types with exactly one
terminator per block, which is the invariant everything else about the CFG rests
on. Exceptions are unwind edges with handler chains; ownership is a dataflow
problem the verifier solves over reverse postorder.

**A generic function is a template, not a copy.** `class Set<T>`'s methods are
lowered once with `this: Set<T>`, and the call site records which instantiation
it selected. That keeps one source of truth instead of a function per
instantiation, and it is what makes "passes `Shared<int>` but the parameter is
`Shared<T>`" checkable rather than a false mismatch.

**Verification.** `verifyModule` enforces 26 documented invariants — SSA
uniqueness, dominance, block reachability, predecessor consistency, operand and
result typing, call signatures, field and index legality, ownership flow, and
the shape of exception edges. `FunctionBuilder::finish()` guarantees two of them
structurally, so a function that was only partly lowered is still valid MIR and
carries its caveat in `Function::incomplete` instead of as a broken graph.

**Lowering** covers expressions, statements, classes and inheritance, generics,
closures including nested ones and ones that capture `this`, `List`/`Map`/`Set`
calls, `match`, `data` record literals, annotated collection literals,
try/catch, async and await, and `Shared<T>`. Unsupported constructs produce a
note and an incomplete function, never malformed MIR. Across `examples/`, 54 of
57 files lower and verify completely and the rest verify with notes; none fail.

A closure that captures `this` needed two things, not one. `this` is captured by
name — semantic analysis puts `"this"` in `captureStorageNames` — so the
enclosing body has to bind it under that name for the capture to resolve; and
inside the closure the captured slot has to become the body's receiver, because
`this` there is spelled `ThisExpr` and answered from the receiver rather than
from the scope. Such a closure written inside `class Box<T>` also closes over
`this: Box<T>`, and an unsubstituted type parameter is only legal inside a
template, so the closure's MIR function is declared generic over its owner
class's parameters.

`match` lowers to a chain of two-way branches, one test block per arm in source
order, with the subject evaluated exactly once before the first test — so a
subject with a side effect cannot run once per arm, and a guard that reassigns
the subject's variable cannot change what a later arm compares against. Arm
bodies store into a result slot the join block reads back, since this IR has no
phi. A type pattern emits the new `TypeTest` and then `Refine`s both the arm's
binding and the subject identifier the body keeps spelling, which is what
narrows `int|string` to `int` inside an `int n =>` arm.

`TypeTest` was the gap that blocked it: MIR could *assert* a type (`Refine`, the
typed form of the VM's `AssertType`) but not *ask* about one, and a match arm has
to survive a "no" and fall through rather than raise. It is the typed form of the
VM's `MatchType`.

**Compiler fixes made along the way**, each at the layer that owned the problem
rather than worked around in MIR: `zlTypeName` moved next to its declaration out
of `type_checker.cpp`; the builtin `Math.PI`/`E`/`TAU` table unified into one
place instead of three copies; `zl::forEachChild` added to the AST so passes
scanning a body have one exhaustive answer to "what is inside this node" —
hand-rolled walkers were silently missing anything nested inside a call
argument; `array[N]` sizing made optional so a dynamic `array<int>` is not
recorded as `array[0]<int>`; and nullability made a question for
`TypeArena::isNullable` rather than for the top-level kind alone. The last was a
real defect: `string` is nullable in ZL even though the VM holds it inline rather
than behind a collector handle, and a union is nullable when any member is — so
`int|string` admits `null` while `int|double` does not. Reading only the kind
called every union non-nullable and rejected the `null` arm of a match over one.

Two more surfaced once `match` and record literals were lowering. `TypeChecker`
records each expression's type for the backend seam in one funnel, `inferExpr` —
but a collection literal written under an annotation (`list<int> xs = [1, 2]`)
took a special-case branch in `inferExpected` that bypassed it, so the checker
proved the type and then never told anyone, and every backend saw UNKNOWN. And
the verifier's `checkIndexAccess` handled lists and arrays but not maps, so it
rejected `index_store` on a `map<K,V>` even though the instruction taxonomy has
always said index access covers maps. Both are the kind of gap that only shows up
once a second consumer starts asking.

The same check turned out to have a third blind spot, found by writing a program
the example corpus does not contain: `set<int> s = [1, 2, 3]`. The VM runs it
fine, but MIR rejected it — a literal is built as `new_collection` plus one
`index_store` per element, and the index check covered lists, arrays and maps
while a set fell through to "indexed access requires a List<T>". Indexing also
recognised the `List<T>` class spelling but not `Set<T>` or `Map<K,V>`, and
`new_collection` recognised none of them, so a spelling could be indexable but
not constructible. `isCollectionType` is now the single answer to "is this a
collection" and both construction and indexing ask it, which is what stops the
two spellings drifting apart again.

One more came from the last two notes in the corpus. `share(x)` is a native with
no namespace and the catalog keys it by its bare name, but the call lowerer only
consulted the catalog for qualified names, so a bare `share` fell through to the
implicit self-call path and reported `call to 'C.()' which was not lowered` —
the empty dispatch being the giveaway that no method had ever been resolved. The
bytecode compiler special-cases `share` by name; the lowerer now asks the
catalog by the bare name instead, which is the same test without the
hardcoding. That cleared the note and, with it, a downstream one: the value
`share` produced had been unresolved, so a later method call on it could not
name a class either.

**Tests.** `tests/mir_tests.cpp` (`zl-mir-tests`) builds MIR by hand and checks
the verifier rejects each class of malformed module — 47 cases.
`tests/mir_lowering_tests.cpp` (`zl-mir-lowering-tests`) drives the real
pipeline end to end and asserts on the specific properties an earlier lowerer
got wrong — 29 cases.

## 2026-09-08 — Thread failures are catchable; `shared` usable as an identifier

Adversarial probing of the runtime found three defects.

**An uncaught exception on a thread killed the process.** `Thread.start`'s
worker wrapper caught everything and called `std::terminate()`, on the premise
that "an exception escaping a Thread is process-fatal by definition". A ZL
program could therefore be aborted by a worker with no way to observe or handle
the failure - the output was a bare `terminate called after throwing an
instance of 'zl::ZlThrownException'`. Threads now capture the failure in a
`StoredException`, exactly as tasks already did, and `Thread.join` rethrows it
in the joining thread. Worker failures are catchable by their real type,
including runtime faults such as an out-of-range index, and a program that
never joins gets an ordinary diagnostic and a nonzero exit instead of an abort.

**Stored exception payloads could be collected.** `StoredException` reported
its managed payload through `appendGCRoots`, which works for holders the
collector traces (tasks, static fields) but not for threads, which it does not
trace. Under allocation pressure a worker's exception object was collected
before the join and the message degraded to `ZL exception`. `StoredException`
now pins its payload with a `ProtectedGCRoot` for its own lifetime, so the
payload survives regardless of who holds it. Verified with twenty concurrently
failing workers and heavy churn between throw and join: all twenty messages
arrive intact.

**`shared` was unusable as a variable name.** It is an ownership modifier, but
the parser already accepted it as an identifier in declarations
(`var shared = ...`). Any later use failed: `shared.push(1)` reported
`Expected a type name`. The statement parser now treats `shared` as a modifier
only when a type actually follows it, and the expression parser accepts it as
an identifier, so `shared int x` and `var shared = ...; shared.push(1)` both
work.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes. Also verified during this pass: GC churn over 140,000 objects, 5,000-
deep recursion, typed exceptions propagating through 500 frames with intact
stack traces, recoverable `StackOverflowError`, typed failures surviving task
and awaited-chain boundaries, and 8 threads x 1000 atomic increments summing
exactly.

## 2026-09-08 — Correct calendar arithmetic; Date/DateTime/TimeOfDay rebuilt

Probing the time facades across a daylight-saving boundary exposed two real
correctness bugs, not just missing features.

**Adding a day shifted the clock.** `addDays` added a fixed 86400 seconds, so
noon on 2024-03-09 plus one day came back as **1 PM** on 2024-03-10 in any zone
observing a spring-forward transition. `addWeeks` inherited the same defect.

**`endOfDay` could return the wrong date entirely.** It was `startOfDay +
86399`, but a DST day is 23 or 25 hours long, so on 2024-03-10 in New York it
produced `2024-03-11 00:59:59` — the following day.

Both are fixed by doing calendar arithmetic on civil day numbers (Howard
Hinnant's `days_from_civil`) and rebuilding the timestamp through
`Time.fromParts`, rather than by manipulating elapsed seconds. `endOfDay` is
now derived from the next day's midnight, which is correct for every day
length. `diffDays` counts calendar days to stay consistent with `addDays`.

**New `zl.time.Calendar`.** The pure civil-calendar math lives in a leaf module
that depends only on the native clock primitives, so `Time` and the value
classes can share it without an import cycle (`Time` already imported `Date`).
`Time` now delegates to it instead of carrying its own copy.

**The two arithmetic families are now named apart.** `addSeconds`/`addMinutes`/
`addHours` are elapsed time and intentionally shift the wall clock across a
transition; `addDays`/`addWeeks`/`addMonths`/`addYears` are calendar time and
preserve the local time of day. `DateTime` exposes both, documented, so
`dt.addDays(1)` and `dt.addHours(24)` correctly differ across a DST boundary.

**Value classes rebuilt.** `Date` gained construction from parts, week/month/
year arithmetic, period boundaries (`startOfMonth`, `endOfYear`, …),
`daysUntil`, weekday/month names, and day-based comparison so two timestamps on
the same date compare equal. `DateTime` gained both arithmetic families,
`date()`/`timeOfDay()` views, difference helpers and instant-based comparison.
`TimeOfDay` gained validated `of(h, m, s)` construction, within-day wrapping
arithmetic, part-of-day predicates and difference helpers.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes. A 40,000-day civil round trip is exact; leap-year rules (1900, 2000,
2024), year boundaries, negative offsets and pre-epoch dates all verified; and
a sweep over every day of four transition months found zero drift in
America/New_York, Europe/London, Australia/Sydney, Asia/Manila and
Pacific/Chatham.

## 2026-09-08 — Concurrency primitives made usable

Adversarial probing of the synchronization primitives found two that could not
be used correctly from ZL at all.

**Semaphore was unusable.** A `Semaphore` starts with zero permits and nothing
exposed a way to set them, so any `acquire()` blocked forever by construction.
Added `Semaphore.setPermits`, a non-blocking `Semaphore.tryAcquire`, and
`Semaphore.releaseMany`. The facade adds `withPermits(n)` construction and
`withPermit`/`tryWithPermit` scope helpers that release the permit through a
`finally`, so an exception in the body cannot leak it. Verified with six
threads contending over two permits: observed concurrency never exceeded two.

**Condition could hang indefinitely.** `Condition.wait` has neither a predicate
nor a timeout, so a notification delivered before a waiter blocks is lost and
that waiter sleeps forever. Added `Condition.waitFor(seconds)`, which returns
whether it was notified before the deadline, and facade helpers `waitUntil` /
`waitUntilTimeout` that re-test a predicate around bounded waits and therefore
cannot miss a wakeup permanently. The lost-wakeup hazard of the bare `wait` is
now documented at the primitive rather than left to be discovered.

The previously empty `Atomic`, `Mutex`, `RwLock`, `Condition` and `Semaphore`
facade classes now carry real content: construction with an initial value
(`Atomic.ofInt/ofBool/ofDouble`, `Semaphore.withPermits`), counter helpers, and
scope helpers. `Atomic.toggle` is explicitly documented as not atomic as a
whole. Atomics were verified genuinely thread-safe: eight threads each adding
1000 produced exactly 8000.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes; catalog and runtime bindings remain in exact correspondence (246 each).

## 2026-09-08 — Lambda return-type annotations

Lambdas may now declare a return type: `func(int x): int => x * 2`. Previously
the parser rejected the annotation outright ("Expected '=>' or '{' after a
lambda's parameter list"), so a lambda's contract could only be inferred, and
typed parameters combined with an explicit result were unwritable.

The annotation is enforced rather than decorative: the body's inferred result
must be assignable to the declared type, and a mismatch is a compile error
naming both types. When present the declaration overrides the call site's
expectation, so a lambda's stated contract remains visible to callers. Block
bodies, expression bodies, `void` lambdas and untyped parameters are all
supported; omitting the annotation preserves the previous inference behaviour.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes; positive cases (typed/untyped params, block and expression bodies,
`void`) and negative cases (`bool` body returning `int`, `int` body returning
`bool`, `string` body returning `int`) all behave correctly.

## 2026-09-08 — Library layer reconciliation

Follow-up to the stdlib expansion, focused on making the three library layers
(compiler builtins, native catalog bindings, public facades) agree.

**One name per operation.** `String.regexMatches`, `regexFullMatches`,
`regexFindAll`, `regexReplace`, `regexFind` and `regexFindMatches` were aliases
bound to exactly the same callbacks as the `Text.*` spellings, and `Text` was
missing the last two. The `String.regex*` duplicates are removed, `Text` gained
`regexFind` and `regexFindMatches`, and the builtin `Regex` builder now
delegates to `Text.*`. `Text.*` was already the documented and used spelling;
`String.regex*` had no callers.

**Honest container return types.** `FileSystem.listDir` and
`Network.resolveAll` declared a bare `list` while always returning strings.
Both now declare `list<string>`, and the runtime pins the same element type, so
the static declaration and the storage contract cannot drift. Wrong-typed
writes are rejected at compile time, or at runtime when they arrive through an
`unknown`. The remaining element-agnostic returns (`Collection.newList`,
`newMap`, `newSet`, `Queue.newQueue`, `Stack.newStack`) are correctly untyped.

**Typed Queue and Stack.** `zl.util.Queue` and `zl.util.Stack` were empty
marker classes, leaving FIFO/LIFO as raw untyped lists while every other
collection was a typed generic. They are now real `Queue<T>` and `Stack<T>`
classes with the same conventions as `List`/`Map`/`Set`: element typing,
`length`/`isEmpty`/`isNotEmpty`, safe `dequeueOr`/`popOr`/`peekOr` fallbacks,
`enqueueAll`/`pushAll`, `clear`, `drain`, `items`, `contains` and `forEach`.
The native `Queue.*`/`Stack.*` functions remain the storage boundary and stay
callable directly; because qualified-namespace calls resolve against the native
table first, the methods are genuine delegations rather than self-recursion.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes; regex, queue/stack, filesystem, text, time and serialization probes
re-verified. Native catalog and runtime bindings remain in exact
correspondence (242 each).

## 2026-09-08 — Typed runtime exceptions, stdlib expansion, generic-resolution fixes

### Runtime failure behaviour

Runtime failures raised inside the VM are now real ZL exception objects and are
catchable by type. A new `ZlRuntimeFault` carries the intended class from the
throw site; the interpreter loop converts it at the failing frame, preserving
the message and stack trace, and rethrows it as an ordinary thrown exception.
Plain `std::runtime_error` from VM internals becomes `RuntimeError` rather than
escaping uncatchably.

The exception hierarchy is now `Exception` → `RuntimeError` →
`{TypeError, IndexError, KeyError, ArithmeticError, StackOverflowError,
IOError, NativeError, RegexError}`. Arithmetic faults, index and key errors,
type-assertion failures, filesystem failures and the call-depth limit all raise
their specific class, and a native that chooses a class keeps it instead of
being flattened into `NativeError`.

### Compiler and type system

- Nested generic declarations such as `List<List<int>> x = ...` parse; the type
  lookahead splits fused `>>` and `>>>` closers.
- A generic body may name its own uninstantiated form (`Map<K,V>` inside
  `class Map<K,V>`) in parameters, returns and locals. `this` types as the
  self-parameterized form, and access control treats the template and that form
  as the same declaring class.
- Fixed a declaration-order bug where a generic instantiation appearing only in
  another class's signature (very often `List<string>` returned from an
  imported class) resolved to a key with no registered shape and appeared to
  have no methods. Self-reference deferral is now restricted to generic bodies,
  and all instantiations are rebuilt once every class shape exists.

### Standard library

Collections gained substantial query, search, bulk-edit and set-algebra
coverage: `List` added 25 operations including `find`, `findIndex`,
`lastIndexOf`, `distinct`, `takeWhile`, `dropWhile`, `removeWhere`,
`removeRange`, `sorted`, `binarySearch`, `minBy`, `maxBy`, `fold` and
`equalsList`; `Map` added `getOrPut`, `putAll`, `copy`, `keyOf`, `filterKeys`,
`removeWhere` and friends; `Set` added `symmetricDifference`, `retainAll`,
`isSupersetOf`, `isDisjointFrom`, `filter` and `copy`.

New native primitives back the systems-facing libraries: 17 filesystem
operations (path decomposition, metadata, directory create/remove, copy,
rename), process/environment access (`execStatus`, `setEnv`, `hasEnv`,
`envOr`, `platform`), string search and comparison (`lastIndexOf`,
`indexOfFrom`, `trimStart`, `trimEnd`, `compare`, `compareIgnoreCase`),
calendar and monotonic time (`fromParts`, `dayOfWeek`, `isLeapYear`,
`utcFormat`, `monotonicMillis`), hashing and encoding (`sha1`, `md5`,
`fnv1a64`, hex and base64 codecs), DNS (`resolveAll`, `hostname`,
`isValidIp`) and the `trace`/`debug`/`fatal`/`at` log levels.

The `zl.fs`, `zl.text`, `zl.time`, `zl.serialize`, `zl.net`, `zl.crypto` and
`zl.logging` facades were rewritten on top of those primitives. They add
composition, safe fallbacks and honest units — calendar-correct date
arithmetic, `List<string>`-returning text splitting, typed JSON field
extraction — without re-wrapping any native that qualified-name resolution
already provides. `Crypto` documents the real strength of each digest and
still invents no cryptography.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes; typed-exception, collection, filesystem, text, time, serialization,
crypto and network probes all verified against known-good values.

## 2026-09-04 — Phase 17: native runtime integration and optimization groundwork

Completed the next two Phase 17 objectives.

A process-local `NativeExportRegistry` now accepts generated export tables, rejects
malformed and duplicate registrations, resolves exports by name, and routes name-based
calls through the existing `NativeError` boundary. This provides the runtime
registration bridge required by generated native modules without embedding raw function
pointers in ZL values.

The compiler IR also gained a backend-neutral optimization pass. It removes unreachable
blocks and folds safe primitive integer/boolean constant expressions while leaving
ownership/borrow instructions untouched. MIR native emission runs this pass before
lowering, so optimized IR feeds both native output and future VM/backend consumers.

**Validation:** `zl-ir-tests`, `zl-native-runtime-registry-tests`, and
`zl-native-compiler-tests` pass. The emitted MIR-native constant-folding path is
covered, as is direct C++ compilation of the generated native source.

## 2026-09-04 — Phase 16 completion

The ownership-aware FFI foundation is complete: opaque handles, borrowed buffers and
struct views, callback lifetime contracts, native resource adapters, dynamic library
loading, and validated `@ffi(symbol)` / `@ffi(library, symbol)` metadata are implemented
and covered by focused tests.

Typed field-by-field C struct schemas remain a later ABI extension.

## Phase 17 — binding safety

`zl-bind` class bindings use smart-pointer-backed native ownership and translate C++
exceptions into deterministic ZL runtime errors. Shared bindings can retain an opaque
handle without exposing C++ pointer types to ZL.

## Phase 16 — native boundary

Established the permanent design rule that C++ is the small, stable native foundation
while ZL owns high-level library policy and composition, together with the current
native inventory and the benchmark gate.

## Phase 15 — standard library expansion

Added user-facing packages for testing, logging, text processing, serialization, time,
and hashing. Public APIs live in ZL packages while low-level operations remain small
native primitives. See [stdlib.md](stdlib.md).

## Phase 11 — standard library and package ecosystem

Phase 11 is complete. The standard library includes reference queue/stack utilities,
text/time helpers, task/channel/thread facades, serialization, filesystem, and a small
portable DNS network facade. `zlpkg` supports version-checked path dependencies and
records resolved package versions in `zlpkg.lock`.

## 2026-08-30 — Phase 9 continuation: inherited static fields

Static fields now support inherited qualified access. A derived class resolves an
inherited static field to its declaring class, so reads and writes use the same backing
storage. Access modifiers are enforced against the declaring owner; protected inherited
access works from subclasses and private inherited access is rejected.
Interface-qualified static-field access is explicitly rejected because interfaces do not
own static field storage.

**Verification:** inherited public/protected access, shared storage, interface/class
coexistence, private inherited rejection, and interface-qualified rejection were
exercised. Existing lazy and re-entrant static behavior remains intact; the concurrent
static fixture passes under a 20-second direct run.

## 2026-08-30 — Phase 8: record methods

The Phase 8 record-method slice is implemented. `data` declarations support public
instance `func` methods, record fields are immutable after construction, and record
methods use the ordinary dispatch and reflection infrastructure. Focused valid and
invalid regressions cover the slice.

## Phase 10 — native compilation

Selected functions can opt into the restricted native compiler with `@native`, with the
VM path preserved as a fallback and MIR-native portable C++ emission via
`zl --emit-native`. See [native.md](native.md).
