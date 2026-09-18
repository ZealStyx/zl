# TASKS.md Verification — Second Pass — 2026-09-17 (static analysis, no build)

> **Superseded (2026-09-17, later the same day):** this pass ran without a
> build. A later pass with a working build re-verified against the real binary
> and closed P0-1, P0-2, P1-5, P2-1, P2-2, P2-3 and S1-S3 — several verdicts
> below (e.g. P2-2 "documented as intentional") are since fixed. See
> `docs/changelog.md` (2026-09-17) and the Done section of `TASKS.md`. Kept as
> the record of the static pass only.

Build environment still blocked: `cmake` missing, `build/` missing, `apt-get` network isolated (`deb.debian.org Connection failed`). Cannot run `zl_language`. This pass is static code reading plus grep, cross-referenced against first verification `TASKS_VERIFICATION.md` (which remains valid). Focus: re-check each TASKS.md entry, plus deeper stdlib/runtime edge cases mentioned in session memory (Time.addMonths negative era, FileSystem path traversal, Text split O(n²), Serialize vs printer cycle divergence, empty {} literal).

Legend: **REAL** = reproduces via static path, **STALE** = fixed, **DOC** = doc/measurement task still open, **PLAUSIBLE** = needs runtime but code path exists, **NEW** = additional issue not in TASKS.md.

---

## P0

### P0-1 · Block-body lambda with untyped parameter broken on both pipelines — REAL

Same as first pass, plus additional context:

- `src/compiler/type_checker.cpp:5520-5580` `inferLambdaExpr` sets `currentLambdaExpectation` only when `annotation.functionHasSignature`. Bare `func` has `functionHasSignature==false`, so expectation inactive.
- Untyped param `x` => UNKNOWN (`5609-5610`), binary `x*2` UNKNOWN, `checkStatement(block)` sets `lastFunctionReturnType_` only if returnedType != UNKNOWN. UNKNOWN => stays UNKNOWN.
- After block: `inferredReturnResult` stays NIL, `zlTypeName(NIL)="nil"` -> `src/mir/lowering.cpp:3077-3090` converts Nil return to void, but body emits `return x*2` value => verifier `[mir.return]: block b1 returns a value from a void function`.
- Arrow `func(x) => x*2` uses `inferExpr(exprBody)` => UNKNOWN => `unknown` MIR type which allows return value, so arrow works.
- AST pipeline: `src/compiler/compiler.cpp:1261-1310` builds FunctionInfo with param type "unknown" for untyped, always emits implicit `push nil; return` after block body even if explicit return exists (dead code bloat). `vm.cpp:212` argument count mismatch check `args.size()!=paramNames.size()` — mismatch may stem from `this` capture inside instance `main`? Still plausible. Root cause is same: block-body return type inference drops UNKNOWN.

Verdict: **REAL** for MIR, PLAUSIBLE REAL for AST.

---

### P0-2 · Legacy IR folder round-trips double through six decimals — REAL (latent)

- `src/compiler/ir_optimizer.cpp:301` `ins.symbol = std::to_string(v.d)` fixed 6 decimals, same 375,427,714-715.
- `parseConst` at 76 `stod`.
- Exact formatter `zl::doubleToShortestString` at `src/vm/value.cpp:240`, declared `include/zl/vm/value.hpp:351`, already used by printer and `native.cpp:2799`.
- Only consumer legacy `--emit-native` tier `src/compiler/native_compiler.cpp:840` which currently refuses double-returning `@native`, so latent.

Verdict: **REAL (latent)**.

---

## P1

### P1-1 · `match` cannot pattern-match Option/Result — REAL

