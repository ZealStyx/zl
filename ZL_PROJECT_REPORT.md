# ZL — Project Report for Non-Technical Readers

**Prepared:** 2026-09-23
**Subject:** The `ZealStyx/zl` repository — the ZL programming language and its complete toolchain
**Snapshot revision:** `792a5f3` (branch `main`)
**Audience:** reporting, management, stakeholders — no coding background assumed

Every number in this report was counted or measured from the repository itself. Where a
figure comes from the project's own measurement work, the source file is named so it can
be checked. Section 12 is a glossary of the technical words used earlier.

---

## 1. The one-paragraph summary

ZL is a **programming language built from scratch**, together with everything needed to
actually use it: the program that reads ZL code and runs it, a library of ready-made
building blocks, a package manager for pulling in other people's code, editor tooling, a
test runner, 52 worked examples, and roughly 8,000 lines of written documentation. It is
written in C++ (about 64,900 lines) and runs on Linux, macOS, and Windows.

The design goal is a language that is **easy to read and hard to get silently wrong**.
Most of its distinctive features exist for one reason: to turn mistakes that would
normally show up in production — a wrong number, a crash, two parts of a program
fighting over the data — into a clear message at the moment the code is written.

The second thing worth reporting is **how it was built**. The repository is a textbook
example of agile, evidence-driven development: small increments, each one shipped with a
written test that proves it works, a backlog that is kept honest by deleting finished
work, and performance claims that were re-measured and publicly corrected when the
measurement contradicted the earlier claim. Section 9 covers this in detail.

---

## 2. What is actually in the box

| Piece | What it is, in plain words | Size (counted) |
| --- | --- | --- |
| `src/`, `include/` | The compiler and the runtime engine, written in C++ | 64,916 lines, 166 files |
| `stdlib/` | The standard library — ready-made parts (math, text, time, files, networking, encryption, logging, testing, threads) written **in ZL itself** | 26 packages, 1,923 lines |
| `zlpkg/` | The package manager — fetches other people's code libraries | 1,713 lines |
| `tools/` | Editor support (`zl-lsp`), a test runner (`zl-test`), a C-binding generator (`zl-bind`), plus the automated architecture checks | 4,015 lines |
| `examples/` | Runnable, tour-style lessons from "hello world" to threads and async | 52 examples + 9 support files |
| `tests/` | The regression corpus — programs whose correct behaviour is pinned down forever | 22,955 lines, 117 ZL fixtures |
| `docs/` | Written documentation, including a dated development log | 7,961 lines, 16 documents |
| `research/` | An independent, measured evaluation of the compiler's own architecture | 16-program benchmark corpus + report |
| `benchmarks/`, `packaging/`, `scripts/` | Performance gates, installer packaging, build helpers | ~2,000 lines |

**Total: 559 files.** This is not a prototype or a proof of concept; it is a self-contained
product with documentation, tests, installers, and editor tooling.

---

## 3. How it works — the same idea explained twice

### 3.1 The translation pipeline (analogy version)

A programming language is a translation service. A human writes instructions in a
readable form; a machine needs them in a form it can execute. ZL's translator has six
stations, and each one has a single job:

| Station | Job | Everyday analogy |
| --- | --- | --- |
| 1. **Lexer** | Cuts the text into individual words and symbols | Separating a paragraph into words |
| 2. **Parser** | Checks the words form legal sentences and builds a map of them | A grammar checker that also draws a sentence diagram |
| 3. **Type checker** | Works out what kind of thing every value is, and rejects nonsense | A proofreader who notices you tried to add a name to a number |
| 4. **MIR** (the blueprint) | Rewrites the checked program into one neutral, fully-labelled internal form | An architect's drawing — complete, unambiguous, and the *only* thing the builders get |
| 5. **Verifier + optimiser** | Inspects the blueprint against the rulebook, then simplifies it without changing its meaning | A building inspector, then a value-engineer who removes waste but may not alter the design |
| 6. **Backend** | Turns the blueprint into something executable | Two different construction crews: one builds for the in-house engine, one produces real machine code |

The critical design decision is station 4. **MIR is the single hand-off point.** Everything
above it decides *what the program means*; everything below it decides *how to execute it*.
The two construction crews are forbidden from talking to the architect. They receive the
identical approved drawing and nothing else.

