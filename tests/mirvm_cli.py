#!/usr/bin/env python3
"""--mir-vm CLI tests: uncaught-exception reporting parity with the default run path.

Usage:
python3 tests/mirvm_cli.py /path/to/zl
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = pathlib.Path(sys.argv[1]).resolve()


def run_mirvm(path):
    run = subprocess.run([str(BINARY), "--mir-vm", str(path)], cwd=ROOT,
                         capture_output=True, text=True, timeout=60)
    return run.returncode, run.stdout, run.stderr


def main():
    with tempfile.TemporaryDirectory(prefix="zl-mirvm-cli-") as tmp:
        folder = pathlib.Path(tmp)
        # An uncaught ZL exception must report `runtime error:` with exit 1,
        # exactly like the default pipeline - never abort (SIGABRT, exit 134).
        throwing = folder / "Throwing.zl"
        throwing.write_text(
            "class Throwing {\n"
            "    func main(): void {\n"
            "        throw new Exception(\"boom\")\n"
            "    }\n"
            "}\n")
        code, out, err = run_mirvm(throwing)
        assert code == 1, (code, out, err)
        assert "runtime error: boom" in err, (code, out, err)
        assert "terminate called" not in err, (code, out, err)

        # A clean program still exits 0 through --mir-vm.
        clean = folder / "Clean.zl"
        clean.write_text(
            "class Clean {\n"
            "    func main(): void {\n"
            "        log(40 + 2)\n"
            "    }\n"
            "}\n")
        code, out, err = run_mirvm(clean)
        assert code == 0, (code, out, err)
        assert "42" in out, (code, out, err)

        # System.exit codes pass through untouched.
        exiting = folder / "Exiting.zl"
        exiting.write_text(
            "class Exiting {\n"
            "    func main(): void {\n"
            "        System.exit(17)\n"
            "    }\n"
            "}\n")
        code, out, err = run_mirvm(exiting)
        assert code == 17, (code, out, err)

    print("mirvm_cli: ok")


if __name__ == "__main__":
    main()
