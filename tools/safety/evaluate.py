#!/usr/bin/env python3
"""Evaluate actual compiler boundaries without bypassing earlier rejections.

Default corpus: existing tests/type_boundaries.py (imported, never duplicated).
Runtime execution is opt-in and uses the reference AST/bytecode backend, NOT
MIR, so a MIR lowering/verifier regression cannot masquerade as runtime safety.
Use only trusted inputs with --run-runtime; a timeout is not a sandbox.
"""
import argparse
import collections
import hashlib
import importlib.util
import json
import os
import pathlib
import platform
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def existing_corpus():
    path = ROOT / "tests/type_boundaries.py"
    spec = importlib.util.spec_from_file_location("zl_boundary_corpus", path)
    corpus = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(corpus)
    cases = []
    for name, body in corpus.INVALID.items():
        cases.append(dict(id=name, source="class TypeBoundaries { func main(): void {\n" + body + "\n} }\n",
                          filename="TypeBoundaries.zl", role="negative", expected_semantic="rejected"))
    cases.append(dict(id="inherited-field-contract", source=corpus.INVALID_HEAP_PROGRAM,
                      filename="TypeBoundaries.zl", role="negative", expected_semantic="rejected"))
    for name, source, expected in [("assignment-union-workflow", corpus.PROGRAM, corpus.EXPECTED),
                                   ("heap-contract-workflow", corpus.HEAP_PROGRAM, corpus.HEAP_EXPECTED)]:
        cases.append(dict(id=name, source=source, filename="TypeBoundaries.zl", role="runtime-guards",
                          expected_semantic="accepted", runtime={"stdout": expected, "returncode": 0, "stderr": ""}))
    cases.append(dict(id="async-failure-propagation", source=corpus.ASYNC_FAILURE_PROGRAM,
                      filename="TypeBoundaries.zl", role="intentional-exception", expected_semantic="accepted",
                      runtime={"returncode": 1, "stderr_contains": "async entry failed safely"}))
    return cases, {"path": str(path.relative_to(ROOT)), "sha256": sha(path.read_bytes())}


def invoke(command, timeout, env):
    try:
        run = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True,
                             errors="replace", timeout=timeout)
        return dict(command=command, returncode=run.returncode, stdout=run.stdout, stderr=run.stderr,
                    status="crash" if run.returncode < 0 else "completed")
    except subprocess.TimeoutExpired:
        return dict(command=command, status="timeout")