Why this matters commercially: it is the difference between a language whose behaviour
depends on which setting you compiled with, and a language where that is provably
impossible. ZL proves it — `zl --pipeline-report` prints a fingerprint of the blueprint
given to each backend and refuses to continue if the two differ, and an automated script
runs all 52 examples three separate ways and compares what each printed. Current result:
**51 of 51 identical**.

### 3.2 The same thing, technically

```text
ZL source → lexer/parser → semantic analysis → typed lowering → MIR
          → MIR verification → MIR optimisation → selected backend
                                                ├── bytecode → VM
                                                └── native   → machine code
```

Two backends exist. The **bytecode backend** covers the whole language and feeds the
shipped stack VM. The **native backend** emits real x86-64 machine code for a deliberately
small, explicitly named subset of functions and refuses everything else by name and reason.
Neither is privileged, and adding a third would not require changing anything above the
boundary (`docs/pipeline.md`).

Three rules are enforced mechanically, not just written down:

1. A backend reads MIR and nothing else — no access to the source text, the parser, or the
   type checker, directly *or* through a chain of includes.
2. The lowering step invents no types of its own; it copies the type checker's recorded
   answers. A second opinion about types would be a second type system.
3. A fact a backend needs but MIR lacks is treated as a bug in MIR, never patched around
   in the backend.

`tools/boundary_lint.sh` walks the include graph of every backend file to a fixed point
and **fails the build** if any of these are violated. It has its own regression tests,
which feed it deliberately broken trees to prove it still catches each violation.
Architecture rules that are only written in documents decay; rules that break the build
do not.

---

## 4. The language, feature by feature, in easy words

Each row is a real feature of ZL. "What it means" avoids jargon; "Why you'd care" is the
business or reliability consequence.

| Feature | What it means | Why you'd care |
| --- | --- | --- |
| **Static typing with inference** (`var`, `let`, `int`, `double`, `string`, `bool`) | The compiler knows what kind of data every value holds, and can usually work it out without being told | Whole categories of typo-level bugs are caught before the program ever runs. `let` additionally marks a value that can never be reassigned, so accidental changes become impossible rather than unlikely |
| **Checked arithmetic** | Adding, multiplying, or dividing whole numbers that overflow or divide by zero produces a loud error, not a wrapped-around wrong number | This is the class of defect behind famous engineering failures. Most languages quietly return garbage. ZL refuses to |
| **Classes, records (`data`), enums, interfaces, inheritance** | The usual toolbox for organising code into meaningful shapes, with `public` / `private` / `protected` control over what other code may touch | Methods default to `private`, so a programmer must deliberately expose something. Safe-by-default beats safe-by-remembering |
| **Real generics** (`List<int>`, `Map<string,int>`, `Set<T>`) | One implementation written once, safely reusable for any data type, with the type checked at compile time | No re-writing the same list logic per data type, and no runtime surprise when the wrong type goes in |
| **Lambdas and function types** (`func(x) => x * 2`, `func(int, string): bool`) | Small functions can be written inline and passed around as values; a function's signature can itself be a declared type | Enables the compact, expressive style modern developers expect, while the signature is checked where it is called |
| **Operator overloading** (`operator +(...)`) | You can define what `+` means for your own type — for example, adding two Money amounts | Code reads like the problem domain instead of like machinery |
| **`Option<T>` and `Result<T,E>` with `match`** | Instead of a value that might be "nothing" (the null that causes crashes), absence and failure are *types*, and the compiler forces you to handle both cases | Directly attacks the single most common cause of application crashes industry-wide. The compiler will not accept a program that ignores the failure branch |
| **`async`, `await`, `Task<T>`, threads, channels, locks, atomics** | Built-in support for doing several things at once, with coordination primitives in the standard library | Concurrency is where the hardest bugs live. ZL's version is checked (see next row) |
| **Compile-time thread confinement** | Passing work to a thread may only carry data that is *safe* to carry. Anything else is a compile error, with a message naming the safe alternative | This is the standout safety feature. Data races — two threads corrupting shared data — are among the most expensive and least reproducible bugs in software. ZL rejects most of them before the program runs |
| **Automatic memory management (tracing garbage collection)** | The program never has to remember to give memory back; an automatic collector does it | Eliminates the memory leaks and "freed memory still in use" crashes that plague languages without it |
| **Ownership annotations (`owned`, `borrow`, `move`, `memory`)** | An in-progress layer that lets a programmer state who is responsible for a piece of data, checked both by the type checker and again over the program's control flow | The documented direction is ownership *combined with* garbage collection — safe by default, explicit when sharing. The contract is checked today; the memory-management lever it will drive is documented as not yet built |
| **Exceptions, `try`/`finally`** | Errors can be raised and caught, and cleanup runs even on the way out | Ordinary, expected error handling rather than program termination |
| **Reflection (`Type.*`)** | A program can ask questions about its own structure at runtime | Needed for serializers, test frameworks, and plugin systems |
| **`@native` and `@ffi`** | Opt-in attributes that let ZL call compiled code and bind to existing C libraries | The escape hatch that makes a young language practical: it can reach the enormous existing ecosystem of C libraries instead of waiting for native re-implementations |
| **Java-style dotted imports, one primary type per file** | `import zl.time.Date`; each file declares one main type whose name matches the filename | A strict convention, and a deliberate one: an import always resolves to exactly one predictable place. The rationale is written down in `docs/packages.md` rather than left as folklore |
| **Standard library written in ZL itself** | Most of the library is ordinary ZL code; only the VM, OS access, parsing engines, and storage primitives stay native | The library is readable and modifiable by anyone who knows the language, and it dogfoods the language daily — the best possible test of whether the language is usable |