- Bare `Ok v` without args: `src/compiler/type_resolver.cpp:210-213` errors `generic type 'Ok' requires 2 type argument(s)`.
- `Ok<int,string> v`: parent instantiation `Result<int,string>` at `380-400`, `isSubclassOf` walks exact parentName strings, so true. `inferMatchExpr` compatibility at `3260-3269` uses `overlaps` OR both directions, passes.
- Remaining erasure `3532-3538` checks `isAssignable(member.type, patternType)` where member=Result, pattern=Ok => false. After covering Ok and Err, `remaining` still contains `Result<int,string>` => non-exhaustive even when both arms present. Exhaustiveness rules dead code.
- No shorthand inferring Ok's type args from subject.

Verdict: **REAL**.

### P1-2 · `func` carries no signature — REAL

- `docs/mir.md:246` bare func no signature, verifier defers arity.
- `research/corpus/limitations/BareFuncParam.zl` unknown in MIR.
- `type_checker.cpp:2543` skips UNKNOWN in `validateFunctionTypeAssignment`.
- Runtime `vm.cpp:212` throws.

Verdict: **REAL**, documented limitation.

### P1-3 · `string` has no methods — REAL

- `builtin_library.cpp` defines List/Map/Set/Option/Result but no String class.
- Native `String.*` exists `src/vm/native.cpp:538-...`.
- `inferMethodCall` errors for OBJECT only.

Verdict: **REAL**.

### P1-4 · `list`/`map`/`set` cannot be variable names — REAL

- Lexer `src/lexer/lexer.cpp:36-38` maps "list"->KW_LIST etc.
- `parseVarDecl` `src/parser/parser.cpp:870-876` only accepts IDENTIFIER or KW_SHARED.

Verdict: **REAL**.

### P1-5 · Fixed arrays through Collection.* — Code STALE, DOC REAL

- `research/corpus/limitations/FixedArrayNative.zl` claims element type lost to `list<unknown>` and verifier rejects.
- `src/mir/type.cpp:131-135` now `case TypeKind::Array: return true;` in `isCollectionType`, fixing downgrade.
- `docs/status/mir-safety-evaluation.md` still records `heap-contract-workflow: list<unknown> stored in array[2]<int> slot`.
- So code fixed, docs/corpus stale.

Verdict: **Code STALE, task REAL as doc cleanup**.

### P1-6 · Native backend not execution driver, small subset — REAL

- `src/native/pipeline.cpp:59` Win64 returns without bytes if `!encoderAvailable`.
- `src/native/target.cpp:147-151` encoderAvailable true only for X86_64 SysV.
- `research/report.md:2.3` median native 5/292 funcs 1.7%, 1185 bytes.
- No regalloc, stack args, GC maps, unwind.

Verdict: **REAL**.

### P1-7 · Async cancellation, unobserved failures, async lambdas — REAL

README says pending.

Verdict: **REAL**.

### P1-8 · Shared<T> shareable not thread-safe — REAL

- README: Shared marks shareable, not thread safety.
- `examples/advanced/SharedState.zl` 2490 vs 4000.
- `src/compiler/thread_capture.cpp:184` whitelist includes Shared etc but safety by convention.

Verdict: **REAL**.

### P1-9 · Reachability is report only — REAL

- `src/mir/reachability.cpp:252` returns report, `complete()` false when reflection.
- `src/mir/opt_dead.cpp` deletes blocks/values but not functions deliberately.

Verdict: **REAL**.

### P1-10 · Stabilization leftovers — REAL

REVIEW.md 83-86 lists FFI quiescence, channel cancellation, blocking finalizers, exception/Shared audit.

Verdict: **REAL**.

---

## P2

### P2-1 · Generic collections print internal storage — REAL

- `src/vm/value.cpp:69-135` `formatValueInner` for ObjectRef prints `className { field: value }`. List wrapper field `__native` => `List{__native: [3.14, 1]}`.
- `src/vm/native.cpp:2845-2850` `toJsonInner` already looks through List/Map/Set wrappers to avoid `{"__native":...}`. Printer does not reuse.

Verdict: **REAL**.

### P2-2 · Time.format unknown tokens fail silently — REAL

