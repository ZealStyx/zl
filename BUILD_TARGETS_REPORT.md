# Objective 8 — Default build scoping report

## 1. Audit of the pre-change build graph

Configured with `cmake -S . -B build -G Ninja` and inspected with
`ninja -t targets all` / `ninja -t commands`.

The old default (`cmake --build build`, i.e. `all` / `ALL_BUILD`) contained:

| Measure | Count |
| --- | --- |
| Build edges (compile + link + custom commands) | **820** |
| Linked binaries (executables + shared libs) | **37** |
| Distinct compile (`.o`) actions | 783 |

The "~613 targets" figure corresponds to this graph — the per-object compile
actions the default build spawns, not 613 distinct CMake targets. The root
cause is structural rather than numeric: **9 test executables link
`ZL_RUNTIME_VM_SOURCES`, the entire compiler minus `main.cpp` (~73 sources
each)**, so every one of them recompiles the whole front end into its own
object directory. That is where the hundreds of actions come from.

### Categorization of all 37 linked targets

| Category | Targets |
| --- | --- |
| **CORE** | `zl_language` — the ZL executable: lexer, parser, compiler, MIR, native backend, VM/runtime, regex, stdlib bundling |
| **IMPORTANT** (shipped, installed) | `zlpkg` (package tooling), `zl-bind` (native binding generator) |
| **TEST** | `zl-runtime-type-tests`, `zl-gc-tests`, `zl-gc-safepoint-tests`, `zl-gc-lifetime-tests`, `zl-runtime-task-executor-tests`, `zl-runtime-sync-tests`, `zl-runtime-scheduler-tests`, `zl-runtime-hardening-vm-tests`, `zl-ir-tests`, `zl-machine-code-tests`, `zl-regex-engine-tests`, `zl-regex-unicode-tests`, `zl-native-backend-tests`, `zl-mir-tests`, `zl-mir-ssa-tests`, `zl-mir-opt-tests`, `zl-mir-opt-pipeline-tests`, `zl-mir-lowering-tests`, `zl-mir-type-tests`, `zl-mir-ownership-tests`, `zl-mir-safety-tests`, `zl-pipeline-tests`, `zl-native-compiler-tests`, `zl-native-ffi-metadata-tests`, `zl-native-ffi-transport-tests`, `zl-native-ffi-lifetime-tests`, `zl-native-ffi-declaration-tests`, `zl-native-boundary-tests`, `zl-native-resource-tests`, `zl-native-runtime-registry-tests`, `zl-native-struct-tests`, `zl-native-library-tests` (32) |
| **STRESS** | `zl-native-resource-stress-tests` |
| **GENERATED / INTERNAL** (test fixture) | `zl-test-native-library` (shared object loaded by `zl-native-library-tests`) |
| **BENCHMARK** | `benchmarks/native_numeric_benchmark.cpp` — no CMake target; compiled on demand by `scripts/native_gate.sh` against generated native C++ |
| **EXAMPLE** | `examples/**.zl` — no CMake targets; run by `scripts/run_examples.*` |
| **OPTIONAL TOOL** | `tools/zl-lsp/zl_lsp.py`, `tools/zl-test/zl_test.py` — Python, not compiled |
| **UTILITY** | `tools/boundary_lint.sh`, `tools/*_diff.sh` — shell, not compiled |
| **PLATFORM-SPECIFIC** | none registered (`zl_add_optional_test` exists but is unused; Win32 only adds `ws2_32` links) |
| **EXPERIMENTAL** | none under `src/`; `research/` is not part of the build |

Result of the audit: **the actual ZL toolchain is 3 targets** —
`zl_language`, `zlpkg`, `zl-bind`. Everything else in the default graph was a
test. Note that `zl-*` naming was *not* used as the signal: `zl-bind` is a
`zl-*` target that *is* core, while `zl-test-native-library` is not.

## 2. Architectural change

### CMake (`CMakeLists.txt`)

Targets are accumulated into three lists (`ZL_CORE_TARGETS`,
`ZL_TEST_TARGETS`, `ZL_OPTIONAL_TARGETS`) as the file is read, and wired into
aggregates at the bottom — one dependency list per bucket, no duplicated build
logic:

```
zl-core        -> zl_language (compiler + MIR + VM + runtime + regex closure)
                  zlpkg, zl-bind
zl  (alias)    -> zl-core
zl-tests       -> zl-core + 33 test executables + zl-test-native-library
zl-benchmarks  -> zl-core   (benchmark gate is driven by scripts/native_gate.sh)
zl-examples    -> zl-core   (examples are .zl programs run by the toolchain)
zl-full        -> zl-core + zl-tests + zl-benchmarks + zl-examples
```

Dependency correctness is preserved by construction: `zl-core` names the three
**root** executables and lets CMake compute their source closure, rather than
hard-coding a list of libraries. The smallest correct closure is what gets
built.

`zl_add_required_test` / `zl_add_optional_test` now mark each test
`EXCLUDE_FROM_ALL`, so tests cannot leak into the default graph merely by being
discovered — while remaining registered in `ZL_CTEST_TARGETS` and therefore in
`ctest`. No test was deleted or disabled.

