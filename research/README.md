# MIR architecture evaluation

An experimental evaluation of ZL's MIR as the compiler boundary. It measures the
existing AST→bytecode compiler against the MIR-based pipeline, and unoptimised
MIR against optimised MIR, over a small representative corpus.

- `report.md` — the evaluation report (measured results, comparisons,
  trade-offs, documented failures/limitations).
- `harness.py` — the measurement harness (regenerates `results/results.json`).
- `corpus/` — the positive corpus (one program per feature area), plus
  `corpus/safety/` (static-rejection vs runtime-check programs) and
  `corpus/limitations/` (known lowering/verification gaps).
- `results/` — raw JSON output (large, regenerable; git-ignored).

## Reproduce

```sh
# 1. Build the compiler (Release), including the --artifact-stats command:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target zl_language -j2

# 2. Run the full measurement:
python3 research/harness.py --binary build/zl_language

# 3. Read the numbers:
python3 -m json.tool research/results/results.json
```

## What is measured, and how

| Quantity | Source |
| --- | --- |
| Successful lowering rate / incomplete functions / verification errors | `zl --artifact-stats` JSON (`ok`, `incomplete_functions`, `verification_errors`) |
| Per-stage compile time (load, semantic, MIR construction, verification, optimisation, codegen, total) | `--artifact-stats` `ms_*` fields (raw doubles, ms) |
| Generated code size | bytecode `Instruction` stream (`sizeof(Instruction)` × count), MIR instruction/block counts, native machine-code bytes |
| Runtime performance | wall-clock of `--mir-vm` / `--reference-compiler` runs (median/min over N), split into wall and exec-only (`wall − compile`) |
| Memory behaviour | per-child `ru_maxrss` via fork/exec/`wait4` |
| Safety: static vs runtime | `zl --safety-check` layer outcomes (parsing/semantic/mir) and verification warning properties |

The harness requires the `--artifact-stats` command, which is part of the
shipped CLI (`src/main.cpp`). It never executes the measured program; runtime
figures come from ordinary `--mir-vm`/`--reference-compiler` runs.

## Configurations

| Label | Meaning |
| --- | --- |
| `ref` | existing AST→bytecode compiler (`--reference-compiler`), kept unchanged |
| `mir-unopt` | MIR pipeline, `ZL_MIR_OPT=0` |
| `mir-opt` | MIR pipeline, default (run-path) optimisation |
| `native` | MIR pipeline, `--backend=native` |

## Notes on honesty

- Execution-only time is `wall − compile`; compile time is measured in a
  separate invocation, so the subtraction is an estimate, not a direct
  measurement.
- The optimiser is deliberately conservative and does not transform loops, so
  hot-loop code is byte-identical after optimisation; optimised-vs-unoptimised
  *execution* differences are therefore within noise and are reported as such.
- Raw JSON reports are kept out of Git (large, ephemeral); the report embeds the
  aggregate numbers and the corpus/harness are committed so anyone can
  regenerate the raw data.
