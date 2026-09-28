# Midterm Report — Speaker's Guide

**Project:** ZL Language · **Course:** CMSC 309 / 3IS1
**Your two parts:** (A) *Risk Profile & Mitigation* · (B) *Agile Execution Cycle*

Everything below is grounded in the actual repository, so any claim you make on
stage can be backed by a file the instructor can open. File pointers are given
in `monospace` — memorise two or three of them, they are what make the report
sound like engineering instead of recitation.

---

## 0. Before anything: your one-sentence thesis

> **"ZL is a compiler, and the dangerous failure of a compiler is not a crash —
> it's a wrong answer delivered quietly. Our process is built around that one
> fact: every iteration ends with automated gates that prove the toolchain still
> tells the truth."**

Part A explains the danger. Part B explains the machine that contains it. Say
the sentence at the start of Part A and echo it at the end of Part B — it's the
thread that makes the two parts feel like one report.

### 30-second context (only if the audience needs it)

> ZL is a small statically typed, class-based language we implemented in C++17 —
> about 80,000 lines. Source goes through a lexer and parser, a type checker,
> then into a typed mid-level IR we call **MIR**. MIR is the contract: both
> backends — bytecode for our VM and a restricted native x86-64 backend — are
> handed the *same verified MIR*, so choosing a backend can never change what a
> program means.

---

## 1. Timing plan

| Segment | Target | If you're cut short |
| :-- | :-- | :-- |
| Opening thesis | 0:20 | keep |
| Part A — risk profile | 2:30 | drop risks 6–8, keep 1–4 |
| Transition | 0:15 | keep |
| Part B — the loop | 2:30 | narrate the loop over ONE worked example only |
| Part B — worked example | 1:30 | merge into the loop |
| Close | 0:15 | keep |
| **Total** | **~7 min** | **~4 min** |

Rule of thumb: one slide per minute. Eight slides maximum for both parts.

---

# PART A — Risk Profile & Mitigation

## A1. Framing (say this first)

Two sentences, then the table:

> Our risk level is **medium to high**. Nothing in ZL is life-critical — nobody
> dies if the compiler is wrong. But a compiler defect doesn't announce itself:
> it produces a program that runs, prints an answer, and the answer is wrong.
> A garbage-collector or native-code bug is worse still, because it corrupts
> memory somewhere far from where the mistake was made. So we don't mitigate
> risk with review alone; we mitigate it with **executable gates**.

The one line worth memorising:

> **"A compiler that crashes is safe. A compiler that lies is dangerous."**

## A2. The risk table (this is your main slide)

Present it as *risk → why it's real here → the gate that catches it*. Don't read
the table aloud; point at a row and talk about it.

| # | Risk | Why it is real in ZL | Mitigation (and where it lives) |
| :-- | :-- | :-- | :-- |
| 1 | **Silent wrong results** — two backends disagree | Bytecode VM and native backend are separate translations of the same program | Both consume the **same verified MIR**; five differential harnesses run on every push — `tools/backend_diff.sh`, `mir_backend_diff.sh`, `mir_opt_diff.sh`, `mir_promotion_diff.sh`, `mir_opt_check_all.sh` |
| 2 | **The optimiser changes meaning** | An optimisation pass that is "almost" correct still ships | MIR **verifier** re-runs at lowering *and at every pass boundary*, and it is non-mutating (`docs/mir-safety.md`); ctest `backend-fold-parity`, `sum-match-parity`, `double-format-parity` |
| 3 | **Memory / GC / native crashes** | Native frames can't enter the GC'd VM world; refs need GC maps the backend doesn't have yet | The native driver **refuses, by name and reason**, anything it cannot honestly call — runtime-call relocations, mixed register files, >6 arguments, non-x86-64-Linux hosts (`include/zl/native/exec.hpp`). Refs and objects are *gated on the GC map*, not attempted early |
| 4 | **Architecture drift** — 3 people editing one shared core | A backend that quietly starts reading the AST still compiles; it just now has its own opinion about ZL | `tools/boundary_lint.sh` enforces four rules in CI: a backend reads MIR and nothing else (directly *and transitively*), the lowerer never infers types, the front end is constructed in exactly one place, the legacy IR is frozen |
| 5 | **Regression as the language grows** | Fixing one feature has broken another more than once | Regression corpus of valid **and invalid** fixtures (`tests/zl/valid`, `tests/zl/invalid`) — rejecting a bad program is just as much a required behaviour as accepting a good one — plus the byte-compared examples corpus |
| 6 | **Platform risk** | Native backend is x86-64 Linux only; Win64 is select-only, no arm64 encoder | CI matrix builds and tests on **Linux, macOS and Windows**; native execution tests self-guard with `ZL_NATIVE_CAN_EXECUTE` and skip loudly rather than fail silently |
| 7 | **Claim drift** — docs that describe a bug that no longer exists | Our own backlog was distilled from reviews; several findings were already fixed when we checked | `TASKS.md` has a **Stale claims** section: a finding that no longer reproduces is *corrected*, not marked done — "a task list that repeats them sends the next person chasing a ghost" |
| 8 | **Building the wrong thing well** (scope risk) | Memory domains, async, native backend can all absorb unlimited effort | Risky features start with a **design plan + measured baseline**. Memory domains' Phase 0 gate: *if a pool or arena cannot beat the cheap wins on the benchmark corpus, domains stay a design and the numbers say why* (`docs/memory-domains.md` §9) |