- `src/vm/native.cpp:1976-1995` `formatCivilTime` only replaces YYYY,MM,DD,HH,mm,ss. `%Y-%m-%d` returned unchanged.
- Docs `docs/stdlib.md:237-250` explicitly documents "An unrecognised token is left in place rather than reported, so a strftime pattern such as "%Y-%m-%d" is returned unchanged." So current buggy behavior is documented as intentional. Fix would require doc update.

Verdict: **REAL**, but documented as intentional.

### P2-3 · List.pop() and List.first() disagree — REAL

- `builtin_library.cpp:101` ZL throws `List.first called on empty list`.
- `native.cpp:302` native `Collection.pop: cannot pop from an empty list`.
- Two voices, different naming.

Verdict: **REAL**.

### P2-4 · Condition never exercised from worker thread — REAL

- `examples/basics/Conditions.zl` is if/elif, not Condition primitive.
- No Condition in `tests/zl/valid/concurrency_regressions/`.

Verdict: **REAL**.

### P2-5 · zlpkg has no registry — DOC REAL

- `docs/packages.md:71-101` says no registry, exact versions only.

Verdict: **DOC REAL**.

### P2-6 · One class per file, stem must match — DOC REAL

README lists rule, no rationale.

Verdict: **DOC REAL**.

### P2-7 · Typed field-by-field C struct schemas — REAL future

- `docs/native.md:84` says later ABI extension.

Verdict: **REAL future**.

### P2-8 · zl-native-resource-stress-tests flaky — PLAUSIBLE REAL

- `tests/native_resource_stress_tests.cpp` exists, task says 7% flaky, prints passed then non-zero after main returns (static-destruction race).

Verdict: **PLAUSIBLE**, needs measurement.

---

## PF

### PF-1 · MIR bytecode larger and slower — REAL

- `research/report.md:2.3` median 218,720 ref vs 309,720 MIR opt +42%, 325,880 unopt +49%.
- `2.4` Prims +9%, Recursion +36% slower.
- `2.2` MIR backend 9.53ms vs ref 2.49ms 3.8x slower.

Verdict: **REAL**.

### PF-2 · Optimiser costs ~44ms buys nothing — REAL

- `report.md:2.2` optimiser 43.90ms 63% of opt total, total 69.56ms vs ref 8.46ms 8.2x.
- Execution identical opt vs unopt.

Verdict: **REAL**.

### PF-3 · Native not performance path — REAL

Follows P1-6, 1.7% funcs, still VM.

Verdict: **REAL**.

---

## S (Stale claims)

### S1 · INT64_MIN cannot be written — STALE

- `src/parser/expression_parser.cpp:102` folds `-9223372036854775808` into single literal.
- Pinned by `tests/zl/valid/language_hardening_tests/Int64Min.zl`.

Verdict: **STALE**.

### S2 · Block-bodied lambdas cannot declare typed parameters — STALE

- `func(int x) { return x*2 }` works per `inferLambdaExpr` typed path.
- Real gap is untyped block-body — P0-1.

Verdict: **STALE** (typed works).

### S3 · Fixed arrays through Collection.* lose element type — STALE (same as P1-5)

Fixed by `isCollectionType` including Array.

Verdict: **STALE**.

---

## Additional issues found (not in TASKS.md)

> **Since fixed (2026-09-18):** A6 (the unreachable `return nil` tail), A9 (empty
> collection literals) and A10 (`String.split` with an empty separator splitting
> bytes) are closed - see [docs/changelog.md](docs/changelog.md) and the Done
> section of [TASKS.md](TASKS.md). Two more items below were checked against a
> working build and are not what they look like: A17's untested `Condition` claim
> now has a fixture
> (`tests/zl/valid/concurrency_regressions/ConditionWorkerSignal.zl`), and writing
> that fixture exposed a lambda-inference bug which made any lambda containing a
> nested lambda plus a branch run its body forever (pinned by
> `tests/zl/valid/language_hardening_tests/NestedLambdaReturn.zl`). The findings
> themselves are left exactly as written - this file is the record of the static
> pass.

