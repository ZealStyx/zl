#!/usr/bin/env python3
"""Backend double-format parity: one decimal spelling per double, on every path.

Runs the self-checking DoubleFormatting fixture under all four execution
configs (default MIR, reference AST, unoptimized MIR, native backend) and
requires byte-identical "ok" output from each. The fixture pins the printing
contract that REVIEW.md tracked as F9:

- a double prints as the shortest decimal that reads back as the same binary
  value (log(3.14) is "3.14", not "3.1400000000000001"), and
- every surface that renders a double - concatenation, Text.format, collection
  printing, Serialize.encode - uses that one spelling, so no backend, folder or
  constant pool can hand the same value two representations.

Usage: double_format_parity.py <path/to/zl_language>
"""
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = pathlib.Path(sys.argv[1]).resolve()
FIXTURE = ROOT / "tests" / "zl" / "valid" / "language_hardening_tests" / "DoubleFormatting.zl"

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
        print(f"double format parity: {failures} config(s) failed", file=sys.stderr)
        return 1
    print("double format parity: all 4 configs agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
