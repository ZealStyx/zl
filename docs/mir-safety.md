# MIR safety analysis and layer evaluation

MIR is a **checked compiler contract**, not a proof that arbitrary ZL programs
cannot fail. The verifier is non-mutating and runs at lowering and pass boundaries.
Static rejection, conservative warnings, incomplete translations, and runtime
obligations are different results. No front-end rule has been disabled or delayed
for this evaluation.

## Running it

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target zl_language zl-mir-safety-tests zl-mir-tests zl-mir-ssa-tests zl-mir-opt-tests -j2
build/zl-mir-safety-tests
python3 tests/safety_pipeline.py build/zl_language
build/zl_language --safety-check examples/basics/HelloWorld.zl
# Prefix probes do not lower or execute:
build/zl_language --safety-check path/to/Program.zl --stop-after=parsing
build/zl_language --safety-check path/to/Program.zl --stop-after=semantic
# Runtime execution of the trusted existing corpus is explicitly opt-in:
python3 tools/safety/evaluate.py build/zl_language \
  --run-runtime --output build/safety-evaluation.json
```

Use an actual file from your checkout in the command examples. `--safety-check`
never executes ZL (even a program calling `System.exit`). It emits one JSON object
on stdout. `--safety-check` includes nonfatal boundary audits; ordinary verifier
clients can opt in with `VerifierOptions::auditBoundaries`. Existing MIR commands
and optimisation passes retain their usual verification behavior.

Exit codes: 0 accepted/verified, 1 parser or semantic rejection, 2 usage, 3
stdlib configuration, 4 MIR rejection, 6 unsupported/incomplete lowering, 7 module
resolution or unclassified compiler exception. Process signals/timeouts are not
language rejections. Lexer failures currently use `std::runtime_error`, not a
separate lexical exception class: the observation API deliberately reports those
as unclassified errors *at parsing*, rather than crediting arbitrary compiler
exceptions as successful safety checks.

The JSON schema has `schema_version: 1`, actual `stage`, `outcome`, per-layer
statuses, lowering notes, incomplete function names, and a `verification` object.
Each MIR diagnostic includes severity, stable `property`, function, block,
instruction index (`-1` for terminators/function-level diagnostics), and source
file/line/column. Property identifiers are assigned by checks/opcode families,
not inferred by searching English diagnostic text. They are property groups,
not unique rule IDs or counts of independent vulnerabilities.

## Safety properties and appropriate boundaries

| Property | MIR validation / analysis | What remains elsewhere |
|---|---|---|
| Type-correct data flow | Operand identities/types, operators, storage, arguments, block parameters/edge arguments | Semantic checker owns inference; dynamic values and heap contracts need runtime checks |
| Use-before-definition | SSA uniqueness and dominance; **must-initialized** mutable slots | The source checker owns lexical name resolution; neither invents default source initialization |
| Use-after-move | Flow-sensitive **may-moved** local slots, including joins and unwind paths | Runtime storage backstops; not a whole-heap alias proof |
| Invalid ownership transitions | Move only from owned storage, drop only owned storage, native borrowed handles cannot close/consume | Foreign registry identity/ownership and metadata trust remain runtime obligations |
| Invalid borrows | May-active exclusion vs must-active use/end; moved/dropped owners; borrow-root resolution at suspension; directly identifiable borrowed returns | Unknown/heap aliases, general interprocedural lifetime relations |
| Unreachable code | CFG reachability with exception edges; unreachable blocks can be errors or warnings by verifier policy | Source statements already discarded by lowering cannot be recovered by MIR; no claim of detecting all source dead code |
| Invalid control flow | Valid block/handler IDs, terminators, target/argument shapes, edge-list agreement, dominance | Parser/checker own source syntax and lexical break/continue legality |
| Impossible type assumptions | `refine` compatibility; `type_test` impossible/no-op warnings | No SMT solver or arbitrary logical predicate proof; union/subtype precision remains limited |
| Suspicious dynamic/static boundaries | Missing required `refine` is invalid MIR; audit warnings identify unknown, generic, or bare-callable inputs | Checked refinement is **legal**, not a compile-time error; its assertion must execute at runtime |
| Invalid returns | Presence/absence and declared type/payload compatibility; directly traceable borrow escape | Runtime validates unknown payloads and heap shape |
| Invalid native calls | MIR argument contract arity/types, return metadata, ABI tags, FFI ownership/view rules, callback signatures | No proof that arbitrary metadata matches a real library/catalog implementation; ABI/registry/liveness checks remain necessary |
| Unsafe resource lifetimes | May-ended SSA native handles/callbacks/parameters across CFG; close/consume/release, explicit consumed call arguments, refinement aliases, borrowed-handle dependency | No exhaustive slot/heap alias analysis, destructor coverage/leak freedom, arbitrary foreign retention, or GC proof |
| Concurrency violations | Known closure captures, async/blocking restrictions, lexical scoped-lock effects, suspension borrow roots; warnings for opaque boundaries, shared RMW, bare waits | Dynamic captures, indirect effects, scheduling races, deadlocks, and foreign callbacks remain runtime/research work |

This table includes existing verifier checks and the additions in `safety.cpp`.
The `zl-mir-tests`, ownership, type, SSA, and optimizer suites remain important:
new checks do not replace the established contract tests.

## Abstract interpretation contract

The analyses operate on a finite intraprocedural CFG. Reachability includes normal
and nominal unwind edges; SSA dominance continues to use normal flow, as defined
by the existing MIR contract.

* Initialization: a set of slots definitely initialized; predecessor join is
  **intersection**, store/borrow generates a definition. Entry starts empty;
  parameters are SSA values, not implicitly initialized slots. Only the catch
  binding on a particular handler edge is initialized by that edge.
* Ownership: moved/dropped slots and dropped parameters are **may** sets (union).
  Borrow exclusions use a may-active map; conflicting roots become unknown.
  Use/end requires a separate must-active set (intersection). This fixes the
  unsound alternative of forgetting a borrow when only one predecessor has it.
  Await root proofs follow the **converged** borrow map, including distinct
  parameter roots, not the first syntactic borrow declaration in the function.
* Explicit resource identities: may-ended SSA identities are unioned at joins.
  Refinement preserves identity. A handle borrow depends on its owner's identity
  but does not gain the right to consume it. A fresh SSA producer in a loop
  creates a new value on each execution. Slot/heap aliases are deliberately not
  reconstructed from a global load map as though storage were immutable.
* Unwind facts merge instruction-prefix states at potentially throwing operations,
  including pre/post states for operations that may mutate then throw. A nominal
  handler with no identifiable throwing point receives conservative entry facts.
  Handler type matching is not used to prune paths. This can over-approximate
  failures; it never assumes the successful exit of a try block initialized
  everything visible in its catch. Implicit exceptional lexical scope exits
  do not yet carry explicit borrow-end metadata; catch flows can conservatively
  retain a claim whose source scope has ended. Such rejections are precision/
  lowering limitations, not evidence of an unsafe source program.

The solver iterates reachable blocks to a fixed point without a guessed iteration
cutoff. Unvisited predecessors are bottom/unreached, not a made-up successful
execution. Diagnostics are emitted in one replay of the converged inputs, not
during convergence; this avoids duplicate counts and transient false diagnostics.
Joins are path-insensitive: infeasible paths are not generally eliminated. Unknown
borrow roots conservatively fail an owned-frame proof. This is an abstract
interpretation foundation, **not** a mechanically checked soundness theorem.

## Source provenance

`ModuleLoader` stamps the AST before merging imports. MIR lowering preserves
file and line for functions, parameters, slots, blocks, instructions, terminators,
match arms, and lambdas. Builtin source strings have explicit virtual identities
`<builtin:N>` (indices in `builtinLibrarySources()`), not fabricated filesystem
paths. Missing instruction locations fall back to the block/function location.
AST columns remain unavailable and are honestly reported as 0. Hand-built MIR
may have no location. Optimizer-generated instructions use existing location
propagation conventions; attribution may name the source operation they replace.

## Evaluation protocol

`tools/safety/evaluate.py` imports **the unchanged** `tests/type_boundaries.py`.
This is the available source adversarial corpus adapter in this checkout:
22 negative assignment/union/native/heap cases, two workflows that intentionally
catch invalid dynamic operations, and one intentional async exception workflow.
It is not a representative corpus of all thirteen properties. Hand-built malformed
MIR tests are a **separate population**, testing compiler/pass defects; adding
those to source rejection percentages would be misleading.

For each source case the harness records source hash, raw process output/status,
actual layer outcomes, lowering completeness, diagnostics, and original semantic
and runtime oracles. It never forces semantically rejected ASTs through lowering.
If the full compiler crashes, independent parsing/semantic prefix probes can
recover only the earlier outcomes they actually observe, while the MIR outcome
stays unobserved. Runtime uses the ordinary **reference AST/bytecode backend**
after semantic acceptance, even if the independently observed MIR path fails.
This distinction is recorded; it does not certify MIR backend execution.

The two runtime guard workflows must match their complete pre-existing stdout,
stderr and exit-code oracles. The async exception workflow must preserve the
expected exception text and status. A nonzero exit code alone is never counted
as a detected safety violation. Timeouts, crashes, missing modules, unsupported
lowering, unknown failures, and oracle mismatches stay separate. Warnings are
not rejection credit, and verified partial MIR is not a full certificate.

The unit is a **program/workflow**, not a log line, defect, or vulnerability.
Multiple errors in one program are correlated. Later layers not reached after an
earlier rejection have no observed detection capability; do not label them as
misses, successes, or hypothetical catches. The corpus does not measure general
recall/precision, concurrency soundness, or FFI implementation safety.

The report records compiler revision, tracked-diff hash, binary hash, corpus and
source hashes, platform, stdlib/extra roots, timeout, and backend selection. Store
raw reports outside Git (e.g. `build/`). Archive the binary/source snapshot and
runtime environment alongside a research result; hashes alone are not a rebuild.
For another existing corpus pass `--manifest manifest.json`, a JSON list of
`id`, `file` (relative to the manifest), `role`, optional `expected_semantic`,
and optional `runtime` oracle (`returncode`, `stdout`, `stderr`, and/or
`stderr_contains`). Only run trusted programs with `--run-runtime`; process timeouts
are not an OS sandbox.

See [the initial evaluation](status/mir-safety-evaluation.md) for measured results
and explicitly uncredited MIR-path defects.
