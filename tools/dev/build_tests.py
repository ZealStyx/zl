#!/usr/bin/env python3
"""Build the C++ test targets with g++ by reading CMakeLists.txt.

cmake is unavailable in this sandbox, so this mirrors the CMake test-target
definitions verbatim: it parses every zl_add_required_test /
zl_add_optional_test call (the single source of truth for (target, sources)
pairs), expands ${ZL_RUNTIME_VM_SOURCES}, compiles each shared source once
into build/obj (cached by mtime), builds the shared-library fixture, and
links each test target with exactly the sources CMake lists.

Usage: python3 tools/dev/build_tests.py [target]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CMAKE = os.path.join(ROOT, "CMakeLists.txt")
BUILD = os.path.join(ROOT, "build")
OBJ = os.path.join(BUILD, "obj")
FIXTURE_SO = os.path.join(BUILD, "libzl-test-native-library.so")

text = open(CMAKE).read()

# ${ZL_RUNTIME_VM_SOURCES} == every src/*.cpp except src/main.cpp.
vm_sources = []
for dirpath, _, files in os.walk(os.path.join(ROOT, "src")):
    for f in sorted(files):
        if f.endswith(".cpp") and os.path.join(dirpath, f) != os.path.join(ROOT, "src", "main.cpp"):
            vm_sources.append(os.path.join(dirpath, f))

targets = []
# Arguments may span multiple lines; source lists contain no parentheses.
for m in re.finditer(r"zl_add_(required|optional)_test\(\s*(\S+)\s+([^)]+)\)", text):
    kind, target, rest = m.group(1), m.group(2), m.group(3)
    parts = rest.split()
    primary = parts[0]
    extras = [e for e in parts[1:] if e != "${ZL_RUNTIME_VM_SOURCES}"]
    if "${ZL_RUNTIME_VM_SOURCES}" in parts[1:]:
        extras += vm_sources
    sources = [os.path.join(ROOT, primary)] + [os.path.join(ROOT, e) for e in extras]
    targets.append((kind, target, sources))

def missing_sources():
    # Required targets must have every source present (fail loud). Optional
    # targets may legitimately be absent on this platform; those are skipped
    # with a documented reason at link time, below.
    out = []
    for kind, t, ss in targets:
        if kind == "optional":
            continue
        for s in ss:
            if not os.path.exists(s):
                out.append((t, s))
    return out

def target_definitions(target):
    if target == "zl-native-library-tests":
        return [f'-DZL_TEST_NATIVE_LIBRARY_PATH="{FIXTURE_SO}"']
    if target == "zl-runtime-hardening-vm-tests" and os.path.exists("/bin/bash"):
        return [f'-DZL_BOUNDARY_LINT_ROOT="{ROOT}"', '-DZL_BOUNDARY_LINT_BASH="/bin/bash"']
    return []

def obj_path(src):
    rel = os.path.relpath(src, ROOT)
    return os.path.join(OBJ, rel + ".o")

CXXFLAGS = ["-std=c++17", "-O1", "-g", "-I", os.path.join(ROOT, "include"), "-pthread"]

def compile_obj(src):
    obj = obj_path(src)
    if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src):
        return 0
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    r = subprocess.run(["g++", *CXXFLAGS, "-c", src, "-o", obj])
    return r.returncode

missing = missing_sources()
if missing:
    for t, s in missing:
        print(f"MISSING SOURCE for {t}: {s}", file=sys.stderr)
    sys.exit(2)

os.makedirs(OBJ, exist_ok=True)

to_compile = []
seen = set()
for kind, target, sources in targets:
    for s in sources:
        if s not in seen and os.path.exists(s):
            seen.add(s)
            to_compile.append(s)
            os.makedirs(os.path.dirname(obj_path(s)), exist_ok=True)

# Parallel compilation (2 cores in this sandbox).
jobs = []
bad = 0
for s in to_compile:
    jobs.append((s, subprocess.Popen(["g++", *CXXFLAGS, "-c", s, "-o", obj_path(s)])))
    if len(jobs) == 2:
        for src, p in jobs:
            if p.wait() != 0:
                bad += 1
                print(f"compile failed: {src}", file=sys.stderr)
        jobs = []
for src, p in jobs:
    if p.wait() != 0:
        bad += 1
        print(f"compile failed: {src}", file=sys.stderr)
if bad:
    print(f"{bad} source file(s) failed to compile; see above", file=sys.stderr)
    sys.exit(1)

# The shared-library fixture (used by zl-native-library-tests).
fixture = os.path.join(ROOT, "tests", "fixtures", "native_library_fixture.cpp")
if os.path.exists(fixture) and (not os.path.exists(FIXTURE_SO) or
        os.path.getmtime(FIXTURE_SO) < os.path.getmtime(fixture)):
    subprocess.run(["g++", *CXXFLAGS, "-shared", "-fPIC", fixture, "-o", FIXTURE_SO],
                   check=True)

only = sys.argv[1] if len(sys.argv) > 1 else None
for kind, target, sources in targets:
    if only and target != only:
        continue
    if kind == "optional" and not os.path.exists(sources[0]):
        print(f"SKIP {target} (optional source absent)")
        continue
    out = os.path.join(BUILD, target)
    objs = [obj_path(s) for s in sources]
    # Sources shared across targets compile without flags. A target that needs
    # extra compile definitions recompiles its own primary source with them.
    defs = target_definitions(target)
    if defs:
        primary_obj = os.path.join(OBJ, target + ".primary.o")
        os.makedirs(os.path.dirname(primary_obj), exist_ok=True)
        prim = sources[0]
        if not os.path.exists(primary_obj) or os.path.getmtime(primary_obj) < os.path.getmtime(prim):
            subprocess.run(["g++", *CXXFLAGS, *defs, "-c", prim, "-o", primary_obj], check=True)
        objs[0] = primary_obj
    cmd = ["g++", *CXXFLAGS] + objs
    if target == "zl-native-library-tests":
        cmd.append("-ldl")
    cmd += ["-o", out]
    print("LINK", target, flush=True)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print(f"link failed: {target}", file=sys.stderr)
        sys.exit(1)

print("all targets built")
