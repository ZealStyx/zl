#!/usr/bin/env python3
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LSP_PATH = ROOT / "tools" / "zl-lsp" / "zl_lsp.py"

spec = importlib.util.spec_from_file_location("zl_lsp", LSP_PATH)
zl_lsp = importlib.util.module_from_spec(spec)
sys.modules["zl_lsp"] = zl_lsp
spec.loader.exec_module(zl_lsp)


class ZlLspAnalysisTests(unittest.TestCase):
    def test_reports_file_class_rule_and_unclosed_delimiter(self):
        source = "class Other {\n    func run(): void {\n        log(\"hi\")\n    }\n"
        _, diagnostics, _ = zl_lsp.analyze(source, "/tmp/Expected.zl")
        messages = [diag.message for diag in diagnostics]
        self.assertTrue(any("must declare class 'Expected'" in msg for msg in messages))
        self.assertTrue(any("unclosed '{'" in msg for msg in messages))

    def test_strings_do_not_affect_delimiter_matching(self):
        source = 'class Demo {\n    func main(): void { log("}") }\n}\n'
        _, diagnostics, _ = zl_lsp.analyze(source, "/tmp/Demo.zl")
        self.assertEqual([], [diag.message for diag in diagnostics])

    def test_extracts_document_symbols(self):
        source = """import zl.util.Queue

class Demo {
    value: int

    public func main(): void {
        log("hi")
    }
}

data Point {
    x: int
    y: int
}
"""
        _, diagnostics, symbols = zl_lsp.analyze(source, "/tmp/Demo.zl")
        self.assertEqual([], [diag.message for diag in diagnostics])
        self.assertEqual(["Demo", "Point"], [symbol.name for symbol in symbols])
        self.assertEqual(["value", "main"], [child.name for child in symbols[0].children])
        self.assertEqual(["x", "y"], [child.name for child in symbols[1].children])

    def test_cli_check_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Demo.zl"
            path.write_text("class Demo { }\n", encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(LSP_PATH), "--check", str(path), "--json"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(0, proc.returncode, proc.stderr)
            self.assertEqual({str(path): []}, json.loads(proc.stdout))


if __name__ == "__main__":
    unittest.main()
