#!/usr/bin/env python3
import importlib.util
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = ROOT / "tools" / "zl-test" / "zl_test.py"

spec = importlib.util.spec_from_file_location("zl_test", RUNNER_PATH)
zl_test = importlib.util.module_from_spec(spec)
sys.modules["zl_test"] = zl_test
spec.loader.exec_module(zl_test)


class ZlTestRunnerTests(unittest.TestCase):
    def test_discovers_examples_with_expected_output_and_skips_libs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "examples" / "basics").mkdir(parents=True)
            (root / "examples" / "basics" / "_lib").mkdir()
            (root / "examples" / "basics" / "_lib" / "Helper.zl").write_text("class Helper {}\n", encoding="utf-8")
            (root / "examples" / "basics" / "Hello.zl").write_text(
                "class Hello { static func main(): void { log(\"Hello\") } }\n"
                "// --- Expected output (verified by examples/run_all.sh) ---\n"
                "// Hello\n"
                "// --- End expected output ---\n",
                encoding="utf-8",
            )
            cases = zl_test.discover_cases(root, "examples")
            self.assertEqual(1, len(cases))
            self.assertEqual("examples/basics/Hello.zl", cases[0].id)
            self.assertEqual("Hello", cases[0].expected_output)

    def test_discovers_valid_and_invalid_regressions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            valid = root / "tests" / "zl" / "valid" / "core_tests"
            invalid = root / "tests" / "zl" / "invalid" / "type_tests"
            valid.mkdir(parents=True)
            invalid.mkdir(parents=True)
            (valid / "Ok.zl").write_text("class Ok { static func main(): void {} }\n", encoding="utf-8")
            (invalid / "Bad.zl").write_text("class Bad { static func main(): void {} }\n", encoding="utf-8")
            cases = zl_test.discover_cases(root, "regressions")
            by_id = {case.id: case for case in cases}
            self.assertEqual(0, by_id["regressions/valid/core_tests/Ok"].expected_exit)
            self.assertEqual(1, by_id["regressions/invalid/type_tests/Bad"].expected_exit)

    def test_cli_list_json(self):
        proc = subprocess.run(
            [sys.executable, str(RUNNER_PATH), "list", "--scope", "examples", "--case", "HelloWorld", "--json"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(0, proc.returncode, proc.stderr)
        payload = json.loads(proc.stdout)
        self.assertEqual(1, len(payload))
        self.assertEqual("examples/basics/HelloWorld.zl", payload[0]["id"])

    def test_runs_filtered_case_against_fake_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "examples" / "basics").mkdir(parents=True)
            (root / "examples" / "basics" / "Hello.zl").write_text(
                "class Hello { static func main(): void { log(\"Hello\") } }\n"
                "// --- Expected output (verified by examples/run_all.sh) ---\n"
                "// Hello\n"
                "// --- End expected output ---\n",
                encoding="utf-8",
            )
            runtime = root / "fake-zl"
            runtime.write_text("#!/bin/sh\necho Hello\n", encoding="utf-8")
            runtime.chmod(runtime.stat().st_mode | stat.S_IXUSR)
            proc = subprocess.run(
                [sys.executable, str(RUNNER_PATH), "--root", str(root), "run", "--scope", "examples", "--runtime", str(runtime), "--json"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(0, proc.returncode, proc.stderr)
            payload = json.loads(proc.stdout)
            self.assertEqual("pass", payload[0]["status"])


if __name__ == "__main__":
    unittest.main()