### 4.1 What ZL looks like

```zl
class Hello {
    func main(): void {
        var names = new List<string>()
        names.push("Ada")
        names.push("Grace")
        names.push("Alan")
        names.forEach(func(n) => log("Hello, " + n))
    }
}
```

Readable by someone who has never programmed. That is deliberate.

---

## 5. Advantages, stated as reporting points

**1. Errors surface at writing time, not at 3 a.m. in production.**
Static types, checked arithmetic, `Option`/`Result`, and compile-time thread-confinement
checks all move failures earlier. The later a defect is found, the more it costs; ZL is
architected around finding them as early as a language can.

**2. It fails loudly instead of guessing.**
This is a consistent, repo-wide principle, and it is the single most important quality
signal. An unknown backend name is a usage error, never a silent fallback. A function the
native backend cannot compile is refused *by name and reason*. A program is never handed
to a backend without verification — with verification disabled, code generation simply
refuses. Partial compilation is not accepted as a certificate. Every exit code has a
documented meaning. A tool that admits what it cannot do is a tool you can trust with the
parts it says it can do.

**3. Two execution paths with provably identical meaning.**
The interpreter path covers the whole language today; the native machine-code path is the
growth route. Because both consume the same verified blueprint, adding speed does not risk
changing behaviour — and that claim is executed by tests rather than asserted in a slide.

**4. Architecture is enforced by the build, not by convention.**
The boundary lint fails CI if a backend ever reaches for the source text or the type
checker, including through indirect include chains. New contributors cannot erode the
design by accident.

**5. Documentation that argues instead of advertising.**
The README has a "Current limitations" section. `decimal` is documented as *not* a
base-10 type but a synonym for binary floating point, with the consequence spelled out.
`Shared<T>` is documented as making a capture *legal, not safe*, and a measurement program
runs on every build to prove the unsynchronised case still loses updates — it is written
to **fail** if the documentation ever becomes stale. That is unusual discipline.

**6. Portable and self-contained.**
Plain CMake, C++17, MSVC/GCC/Clang, no exotic dependencies. An installed runtime finds its
own bundled standard library with no environment variables set. CI builds and tests on
Linux, macOS, and Windows on every push.

**7. A measured performance story, including the reversals.**
Dead-code elimination cut bytecode size by **81.3%** across the corpus (17.5 MB → 3.3 MB)
and **86.6%** against the reference compiler. The optimiser went from costing 43.9 ms
median per program to 1.9 ms — it now pays for itself. Compiled native kernels measured
**~140–150× faster** than their interpreted twins. And where a claim was wrong, the report
says so: native compilation currently covers about **1.7% of functions** in a realistic
module, so native is *not yet* a whole-program performance path. The gap is identified
precisely as subset coverage, not code quality.