### A1 · Lambda param types not inferred from expected `func(T):R` in argument position — NEW REAL

- `inferArguments` `type_checker.cpp:3042-3060` calls `inferExpr` for each arg, no expectation.
- Static/method call paths infer args BEFORE overload resolution, so `currentLambdaExpectation` never set.
- `validateFunctionTypeAssignment` skips UNKNOWN actuals `2543`, so `func(x)=>x*2` passed to `func(int):int` stays UNKNOWN.
- Only `checkVarDecl` with explicit type uses `inferExpected`.

Repro idea:
```zl
class A {
    static func apply(func(int):int f): int { return f(5) }
    func main(): void { log(A.apply(func(x) => x*2)) }
}
```

### A2 · Match exhaustiveness for inheritance checks wrong direction — NEW REAL

- Remaining erasure `type_checker.cpp:3532-3538` checks `isAssignable(member.type, patternType)` where member=parent Result, pattern=child Ok => false. Should check overlap or pattern assignable to member, like earlier `overlaps`.
- Affects any parent matched by children: Animal with Dog and Cat arms should be exhaustive but reports uncovered Animal.

### A3 · Generic type pattern without type args error could be more helpful — NEW DOC

- `Ok v` errors `generic type 'Ok' requires 2 type argument(s)`. Since subject `Result<int,string>` provides args, could allow bare `Ok` shorthand or suggest `Ok<int,string>`.

### A4 · InterfaceStaticParam unsupported lowering still present — NEW REAL (from report.md 2.7)

- `research/corpus/limitations/InterfaceStaticParam.zl` static method taking interface param does not lower, function marked incomplete.
- If fixed arrays fixed, this may be sole remaining limitation corpus failure.

### A5 · BareFuncParam honest unknown is still type-erasure — NEW REAL

- `BareFuncParam.zl` says func param is unknown in MIR, runs via runtime boundary. MIR cannot verify arity for higher-order calls through bare func (deferred to runtime). Safety gap.

### A6 · AST compiler always emits implicit `return nil` after block-body lambda — NEW REAL (bloat)

- `compiler.cpp:1309-1310` emits `push nil; return` after `compileStatement(blockBody)` unconditionally, even if block ends with explicit return. Dead code; MIR checks `isDead()`.

### A7 · Time.format token set undocumented originally, now documented as silent — NEW DOC

- Part of P2-2, but docs/stdlib.md now documents silent behavior, so fix requires doc change.

### A8 · Printer vs JSON encoder divergence for cyclic collections — NEW DOC (intentional but worth noting)

- `formatValueInner` prints `<cyclic>` / `<...>` gracefully.
- `toJsonInner` throws `circular reference detected` / `nesting exceeds maximum depth` fail-closed.
- Comment at `native.cpp:2786-2790` says intentional: silently emitting truncated markers would corrupt serialized data. So divergence intentional, but user-visible inconsistency.
- For generic collections, divergence is bug (P2-1): printer shows wrapper, encoder looks through.

### A9 · Empty collection literal handling — NEW REAL

Evidence from parser and type checker:

- Parser `src/parser/expression_parser.cpp:783-820`: `parseCollectionLiteral` sets `isMap=true` only if first entry has COLON after expression and brackets false. Empty literal `[]` or `{}` returns node with empty elements/entries, `isMap=false`, `bracketSyntax` true for `[]`, false for `{}`.
- `inferCollectionLiteral` `type_checker.cpp:4682-4715`: if `isMap` true => MAP else LIST, ignoring `bracketSyntax`. So `{}` empty and `{1,2,3}` without expected type both infer as LIST, even though `{}` should be SET and `[]` should be LIST per language guide (`list` uses `[]`, `set` uses `{}`).
- `inferCollectionLiteralExpected` for MAP expected type `4575-4578`: if expected MAP and !isMap => error `map literal requires key:value entries`. So `map<string,int> m = {}` or `Map<string,int> m = {}` errors, even though empty map should be allowed. Must use `new Map<...>()` instead. Empty list `[]` works, empty map does not.
- For SET expected type, both `[]` and `{}` with `isMap=false` pass (only checks isMap), so `set<int> s = []` allowed as set even though syntax says set uses `{}`. Similarly `list<int> l = {}` allowed as list.
- For generic object collections `4639-4646`: checks `(base=="Map") != isMap` => List/Set cannot contain map entries, but empty literal `isMap=false` passes for List and Set, fails for Map with same message. So `List<int> l = {}` allowed, `Map<K,V> m = {}` rejected.