Configure time now prints the boundary:

```
-- [ZL] core targets: 3 (zl_language;zlpkg;zl-bind)
-- [ZL] test targets: 34 (built only via zl-tests / zl-full)
```

### `scripts/build.bat` and `scripts/build.sh`

Mode-driven, using the existing option conventions. `build.bat` was the only
script in `scripts/` without a POSIX twin, so `build.sh` was added with
identical mode semantics (the regression guard asserts the two stay in sync):

| Command | CMake target |
| --- | --- |
| `build.bat` / `build.sh` (or `core`) | `zl-core` |
| `build.bat test` / `build.sh test` | `zl-tests` |
| `build.bat full` / `build.sh full` | `zl-full` |
| `build.bat clean` / `build.sh clean` | removes the build directory |
| `build.bat help` / `build.sh help` | explains the modes |

All previous flags (`--debug`, `--release`, `--clean`, `--target`, `--jobs`,
`--run`, `--build-dir`, …) still work; `--target` overrides the mode. New:
`--ctest` builds the tests and then runs the suite. Output is prefixed:

```
[ZL] Configuring core build (Release) in "...\build"...
[ZL] Building the core toolchain (compiler, runtime, VM, required tools)...
[ZL] Tests, benchmarks and examples are excluded - use "build.bat test" or "build.bat full".
[ZL] Core build complete.
```

On failure it prints `[ZL] Build FAILED` and propagates the exit code — the raw
compiler/CMake diagnostics are never swallowed.

`ALL_BUILD` is never referenced; the script always passes an explicit
`--target`.

## 3. Measured result

```
Previous default build:
    820 build edges  (783 compile + 37 link)  — the "~613 targets" graph
    37 linked binaries

New default build (scripts\build.bat  ->  zl-core):
    86 build edges   (83 compile + 3 link + stdlib staging)
    3 linked binaries          -> 89.5% fewer build actions

Test build (scripts\build.bat test  ->  zl-tests):
    820 build edges, 37 linked binaries

Full build (scripts\build.bat full  ->  zl-full):
    820 build edges, 37 linked binaries
    (identical to zl-tests today: benchmarks and examples have no compiled
     CMake targets — they are script-driven over the core toolchain)

Plain `cmake --build build` (the `all` target):
    86 build edges — tests are EXCLUDE_FROM_ALL, so even `all` is now core-sized
```

Wall clock for a clean `zl-core` build with 4 jobs on the verification host:
**1m43s**.

## 4. Regression validation

`tools/dev/check_build_modes.py` configures a throwaway tree and asserts the
boundary. All 38 checks pass:

```
[PASS] build.bat exists and is mode-driven
[PASS] default mode targets zl-core
[PASS] test mode targets zl-tests
[PASS] full mode targets zl-full
[PASS] clean mode implemented
[PASS] help documents the modes
[PASS] build.bat never builds the whole tree implicitly
[PASS] build.sh exists / is valid bash / never references ALL_BUILD
[PASS] build.sh core|test|full modes target zl-core|zl-tests|zl-full
[PASS] build.sh implements clean and help
[PASS] both drivers support --debug --release --clean --ctest --jobs
                            --target --run --build-dir --configure-only
[PASS] clean configure succeeds
[PASS] test targets still registered in CMake - 34 test executables
[PASS] zl-core is far smaller than a full build - 86 vs 820
[PASS] zl-core builds zl_language
[PASS] zl-core includes zlpkg and zl-bind
[PASS] zl-tests is a superset of zl-core
[PASS] zl-full is a superset of zl-tests
[PASS] core build does not depend on test targets - []
[PASS] plain `cmake --build` no longer builds the tests - all=86, core=86
[PASS] zl-core builds successfully from a clean build directory
[PASS] core build produces the toolchain binaries
```

Additionally verified by executing every mode end-to-end on a clean tree
(`scripts/build.sh`, which shares the mode dispatch with `build.bat`):

- `build.sh help` — prints the modes; exit 0.
- `build.sh clean` — handles both an existing and an absent build directory.
- `build.sh --configure-only` — reports `core targets: 3`, `test targets: 34`.
- `build.sh` (default) — clean-tree build produced **exactly 3 binaries**
  (`zl_language`, `zlpkg`, `zl-bind`) and **zero** `*-tests` executables.
- `build.sh --run examples/basics/HelloWorld.zl` — prints `Hello, World!`;
  `zl_language --version` reports `ZL 0.1.0` and `zlpkg --help` works, so the
  core build is a genuinely usable toolchain, not just a set of links.
- `build.sh test --ctest` — built the remaining 735 edges, then
  **37/37 tests passed** (33 C++ targets + boundary-lint,
  boundary-lint-regressions, safety-pipeline, pipeline-report), so the
  Objective 6 restorations are intact and still exercised. Exit 0.
- `zl-full` completes with no additional work beyond `zl-tests`.

The strongest structural check: `ninja -t inputs zl-core` resolves to only
three object directories — `zl_language.dir`, `zlpkg.dir`, `zl-bind.dir` — and
matches **no** file under `tests/` or `tests/fixtures/`. The core closure
cannot reach a test target even transitively.

