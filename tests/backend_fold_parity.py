#!/usr/bin/env python3
"""Backend fold-parity: every constant folder must agree with checked arithmetic.

Runs the self-checking LiteralFoldParity fixture under all four execution
configs (default MIR, reference AST, unoptimized MIR, native backend) and
requires byte-identical "ok" output from each. The fixture pins two past
miscompiles:

- folders that turned an overflowing INT literal expression into a DOUBLE
  constant (changing the type and masking whichever fault the other operand
  carried), and
- constant pools that conflated -0.0 with +0.0 (log prints "-0" vs "0").

Usage: backend_fold_parity.py <path/to/zl_language>
"""
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = pathlib.Path(sys.argv[1]).resolve()
FIXTURE = ROOT / "tests" / "zl" / "valid" / "language_hardening_tests" / "LiteralFoldParity.zl"

CONFIGS = [
    ("mir", [], {}),
    ("ast", [], {"ZL_COMPILER": "ast"}),
    ("mir-opt-off", [], {"ZL_MIR_OPT": "0"}),
    ("native", ["--backend", "native"], {}),
]


def run_config(name, extra_args, extra_env):
    env = dict(os.environ)
    env.update(extra_env)
    run = subprocess.run([str(BINARY), *extra_args, str(FIXTURE)], cwd=ROOT,
                         capture_output=True, text=True, timeout=120, env=env)
    return run.returncode, run.stdout, run.stderr


def main():
    assert FIXTURE.exists(), f"missing fixture: {FIXTURE}"
    failures = 0
    for name, args, env in CONFIGS:
        code, out, err = run_config(name, args, env)
        if code != 0 or out != "ok\n":
            failures += 1
            print(f"[{name}] FAIL rc={code} stdout={out!r}", file=sys.stderr)
            print(f"[{name}] stderr tail: {err[-500:]}", file=sys.stderr)
        else:
            print(f"[{name}] ok")
    if failures:
        print(f"backend fold parity: {failures} config(s) failed", file=sys.stderr)
        return 1
    print("backend fold parity: all 4 configs agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
