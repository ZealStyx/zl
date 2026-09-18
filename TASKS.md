# Tasks

The actionable backlog, distilled from [examples/REVIEW.md](examples/REVIEW.md),
[docs/status/mir-safety-evaluation.md](docs/status/mir-safety-evaluation.md),
[research/report.md](research/report.md), [README.md](README.md#current-limitations)
and the reproductions below.

Those documents are the evidence; this file is the work list. It stays lean on
purpose - one line of symptom, one pointer, one acceptance test per task - so it
can be read top to bottom in a minute and picked up by anyone.

Rules of upkeep:

- **A finished task is deleted**, and its fix is written up in
  [docs/changelog.md](docs/changelog.md) with the commit. Only the most recent
  completions are kept in [Done](#done) so the gates that cover them are visible.
- **Every task carries its acceptance test.** "Looks fixed" is not a gate: the
  gates below must all be green before anything here is closed.
- **A claim that no longer reproduces moves to [Stale](#stale-claims-verified-2026-09-17),**
  not to Done. Several of the findings these tasks came from are already fixed,
  and a task list that repeats them sends the next person chasing a ghost.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release           # once
cmake --build build --config Release --target zl-tests   # core + every regression target
(cd build && ctest -C Release)                           # 41 tests, includes both parity scripts
examples/run_all.sh build/zl_language                    # 51 examples, byte-compared to their expected output
bash scripts/run_regressions.sh build/zl_language all    # 57 fixtures under tests/zl
bash scripts/native_gate.sh build/zl_language            # native tier gate
```

Priorities: **P0** produces a wrong result or refuses a valid program ·
**P1** is a capability the docs promise or a gap users hit daily ·
**P2** is polish, measurement, or ecosystem.

## Start here

The three with the best value-to-risk ratio right now:

| # | Task | Why first |
| --- | --- | --- |
| [P1-3](#p1-3--string-has-no-methods) | `string` methods | The most-felt daily gap, and it maps onto existing `String.*` natives - no second implementation |
| [P1-4](#p1-4--list--map--set-cannot-be-variable-names) | `list`/`map`/`set` as names | Either the lexer learns context, or the restriction gets documented where it is hit |
| [P1-1](#p1-1--match-cannot-pattern-match-option--result-reviewmd-o21) | `match` on `Option`/`Result` | The exhaustiveness rules already exist as dead code - scope `isAssignable` before starting |

---

## P0 - wrong results, or a valid program refused

Nothing open right now. The last three - the block-body lambda with an untyped
parameter and the legacy IR folder's six-decimal double round-trip (both closed
2026-09-17), and the lambda-inside-a-lambda that ran its body forever (found and
closed 2026-09-18 while writing the `Condition` fixture below) - are in
[Done](#done) and [docs/changelog.md](docs/changelog.md).

---

## P1 - promised capability, or a daily gap

### P1-1 · `match` cannot pattern-match `Option` / `Result` ([REVIEW.md O21](examples/REVIEW.md))

The exhaustiveness rules for exactly these subjects exist and are unreachable
dead code; `ResultType.zl` / `OptionType.zl` use `if (r.isOk())` because it is
the only option. Both spellings are rejected - `Ok v` before it gets anywhere
(`generic type 'Ok' requires 2 type argument(s)`), `Ok<int,string> v` by the
type-pattern test at `src/compiler/type_checker.cpp:3044`, which routes through
`isAssignable`.

**Done when** `match r { Ok v => … Err e => … }` compiles for a
`Result<int,string>`, exhaustiveness is enforced, and a fixture covers the
`Option` pair too. REVIEW.md is explicit that widening `isAssignable` touches
every assignment in the language - scope it before starting.

### P1-2 · `func` carries no signature ([README](README.md#current-limitations), `research/corpus/limitations/BareFuncParam.zl`)

A `func`-typed slot holds no parameter or return types, so an arity mismatch is
a runtime error (`VM: function 'Bare.$lambda0' argument count mismatch`) and MIR
records the parameter as `unknown`.

**Done when** a declared `func(int, string): bool` is checked at the call site at
compile time, bare `func` keeps working for compatibility, and the README
limitation is deleted.

### P1-3 · `string` has no methods

`s.length()` → `type error: cannot call method 'length' on value of type string`;
everything goes through `Text.length(s)` / `String.*`.

**Done when** a small intrinsic surface (`length`, `substring`, `contains`,
`startsWith`, `indexOf`, …) maps onto the existing native `String.*` primitives -
no second implementation - with a fixture and an updated
[docs/language-guide.md](docs/language-guide.md).

### P1-4 · `list` / `map` / `set` cannot be variable names

`var list = new List<int>()` → `syntax error: Expected variable name -- got "list"`.
Reserved as type spellings, with no escaping hatch.

**Done when** either they are usable as identifiers where no type can appear
(context-sensitive lexering, with a fixture for each ambiguity), or the
restriction is stated in [docs/language-guide.md](docs/language-guide.md) next to
the reserved-word list instead of being discovered at the keyboard.

### P1-6 · Native backend: not an execution driver, and a small subset

- `--backend native` generates machine code and still executes on the VM
  (`zl --help` says so outright); mixed-mode native execution is unimplemented.
- The subset is ~1.7% of the functions in a realistic module
  ([research/report.md](research/report.md) §2.7) - ints and floats, no refs,
  objects, collections, closures, exceptions or async.
- Win64 is select-only (`TargetMachine::encoderAvailable()` false,
  `src/native/pipeline.cpp:59`); there is no arm64 encoder, so macOS CI
  cross-compiles to SysV.
- No register allocator, no stack arguments, no GC maps or safepoints, no unwind
  tables, no object-file or JIT writer.
- An arithmetic fault in native bytes is `SIGILL`, not a catchable
  `ArithmeticError`.

**Done when** one value class at a time joins the subset - refs and objects are
the useful next step - and a named program (start with
`tests/zl/valid/native/NumericKernel.zl` plus collections) runs natively end to
end with a measured wall time against the VM.

### P1-7 · Async: cancellation, unobserved failures, async lambdas

`async func`, `Task<T>`, `await` and `block()` work; cancellation propagation,
unobserved-failure reporting and async lambdas are pending
([README](README.md#current-limitations)).

**Done when** a cancelled task propagates to the tasks it spawned, a task dropped
without `block()`/`ignore()` reports its failure instead of vanishing, and
`async func(x) => …` parses - each with a `tests/zl/valid/concurrency_regressions`
fixture.

### P1-8 · `Shared<T>` is shareable, not thread-safe

Measured, not hypothetical: two threads doing 2000 unsynchronised increments each
land on 2490, while `Atomic` lands on 4000 exactly
([REVIEW.md O20](examples/REVIEW.md), `examples/advanced/SharedState.zl`). The
top-level `share()` helper and compile-time confinement checks are pending.

**Done when** either confinement is checked at compile time or `share()` exists
and the docs stop implying safety; the 2490 measurement becomes a regression that
must fail loudly rather than silently.

### P1-9 · Reachability is a report; nothing deletes dead code

`src/mir/reachability.cpp` produces a complete, over-approximated report and
`src/mir/opt_dead.cpp` deletes dead blocks and values - but never a function,
deliberately, because reflection reaches functions by name. Dead functions are
still lowered, verified, optimised and emitted.

**Done when** the boundary is made precise: whatever the report can prove
unreachable *given* reflection and unpinned function values is removed, with the
argument written down and a measured bytecode-size delta on the benchmark corpus.

### P1-10 · Stabilization leftovers ([REVIEW.md:83-86](examples/REVIEW.md))

FFI callback quiescence and ownership; channel cancellation and progress
guarantees; blocking native-resource finalizers; the remaining exception and
`Shared` audit.

**Done when** each has a fixture that would have caught the original report, or a
written argument that the behaviour is correct as it stands.

---

## P2 - polish, measurement, ecosystem

### P2-5 · `zlpkg` has no registry

Git URL or local `path` only; exact versions, no ranges, no workspaces, no
dev-dependencies ([docs/packages.md](docs/packages.md#external-dependencies-zlpkg) -
the pointer is an anchor, not a line range, because this file's own edits keep
moving the lines).

**Done when** the intended scope is decided and written down - a registry is a
service, not a feature, and "no registry yet" should say what replaces it.

### P2-7 · Typed field-by-field C struct schemas

FFI passes opaque buffers; a typed schema per C struct is a later ABI extension
([docs/native.md](docs/native.md)).

**Done when** `zl-bind` emits a typed schema for a struct with mixed field types
and a test reads and writes each field by name.

### P2-9 · An empty literal in argument position still has nothing to declare it

Found 2026-09-18 while closing A9. The empty-literal fix reads the container kind
from the *declaration*, and in argument position there is no declaration yet:
arguments are inferred before overload resolution runs. So `map<string,int> empty = {}`
followed by `countEntries(empty)` compiles, while `countEntries({})` against
`static func countEntries(map<string,int> m)` is still
`no overload of 'countEntries' matches the given argument types` - on every backend.
[docs/language-guide.md](docs/language-guide.md#empty-literals) tells users to bind
first; that is a workaround, not an answer.

The real fix is expected-type propagation into call arguments, which is exactly the
plumbing already scoped as P1-1 and as A1 in
[TASKS_VERIFICATION.md](TASKS_VERIFICATION.md) - it should not be attempted as a
one-off, and it must not disturb the non-empty `{1, 2}` case (TextLib.zl:61 relies on
it being a variadic list).

**Done when** `countEntries({})` compiles against a `map<K,V>` parameter on all four
configs, `Text.format("{0}", {1})` still passes a list, and
`tests/zl/valid/core_tests/EmptyCollectionLiterals.zl` covers the argument-position
form directly instead of only the bound one.

---

## Performance ([research/report.md](research/report.md))

Measured, not estimated. Highest-value gap if you care about speed.

### PF-1 · MIR bytecode is larger and slower than the reference compiler

~42-49% more bytecode (§2.3) and ~9-36% slower on compute-heavy programs (§2.4:
Prims +9%, Recursion +36%); the MIR backend is ~3.8× slower to *run* than the
reference code generator (§2.2).

**Done when** bytecode size is within 10% of the reference on the corpus, or the
wall-time delta is explained instruction by instruction and the difference is
bought back where it matters.

### PF-2 · The optimiser costs ~44 ms and buys nothing at runtime

No loop transforms; the default compile is ~8× the reference path, and for small
programs the fixed ~44 ms dominates (§2.2, §2.4, §5.2).

**Done when** there is either a time budget (scale passes to program size) or a
transform that measurably pays for itself on the corpus.

### PF-3 · Native is not a performance path yet

Follows from [P1-6](#p1-6--native-backend-not-an-execution-driver-and-a-small-subset):
~1.7% of functions, and the program still executes on the VM.

---

## Stale claims (verified 2026-09-17)

Findings that are written up as open and no longer reproduce. Delete or correct
the write-up - each one costs the next reader a full reproduction.

None recorded right now. S1 (REVIEW.md O19, `INT64_MIN` literal), S2 (the
block-bodied-lambda claim in `research/report.md` §2.7) and S3 (the fixed-array
`Collection.*` gap in the corpus and the status document) were corrected or
confirmed already fixed on 2026-09-17; see [Done](#done) and
[docs/changelog.md](docs/changelog.md).

---

## Done

Most recent first. Kept briefly so the gates that cover each fix are findable,
then deleted - [docs/changelog.md](docs/changelog.md) is the permanent record.

- [x] **P0-3 · A lambda created inside a lambda body handed the outer lambda its
  return type** (found 2026-09-18 while writing the P2-4 fixture) - the checker
  infers a block body's return from an accumulator every `return` writes into, and
  a nested lambda left its own result in it, so
  `func() { var n = callIt(func() { return 1 }) if (n == 1) { log("y") } }` was
  typed `func(): int`: a non-void body with no return, which MIR lowered to an
  `unreachable` exit block and the VM ran as an endless loop (the reference
  compiler died with `type assertion failed for return: expected int, got void`).
  The accumulator is now saved and restored around each lambda. Gates:
  `tests/zl/valid/language_hardening_tests/NestedLambdaReturn.zl` under MIR,
  `ZL_COMPILER=ast`, `ZL_MIR_OPT=0` and `--backend native`; the fixture did not
  terminate at all before the fix.
- [x] **P2-4 · `Condition` was never exercised from a worker thread** - Gates:
  `tests/zl/valid/concurrency_regressions/ConditionWorkerSignal.zl`: twenty rounds
  of wait-until-signalled across two threads, the bare `Condition.wait` /
  `notifyAll` primitive blocking and waking a worker, and `Condition.waitFor`
  reporting its timeout from a worker. Writing it is what surfaced P0-3.
- [x] **P2-6 · One primary type per file - the rule and its reason are written
  down** - [docs/packages.md](docs/packages.md#one-primary-type-per-file): an
  import names a type and resolves by path alone, so the stem match is what lets
  one name be both the file and the type; the dotted name is also the module
  identity that diamond, cycle and duplicate-declaration detection key on. The
  rule is narrower than it was documented to be: a `data`, `interface` or `enum`
  satisfies it, and helper declarations may share the file (they travel with the
  primary import, they just are not importable by name). README and
  [docs/language-guide.md](docs/language-guide.md) corrected to match.
- [x] **P2-8 · `zl-native-resource-stress-tests` was flaky - three assertions in
  the test, not a teardown race after `main`** - (1) a borrow that goes invalid
  because the owner was consumed between the lookup and the check is the lifetime
  tracking *working*, and was counted as a dangling handle; (2) the "hammer the
  drained pool" phase ran before the pool was drained - leaving the claim loop
  only means every token was claimed, not consumed - so it stole tokens its
  claimer had not reached; (3) two 5 ms sleeps assumed the invoker threads would
  be scheduled inside them, which on a loaded 2-core box they were not. All three
  now wait for the fact they assert on. Gates: 500 solo runs and 50 `ctest -j2`
  runs clean, plus 300 runs with two cores busy (before: 3 failures in 6 loaded
  runs, 7 in 40 solo).
- [x] **An empty `{}` was refused by a map-typed slot and inferred `list` with no
  declaration** (TASKS_VERIFICATION.md A9) - `map<string,int> m = {}` and
  `Map<string,int> m = {}` failed with "map literal requires key:value entries",
  and `var s = {}` inferred `list<unknown>`, so `set<int> t = s` was refused with
  a MIR type-flow violation. An empty literal now takes its container from the
  declaration, or from its spelling when there is none (`[]` list, `{}` set); a
  *non-empty* `{1, 2}` stays a list, because that spelling is also how a variadic
  argument list is written. Gates:
  `tests/zl/valid/core_tests/EmptyCollectionLiterals.zl` under MIR, ast, unopt and
  native; [docs/language-guide.md](docs/language-guide.md#empty-literals).
- [x] **`String.split(s, "")` split bytes, not characters**
  (TASKS_VERIFICATION.md A10) - `String.split("héllo", "")` returned six
  fragments with `é` cut in half; it now walks the same UTF-8 scalars as
  `String.utf8CharAt`, and `Text.split` inherits it. Gates: the split cases in
  `tests/zl/valid/language_hardening_tests/Utf8Text.zl`;
  [docs/stdlib.md](docs/stdlib.md#zltext).
- [x] **Every function body carried an implicit `return nil` the VM could not
  reach** (TASKS_VERIFICATION.md A6) - a `return` does not fall through, so the
  tail after a body that ends in one (or in an if/else whose arms both return) was
  dead bytes on the reference pipeline. Now emitted only where control flow can
  actually reach the end of the body. Gates:
  `testUnreachableReturnTailIsNotEmitted` in `tests/pipeline_tests.cpp`, which
  counts returns per function and also pins the conservative direction (an `if`
  with no `else` keeps its tail); it fails 4 checks against the old behaviour.
- [x] **`zl-runtime-sync-tests` failed whenever the machine was quiet** - the
  documented gate (`ctest -C Release`, no `-j`) failed it 199 runs in 200 on an idle
  2-core box and passed 30 in 30 with both cores busy: `testReadWriteLockState`
  asserts "readers actually ran" while two writers own both cores and the four
  readers are never scheduled until `writersDone` is set. Writers now wait for every
  reader's first read (a fact the test establishes instead of hoping for), and
  readers yield between reads so four spinners do not starve the writers' exclusive
  lock. Gates: 100 solo runs and 25 loaded runs clean, 0.095 s per run - 4.0 s with
  the handshake but without the yield.
- [x] **The boundary lint's own suite reported phantom failures under load** -
  `printf '%s\n' "$output" | grep -q` with `set -o pipefail`: grep exits at the
  first match, printf dies of SIGPIPE, and the pipeline reports failure - so
  `boundary-lint-regressions` announced "diagnostic does not name src/main.cpp:3"
  while printing that line underneath, and `ctest -j2` went red for reasons that
  had nothing to do with the change under test (2 of 2 runs before, 0 of 3 after;
  4 false negatives in 200 loaded trials of the old shape, 0 in 200 of the new).
  The same shape in `tools/boundary_lint.sh`'s ignore-pattern check would have
  meant an ignore silently not ignoring. Both are pipe-free now.
- [x] **P0-1 · A block-body lambda with an untyped parameter was refused on both
  pipelines** - the checker keeps an UNKNOWN return for a bare-`func` block body
  the way the arrow form always did. Gates: reproducer prints `6` under MIR,
  `ZL_COMPILER=ast`, `ZL_MIR_OPT=0` and `--backend native`;
  `tests/zl/valid/language_hardening_tests/LambdaBlockBody.zl` in
  `scripts/run_regressions.sh`.
- [x] **P0-2 · The legacy IR folder round-tripped a folded double through six
  decimals** - every double-writing site in `src/compiler/ir_optimizer.cpp` now
  uses `zl::doubleToShortestString`. Gates: `testOptimizerFoldsDoublesExactly`
  in `tests/ir_tests.cpp` (17-digit fold, negation, bit-exact round-trip).
- [x] **P2-1 · Generic collections printed their internal storage** - the
  printer reuses the encoder's `List`/`Map`/`Set` look-through, so `log(l)` is
  `[3.14, 1]`. Gates: `tests/zl/valid/core_tests/CollectionPrinting.zl`;
  `docs/language-guide.md` and REVIEW.md O11 updated.
- [x] **P2-2 · `Time.format` unknown tokens failed silently** - an unrecognised
  `%`-token raises, naming the token set. Gates:
  `tests/zl/valid/language_hardening_tests/TimeFormatTokens.zl`;
  `docs/stdlib.md`, `examples/advanced/TimeLib.zl` and REVIEW.md O10 updated.
- [x] **P2-3 · `List.pop()` and `List.first()` disagreed about an empty list** -
  `List.pop` guards emptiness in ZL; the raw `Collection.pop` keeps its own name
  when called directly. Gates:
  `tests/zl/valid/core_tests/EmptyListErrors.zl`; REVIEW.md O18 note updated.
- [x] **P1-5 / S1-S3 · Stale write-ups corrected** - the fixed-array
  `Collection.*` "gap" re-verified as fixed (`FixedArrayNative.zl` runs clean on
  both pipelines), O19/research report/status document now record reality.
- [x] **F9 · `log(3.14)` printed `3.1400000000000001`** - a double now prints as
  the shortest decimal that reads back as the same bits, in one formatter shared
  by `log`, concatenation, `Text.format` and `Serialize.encode`. Gates:
  `tests/zl/valid/language_hardening_tests/DoubleFormatting.zl` under all four
  backends (`tests/double_format_parity.py`, ctest `double-format-parity`) and
  `testDoubleFormatting` in `tests/runtime_type_tests.cpp`. See the F9 verdict in
  [examples/REVIEW.md](examples/REVIEW.md).