**If you can only say four:** 1, 3, 4, 8. They cover correctness, memory safety,
team scale, and scope.

## A3. The proof line (memorise these numbers)

> Every one of those mitigations is a command, not an intention. The gate line
> at the end of an iteration reads: **CTest suite green, regression corpus
> green, all 52 examples byte-identical to their expected output, native gate
> PASS.** If any one is red, the task is not closed — it goes back to
> implementation.

Numbers as written in our submitted document: **42 CTest tests, 88 regression
fixtures, 52 examples**. If asked why the repo now says 43 tests and 91–94
fixtures: *the corpus grows with every iteration — the 2026-09-20 entry added a
test and three fixtures along with the feature. That the number moves is the
point; the gate is "all of them", not "a fixed number".*

## A4. What we deliberately do NOT claim (use if the instructor pushes)

This earns more credit than over-claiming:

- MIR is a **checked compiler contract, not a proof** that arbitrary ZL programs
  cannot fail (`docs/mir-safety.md`, first line).
- Our safety evaluation is a **baseline observation, not a safety score**: 25
  programs, 22 rejected at the type checker, 3 reaching MIR. The 22 that never
  reach MIR — whether MIR would catch them is *unknown, not zero and not
  perfect*.
- The two MIR rejections in that evaluation are **not** credited as safety
  detections; they revealed limitations in our own lowering, and one of them
  (a fixed array losing its element type) became a fix plus a permanent
  regression fixture.

> Line to use: **"The evaluation was designed to find disagreements between our
> compiler and our runtime. Relaxing it to score better would hide exactly what
> it was built to expose."**

---

## Transition into Part B (say it as one breath)

> So those are the risks. None of them is contained by a document — they're
> contained by a *cycle* that refuses to close a task until the gates are green.
> Here's that cycle.

---

# PART B — Agile Execution Cycle

## B1. The loop, in one pass (your diagram slide)

Backlog → Iteration planning → Implementation → **Verification gate** → Review &
document → Working increment → back to Backlog. Two extra arrows matter and you
should point at them physically:

- the **fix loop**: a red gate sends work straight back to implementation — it
  never advances;
- the **feedback arrow**: writing examples and stdlib code *in ZL* keeps
  exposing compiler gaps, and those become new backlog items. Our backlog is not
  handed to us by a client; it is generated by using our own language.

## B2. Each step, with its artefact

Say the step, then immediately name where it lives — this is what makes it
concrete.

| Step | What we actually do | Artefact |
| :-- | :-- | :-- |
| **1. Backlog** | One prioritised list. Every item is one line of symptom, one pointer to evidence, one acceptance test. **P0** = wrong result or a valid program refused · **P1** = a capability the docs promise · **P2** = polish or ecosystem | `TASKS.md` |
| **2. Iteration planning** | Pick a slice and agree its **exit criteria before coding**. Large features get a written design first, with phases and a gate per phase | `docs/memory-domains.md` §9 — seven phases, each with its own exit criteria |
| **3. Implementation** | Three members in parallel on separate components, all integrating against the same MIR boundary | `src/lexer`, `src/parser`, `src/compiler`, `src/mir`, `src/native`, `src/vm`, plus `zlpkg/` and `stdlib/` |
| **4. Verification gate** | The same commands a developer runs by hand, run automatically on every push and PR. No check is special-cased for CI | `.github/workflows/ci.yml` — build, CTest, regression corpus, five differential harnesses, native gate, on three operating systems |
| **5. Review & document** | Write up *what changed and why, with the measurement*. Finished tasks are **deleted** from the backlog; claims that no longer reproduce move to Stale | `docs/changelog.md`, `TASKS.md`, `README.md` |
| **6. Working increment** | The iteration closes with a toolchain that builds and runs. The next iteration starts from it | `zl_language` and `zlpkg` binaries |

