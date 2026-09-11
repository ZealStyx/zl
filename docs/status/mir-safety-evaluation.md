# Initial MIR safety evaluation — 2026-09-11

This is a **baseline observation**, not an overall safety score. Protocol and
limitations: [MIR safety analysis](../mir-safety.md).

## Reproduction and population

```sh
python3 tools/safety/evaluate.py build/zl_language \
  --run-runtime --output build/safety-evaluation.json
```

Measured on Linux x86-64, GCC 12.2, C++17 Debug build, from parent revision
`62fbf31b4554334ecd0ac0511098c56550d0f82a` plus this safety-analysis change.
The generated JSON records the actual binary/diff hashes and raw per-case
observations. Reports are deliberately kept in ignored `build/`, not committed
as large, ephemeral artifacts.

Source corpus: **unchanged `tests/type_boundaries.py`**, SHA-256
`7c9efb249a49224ba36283a712ee485340c997fdd9c3ee14bd04a8b34d888f3f`.
There are 25 programs/workflows: 22 negative semantic cases, two accepted
workflows exercising caught runtime contract violations, and one intentional
async exception-propagation workflow. No synthetic MIR mutation is added to
this denominator. No claim that this population represents every safety property.

## Observed layers

| Layer | Reached | Outcome |
|---|---:|---|
| Parsing/module loading | 25 | 25 accepted; no parser-negative source case in this population |
| Semantic/type checking | 25 | **22 rejected**, 3 accepted |
| MIR lowering/verification | 3 | **2 rejected translations**, 1 complete verified translation |
| Reference runtime | 3 | **3 original runtime oracles confirmed** |

The remaining 22 programs do **not reach MIR or runtime** in this protocol.
Their hypothetical detection at those layers is unknown, not zero or perfect.
The MIR-rejected assignment/union workflow also has **two incomplete functions**;
its partial translation is recorded, not counted as a successful certificate.

**The two MIR rejections are not credited as source safety detections.** They
occur in programs accepted by the language checker whose existing reference-runtime
oracles pass. They reveal type/lowering limitations or verifier disagreement:

* `assignment-union-workflow`:
  * wildcard remainder passed as `bool|int` to a `bool` parameter;
  * `list<unknown>` stored in an `array[2]<int>` slot;
  * `int|string` argument rejected against `bool|int|string`;
  * field access still represented with an `int|Packet` receiver;
  * incomplete `snapshot`/`main` translations on assignment to refined match
    subject bindings (`value` / `subject`).
* `heap-contract-workflow`: `list<unknown>` stored in an `array[2]<int>` slot.

These remain visible follow-up work. Relaxing verification, removing source
cases, or reporting partial MIR as verified would conceal precisely the
compiler/runtime disagreements this evaluation is intended to find.

The two guard workflows validate multiple runtime boundaries via complete
existing output oracles, but are counted as **two workflows**, not dozens of
independent detections. The async workflow tests intentional exception propagation;
it is not an accidental vulnerability newly caught by the runtime.

## Defects exposed and corrected in this phase

* Conditional borrows were forgotten at joins because an intersection was used
  for exclusion. Exclusion now uses may-active facts; use/end uses must-active.
* Ownership diagnostics were produced while solving the fixed point. They now
  report converged facts once, with no guessed iteration cap.
* Normal lexical exits, `break`, and `continue` were missing `end_borrow` events
  in lowering. These are now explicit, preserving the source checker's lexical
  rule rather than weakening the verifier to forget claims at joins.
* Two lowerer crashes treated refined SSA bindings as mutable slots: calling a
  matched callable and capturing a refined subject in a lambda. Both now use
  the existing SSA value binding. The remaining type issues above are still
  reported; eliminating a crash does not turn a partial translation into success.
* The old “valid FFI” shape test consumed a handle and closed the original token.
  Its legal fixture now closes the transferred result; a separate negative test
  pins consume-then-close rejection. This is a MIR-fixture correction, not a
  moved source rejection.
* The standalone SSA test target omitted existing folding/effects dependencies;
  these are linked so the original suite can run.

## Validation, kept separate from source-layer counts

Passed:

* New MIR safety suite: **24 checks** (initialization joins/loops/exception
  prefixes, source attribution, may/must borrows, suspension roots, explicit
  native resource transitions, native contracts, nonfatal boundary audits).
* Safety CLI/Python tests: parsing vs semantic attribution, prefix probes, no
  execution, imported and quoted source paths, callable/capture crash regressions,
  lexical and loop borrow exits, JSON, and accounting.
* Existing MIR verifier and SSA suites.
* Existing MIR ownership suite: **16**; type and lowering suites.
* Existing optimizer unit suite: **146**; optimizer pipeline suite: **79**.
* Original `tests/type_boundaries.py` semantic/runtime oracles.
* `tools/mir_opt_check_all.sh`: **76** example/stdlib translations equivalent
  before and after optimization.

The full-pipeline C++ suites were linked against the freshly built interpreter
objects (excluding `main`) to avoid recompiling identical sources per test target;
their existing test sources were unchanged. The new tests also have CTest entries.

## Research implications and next measurements

1. Lexical/nominal source rules are naturally enforced by parsing and semantics;
   this corpus's 22 negatives specifically support semantic enforcement, not
   parser or MIR superiority.
2. MIR is valuable as an independent consistency check on lowered types,
   control flow, and explicit lifetime events. Accepted-source disagreements
   should be tracked as compiler defects/coverage limitations, separately from
   rejected unsafe source and malformed-MIR mutation tests.
3. Dynamic heap contracts, untyped callable payloads, foreign resources, and
   opaque concurrency effects still need runtime validation. Audit warnings
   inventory these obligations without prohibiting legal dynamic code.
4. Expand the source corpus with labeled parsing, ownership, borrows, resource,
   and concurrency families and legal near-neighbors before calculating
   per-property recall/precision. Pin runtime oracles, repeat nondeterministic
   concurrency tests, retain crash/timeout buckets, and archive environments.
5. Close the documented match/collection lowering gaps and improve exceptional
   lexical lifetime metadata, heap alias tracking, indirect call effects, and
   native signature provenance before making broader formal safety claims.
