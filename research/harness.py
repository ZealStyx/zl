#!/usr/bin/env python3
"""MIR architecture evaluation harness.

Measures the ZL compiler pipeline (reference AST->bytecode vs MIR bytecode vs
MIR native; MIR unoptimised vs MIR optimised) over the research corpus, and
writes an aggregated, machine-readable results file plus a human summary.

Everything reported here is measured: per-stage timings come from the
compiler's own `--artifact-stats` ledger (millisecond-resolution, raw doubles),
code sizes come from the generated artifacts, and runtime/memory figures come
from wall-clock and `wait4` rusage of real process runs. Nothing is estimated
or inferred beyond what the numbers support.

Usage:
    python3 research/harness.py [--binary build/zl_language] \
        [--iterations 5] [--compile-iterations 3] [--out research/results/results.json]

Requires the binary to be built with the `--artifact-stats` measurement command
(the same build as the shipped CLI).
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "research" / "corpus"
POSITIVE = [
    "Prims", "ControlFlow", "Recursion", "Generics", "Unions", "Collections",
    "Objects", "Inheritance", "Interfaces", "Closures", "Ownership",
    "Exceptions", "Tasks", "Concurrency", "NativeResources", "Ffi",
]
LIMITATIONS = ["BareFuncParam", "FixedArrayNative", "InterfaceStaticParam"]


def run_measured(cmd, env=None, timeout=120):
    """Run `cmd`, capturing stdout/stderr, wall time and the child's peak RSS.

    Uses fork/exec/wait4 so the per-child rusage (ru_maxrss, KB on Linux) is the
    one from this exact child rather than a cumulative maximum over siblings.
    """
    out_fd, out_path = tempfile.mkstemp()
    err_fd, err_path = tempfile.mkstemp()
    try:
        pid = os.fork()
        if pid == 0:
            try:
                os.dup2(out_fd, 1)
                os.dup2(err_fd, 2)
                os.close(out_fd)
                os.close(err_fd)
                os.execvpe(cmd[0], cmd, env if env is not None else os.environ)
            finally:
                os._exit(127)
        os.close(out_fd)
        os.close(err_fd)
        t0 = time.perf_counter()
        _, status, ru = os.wait4(pid, 0)
        wall = time.perf_counter() - t0
        with open(out_path, "rb") as f:
            out = f.read().decode("utf-8", "replace")
        with open(err_path, "rb") as f:
            err = f.read().decode("utf-8", "replace")
        rc = os.waitstatus_to_exitcode(status)
        return out, err, rc, wall, ru.ru_maxrss
    finally:
        try:
            os.unlink(out_path)
            os.unlink(err_path)
        except OSError:
            pass


def stats(binary, program, extra=None, env_extra=None):
    """Run `--artifact-stats` and parse its JSON object."""
    cmd = [str(binary), "--artifact-stats"]
    if extra:
        cmd.extend(extra)
    cmd.append(str(program))
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    out, err, rc, wall, rss = run_measured(cmd, env=env)
    try:
        return json.loads(out.strip().splitlines()[-1]), err
    except (ValueError, IndexError):
        return None, err


def min_stats(binary, program, extra=None, env_extra=None, iters=3):
    """Run artifact-stats several times; return the per-field minimum timing."""
    runs = []
    for _ in range(iters):
        js, err = stats(binary, program, extra, env_extra)
        if js is not None:
            runs.append(js)
    if not runs:
        return None
    best = dict(runs[0])
    for key in list(best.keys()):
        if key.startswith("ms_") or key == "ms_wall":
            vals = [r[key] for r in runs if key in r]
            best[key] = min(vals) if vals else best[key]
    best["_runs"] = len(runs)
    return best


def execute(binary, program, mode, iters=5):
    """Run the program `iters` times; return stdout, median/min wall, peak RSS."""
    times, rsses = [], []
    out, err, rc = "", "", None
    for i in range(iters):
        if mode == "ref":
            cmd = [str(binary), "--reference-compiler", str(program)]
        elif mode == "mir-opt":
            cmd = [str(binary), "--mir-vm", str(program)]
        elif mode == "mir-unopt":
            cmd = [str(binary), "--mir-vm", str(program)]
            env = dict(os.environ); env["ZL_MIR_OPT"] = "0"
            o, e, c, w, r = run_measured(cmd, env=env)
        if mode != "mir-unopt":
            o, e, c, w, r = run_measured(cmd)
        out, err, rc = o, e, c
        times.append(w * 1000.0)
        rsses.append(r)
    return {
        "stdout": out, "stderr": err, "exit": rc,
        "wall_ms_median": sorted(times)[len(times) // 2],
        "wall_ms_min": min(times),
        "rss_kb_max": max(rsses),
    }


def safety_check(binary, program):
    cmd = [str(binary), "--safety-check", str(program)]
    out, err, rc, wall, rss = run_measured(cmd)
    try:
        return json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"error": "no-json", "stderr": err, "exit": rc}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(ROOT / "build" / "zl_language"))
    ap.add_argument("--iterations", type=int, default=5)
    ap.add_argument("--compile-iterations", type=int, default=3)
    ap.add_argument("--out", default=str(ROOT / "research" / "results" / "results.json"))
    ap.add_argument("--skip-run", action="store_true", help="skip runtime execution (compile-only)")
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        sys.exit(f"binary not found: {binary} (build it first: cmake --build build --target zl_language)")

    results = {
        "meta": {
            "binary": str(binary),
            "iterations": args.iterations,
            "compile_iterations": args.compile_iterations,
            "generated_by": Path(__file__).name,
        },
        "positive": {},
        "limitations": {},
        "safety": {},
    }

    # ---- Positive corpus: compile-time stats (4 configs) + runtime (3 modes)
    for name in POSITIVE:
        prog = CORPUS / f"{name}.zl"
        entry = {"program": name, "compile": {}, "runtime": {}, "notes": []}

        for label, extra, env in [
            ("ref", ["--reference-compiler"], None),
            ("mir-unopt", None, {"ZL_MIR_OPT": "0"}),
            ("mir-opt", None, None),
            ("native", ["--backend=native"], None),
        ]:
            js = min_stats(binary, prog, extra, env, args.compile_iterations)
            if js is None:
                entry["compile"][label] = {"ok": False, "error": "no-json"}
                continue
            entry["compile"][label] = js

        if not args.skip_run:
            outs = {}
            for mode in ("ref", "mir-opt", "mir-unopt"):
                entry["runtime"][mode] = execute(binary, prog, mode, args.iterations)
                outs[mode] = (entry["runtime"][mode]["stdout"], entry["runtime"][mode]["exit"])
            ref_key = (outs["ref"][0], outs["ref"][1])
            agree = all((outs[m][0], outs[m][1]) == ref_key for m in outs)
            entry["runtime"]["outputs_agree_with_reference"] = agree

        results["positive"][name] = entry
        print(f"positive  {name:16s} done")

    # ---- Limitations: how each fails / degrades
    for name in LIMITATIONS:
        prog = CORPUS / "limitations" / f"{name}.zl"
        sc = safety_check(binary, prog)
        js, err = stats(binary, prog)
        results["limitations"][name] = {
            "safety_check": sc,
            "artifact_stats": js,
        }
        outcome = sc.get("outcome") if isinstance(sc, dict) else "?"
        print(f"limit     {name:16s} outcome={outcome}")

    # ---- Safety corpus: static vs runtime classification
    manifest = CORPUS / "safety" / "manifest.json"
    with open(manifest) as f:
        m = json.load(f)
    for p in m["programs"]:
        prog = CORPUS / "safety" / p["file"]
        sc = safety_check(binary, prog)
        rec = {"role": p["role"], "expect": p["expect"], "safety_check": sc}
        if p["role"] == "runtime-check":
            rec["runtime"] = execute(binary, prog, "mir-opt", args.iterations)
        results["safety"][p["id"]] = rec
        print(f"safety    {p['id']:24s} role={p['role']}")

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nwrote {out_path}")

    # ---- Human summary
    print_summary(results)


def print_summary(r):
    pos = r["positive"]
    print("\n" + "=" * 78)
    print("SUMMARY")
    print("=" * 78)

    # Lowering / verification success
    lowered = verif = incompl = 0
    for name, e in pos.items():
        c = e["compile"].get("mir-opt") or {}
        if c.get("ok"):
            lowered += 1
            if c.get("verification_errors", 0) == 0 and c.get("incomplete_functions", 0) == 0:
                verif += 1
            if c.get("incomplete_functions", 0):
                incompl += 1
    print(f"positive corpus: {len(pos)} programs")
    print(f"  fully lowered (ok):        {lowered}/{len(pos)}")
    print(f"  verified clean (0 errors, 0 incomplete): {verif}/{len(pos)}")
    print(f"  with incomplete functions: {incompl}/{len(pos)}")

    # Aggregate stage timings (median across programs), MIR-opt bytecode path
    def median(key):
        vals = []
        for e in pos.values():
            c = e["compile"].get("mir-opt") or {}
            if key in c:
                vals.append(c[key])
        return sorted(vals)[len(vals) // 2] if vals else 0.0

    print("\ncompile-stage medians (ms, MIR-opt bytecode path):")
    for label, key in [
        ("load", "ms_load"), ("semantic", "ms_semantic"),
        ("typed lowering (MIR construction)", "ms_lower"),
        ("MIR verification", "ms_verify"), ("MIR optimisation", "ms_opt"),
        ("bytecode generation", "ms_codegen"), ("total (sum of stages)", "ms_total"),
    ]:
        print(f"  {label:38s} {median(key):8.2f}")

    # Reference vs MIR codegen
    ref_cg = median_ref = 0.0
    refs = [e["compile"].get("ref", {}).get("ms_codegen", 0) for e in pos.values()]
    ref_cg = sorted(refs)[len(refs) // 2]
    print(f"\nreference AST->bytecode codegen median: {ref_cg:.2f} ms")

    # Code sizes (median bytes)
    def size_median(label, key):
        vals = [e["compile"].get(label, {}).get(key, 0) for e in pos.values()]
        vals = [v for v in vals if v]
        return sorted(vals)[len(vals) // 2] if vals else 0
    print("\ngenerated code size medians (bytes):")
    print(f"  bytecode: ref={size_median('ref','bytecode_bytes')}  "
          f"mir-unopt={size_median('mir-unopt','bytecode_bytes')}  "
          f"mir-opt={size_median('mir-opt','bytecode_bytes')}")
    print(f"  MIR instructions: unopt={size_median('mir-unopt','mir_instructions')}  "
          f"opt={size_median('mir-opt','mir_instructions')}")
    print(f"  native machine code (compiled subset): {size_median('native','native_code_bytes')} bytes")

    # Runtime
    if any("runtime" in e and e["runtime"] for e in pos.values()):
        print("\nruntime (median wall ms / peak RSS KB), [ref | mir-opt | mir-unopt]:")
        for name, e in pos.items():
            rt = e.get("runtime") or {}
            if not rt:
                continue
            cells = []
            for mode in ("ref", "mir-opt", "mir-unopt"):
                m = rt.get(mode) or {}
                cells.append(f"{m.get('wall_ms_median', 0):.0f}/{m.get('rss_kb_max', 0)}")
            agree = rt.get("outputs_agree_with_reference")
            print(f"  {name:16s} {' | '.join(cells):28s} agree={agree}")

    # Safety
    saf = r["safety"]
    static = [v for v in saf.values() if v["role"] == "static-reject"]
    dynamic = [v for v in saf.values() if v["role"] == "runtime-check"]
    sem_rej = sum(1 for v in static if v["safety_check"].get("layers", {}).get("semantic") == "rejected")
    dyn_ver = sum(1 for v in dynamic if v["safety_check"].get("layers", {}).get("mir") == "verified")
    dyn_caught = sum(1 for v in dynamic if "caught" in (v.get("runtime") or {}).get("stdout", ""))
    print(f"\nsafety: {len(static)} static-reject cases -> {sem_rej} rejected at semantic layer")
    print(f"  {len(dynamic)} runtime-check cases -> {dyn_ver} verified statically, "
          f"{dyn_caught} demonstrated caught runtime checks")

    # Limitations
    print("\nlimitations:")
    for name, e in r["limitations"].items():
        sc = e.get("safety_check") or {}
        print(f"  {name:22s} outcome={sc.get('outcome')}  layers={sc.get('layers')}")


if __name__ == "__main__":
    main()
