#!/usr/bin/env python3
"""Non-executing safety CLI, source provenance, and evaluation aggregation tests."""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = pathlib.Path(sys.argv[1]).resolve()


def observe(path, *options):
    run = subprocess.run([str(BINARY), "--safety-check", str(path), *options], cwd=ROOT,
                         capture_output=True, text=True, timeout=30)
    return run.returncode, json.loads(run.stdout)


def main():
    with tempfile.TemporaryDirectory(prefix="zl-safety-cli-") as tmp:
        folder = pathlib.Path(tmp)
        file = folder / "Entry.zl"
        file.write_text("class Entry { func main(): void { System.exit(17) } }\n")
        code, report = observe(file)
        assert code == 0 and report["layers"]["runtime"] == "not-run", report
        code, report = observe(file, "--stop-after=parsing")
        assert code == 0 and report["layers"]["semantic"] == "not-reached", report
        code, report = observe(file, "--stop-after=semantic")
        assert code == 0 and report["layers"]["mir"] == "not-reached", report
        file.write_text("class Entry { func main(): void { int value = false } }\n")
        code, report = observe(file)
        assert code == 1 and report["layers"]["parsing"] == "accepted", report
        assert report["layers"]["semantic"] == "rejected" and report["layers"]["mir"] == "not-reached", report
        file.write_text("class Entry { func main( }\n")
        code, report = observe(file)
        assert code == 1 and report["layers"]["parsing"] == "rejected", report
        # Filename contains JSON metacharacters and the diagnostic originates
        # in an imported module, not the entry file or a builtin.
        support = folder / 'support"quoted'
        support.mkdir()
        helper = folder / "Helper.zl"
        helper.write_text("class Helper {\n public static func checked(unknown value): int { return value }\n}\n")
        file.write_text("import Helper\nclass Entry { func main(): void { log(Helper.checked(1)) } }\n")
        code, report = observe(file)
        assert code == 0, report
        ds = [d for d in report["verification"]["diagnostics"] if d["function"].startswith("Helper.")]
        assert any(d["property"] == "dynamic-boundary" and d["location"]["file"] == str(helper)
                   and d["location"]["line"] == 2 for d in ds), ds
        quoted = support / "Entry.zl"
        quoted.write_text("class Entry { static func checked(unknown v): int { return v } func main(): void {} }")
        code, report = observe(quoted)
        assert code == 0 and any(d["location"]["file"] == str(quoted)
                                for d in report["verification"]["diagnostics"]), report
        # Regression for the corpus-discovered lowerer crash: a match binding
        # holding a callable is an SSA value, not a mutable slot.
        file.write_text('''class Entry { func main(): void {
 int|func(int): int operation = func(int x) => x + 2
 log(match operation { func(int): int invoke => invoke(4), int n => n, null => 0 })
} }''')
        code, report = observe(file)
        assert code in (0, 4, 6) and report["layers"]["semantic"] == "accepted", report
        file.write_text('''class Entry { func main(): void {
 int|string stable = 3
 var escaped = match stable { int _ => func() => stable + 1, _ => func() => 0 }
 stable = "later"
 log(escaped())
} }''')
        code, report = observe(file)
        assert code in (0, 4, 6) and report["layers"]["semantic"] == "accepted", report
        # Borrow scope endings must be represented explicitly, not erased by
        # intersection at a join. Covers normal, break and continue exits.
        for transfer in ("", "break", "continue"):
            body = "{ borrow Resource view = owner\n log(view.tag) }" if not transfer else (
                "for i in 0..1 { borrow Resource view = owner\n log(view.tag)\n " + transfer + " }")
            file.write_text('''class Resource { public int tag }
class Entry {
 static func consume(owned Resource value): void {}
 func main(): void {
 owned Resource owner = new Resource()
''' + body + "\n Entry.consume(move owner)\n} }")
            code, report = observe(file)
            assert code == 0, report
    spec = importlib.util.spec_from_file_location("evaluation", ROOT / "tools/safety/evaluate.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    rows = [dict(role="negative", observation={"layers": {"parsing": "accepted", "semantic": "rejected",
                                                         "mir": "not-reached"}},
                 runtime={"outcome": "not-reached"}, oracle_mismatches=[]),
            dict(role="control", observation={"layers": {"parsing": "accepted", "semantic": "accepted",
                                                         "mir": "partial"}},
                 runtime={"outcome": "timeout"}, oracle_mismatches=["timeout"])]
    totals = module.summary(rows)
    assert totals["first_static_rejection"] == {"semantic": 1, "none-observed": 1}, totals
    assert totals["mir"] == {"not-reached": 1, "partial": 1} and totals["oracle_mismatches"] == 1, totals
    print("safety pipeline: PASS (layer attribution, no execution, imports, JSON, callable match, accounting)")


if __name__ == "__main__":
    main()