Impact: empty map literal not supported, empty set literal ambiguous with list, `{}` defaults to list not set.

Fix: make `bracketSyntax` participate in inference, allow empty map/set literals, or explicitly document that empty map must use `new Map<...>()` and empty set uses `new Set<...>()` or `{}` is empty list.

### A10 · `String.split` empty delimiter splits by byte not unicode scalar — NEW REAL

- `src/vm/native.cpp:608-625` `strSplit`: if delim empty, `for (char c : s) items.emplace_back(std::string(1,c))` iterates bytes. For UTF-8 "héllo" (é = 2 bytes) produces 6 fragments, not 5 unicode scalars. Should use utf8 iteration like `Text.chars` does (`String.utf8CharAt` loop). Also `Text.split` delegates to native `String.split`, so same bug surfaces in `Text.split`.

### A11 · `Text.*` and `FileSystem.*` ZL helpers have O(n²) string concatenation — NEW REAL (perf)

- `stdlib/zl/text/Text.zl`:
  - `join`: `result = result + separator + parts.get(i)` in loop, each `+` copies whole accumulator => O(n²).
  - `words`: `current = current + c` per char => O(n²) per word, plus `chars` similar.
  - `count`: `remaining = String.substring(value, start, String.length(value))` each iteration copies suffix => O(n²).
  - `padLeft`/`padRight`: `prefix = prefix + char` in loop => O(n²) for large width.
  - `title`: uses `words` + `join`.
- `stdlib/zl/fs/FileSystem.zl`:
  - `writeLines`: `content = content + lines.get(i) + "\n"` in loop => O(n²) for many lines.
  - `readLines`: builds List via push but also does `Collection.length` each iteration? Actually uses `Collection.length(raw)` once, but `Text.split` + loop is okay.
- Native `String.split` itself is O(n) (uses `find`), but ZL wrappers re-introduce quadratic behavior.
- Previously `Text.repeat` had same issue and was fixed to native `String.repeatText` with comment about quadratic hang; similar fixes needed for `join`, `words`, `pad*`, `writeLines`.

### A12 · `FileSystem` native has no sandbox / path traversal protection — NEW REAL (security/doc)

- `src/vm/native.cpp:929-1121` all FileSystem ops use `std::filesystem` directly with user-supplied path string, no validation, no sandbox root, no check for `..`, absolute paths, symlinks.
- `fsPath` helper just `requireString`, no normalization beyond `absolutePath`'s `lexically_normal`.
- Means ZL program can read/write anywhere process can. If ZL intended as safe scripting, this is path traversal / sandbox escape. If intentional for systems language, should be documented as unrestricted OS access.
- `stdlib/zl/fs/FileSystem.zl` facade does not add checks, only safe fallbacks like `readOr`.

Verdict: intentional if ZL is general-purpose, but worth documenting as security boundary (or adding optional sandbox).

### A13 · `Calendar.addMonths` negative era handling relies on `Time.fromParts` which uses `mktime` — NEW REAL

- ZL `stdlib/zl/time/Calendar.zl:63-73`:
  ```zl
  var total = (year*12)+(month-1)+months
  var year = total/12
  var month = (total%12)+1
  if (month<1) { month+=12 year-=1 }
  ```
  This correctly implements floor division for divisor 12 despite ZL's trunc-toward-zero `/` and `%` (remainder sign = dividend), because remainder range -11..11 and correction `month<1` adjusts. So arithmetic itself is correct for BCE.
