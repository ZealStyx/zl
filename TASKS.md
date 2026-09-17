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
bash scripts/run_regressions.sh build/zl_language all    # 50 fixtures under tests/zl
bash scripts/native_gate.sh build/zl_language            # native tier gate
```

Priorities: **P0** produces a wrong result or refuses a valid program ·
**P1** is a capability the docs promise or a gap users hit daily ·
**P2** is polish, measurement, or ecosystem.

## Start here

The five with the best value-to-risk ratio right now:

| # | Task | Why first |
| --- | --- | --- |
| [P0-1](#p0-1--a-block-body-lambda-with-an-untyped-parameter-is-broken-on-both-pipelines) | Block-body lambda, untyped parameter | A valid program is refused outright on the default pipeline |
| [P2-1](#p2-1--generic-collections-print-their-internal-storage) | `List{__native: …}` printing | One function, and the JSON encoder already has the fix to copy |
| [P1-5](#p1-5--fixed-arrays-through-collection-the-recorded-gap-no-longer-reproduces) | Close or re-prove the fixed-array MIR gap | The corpus claims a failure that no longer happens |
| [P2-2](#p2-2--timeformat-unknown-tokens-fail-silently) | `Time.format("%Y")` fails silently | A wrong answer that looks like a right one |
| [S1-S3](#stale-claims-verified-2026-09-17) | Delete the three stale write-ups | They cost every reader a reproduction |

---

## P0 - wrong results, or a valid program refused

### P0-1 · A block-body lambda with an untyped parameter is broken on both pipelines

```zl
class Lam3 {
    static func twice(func f): int { return f(3) }
    func main(): void { log(Lam3.twice(func(x) { return x * 2 })) }
}
```

- MIR pipeline (the default): `MIR verification error … [mir.return]: in
  Lam3.$lambda0: block b1 returns a value from a void function` - the whole
  program is refused, not just the lambda.
- `ZL_COMPILER=ast`: compiles, then dies at runtime (`argument count mismatch`).
- `func(x) => x * 2` works, and `func(int x) { return x * 2 }` works.

**Done when** both pipelines infer the lambda's signature from the `func`-typed
callee the way the arrow form already does, the reproducer prints `6`, and a
`tests/zl/valid` fixture pins the block and arrow forms side by side.

### P0-2 · The legacy IR folder round-trips a folded double through six decimals

`rewriteInstructionConstants` writes a folded double into the instruction's
`symbol` with `std::to_string` (`src/compiler/ir_optimizer.cpp:301`, also 375,
427, 715) - fixed, six decimals - and `parseConst` reads it back with `stod`
(`src/compiler/ir_optimizer.cpp:76`). Any constant that needs more precision is
silently rounded.

Latent rather than live: the only consumer is the legacy `--emit-native` tier
(`src/compiler/native_compiler.cpp:840`), which currently refuses
double-returning `@native` functions, so no wrong constant has been produced yet.
That is one feature away from being a miscompile.

**Done when** those sites use the runtime's `zl::doubleToShortestString` (exact,
and already the language's one spelling), or the dead path is deleted under
boundary-lint rule 4 - plus a regression that folds a double needing 17 digits.

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

### P1-5 · Fixed arrays through `Collection.*`: the recorded gap no longer reproduces

`research/corpus/limitations/FixedArrayNative.zl` says the element type is lost
(`array[5]<int>` → `list<unknown>`) and the verifier rejects it. It does not:
that fixture verifies clean and runs on the MIR pipeline today, as do a native
`list` stored into an `array[2]<int>` slot and `Collection.get` results pushed
into a `List<int>` - the two shapes
[docs/status/mir-safety-evaluation.md](docs/status/mir-safety-evaluation.md)
records as failures (`heap-contract-workflow`).

**Done when** either a reproducer that still fails is committed in its place, or
the corpus file and the status document are corrected. An unverified "known gap"
is worse than none: it stops people using fixed arrays with the primitives.

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

### P2-1 · Generic collections print their internal storage

```zl
var l = new List<double>()
l.push(3.14); l.push(1.0)
log(l)          // List{__native: [3.14, 1]}
```

[REVIEW.md O11](examples/REVIEW.md) fixed exactly this for `Serialize.encode` -
which now looks through the `List`/`Map`/`Set` wrappers - but the printer
(`formatValueInner`, `src/vm/value.cpp`) still walks the wrapper object's fields.

**Done when** `log(l)` prints `[3.14, 1]` and `log(m)` prints `{"pi": 3.14}`,
reusing the wrapper look-through the encoder already has, with the affected
example and fixture expectations updated.

### P2-2 · `Time.format` unknown tokens fail silently

`Time.format(0, "%Y-%m-%d")` prints `%Y-%m-%d`; the tokens are ZL's own
(`YYYY-MM-DD`, [docs/stdlib.md:237-250](docs/stdlib.md),
[REVIEW.md O10](examples/REVIEW.md)). Documented, and still the easiest way to
print a wrong date confidently.

**Done when** an unknown `%`-token raises, or is passed through with a warning the
caller can see; a fixture covers both a valid pattern and a strftime-shaped one.

### P2-3 · `List.pop()` and `List.first()` disagree about an empty list

`List.pop` delegates to the native `Collection.pop: cannot pop from an empty
list` (`src/vm/native.cpp:302`), while `List.first` throws from ZL with
`List.first called on empty list` (`src/compiler/builtin_library.cpp:101`). Two
voices for one condition - and `List.first` names the collection, `Collection.pop`
names the primitive.

**Done when** both name the type the user declared and the operation they asked
for, and a fixture pins the two messages.

### P2-4 · `Condition` is never exercised from a worker thread

`examples/basics/Conditions.zl` waits and signals on the main thread;
`tests/runtime_sync_tests.cpp` covers the primitive, and
`tests/zl/valid/concurrency_regressions/` has no `Condition` case at all.

**Done when** a concurrency regression waits on a `Condition` inside a spawned
thread, is signalled from another, and passes repeatedly (not once).

### P2-5 · `zlpkg` has no registry

Git URL or local `path` only; exact versions, no ranges, no workspaces, no
dev-dependencies ([docs/packages.md:71-101](docs/packages.md)).

**Done when** the intended scope is decided and written down - a registry is a
service, not a feature, and "no registry yet" should say what replaces it.

### P2-6 · One class per file, stem must match the class name

Enforced by the compiler - the file rule in [README](README.md#run-a-program). Fine as a rule; it is
listed here because nothing states the reason, and every newcomer asks.

**Done when** the rule and its rationale (module identity, import resolution) are
in [docs/packages.md](docs/packages.md), or relaxed for helper types.

### P2-7 · Typed field-by-field C struct schemas

FFI passes opaque buffers; a typed schema per C struct is a later ABI extension
([docs/native.md](docs/native.md)).

**Done when** `zl-bind` emits a typed schema for a struct with mixed field types
and a test reads and writes each field by name.

### P2-8 · `zl-native-resource-stress-tests` is flaky

~7% of runs on a 2-core box (4 of 60 solo, and intermittently under `ctest -j2`):
it prints `all native resource stress regressions passed` and then exits non-zero,
which points after `main` returns - a static-destruction or thread-teardown race.
Pre-existing, and unrelated to formatting or the VM's value path.

**Done when** 500 consecutive solo runs and 50 `ctest -j2` runs are clean, or the
teardown race is named and fixed. Until then a red suite here is not evidence
about your change.

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

### S1 · "`INT64_MIN` cannot be written as a literal" ([REVIEW.md O19](examples/REVIEW.md))

Fixed: the parser folds the exact spelling `-9223372036854775808` into one
negative literal (`src/parser/expression_parser.cpp:102`), and one past it in
either direction is still rejected. Pinned by
`tests/zl/valid/language_hardening_tests/Int64Min.zl` and
`tests/zl/invalid/type_errors/IntLiteralOutOfRange.zl`. O19 still says
"Deliberately **not** fixed" and teaches the `(0 - INT64_MAX) - 1` workaround.

### S2 · "Block-bodied lambdas cannot declare typed parameters"

`func(int x) { return x * 2 }` and `func(int x): int { return x * 2 }` both
compile and run. The real remaining gap is the *untyped* block-body parameter -
[P0-1](#p0-1--a-block-body-lambda-with-an-untyped-parameter-is-broken-on-both-pipelines).

### S3 · "Fixed arrays through `Collection.*` lose the element type; the verifier rejects"

See [P1-5](#p1-5--fixed-arrays-through-collection-the-recorded-gap-no-longer-reproduces):
the corpus file and the status document both still record a verifier rejection
that does not happen.

---

## Done

Most recent first. Kept briefly so the gates that cover each fix are findable,
then deleted - [docs/changelog.md](docs/changelog.md) is the permanent record.

- [x] **F9 · `log(3.14)` printed `3.1400000000000001`** - a double now prints as
  the shortest decimal that reads back as the same bits, in one formatter shared
  by `log`, concatenation, `Text.format` and `Serialize.encode`. Gates:
  `tests/zl/valid/language_hardening_tests/DoubleFormatting.zl` under all four
  backends (`tests/double_format_parity.py`, ctest `double-format-parity`) and
  `testDoubleFormatting` in `tests/runtime_type_tests.cpp`. See the F9 verdict in
  [examples/REVIEW.md](examples/REVIEW.md).