**8. Practical ecosystem pieces exist already.**
A package manager (`zlpkg`) with git/path dependencies and a lockfile pinning exact
commits; editor support that is fast enough to run on every keystroke; a cross-platform
test runner; a binding generator that produces typed, field-by-field access to C structs
with layout checked by compile-time assertions.

---

## 6. Honest limitations — the same information a sceptical reviewer would find

Reporting these is a strength, not a weakness: they are documented in the repository, not
discovered by us.

| Limitation | Status |
| --- | --- |
| **Native (machine-code) compilation covers only a small subset** | ~1.7% of functions in a realistic module; the whole-program path still runs on the VM. The execution driver currently handles integer-only and double-only signatures; references and objects stop at the garbage-collector map problem. This is the only remaining major workstream (tasks P1-6 and PF-3) |
| **`decimal` is not a base-10 decimal type** | It is a synonym for 64-bit binary floating point, so `0.1 + 0.2` is not exactly `0.3`. Documented explicitly. Any financial-rounding use needs a real decimal type first |
| **Async is cooperative** | A cancellation request is observed at the task's next `await`. A purely synchronous spawned task cannot notice mid-run |
| **`zlpkg` has no central registry** | Dependencies resolve by git URL or local path only; no version ranges, workspaces, or dev-dependencies. This was *decided*, not deferred — a registry is a hosted service requiring curation and is documented as out of scope, with the reasoning written into `docs/packages.md` |
| **One primary type per file, matching the filename** | A real constraint on code organisation, with its rationale documented and enforced by the compiler |
| **Non-string-keyed maps scan linearly** | `map` is insertion-ordered by construction. String keys are O(1); other key types are O(n) per lookup. Documented with the practical advice that follows |
| **Interpreter speed** | Bytecode execution speed is unchanged by the recent optimisation work and remains an open item tied to the native line |
| **Licensing** | PolyForm Noncommercial License 1.0.0 with Runtime Exception — **noncommercial use**. Any commercial deployment needs a separate arrangement. This is a business decision that should be surfaced early in any adoption conversation |

**One documentation defect we found while auditing:** `examples/README.md` and the root
`README.md` still say "46 examples / 46/46 PASS". The repository actually contains **52**
example programs, and the current gate result quoted elsewhere in the repo is 52/52. A
stale count in a project whose stated rule is "a claim that no longer reproduces gets
corrected". Trivial to fix, and worth fixing precisely because the project's credibility
rests on that rule.

---

## 7. How it is built — the engineering process, step by step

