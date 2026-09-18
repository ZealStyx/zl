#!/usr/bin/env python3
"""Sum-match parity: `match` on Option/Result must compile and run everywhere.

Runs the self-checking OptionResultMatch fixture under all four execution
configs (default MIR, reference AST, unoptimized MIR, native backend) and
requires byte-identical output from each. The fixture pins the contract that
TASKS.md tracked as P1-1 plus the statement-position `match` MIR refused:

- a bare case pattern takes its type arguments from the subject
  (`Ok v` against a `Result<int,string>`), and the spelled-out spelling is the
  same program;
- a case name alone (`None`, `Err`) names the case instead of binding a
  variable;
- covering both cases is exhaustive with no wildcard and no `null` arm, while
  a single case reports the missing one by name;
- nil is not a case: it matches no arm and raises catchably unless a `null` or
  catch-all arm takes it;
- a `match` whose arms produce no value (every arm a `log`) is a statement, and
  the MIR verifier no longer rejects the module for it.

Usage: sum_match_parity.py <path/to/zl_language>
"""
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = pathlib.Path(sys.argv[1]).resolve()
FIXTURE = ROOT / "tests" / "zl" / "valid" / "language_hardening_tests" / "OptionResultMatch.zl"

# The fixture's own output: the statement-position match logs its arm, then the
# fixture's final self-check line.
EXPECTED_STDOUT = "side ok\nok\n"

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
        if code != 0 or out != EXPECTED_STDOUT:
            failures += 1
            print(f"[{name}] FAIL rc={code} stdout={out!r}", file=sys.stderr)
            print(f"[{name}] stderr tail: {err[-500:]}", file=sys.stderr)
        else:
            print(f"[{name}] ok")
    if failures:
        print(f"sum match parity: {failures} config(s) failed", file=sys.stderr)
        return 1
    print("sum match parity: all 4 configs agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
