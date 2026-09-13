#!/usr/bin/env python3
"""--pipeline-report CLI contract: a runnable program's pipeline, or a precise refusal.

python3 tests/pipeline_report_cli.py /path/to/zl

The report describes the whole pipeline, code generation included, so its
input contract is a *program*: a valid entry point. These cases pin the
observable behaviour of the command - the exit codes, the sections a success
prints, and the diagnostics a library or an invalid entry point gets -
against the C++ pipeline tests, which pin the same contract at the stage
level.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = pathlib.Path(sys.argv[1]).resolve()

PROGRAM = """class Report {
    static func add(int a, int b): int { return a + b }
    func main(): void {
        log("total=" + Report.add(20, 22))
    }
}
"""

LIBRARY = """class ReportLib {
    static func add(int a, int b): int { return a + b }
}
"""

BAD_MAIN_RETURN = """class ReportBad {
    func main(): int { return 0 }
}
"""

BAD_MAIN_STATIC = """class ReportBadStatic {
    static func main(): void { log("nope") }
}
"""

TYPE_ERROR = """class ReportTyped {
    func main(): void { int value = false }
}
"""


def run_report(path):
    run = subprocess.run([str(BINARY), "--pipeline-report", str(path)], cwd=ROOT,
                         capture_output=True, text=True, timeout=60)
    return run.returncode, run.stdout, run.stderr


def main():
    failures = 0

    def check(name, condition, detail=""):
        nonlocal failures
        if condition:
            print(f"PASS: {name}")
        else:
            failures += 1
            print(f"FAIL: {name}")
            if detail:
                for line in detail.splitlines():
                    print(f"    {line}")

    with tempfile.TemporaryDirectory(prefix="zl-pipeline-report-") as tmp:
        folder = pathlib.Path(tmp)

        # 1. A normal program: the report runs both backends, prints the stage
        #    invariants, and proves backend parity.
        program = folder / "Report.zl"
        program.write_text(PROGRAM)
        code, out, err = run_report(program)
        check("a program reports exit 0", code == 0, f"code={code}\nstdout={out}\nstderr={err}")
        for section in ("stage invariants", "code generation:", "backend parity", "identical",
                        "reachability"):
            check(f"the report contains the {section!r} section", section in out, out)
        check("the report names the reachable set from the entry point",
              re.search(r"\d+ of \d+ function\(s\) can run, from Report\.main\(\)", out) is not None,
              out)

        # 2. A library: refused with the entry-point diagnostic, not with a
        #    report that pretends a code-generation stage exists for it.
        library = folder / "ReportLib.zl"
        library.write_text(LIBRARY)
        code, out, err = run_report(library)
        check("a library is refused (nonzero exit)", code != 0,
              f"code={code}\nstdout={out}\nstderr={err}")
        check("the refusal is the semantic-stage entry-point diagnostic",
              "program has no main() func" in err, err)
        check("a library gets no report on stdout", out.strip() == "", out)

        # 3. Invalid entry points: the same stage names the exact violation.
        bad_return = folder / "ReportBad.zl"
        bad_return.write_text(BAD_MAIN_RETURN)
        code, out, err = run_report(bad_return)
        check("a main returning a value is refused (nonzero exit)", code != 0,
              f"code={code}\nstdout={out}\nstderr={err}")
        check("the diagnostic says main must return void", "must return void" in err, err)

        bad_static = folder / "ReportBadStatic.zl"
        bad_static.write_text(BAD_MAIN_STATIC)
        code, out, err = run_report(bad_static)
        check("a static main is refused (nonzero exit)", code != 0,
              f"code={code}\nstdout={out}\nstderr={err}")
        check("the diagnostic says main must be an instance method",
              "must be an instance method" in err, err)

        # 4. A pipeline-report failure diagnostic: a type error is labelled
        #    and carries the checker's message, like every other front-end
        #    failure.
        typed = folder / "ReportTyped.zl"
        typed.write_text(TYPE_ERROR)
        code, out, err = run_report(typed)
        check("a type error is refused (nonzero exit)", code != 0,
              f"code={code}\nstdout={out}\nstderr={err}")
        check("the failure carries the front-end label", "compile error" in err, err)
        check("the failure carries the checker's diagnostic", "cannot assign bool" in err, err)

    if failures:
        print(f"pipeline-report-cli: {failures} check(s) failed")
        return 1
    print("pipeline-report-cli: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
