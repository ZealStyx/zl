# Development Checkpoints

Dated progress notes, newest first. These were previously appended to `README.md`.

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
