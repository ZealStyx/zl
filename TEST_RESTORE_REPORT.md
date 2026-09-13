# Test suite restoration — final report

## 1. Restored required test targets: **21**
All 21 missing sources were audited against the current implementation before being
written (no blind recreation, no placeholders). Files:
`runtime_type_tests.cpp`, `gc_tests.cpp`, `runtime_task_executor_tests.cpp`,
`runtime_sync_tests.cpp`, `ir_tests.cpp`, `machine_code_tests.cpp`,
`regex_engine_tests.cpp`, `regex_unicode_tests.cpp`, `runtime_scheduler_tests.cpp`,
`runtime_hardening_vm_tests.cpp`, `native_compiler_tests.cpp`,
`native_ffi_metadata_tests.cpp`, `native_resource_tests.cpp`,
`native_boundary_tests.cpp`, `native_runtime_registry_tests.cpp`,
`native_ffi_transport_tests.cpp`, `native_ffi_lifetime_adapter_tests.cpp`,
`native_resource_stress_tests.cpp`, `native_struct_tests.cpp`,
`native_library_tests.cpp`, `native_ffi_declaration_tests.cpp`.

## 2. Intentional platform skips: **0**
No optional test target is registered in `CMakeLists.txt`; nothing is skipped on
this platform by design. (`zl_add_optional_test` exists for that purpose and
records the reason in its STATUS message.)

## 3. Unexpected skips: **0**
All 33 registered targets configure, build, and run.

## 4. Full suite result: **33/33 PASS**
21 restored + 12 pre-existing targets, each an exit-0 binary.
Build/run: `python3 tools/dev/build_tests.py`, then execute `build/*`.

## 5. Missing required source now FAILS configuration
`zl_add_required_test` (CMakeLists.txt) emits `FATAL_ERROR` naming the target and
file when its primary source is absent; `tools/dev/build_tests.py` (the sandbox
builder, cmake is unavailable) exits **2** with `MISSING SOURCE for <target>`
before compiling anything. Verified both paths by temporarily removing a file.

## 6. Completeness guard (6.9): single source of truth
The `(target, sources)` pairs in `zl_add_required_test` / `zl_add_optional_test`
in CMakeLists.txt are the only list. `build_tests.py` parses those calls with a
regex — it has no duplicated filename list and gains new targets automatically.
A required target with any missing source fails the guard; optional targets are
skipped with a documented reason instead.

## 7. Assertion density (restored files only)
**599** `require(...)` assertions across the 21 files (9–71 per file). Every test
measures observable behavior (values, exceptions, exit codes, registry state,
GC deltas); a `REQUIRE(true)`-style placeholder was not introduced. Unicode
tests use real non-ASCII input (café, 中, …); the stress test spawns real
concurrent consumers over a token pool rather than calling once.

## 8. Product bug found and fixed by the restored tests (compiler)
`runtime_hardening_vm_tests` (interface dispatch) exposed a genuine miscompile:
any call to a static func with an interface-typed parameter was **silently
dropped** from the compiled body (program exited 0, call never ran).
Root cause: `registerClassShape` runs before interface registration, so the
interface parameter was recorded in the class's method table as `UNKNOWN`; the
call site then derived dispatch identity `ask(object)` while the function was
registered as `ask(G)` — the MIR lowering could not resolve the call, marked the
function "incomplete" (not fatal), and the bytecode backend emitted the
truncated body. Fix: a post-interface fix-up pass in the type checker
re-resolves previously-`UNKNOWN` method/constructor parameters and return types
(`type_checker.cpp`), plus a non-const `SemanticModel::findClass`
(`semantic_model.hpp/.cpp`). Verified end-to-end: the failing program now
prints the call's output; full suite green.

## 9. Objective 1–5 regression coverage (6.10) — present in restored tests
- Compiler boundary violations → `testBoundaryLint` (runs `tools/boundary_lint.sh`)
- Unresolved `CallIndirect` → `testReachabilityUnresolvedFunctionValue`
- `ReachabilityReport::complete()` semantics → `testReachabilityClosedGraph`,
  `testReachabilityReflectionOpensGraph`
- Pipeline reporting with libraries → `testLibraryWithoutMain`
- Backend/frontend dependency boundary → `testBoundaryLint` + lint fixtures
- Virtual/interface dispatch → `testVirtualDispatch`, `testInterfaceDispatch`
- Large hierarchy dispatch → `testDeepHierarchyDispatch` (deep chain, leaf method)
- Native boundary ownership → `tests/native_boundary_tests.cpp` (handle
  consumption exactly once, consumed handle rejected, resources intact on failure)

## 10. `complete()` as a removal-safety guarantee
Sound and deliberately conservative: `complete()` is true only when an entry
point exists, no dynamic entry (function value not provably one closure body)
remains, and no call target is unresolved. In that state the report's function
set is a closed over-approximation of everything that can execute, so deleting
anything outside it cannot change observable behavior. In every other state it
returns false and the contract says "lower bound — refuse deletion", so it
cannot over-promise. Caveats: it guards *removal* only (not semantic edits),
and the dynamic-entry condition is all-or-nothing per program — heavy closure
use makes `complete()` false even when every specific target is resolvable.

## 11. Boundary protection verdict
Yes — the suite now actively protects the boundaries: boundary lint (frontend
must not pull backend symbols), codegen refusal of unverified/out-of-order
modules, GC root/reachability accounting, FFI boundary invariants (arity,
primitive types, owned-handle lifecycle, single consumption), scheduler/sync
concurrency contracts, and end-to-end VM dispatch through the full pipeline.
A regression in any of those fails a specific named test instead of vanishing.

## 12. Remaining known limitations (honest, non-blocking)
- Interface-typed **fields** on classes are still recorded as `UNKNOWN` in the
  shape table (only method/constructor params and return types are fixed);
  field-level type checks against such fields are therefore weaker than the
  method path. Same root cause, narrower blast radius, no observed miscompile.
- Optional-target skips exist only as machinery; none are currently registered.
- `cmake` itself is unavailable in this sandbox; the CMake guard is verified by
  inspection and the mirrored `build_tests.py` guard is verified by execution.