## 5. Objective 8.10 checklist

| Requirement | Status |
| --- | --- |
| `build.bat` does NOT build ~613 targets | ✅ 86 edges / 3 binaries |
| builds the ZL executable | ✅ `zl_language` |
| builds all required runtime/compiler dependencies | ✅ full closure via CMake, not a hand-written list |
| succeeds from a clean build directory | ✅ verified |
| `build.bat test` builds the restored tests | ✅ 34 targets, 37/37 ctest pass |
| `build.bat full` builds the complete repository | ✅ `zl-full` |
| `build.bat clean` works | ✅ dedicated mode, verifies removal |
| `build.bat help` explains the modes | ✅ |
| Tests remain available, not removed from CMake | ✅ `EXCLUDE_FROM_ALL`, still in `ZL_CTEST_TARGETS` |
| Optional components available through explicit targets | ✅ `zl-tests`, `zl-benchmarks`, `zl-examples`, `zl-full` |
| Core build does not depend on test targets | ✅ asserted by the regression script and by `ninja -t inputs` |

## 6. Windows link-requirement fix (follow-up)

A Windows build of `zl-mir-type-tests` / `zl-mir-ownership-tests` failed with
undefined references to `__imp_WSAStartup`, `__imp_getaddrinfo`, etc. Both link
`ZL_RUNTIME_VM_SOURCES`, which includes `src/vm/native.cpp` (Winsock-based
`Network.resolve`), but neither linked `ws2_32`.

The reported fix was to add `ws2_32` to the two failing targets. The audit
showed the real defect is structural: the `ws2_32` / `Threads::Threads` links
were **copy-pasted per target** across 7 VM-sources tests, and 2 copies were
missing. Adding 2 more copies preserves the trap for the 8th target.

Instead the requirement is attached once, to the source set it belongs to:

```cmake
macro(zl_add_vm_test target primary_source)
    zl_add_required_test(${target} ${primary_source} ${ZL_RUNTIME_VM_SOURCES} ${ARGN})
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()
    list(APPEND ZL_VM_TEST_TARGETS ${target})
endmacro()
```

All 7 VM-sources targets now route through it: `zl-runtime-hardening-vm-tests`,
`zl-native-backend-tests`, `zl-mir-opt-pipeline-tests`, `zl-pipeline-tests`,
`zl-mir-lowering-tests`, `zl-mir-type-tests`, `zl-mir-ownership-tests`. This
also removed a latent bug — the old block set `Threads`/`ws2_32` on
`zl-mir-lowering-tests` *twice* while `zl-mir-ownership-tests` got neither.

A configure-time guard makes the failure impossible to reintroduce, turning a
Windows-only link error at the end of a long build into an immediate,
cross-platform configuration error:

```cmake
foreach(zl_vm_target IN LISTS ZL_VM_TEST_TARGETS)
    get_target_property(zl_vm_libs ${zl_vm_target} LINK_LIBRARIES)
    if(WIN32 AND NOT "ws2_32" IN_LIST zl_vm_libs)
        message(FATAL_ERROR "${zl_vm_target} links the VM sources but not ws2_32 ...")
    endif()
endforeach()
```

Verified by forcing `WIN32` in a scratch project: a hand-registered target
without `ws2_32` aborts configuration with the diagnostic; the compliant one
passes. All 7 targets link cleanly on Linux, and the suite is **37/37**.

### `[[nodiscard]]` warnings

The same Windows run reported `-Wunused-result` warnings. A repo-wide sweep of
every test source found **29** across 4 files, all deliberate discards, fixed
with an explicit `(void)` cast:

| File | Fixed |
| --- | --- |
| `tests/mir_tests.cpp` | 13 |
| `tests/mir_ownership_tests.cpp` | 9 |
| `tests/native_resource_tests.cpp` | 6 |
| `tests/native_boundary_tests.cpp` | 1 |

Re-scanning all test sources afterwards reports **0** remaining. These were
warnings only (no `-Werror`), so no build was ever broken by them.

## 7. Files changed

| File | Change |
| --- | --- |
| `CMakeLists.txt` | target buckets + `zl-core`/`zl`/`zl-tests`/`zl-benchmarks`/`zl-examples`/`zl-full` aggregates; tests marked `EXCLUDE_FROM_ALL`; configure-time boundary report |
| `scripts/build.bat` | mode-driven driver (`core`/`test`/`full`/`clean`/`help`), explicit `--target`, `[ZL]` output, new `--ctest` |
| `scripts/build.sh` | **new** — POSIX twin with identical mode semantics |
| `tools/dev/check_build_modes.py` | **new** — 43-check regression guard for the target boundary and the VM-test link requirements |
| `tests/mir_tests.cpp`, `mir_ownership_tests.cpp`, `native_resource_tests.cpp`, `native_boundary_tests.cpp` | 29 `[[nodiscard]]` discards made explicit |
| `BUILD_TARGETS_REPORT.md` | **new** — this report |
| `README.md`, `docs/development.md` | document the modes and aggregate targets |