### 7.1 Building and running it

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release          # configure
cmake --build build --config Release --target zl-core   # build the toolchain
build/zl_language examples/basics/HelloWorld.zl         # run a program
```

The build is organised in four layers, so a developer's normal build is not a whole-repo
build: `zl-core` (the toolchain — three binaries), `zl-tests` (core plus all 36 test
executables), `zl-benchmarks` / `zl-examples`, and `zl-full` (everything). Identical
wrapper scripts exist for Linux/macOS and Windows with the same modes, and installation
produces a clean tree with the launcher, runtime, package manager, bundled standard
library, and public headers.

### 7.2 The quality gates

Every claim the project makes is executed by a check. These run on every push:

| Gate | What it proves | Current result |
| --- | --- | --- |
| CTest suite | 36 unit/regression test executables covering the VM, garbage collector, MIR contract, lowering, optimiser, native backend, FFI, scheduler, package manager | **42/42** |
| Example corpus | All 52 examples run and their output is **byte-compared** to the expected output recorded in each file | **52/52** |
| Regression fixtures | ZL programs under `tests/zl/valid` and `tests/zl/invalid` — the second category asserts the compiler *rejects* what it should reject | **91/91** in the documented suite (117 `.zl` files present, including support and helper files) |
| Differential harnesses | The same programs run through the reference compiler, the bytecode backend, and the native backend, comparing printed output, exit status, and blueprint fingerprints | **51/51 identical**, on four separate harnesses |
| Boundary lint | The architecture rules from §3.2, including its own regressions proving each rule still fails on a broken tree | Pass |
| Native gate | The legacy native-compile path plus benchmark parity | Pass |
| Cross-platform CI | Linux runs the full gate; macOS and Windows build and run CTest so "portable" is verified rather than assumed | 3 jobs |

The CI configuration is worth a note for anyone who has managed engineering teams: no
check in it is special-cased for CI — it runs exactly the commands a developer runs by
hand. Its comments explain two deliberate details that are easy to delete by accident
(bounded parallelism so a small runner does not die of memory exhaustion and look like a
code failure; an explicit build configuration because the Windows generator is
multi-config). Failures are published as annotations so a red build on a platform nobody
can reproduce locally is still diagnosable without downloading logs.

### 7.3 The tooling around the language

- **`zl-lsp`** — editor feedback safe to run on every keystroke: lexical errors, unmatched
  brackets, the filename/type rule, and a symbol outline. Deliberately conservative;
  semantic checking stays in the compiler.
- **`zl-test`** — cross-platform discovery and running of examples and the regression
  corpus, with machine-readable output for editors and CI.
- **`zl-bind`** — generates typed bindings to C libraries. Plain-data C structs get
  field-by-field getters and setters, layout queries, and a generated schema whose offsets
  and sizes are checked by compile-time assertions.
- **`research/harness.py`** — the measurement rig behind the performance report: four
  compile configurations and three run configurations per program, per-stage timings taken
  from the compiler's own ledger, medians over repeated runs, peak memory from the
  operating system.

---

## 8. Why the agile model is the right fit for this project

"Agile" means: build a small, working piece; prove it works; show it; learn; re-plan;
repeat. The opposite is the waterfall approach — write a complete specification up front,
build to it for a long time, and discover at the end whether the specification was right.

For a project of this kind, agile is not merely convenient. It is close to the only
approach that works, and this repository shows why in seven specific ways.

### 8.1 The problem cannot be fully specified in advance

A programming language is hundreds of interacting decisions — what `+` does on overflow,
what an empty `{}` means, whether a lambda inside a lambda donates its return type. No
document written before the work began could have settled these, and the evidence is in
the changelog: **61 dated checkpoints over roughly four weeks** (2026-08-30 to
2026-09-20), most of them resolving a question that did not exist as a question until the
previous increment exposed it. Under waterfall, each of those would have been a
change-request process. Under agile, each was a week's work with a test attached.

### 8.2 Every increment ships with its own proof, so progress is a fact

The repository's rule is that **"looks fixed" is not a gate.** Each task carries one line
of symptom, one pointer to evidence, and one acceptance test. Each changelog entry ends
with the gates that cover it — "42/42 ctest, 52/52 examples byte-compared, 91/91
regression fixtures". Progress reporting therefore does not depend on anyone's estimate or
optimism. For a stakeholder, this is the most valuable property in the whole project:
**percent-complete is measurable, not claimed.**

### 8.3 Small increments make correctness tractable

The current workstream is a good illustration. Native machine-code execution is an
enormous problem. It is being delivered **one value class at a time** — integers first,
then doubles, then (later) references and objects, each blocked on a specific named
obstacle (the garbage-collector map). Every step is refused-by-name-and-reason rather than
partially faked, so the system is never in a state where it is quietly wrong. A waterfall
plan would have delivered "native compilation" as one large, unverifiable release.

### 8.4 Testing the product *is* the work of writing the product

The 52 examples were written as documentation and immediately functioned as the most
thorough test the language had ever had. From `examples/REVIEW.md`: *"Writing 46 runnable
examples means running every code path a new user runs, in the order they run it. That
surfaced four bugs worth fixing outright — one of them severe enough to break the
language's headline feature."* Only an iterative model captures that value, because only
an iterative model produces a runnable product early enough for this kind of review to
find anything.

### 8.5 Evidence beats authority — including the project's own earlier evidence

The performance report originally concluded that the optimiser *"costs ~44 ms and buys
nothing"*. After a later increment (dead-function elimination) changed the facts, the
conclusion was **re-measured and inverted**: the optimiser now costs ~1.9 ms median, cuts
artifact size by 86.6%, and brings compile time from 8.2× the reference path down to 2.2×.
The report was updated rather than quietly replaced, and the task was closed against its
original "done when" wording. In a waterfall culture an early measured finding becomes a
design constraint defended for years. Here it became a data point with an expiry date.

The same instinct produced the **stale-claims** discipline: a finding that no longer
reproduces moves to a dated "Stale" section rather than sitting in the backlog sending the
next person chasing a ghost. Three such claims were verified and cleared on 2026-09-17;
the section currently reads "None recorded right now."

### 8.6 The backlog is curated, not accumulated

`TASKS.md` states its rules of upkeep explicitly: **a finished task is deleted** and its
fix is written up in the changelog; only the most recent completions are kept visible so
the gates covering them remain findable. The document is kept short *"on purpose — so it
can be read top to bottom in a minute and picked up by anyone."* As of this snapshot the
open P0 list (wrong results, or a valid program refused) is **empty**, and exactly two
items remain open, both on the native line.

This is what a healthy agile backlog looks like. The failure mode of most long-running
projects is a backlog that grows monotonically and stops being read; here it shrinks as
work completes and is deliberately re-readable.

### 8.7 Scope decisions are made and argued, not parked

Task P2-5 asked whether `zlpkg` needs a package registry. The answer was **no**, with a
written argument: a registry is a hosted service (name index, curation, removals) outside
this repository's scope; the lockfile's pinned commits already play the artifact-store
role; the grammar leaves a shaped hole for one later. The changelog entry's title is
*"the `zlpkg` registry question gets an answer, not a backlog line."* Deciding and
recording is cheaper than deferring, and it stops the same question being re-litigated
every quarter.

### 8.8 The mechanisms that make this safe to do at speed

Agile fails when fast iteration means unverified change. Four things prevent that here,
and they are the transferable lessons:

- **A hard architectural boundary** (§3) means two lines of work — front end and backends —
  can move independently without re-coordinating meaning.
- **Differential testing** means any refactor can be checked against the previous
  behaviour automatically, on the whole example corpus, three ways.
- **Flaky tests are treated as bugs.** Two changelog entries fix tests that failed
  intermittently or on quiet machines, rather than marking them retry-able. A gate nobody
  trusts is worse than no gate.
- **Fail-closed design** means an unfinished feature cannot silently produce a wrong
  answer; it refuses and says why. Iterating quickly on a system that admits ignorance is
  safe. Iterating quickly on a system that guesses is not.

**Summary for reporting:** the agile model fits because the domain is
discovery-heavy, the increments are independently verifiable, and the project invested
early in the automation that makes verification cheap. The measurable result is a
four-week run of 61 shipped increments with an empty critical-defect list at the end and a
backlog of two items.

---

## 9. Scorecard — the numbers to quote

| Measure | Value | Source |
| --- | --- | --- |
| C++ implementation | 64,916 lines / 166 files | counted |
| Documentation | 7,961 lines / 16 documents | counted |
| Standard-library packages | 26, written in ZL | `stdlib/` |
| Runnable examples | 52 (README still says 46) | `examples/` |
| Regression fixtures | 117 ZL files present; 91 run as the regression suite | `tests/zl/`, `scripts/run_regressions.sh` |
| Test executables | 36, across 42 CTest entries | `CMakeLists.txt`, `TASKS.md` |
| Backend parity | 51 of 51 identical, three ways | `tools/backend_diff.sh` |
| Bytecode size reduction | −81.3% corpus / −86.6% vs reference | `research/report.md` §2.9 |
| Optimiser cost | 43.9 ms → 1.9 ms median per program | `research/report.md` §2.9 |
| Compile-time ratio vs reference | 8.2× → 2.2× median | `research/report.md` §2.9 |
| Native vs interpreted kernel | ~140–150× on measured loops | `docs/changelog.md` 2026-09-20 |
| Native subset coverage | ~1.7% of functions in a realistic module | `research/report.md` §3 |
| Shipped increments | 61 dated checkpoints, 2026-08-30 → 2026-09-20 | `docs/changelog.md` |
| Open critical defects (P0) | 0 | `TASKS.md` |
| Open tasks overall | 2 (both native-line) | `TASKS.md` |
| Platforms verified in CI | Linux (full gate), macOS, Windows | `.github/workflows/ci.yml` |

---

## 10. Risks and recommendations

| Risk | Assessment | Recommendation |
| --- | --- | --- |
| **Licensing is noncommercial** | PolyForm Noncommercial 1.0.0 with Runtime Exception. This is the first thing any adoption conversation must resolve | Confirm intent early. If commercial use is a goal, a separate licence arrangement is required before any evaluation deepens |
| **Native performance path incomplete** | The headline speed story is real but narrow (~1.7% of functions). Whole programs run on the VM today | Report ZL's performance as "measured and improving, interpreter-speed for whole programs". Do not let the ~150× kernel figure be quoted without its coverage caveat |
| **Single-lane delivery** | The changelog shows an extremely disciplined process, but the entire history is squashed into one commit in this snapshot, and the work reads as one continuous line of development | If the project is to be staffed up, the onboarding path (`TASKS.md` rules, the gates, the boundary lint) is already written and unusually good — use it. The process would scale; the documentation would need a "first week" guide |
| **No decimal type** | Blocks a common class of business application (money) | Only relevant if that is a target use case; the limitation is already documented honestly |
| **Documentation drift** | The example count is stale in two READMEs | Small fix, worth doing immediately because the project's credibility rests on its own "correct stale claims" rule |

**Overall assessment.** As an engineering artefact this repository is unusually mature for
its age: complete toolchain, cross-platform verification, enforced architecture, measured
performance claims, and self-published limitations. As a product it is pre-adoption — the
interpreter is the real execution path, the ecosystem has no registry, and the licence is
noncommercial. The development *process* is the most immediately transferable asset here,
and it is what Section 8 documents.

---

## 11. Where to verify any claim in this report

| To check | Read |
| --- | --- |
| What the language does | `docs/language-guide.md`, then run any file in `examples/` |
| The architecture and its rules | `docs/pipeline.md`, `docs/mir.md` |
| Performance numbers and method | `research/report.md` |
| What is unfinished | `TASKS.md` (the open list) and `README.md` → "Current limitations" |
| What was built, when, and with what proof | `docs/changelog.md` (61 dated entries) |
| Bugs found by writing the examples | `examples/REVIEW.md` |
| The gates and how to run them | `TASKS.md` header, `docs/development.md`, `.github/workflows/ci.yml` |
| The licence | `LICENSE` |

---

## 12. Glossary

| Term | Plain meaning |
| --- | --- |
| **Compiler** | The program that translates human-written code into something a machine executes |
| **Runtime / VM (virtual machine)** | The engine that actually executes the program. A "stack VM" is a simple, portable engine design |
| **Bytecode** | A compact instruction format meant for a VM rather than for a physical chip |
| **Native / machine code / AOT** | Instructions a physical processor runs directly. AOT means compiled ahead of time rather than while running |
| **Interpreter** | Runs the program directly, without producing a standalone executable. Simpler and more portable; typically slower |
| **IR (intermediate representation)** | A halfway format between source code and machine code. **MIR** is ZL's version: typed, verified, and the single hand-off point |
| **SSA** | A discipline where every temporary value is assigned exactly once, which makes programs much easier to analyse and optimise |
| **Verifier** | An automatic inspector that checks the intermediate form against its rulebook before anything is allowed to use it |
| **Optimiser** | Rewrites the program to be smaller or faster while provably preserving its meaning |
| **Garbage collector (GC)** | Automatic memory cleanup. Finds data nothing can use any more and reclaims it |
| **Ownership / borrowing** | A way of stating who is responsible for a piece of data, checked by the compiler |
| **Type checker** | Verifies that every value is used consistently with what it is — no adding a name to a number |
| **Generics** | Code written once that works for any data type, with the type still checked |
| **Lambda** | A small function written inline, often passed to other code as a value |
| **Reflection** | A program inspecting its own structure at runtime |
| **FFI (foreign function interface)** | The bridge that lets ZL call code written in another language, typically C |
| **Concurrency / async / race condition** | Doing several things at once. A race condition is when two of them touch the same data in an unlucky order and corrupt it |
| **Regression test / fixture** | A stored program with a pinned expected result, so a fix can never be silently undone |
| **Differential testing** | Running the same input through two or more implementations and comparing the outputs |
| **CI (continuous integration)** | Automated building and testing on every change, on every supported platform |
| **Backlog** | The curated list of remaining work |
| **Fail-closed** | Refusing to proceed when unsure, rather than producing a possibly-wrong answer |
| **Lockfile** | A record pinning the exact versions of outside code a project depends on, so builds are reproducible |