One detail worth calling out at step 4, because it shows the gate is designed
and not copy-pasted:

> Our CI builds with `--parallel 4`, not unbounded. 36 test executables with
> unbounded parallelism exhausts a small runner and dies with "Killed signal
> terminated program cc1plus" — a *resource* failure that reads like a *code*
> failure. We bounded it so that a red build always means a real regression.

## B3. Worked example — one iteration, start to finish (your strongest slide)

Walk the same loop again over a real iteration: **P1-6, the native execution
driver, 20 September 2026.** This is the part that proves you're describing
something you did rather than something you read.

1. **Backlog.** P1-6 in `TASKS.md`. Symptom: `--backend native` had been a code
   generator with *no consumer* since day one — emitted x86-64 was reported,
   packaged, and never called. Evidence: `research/report.md` §2.7 measured the
   compilable subset at ~1.7% of the functions in a realistic module.
2. **Planning.** Exit criteria written before coding: *one value class at a time
   joins the driver, until a named program runs end-to-end with a measured wall
   time against the VM.* Note what that criterion does — it forbids "it mostly
   works".
3. **Implementation.** `zl --run-native <file> [--call NAME] [--int64 v |
   --double v]... [--iters n]`: maps the emitted module into one page-aligned
   executable arena, binds direct-call relocations, dispatches on the signature.
   Loader in `include/zl/native/exec.hpp`; benchmark fixture
   `tests/zl/valid/native/NativeExecBench.zl` — a native kernel, its VM twin,
   and a main the interpreter runs.
4. **Verification gate.** ctest `native-exec-parity` requires the driver and the
   VM to agree *as printed* — and for the double kernel that means bit-for-bit,
   because both sides render the shortest round-trip decimal of the same
   binary64 accumulation. Both tiers' ten-thousand-step sums end at exactly
   `11499.999999997886`. Python's independent evaluation of the same arithmetic
   is the third opinion.
5. **Review & document.** Changelog entry with the measurement: **6.0 ms of
   machine code against 870 ms on the VM** for 10,000 calls of `sumSquares`
   (~140×); 0.16 ms vs 24 ms for the double kernel (~150×).
   `docs/native-backend.md` gained the driver section, `--help` was updated, and
   `TASKS.md` was rewritten to state what remains.
6. **Working increment.** Gates at close: **ctest 43/43, regressions 94/94,
   examples 52/52, native gate PASS.** Next iteration starts here: refs and
   objects, gated on the GC map.

**The finding to end on** — it's the best single sentence in that entry:

> The gap between the two tiers is **subset coverage, not the quality of the
> code we emit.** We only know that because the iteration was required to
> measure, not to claim.

## B4. Second example, 20 seconds — planning that says "no"

> Memory domains began as a draft design. Before implementing it we checked
> every claim in the draft against the code, and wrote a *"What changed from the
> draft"* table — sixteen rows, each one a draft claim the code contradicted.
> Then Phase 0 was defined as pure measurement with a veto: if a pool or an
> arena can't beat the cheap improvements to the existing allocator, domains
> stay a design and the numbers say why. That's the one idea we borrowed from
> Spiral — a risk-first design step — without adopting a formal risk review
> every loop, which three students cannot sustain.

This also pre-answers "why not Spiral?" without you having to defend it.

## B5. Optional 45-second live demo

Only if you have a terminal and it's already built. Rehearse it once.

```bash
(cd build && ctest -C Release)                      # the gate, all of it
bash tools/boundary_lint.sh                         # architecture, as a check
examples/run_all.sh build/zl_language               # 52 examples, byte-compared
```

Say while it runs: *"None of this is a CI-only script — it's the same command we
each run before pushing."* If the build isn't ready, show `.github/workflows/ci.yml`
on screen instead and point at the step names. **Never debug live.**

