#!/usr/bin/env python3
"""Unified ZL test discovery and runner.

This command is intentionally editor/CI friendly: it can list runnable examples and
regression cases as JSON, then run a filtered subset through a supplied runtime.
The existing shell/batch scripts remain supported; this tool gives integrations one
stable cross-platform entry point.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

EXPECTED_BEGIN = "// --- Expected output (verified by examples/run_all.sh) ---"
EXPECTED_END = "// --- End expected output ---"


@dataclass(frozen=True)
class TestCase:
    id: str
    scope: str
    category: str
    name: str
    kind: str
    path: str
    expected_exit: int
    expected_output: Optional[str] = None


@dataclass(frozen=True)
class TestResult:
    id: str
    status: str
    exit_code: Optional[int]
    expected_exit: int
    output: str
    message: str = ""


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def is_under_lib_dir(path: Path) -> bool:
    return any(part == "_lib" for part in path.parts)


def has_main(path: Path) -> bool:
    try:
        return "func main(" in path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return False


def expected_output(path: Path) -> Optional[str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    try:
        start = lines.index(EXPECTED_BEGIN)
        stop = lines.index(EXPECTED_END)
    except ValueError:
        return None
    out: List[str] = []
    for line in lines[start + 1 : stop]:
        if line.startswith("// "):
            out.append(line[3:])
        elif line == "//":
            out.append("")
        else:
            out.append(line)
    return "\n".join(out)


def discover_example_cases(root: Path) -> List[TestCase]:
    examples = root / "examples"
    cases: List[TestCase] = []
    if not examples.exists():
        return cases
    for path in sorted(examples.rglob("*.zl")):
        if is_under_lib_dir(path):
            continue
        rel = path.relative_to(root)
        category = path.parent.name if path.parent != examples else "examples"
        cases.append(
            TestCase(
                id="examples/" + str(path.relative_to(examples)).replace(os.sep, "/"),
                scope="examples",
                category=category,
                name=path.stem,
                kind="zl",
                path=str(rel),
                expected_exit=0,
                expected_output=expected_output(path),
            )
        )
    return cases


def find_entry(directory: Path) -> Optional[Path]:
    for path in sorted(directory.rglob("*.zl")):
        if has_main(path):
            return path
    return None


def find_manifest(case_dir: Path) -> Optional[Path]:
    direct = case_dir / "zlpkg.toml"
    if direct.exists():
        return direct
    nested = case_dir / "project" / "zlpkg.toml"
    if nested.exists():
        return nested
    return None


def list_categories(root: Path) -> Iterable[Path]:
    if not root.exists():
        return []
    return sorted(path for path in root.iterdir() if path.is_dir() and path.name != "demos")


def discover_regression_cases(root: Path) -> List[TestCase]:
    tests_root = root / "tests" / "zl"
    cases: List[TestCase] = []
    for validity, expected in (("invalid", 1), ("valid", 0)):
        validity_root = tests_root / validity
        for category_dir in list_categories(validity_root):
            for entry in sorted(category_dir.iterdir()):
                if entry.is_dir():
                    if entry.name == "closure_support":
                        continue
                    manifest = find_manifest(entry)
                    if manifest:
                        project_dir = manifest.parent
                        cases.append(
                            TestCase(
                                id=f"regressions/{validity}/{category_dir.name}/{entry.name}",
                                scope="regressions",
                                category=f"{validity}/{category_dir.name}",
                                name=entry.name,
                                kind="pkg",
                                path=str(project_dir.relative_to(root)),
                                expected_exit=expected,
                            )
                        )
                        continue
                    case_entry = find_entry(entry)
                    if case_entry:
                        cases.append(
                            TestCase(
                                id=f"regressions/{validity}/{category_dir.name}/{entry.name}",
                                scope="regressions",
                                category=f"{validity}/{category_dir.name}",
                                name=entry.name,
                                kind="zl",
                                path=str(case_entry.relative_to(root)),
                                expected_exit=expected,
                            )
                        )
                elif entry.suffix == ".zl" and has_main(entry):
                    cases.append(
                        TestCase(
                            id=f"regressions/{validity}/{category_dir.name}/{entry.stem}",
                            scope="regressions",
                            category=f"{validity}/{category_dir.name}",
                            name=entry.stem,
                            kind="zl",
                            path=str(entry.relative_to(root)),
                            expected_exit=expected,
                        )
                    )
    return cases


def discover_cases(root: Path, scope: str) -> List[TestCase]:
    cases: List[TestCase] = []
    if scope in ("all", "examples"):
        cases.extend(discover_example_cases(root))
    if scope in ("all", "regressions"):
        cases.extend(discover_regression_cases(root))
    return cases


def find_runtime(root: Path, explicit: Optional[str]) -> Optional[Path]:
    candidates: List[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get("ZL_COMPILER_PATH"):
        candidates.append(Path(os.environ["ZL_COMPILER_PATH"]))
    candidates.extend(
        [
            root / "build" / "zl_language",
            root / "build" / "Release" / "zl_language",
            root / "build" / "Debug" / "zl_language",
            root / "zl_language",
        ]
    )
    for name in ("zl_language", "zl"):
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    for candidate in candidates:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    return None


def example_roots(root: Path) -> List[str]:
    examples = root / "examples"
    if not examples.exists():
        return []
    roots: List[str] = []
    for lib in sorted(path for path in examples.rglob("_lib") if path.is_dir()):
        roots.extend(["--root", str(lib)])
    return roots


def clean_package_outputs(project_dir: Path) -> None:
    for generated in (project_dir / ".zlpkg", project_dir / "zlpkg.lock"):
        if generated.is_dir():
            shutil.rmtree(generated, ignore_errors=True)
        elif generated.exists():
            generated.unlink()


def run_case(root: Path, case: TestCase, runtime: Path, zlpkg: Optional[Path], timeout: float) -> TestResult:
    full_path = root / case.path
    if case.kind == "pkg":
        if zlpkg is None:
            return TestResult(case.id, "skip", None, case.expected_exit, "", "zlpkg was not found next to the runtime")
        entry = find_entry(full_path)
        if entry is None:
            return TestResult(case.id, "fail", None, case.expected_exit, "", "no main()-declaring .zl file found")
        rel_entry = str(entry.relative_to(full_path))
        clean_package_outputs(full_path)
        try:
            proc = subprocess.run([str(zlpkg), "run", rel_entry], cwd=full_path, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, check=False)
        finally:
            clean_package_outputs(full_path)
    else:
        command = [str(runtime)]
        if case.scope == "examples":
            command.extend(example_roots(root))
        command.append(str(full_path))
        proc = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, check=False)

    output = proc.stdout.rstrip("\n")
    if proc.returncode != case.expected_exit:
        return TestResult(case.id, "fail", proc.returncode, case.expected_exit, output, "unexpected exit code")
    if case.expected_output is not None and output != case.expected_output:
        return TestResult(case.id, "fail", proc.returncode, case.expected_exit, output, "output differs from expected block")
    return TestResult(case.id, "pass", proc.returncode, case.expected_exit, output)


def apply_filters(cases: Sequence[TestCase], patterns: Sequence[str]) -> List[TestCase]:
    if not patterns:
        return list(cases)
    selected: List[TestCase] = []
    for case in cases:
        haystacks = (case.id, case.name, case.category, case.path)
        for pattern in patterns:
            if any(fnmatch.fnmatch(text, pattern) or pattern in text for text in haystacks):
                selected.append(case)
                break
    return selected


def print_case_list(cases: Sequence[TestCase], json_output: bool) -> int:
    if json_output:
        print(json.dumps([asdict(case) for case in cases], indent=2))
    else:
        for case in cases:
            expected = "with expected output" if case.expected_output is not None else f"exit {case.expected_exit}"
            print(f"{case.id}\t{case.kind}\t{case.path}\t{expected}")
    return 0


def run_cases(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    runtime = find_runtime(root, args.runtime)
    if runtime is None:
        print("error: cannot find zl_language; pass --runtime /path/to/zl_language", file=sys.stderr)
        return 2
    zlpkg_candidate = runtime.parent / ("zlpkg.exe" if os.name == "nt" else "zlpkg")
    zlpkg = zlpkg_candidate if zlpkg_candidate.exists() and os.access(zlpkg_candidate, os.X_OK) else None
    cases = apply_filters(discover_cases(root, args.scope), args.case)
    results: List[TestResult] = []
    for case in cases:
        try:
            result = run_case(root, case, runtime, zlpkg, args.timeout)
        except subprocess.TimeoutExpired as exc:
            result = TestResult(case.id, "fail", None, case.expected_exit, (exc.stdout or "").rstrip("\n"), f"timed out after {args.timeout:g}s")
        results.append(result)
        if not args.json:
            label = result.status.upper()
            suffix = f" ({result.message})" if result.message else ""
            print(f"{label:4} {result.id}{suffix}")
            if result.status == "fail" and result.output:
                for line in result.output.splitlines():
                    print(f"     | {line}")
    if args.json:
        print(json.dumps([asdict(result) for result in results], indent=2))
    failed = sum(1 for result in results if result.status == "fail")
    if not args.json:
        passed = sum(1 for result in results if result.status == "pass")
        skipped = sum(1 for result in results if result.status == "skip")
        print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
    return 1 if failed else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Discover and run ZL examples/regressions")
    parser.add_argument("--root", default=str(repo_root()), help="repository root, default: current checkout")
    sub = parser.add_subparsers(dest="command", required=True)

    list_cmd = sub.add_parser("list", help="list discovered tests")
    list_cmd.add_argument("--scope", choices=["all", "examples", "regressions"], default="all")
    list_cmd.add_argument("--case", action="append", default=[], help="substring or glob filter; may be repeated")
    list_cmd.add_argument("--json", action="store_true")

    run_cmd = sub.add_parser("run", help="run discovered tests")
    run_cmd.add_argument("--scope", choices=["all", "examples", "regressions"], default="all")
    run_cmd.add_argument("--case", action="append", default=[], help="substring or glob filter; may be repeated")
    run_cmd.add_argument("--runtime", help="path to zl_language; auto-detected when omitted")
    run_cmd.add_argument("--timeout", type=float, default=20.0, help="per-case timeout in seconds")
    run_cmd.add_argument("--json", action="store_true")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    if args.command == "list":
        return print_case_list(apply_filters(discover_cases(root, args.scope), args.case), args.json)
    if args.command == "run":
        return run_cases(args)
    parser.error("unknown command")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
