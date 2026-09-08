# Development Checkpoints

Dated progress notes, newest first. These were previously appended to `README.md`.

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