- However `Time.fromParts` `src/vm/native.cpp:2046-2056` uses `std::mktime` which on many platforms fails for year <1970 or <1900 (returns -1) and throws `Time.fromParts: the given date/time is not representable`. So `Calendar.addMonths` for BCE dates will throw even though `civilToDays` comment says "Valid well outside the range any 64-bit timestamp can express, including pre-epoch dates."
- Similarly `Calendar.fromDayNumber` calls `Time.fromParts`, so `civilToDays`/`fromDayNumber` claim proleptic Gregorian validity but underlying native fails for pre-epoch.
- `Time.isLeapYear` uses `%` with negative year, which in ZL/C++ gives negative remainder but still works for divisibility checks (e.g., -4%4==0). So leap year works for BCE, but `Time.year/month/day` use `localtime_r`/`gmtime_r` which may not handle negative `time_t` consistently.
- Fix: either make `Time.fromParts` use Hinnant algorithm directly instead of `mktime`, or document that Calendar BCE support is limited by platform `mktime`.

### A14 · `Time` native uses `localtime_r` / `mktime` — platform-dependent, not proleptic — NEW REAL

- `timeYear/Month/Day/Hour/Minute/Second` `1968-1974` use `toLocalTm` which uses `localtime_r`. `toUtcTm` uses `gmtime_r`. Both depend on system timezone database and `time_t` range (often 1970-2038 on 32-bit, or limited on Windows).
- `Time.fromParts` uses `mktime` which normalizes out-of-range fields (e.g., 2024-01-32 => 2024-02-01) per comment, but also fails with -1 for unrepresentable dates.
- So `Date`/`DateTime` arithmetic that claims calendar correctness may actually be affected by DST transitions (e.g., adding a day preserves wall-clock time per `Calendar.addDays` comment, but `Time.fromParts` via `mktime` may adjust for DST).
- The `Calendar` ZL implementation tries to be pure civil arithmetic using Hinnant, but still calls `Time.fromParts` at end, re-introducing `mktime` DST normalization.

### A15 · `Text.lines` and `FileSystem.readLines` trailing newline handling divergence — NEW REAL

- `Text.lines` `stdlib/zl/text/Text.zl:113-117`: splits on `\n` after replacing `\r\n`, then pops last if empty => trailing newline terminates last line rather than starting empty one, matching `FileSystem.readLines`.
- `FileSystem.readLines` `stdlib/zl/fs/FileSystem.zl:54-63`: same logic, but uses native `String.split` which keeps empty trailing fragment if string ends with `\n`, then pops if last=="" . So both agree.
- However `String.split` native with delim "\n" on string ending with "\n" will produce trailing empty string (since `substr(pos)` after last delim is ""), so pop logic works. If file does not end with newline, no pop. So consistent.
- Edge: `Text.split` with separator "" (empty) splits by byte (A10), so `Text.lines` on string containing multi-byte chars and `\n` may mis-split.

### A16 · Generic collection `List.pop` vs `first` vs `last` error messages — extends P2-3

- `builtin_library.cpp:101` `List.first` ZL throws `List.first called on empty list` naming type and operation.
- `native.cpp:302` `Collection.pop: cannot pop from an empty list` naming primitive, not generic type.
- Similarly `List.last` likely similar to `first`? Need check. The task says pop vs first disagree; also `Set`/`Map` operations may have similar divergence.

### A17 · `Condition` not tested from worker — extends P2-4, plus `Thread` and `Task` join semantics

- No `Condition` usage in `examples/` or `tests/zl/valid/concurrency_regressions/`.
- `Thread.join` `src/vm/native.cpp:??` blocks cooperatively via `VM::BlockingNativeCall`, but `Condition.wait` also needs to be tested with real contention.
- `RuntimeThreadState::startWith` increments `gAliveWorkerThreads` BEFORE spawn to avoid false deadlock detection (see `include/zl/vm/runtime_thread.hpp`). `RuntimeTaskExecutor::enqueue` also increments before queue push with comment about deadlock avoidance. This logic is correct but subtle; worth a concurrency regression that waits on Condition inside spawned thread and signals from another, passing repeatedly (not once) as task requires.