def summary(rows):
    result = {"cases": len(rows), "roles": dict(collections.Counter(r["role"] for r in rows))}
    for layer in ("parsing", "semantic", "mir"):
        result[layer] = dict(collections.Counter(r.get("observation", {}).get("layers", {}).get(layer, "unobserved")
                                               for r in rows))
    result["runtime"] = dict(collections.Counter(r["runtime"]["outcome"] for r in rows))
    # Warnings and partial verification never receive rejection credit.
    result["first_static_rejection"] = dict(collections.Counter(
        next((layer for layer in ("parsing", "semantic", "mir")
              if r.get("observation", {}).get("layers", {}).get(layer) == "rejected"), "none-observed")
        for r in rows))
    result["mir_rejections_by_role"] = dict(collections.Counter(
        r["role"] for r in rows if r.get("observation", {}).get("layers", {}).get("mir") == "rejected"))
    result["oracle_mismatches"] = sum(bool(r["oracle_mismatches"]) for r in rows)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--run-runtime", action="store_true")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--manifest", type=pathlib.Path,
                        help="JSON list: id, file, role, optional expected_semantic and runtime oracle")
    args = parser.parse_args()
    binary = args.binary.resolve()
    if args.manifest:
        manifest = args.manifest.resolve()
        cases = json.loads(manifest.read_text())
        provenance = {"path": str(manifest), "sha256": sha(manifest.read_bytes())}
        for case in cases:
            case["file"] = str((manifest.parent / case["file"]).resolve())
    else:
        cases, provenance = existing_corpus()
    env = dict(os.environ)
    env.setdefault("ZL_STDLIB_ROOT", str(binary.parent / "stdlib"))
    # Eliminate optimisation environment effects; observe unoptimised MIR.
    for key in list(env):
        if key.startswith("ZL_MIR_"):
            del env[key]
    rows = []
    with tempfile.TemporaryDirectory(prefix="zl-safety-") as directory:
        for n, case in enumerate(cases):
            if "source" in case:
                folder = pathlib.Path(directory) / str(n)
                folder.mkdir()
                path = folder / case["filename"]
                path.write_text(case["source"])
            else:
                path = pathlib.Path(case["file"])
            row = {"id": case["id"], "role": case["role"], "source_sha256": sha(path.read_bytes()),
                   "oracle_mismatches": []}
            result = invoke([str(binary), "--safety-check", str(path)], args.timeout, env)
            row["static_process"] = result
            try:
                observation = json.loads(result.get("stdout", ""))
                if observation.get("schema_version") != 1:
                    raise ValueError("unknown report schema")
                row["observation"] = observation
            except (ValueError, AttributeError):
                observation = {"outcome": result["status"], "layers": {"mir": "unobserved"}}
                row["oracle_mismatches"].append("missing or malformed static report")
                # A compiler crash loses the buffered report. Independent,
                # non-executing prefix probes recover only what they observe.
                row["prefix_probes"] = []
                for layer in ("parsing", "semantic"):
                    probe = invoke([str(binary), "--safety-check", str(path), "--stop-after=" + layer],
                                   args.timeout, env)
                    row["prefix_probes"].append(probe)
                    try:
                        observation["layers"][layer] = json.loads(probe.get("stdout", ""))["layers"][layer]
                    except (ValueError, KeyError, TypeError):
                        observation["layers"][layer] = "unobserved"
                row["observation"] = observation
            layers = observation.get("layers", {})
            expected = case.get("expected_semantic")
            if expected and layers.get("semantic") != expected:
                row["oracle_mismatches"].append("semantic outcome differs from existing oracle")
            row["runtime"] = {"outcome": "not-requested"}
            if args.run_runtime:
                if layers.get("semantic") != "accepted":
                    row["runtime"] = {"outcome": "not-reached"}
                else:
                    runtime = invoke([str(binary), str(path)], args.timeout, env)
                    oracle = case.get("runtime")
                    outcome = "unclassified"  # exit status alone is not a safety detector
                    if runtime["status"] in ("timeout", "crash"):
                        outcome = runtime["status"]
                    elif oracle:
                        matches = all(runtime.get(k) == v for k, v in oracle.items() if k != "stderr_contains")
                        matches &= oracle.get("stderr_contains", "") in runtime.get("stderr", "")
                        matches &= not any(x in runtime.get("stderr", "") for x in
                                           ("AddressSanitizer", "UndefinedBehaviorSanitizer"))
                        outcome = "oracle-confirmed" if matches else "oracle-mismatch"
                    else:
                        outcome = "completed-unclassified"
                    row["runtime"] = {"outcome": outcome, "process": runtime, "oracle": oracle}
                    if outcome in ("oracle-mismatch", "timeout", "crash"):
                        row["oracle_mismatches"].append("runtime oracle did not complete successfully")
            rows.append(row)
    git = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True)
    dirty = subprocess.run(["git", "diff", "--binary", "HEAD"], cwd=ROOT, capture_output=True)
    report = {"schema_version": 1, "corpus": provenance,
              "toolchain": {"revision": git.stdout.strip(), "tracked_diff_sha256": sha(dirty.stdout),
                            "binary_sha256": sha(binary.read_bytes()), "platform": platform.platform(),
                            "stdlib_root": env["ZL_STDLIB_ROOT"], "extra_roots": env.get("ZL_EXTRA_ROOTS", "")},
              "method": {"runtime_backend": "reference-ast-bytecode", "runtime_opt_in": args.run_runtime,
                         "timeout_seconds": args.timeout, "unit": "program/workflow, not log line or vulnerability",
                         "mir_rejection_may_be_compiler_defect": True},
              "summary": summary(rows), "cases": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))
    return 1 if report["summary"]["oracle_mismatches"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