---

## 2. Q&A preparation

| Likely question | Your answer |
| :-- | :-- |
| *"Isn't Agile just an excuse for no planning?"* | The opposite here. Nothing starts without exit criteria, and the big features start with a written design — memory domains has seven phases each with its own gate. What we refuse to freeze is the *requirements*, because we discover them by running real programs in our own language. |
| *"Why not Waterfall?"* | Language behaviour is discovered by running programs. If we tested only at the end, wrong-code bugs would pile up behind each other and we'd be debugging a compiler and a runtime and a GC simultaneously. |
| *"Why not Spiral?"* | Parts of ZL genuinely are high-risk — native backend, GC. But a formal risk review every loop is too heavy for three students. We keep its best idea inside Agile: risky features begin with a design and a measured baseline before any behaviour changes. |
| *"How do you know a fix actually worked?"* | Every task carries its acceptance test — usually a new fixture pair. "Looks fixed" is not a gate. The lambda-inside-a-lambda bug that ran forever was found *while writing a fixture* for something else, and it left a fixture behind. |
| *"What's your biggest weakness right now?"* | Honestly: the native backend covers ~1.7% of a realistic module, whole-program native still runs on the VM, and an arithmetic fault in native bytes is a `SIGILL` rather than a catchable `ArithmeticError`. All three are written in `TASKS.md` under P1-6, because a limitation you've written down is a task and one you haven't is a surprise. |
| *"Who arbitrates priorities with no client?"* | We do, which is a real weakness — mitigated by instructor checkpoints and by priority rules that aren't about preference: P0 is "wrong result or a valid program refused", and that beats anything anyone wants to build. |
| *"How do three people avoid stepping on each other?"* | Everyone integrates against MIR. And the boundary is machine-checked — `boundary_lint.sh` fails the build if a backend starts reading the AST, even transitively. |
| *"What happens when CI is red?"* | The task stays open. That's the whole fix loop: a red gate returns work to implementation, it never advances to review. |

---

## 3. Glossary (in case you're asked to define a term mid-sentence)

- **MIR** — mid-level intermediate representation. A typed, checked form of the
  program that sits between the type checker and the backends; it is the
  compiler's boundary and its contract.
- **Verifier** — a non-mutating check that MIR obeys its rules; runs at lowering
  and at every optimisation-pass boundary.
- **Differential testing** — run the same program two ways (VM vs native,
  optimiser on vs off) and require identical output. Catches wrong answers that
  neither side would report as an error.
- **Parity test** — a CTest case that asserts that agreement automatically.
- **Fixture** — a small ZL program plus its expected result; `valid/` must be
  accepted, `invalid/` must be rejected.
- **Boundary lint** — a script that reads our own source and fails the build if
  an architectural rule is broken.
- **Exit criteria** — the pre-agreed, testable definition of "this iteration is
  done".
- **Bit-for-bit agreement** — for floating point, identical printed digits, not
  "close enough".

---

## 4. Delivery notes

- **Lead with the danger, close with the gate.** Part A is why you should be
  worried; Part B is why you shouldn't be.
- **Use one real artefact per claim.** Saying "`tools/boundary_lint.sh` enforces
  four rules" beats "we have code standards" every time.
- **Numbers, not adjectives.** 6 ms vs 870 ms. 52/52. 22 of 25 rejected at the
  type checker. ~1.7% subset coverage.
- **Admit the limits out loud.** The safety evaluation's own wording — *baseline
  observation, not a safety score* — is the single most credible thing in either
  part. Instructors reward calibrated claims.
- **Don't read the tables.** They're for the audience's eyes; you talk about
  two rows and move.
- **Practice the transition sentence.** Two-part reports fall apart at the seam.

---

## 5. Accuracy footnote (keep for yourself, don't present)

The submitted document cites **42 CTest tests, 88 regression fixtures, 52
examples, 228 ZL files, ~80,000 lines of C++**. The repository as it stands now
reports ~80,226 lines of C++/headers, 234 `.zl` files, 52 verified examples, and
a most-recent gate line of **ctest 43/43, regressions 94/94** (the 2026-09-20
iteration added one test and several fixtures with the feature). Both are
correct *as of their date*. If the discrepancy comes up, say the date — it
demonstrates the iteration cycle rather than undermining it.