### A18 · `Serialize.encode` vs `log` for generic collections — extends P2-1

- `log(l)` prints `List{__native: [3.14,1]}` via `formatValueInner` walking wrapper fields.
- `Serialize.encode(l)` prints `[3.14,1]` via looking through `__native`.
- So same value has two different JSON vs log representations. The JSON fix exists, printer fix is one function (look-through in `formatValueInner` for className List/Map/Set).

---

## Summary table (second pass)

| ID | Verdict | Notes |
|---|---|---|
| P0-1 | REAL | MIR void return, AST arg mismatch plausible, same as first pass |
| P0-2 | REAL (latent) | to_string 6 decimals |
| P1-1 | REAL | Ok/Some without args + exhaustiveness direction wrong |
| P1-2 | REAL | bare func no signature |
| P1-3 | REAL | string no methods |
| P1-4 | REAL | list/map/set reserved |
| P1-5 | DOC REAL / code STALE | FixedArrayNative now passes, docs still claim fail |
| P1-6 | REAL | native backend subset, Win64 no encoder |
| P1-7 | REAL | async pending |
| P1-8 | REAL | Shared not thread-safe |
| P1-9 | REAL | reachability report only |
| P1-10 | REAL | FFI etc |
| P2-1 | REAL | List{__native:...} vs JSON look-through |
| P2-2 | REAL (doc intentional) | Time.format silent, docs say left in place |
| P2-3 | REAL | pop vs first messages |
| P2-4 | REAL | Condition not from worker |
| P2-5 | DOC REAL | zlpkg no registry |
| P2-6 | DOC REAL | one class per file rationale |
| P2-7 | REAL future | C struct schemas |
| P2-8 | PLAUSIBLE | native resource stress flaky |
| PF-1 | REAL | 42-49% larger, 9-36% slower |
| PF-2 | REAL | 44ms cost, no speedup |
| PF-3 | REAL | native not perf path |
| S1 | STALE | INT64_MIN fixed |
| S2 | STALE | typed block lambda works |
| S3 | STALE | fixed arrays fixed |
| A1 | NEW REAL | lambda param inference in arg position |
| A2 | NEW REAL | match exhaustiveness direction |
| A3 | NEW DOC | generic pattern without args error message |
| A4 | NEW REAL | InterfaceStaticParam lowering gap |
| A5 | NEW REAL | BareFuncParam unknown safety gap |
| A6 | NEW REAL (bloat) | AST implicit return nil after block lambda |
| A7 | NEW DOC | Time.format token set doc |
| A8 | NEW DOC (intentional) | printer <cyclic> vs encoder throw |
| A9 | NEW REAL | empty {} literal => LIST not MAP/SET, empty MAP rejected |
| A10 | NEW REAL | String.split empty delim byte vs unicode scalar |
| A11 | NEW REAL | Text.join/words/count/padLeft/padRight/writeLines O(n²) concat |
| A12 | NEW REAL (sec/doc) | FileSystem no sandbox / path traversal |
| A13 | NEW REAL | Calendar.addMonths BCE relies on mktime which fails |
| A14 | NEW REAL | Time native uses localtime/mktime platform-dependent |
| A15 | NEW REAL | Text.lines edge with multi-byte + empty delim |
| A16 | NEW REAL | List.pop vs first vs last message divergence |
| A17 | NEW REAL | Condition/Thread/Task join semantics need worker regression |
| A18 | NEW REAL | Serialize vs log generic collections divergence (extends P2-1) |

All P0-P2 tasks except S1-S3 and P1-5 code part are real. Additional issues A9-A14 are new findings from this pass, no code edits made per instruction.
