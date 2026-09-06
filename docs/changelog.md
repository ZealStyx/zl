# Development Checkpoints

Dated progress notes, newest first. These were previously appended to `README.md`.

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
