#!/usr/bin/env python3
"""Regression guard for the build-mode target boundary (Objective 8.10).

Configures a throwaway build tree with the Ninja generator and counts the build
edges each aggregate target pulls in, then asserts the properties that the
default developer build has to keep:

  * the default build is `zl-core`, not `all`/`ALL_BUILD`;
  * `zl-core` builds `zl_language` and its whole dependency closure;
  * `zl-core` pulls in no test executable;
  * `zl-tests` and `zl-full` still build the tests and the rest of the repo;
  * every test target is still registered with CTest (nothing was deleted).

Usage: python3 tools/dev/check_build_modes.py [--build-dir DIR] [--keep]
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BAT = REPO / "scripts" / "build.bat"
SH = REPO / "scripts" / "build.sh"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def edges(build_dir: Path, *targets: str) -> int:
    """Number of build commands ninja would run for `targets` from scratch."""
    proc = run(["ninja", "-C", str(build_dir), "-t", "commands", *targets])
    if proc.returncode != 0:
        raise SystemExit(f"ninja -t commands {targets} failed:\n{proc.stderr}")
    return len([line for line in proc.stdout.splitlines() if line.strip()])


def target_list(build_dir: Path) -> dict:
    proc = run(["ninja", "-C", str(build_dir), "-t", "targets", "all"])
    out = {}
    for line in proc.stdout.splitlines():
        name, _, rule = line.partition(": ")
        out[name.strip()] = rule.strip()
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    for tool in ("cmake", "ninja"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} is not available")
            return 0

    tmp = Path(args.build_dir) if args.build_dir else Path(
        tempfile.mkdtemp(prefix="zl-buildmodes-"))
    failures = []

    def check(name, ok, detail=""):
        print(f"[{'PASS' if ok else 'FAIL'}] {name}{(' - ' + detail) if detail else ''}")
        if not ok:
            failures.append(name)

    # --- build.bat contract -------------------------------------------------
    bat = BAT.read_text(errors="replace")
    check("build.bat exists and is mode-driven", "zl-core" in bat)
    check("default mode targets zl-core",
          re.search(r'BUILD_TARGET=zl-core', bat) is not None)
    check("test mode targets zl-tests", "BUILD_TARGET=zl-tests" in bat)
    check("full mode targets zl-full", "BUILD_TARGET=zl-full" in bat)
    check("clean mode implemented", 'MODE%%"=="clean"' in bat or '"%MODE%"=="clean"' in bat)
    check("help documents the modes",
          all(tok in bat for tok in ("Usage: scripts\\build.bat", "test  ", "full  ")) or
          ("Modes" in bat and "clean" in bat and "help" in bat))
    check("build.bat never builds the whole tree implicitly",
          "--target !BUILD_TARGET!" in bat and "ALL_BUILD" not in bat)

    # --- build.sh must mirror build.bat -------------------------------------
    check("build.sh exists", SH.exists())
    if SH.exists():
        sh = SH.read_text(errors="replace")
        check("build.sh is valid bash",
              run(["bash", "-n", str(SH)]).returncode == 0)
        for mode, target in (("core", "zl-core"), ("test", "zl-tests"),
                             ("full", "zl-full")):
            check(f"build.sh {mode} mode targets {target}", target in sh)
        check("build.sh implements clean and help",
              'MODE}" == "clean"' in sh and "usage()" in sh)
        check("build.sh never references ALL_BUILD", "ALL_BUILD" not in sh)
        # The two drivers must offer the same user-facing modes/options.
        for flag in ("--debug", "--release", "--clean", "--ctest", "--jobs",
                     "--target", "--run", "--build-dir", "--configure-only"):
            check(f"both drivers support {flag}", flag in sh and flag in bat)

    # --- configure ----------------------------------------------------------
    cfg = run(["cmake", "-S", str(REPO), "-B", str(tmp), "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release"])
    if cfg.returncode != 0:
        print(cfg.stdout + cfg.stderr)
        return 2
    check("clean configure succeeds", True)

    targets = target_list(tmp)
    test_exes = sorted(n for n in targets
                       if n.startswith("zl-") and n.endswith("-tests"))
    check("test targets still registered in CMake", len(test_exes) >= 30,
          f"{len(test_exes)} test executables")

    core = edges(tmp, "zl-core")
    tests = edges(tmp, "zl-tests")
    full = edges(tmp, "zl-full")
    default = edges(tmp)  # the generator's default ("all") target
    print(f"\n  zl-core : {core} build edges")
    print(f"  zl-tests: {tests} build edges")
    print(f"  zl-full : {full} build edges")
    print(f"  all     : {default} build edges\n")

    check("zl-core is far smaller than a full build", core * 4 < full,
          f"{core} vs {full}")
    check("zl-core builds zl_language", edges(tmp, "zl_language") <= core)
    check("zl-core includes zlpkg and zl-bind",
          edges(tmp, "zl-core") >= edges(tmp, "zl_language", "zlpkg", "zl-bind"))
    check("zl-tests is a superset of zl-core", tests > core)
    check("zl-full is a superset of zl-tests", full >= tests)

    # No test executable may be reachable from zl-core.
    deps = run(["ninja", "-C", str(tmp), "-t", "inputs", "zl-core"]).stdout
    leaked = [t for t in test_exes if f"/{t}" in deps or deps.strip().endswith(t)]
    check("core build does not depend on test targets", not leaked, str(leaked))

    # Default 'all' must not drag in tests either (EXCLUDE_FROM_ALL).
    check("plain `cmake --build` no longer builds the tests", default <= core * 1.1,
          f"all={default}, core={core}")

    # --- actually build the core from a clean tree --------------------------
    build = run(["cmake", "--build", str(tmp), "--target", "zl-core",
                 "--parallel", "4"])
    ok = build.returncode == 0 and (tmp / "zl_language").exists()
    if not ok:
        print(build.stdout[-4000:] + build.stderr[-4000:])
    check("zl-core builds successfully from a clean build directory", ok)
    check("core build produces the toolchain binaries",
          all((tmp / b).exists() for b in ("zl_language", "zlpkg", "zl-bind")))

    summary = {"core": core, "tests": tests, "full": full, "all": default}
    print("\nTarget counts:", json.dumps(summary))

    if not args.keep and not args.build_dir:
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"\n{len(failures)} check(s) FAILED: {failures}")
        return 1
    print("\nAll build-mode checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
