#!/usr/bin/env python3
"""Generate a source-grounded ZL compiler/runtime/stdlib capability matrix.

This deliberately reports what exists; it does not infer missing APIs from
other languages. Ownership is reported as metadata status because the native
catalog currently exposes ownership fields but the catalog entries are legacy
ownership-neutral unless populated explicitly.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_MD = ROOT / "docs/status/ZL_STDLIB_CAPABILITY_MATRIX.md"
OUT_JSON = ROOT / "docs/status/ZL_STDLIB_CAPABILITY_MATRIX.json"


def parse_native_catalog() -> list[dict]:
    text = (ROOT / "src/compiler/native_catalog.cpp").read_text(encoding="utf-8")
    pattern = re.compile(
        r'NativeId::(?P<id>[A-Z0-9_]+),\s*"(?P<name>[^"]+)"'
    )
    rows = []
    for m in pattern.finditer(text):
        name = m.group("name")
        group = name.split(".", 1)[0]
        facade = {
            "Collection": "compiler builtin: List/Map/Set",
            "Queue": "stdlib: zl.util.Queue",
            "Stack": "stdlib: zl.util.Stack",
            "String": "stdlib: zl.text.Text + primitive String surface",
            "Text": "stdlib: zl.text.Text",
            "FileSystem": "stdlib: zl.fs.FileSystem",
            "Network": "stdlib: zl.net.Network",
            "Time": "stdlib: zl.time.*",
            "Thread": "stdlib: zl.lang.Thread",
            "Task": "stdlib: zl.lang.Task",
            "Mutex": "stdlib: zl.lang.Mutex",
            "RwLock": "stdlib: zl.lang.RwLock",
            "Atomic": "stdlib: zl.lang.Atomic",
            "Semaphore": "stdlib: zl.lang.Semaphore",
            "Condition": "stdlib: zl.lang.Condition",
            "Channel": "stdlib: zl.lang.Channel",
            "Test": "stdlib: zl.test.Test",
            "Log": "stdlib: zl.logging.Log",
            "Serialize": "stdlib: zl.serialize.Serialize",
            "Crypto": "stdlib: zl.crypto.Crypto",
            "System": "compiler/runtime system boundary",
            "IO": "compiler/runtime IO boundary",
            "Math": "compiler builtin: Math",
            "Type": "compiler builtin: Type",
            "Reflection": "compiler builtin: Reflection",
            "Shared": "compiler builtin: Shared",
            "Hash": "compiler/runtime primitive",
            "Int": "compiler/runtime primitive",
            "Double": "compiler/runtime primitive",
            "Bool": "compiler/runtime primitive",
        }.get(group, "compiler/runtime native surface")
        rows.append({
            "native_id": m.group("id"),
            "qualified_name": name,
            "domain": group,
            "source_layer": "native catalog + VM native runtime",
            "public_surface": facade,
            "status": "implemented",
            "ownership_metadata": "not explicitly declared in native catalog entry; default NONE",
        })
    return rows


def parse_builtin_classes() -> list[dict]:
    text = (ROOT / "src/compiler/builtin_library.cpp").read_text(encoding="utf-8")
    rows = []
    for m in re.finditer(r'^class\s+([A-Za-z_][A-Za-z0-9_<> ,]*)\s*\{', text, re.M):
        rows.append({"name": m.group(1).strip(), "source_layer": "compiler embedded builtin"})
    return rows


def parse_stdlib_files() -> list[dict]:
    rows = []
    for p in sorted((ROOT / "stdlib").rglob("*.zl")):
        text = p.read_text(encoding="utf-8")
        classes = re.findall(r'^class\s+([A-Za-z_][A-Za-z0-9_]*)', text, re.M)
        imports = re.findall(r'^import\s+([^\n]+)', text, re.M)
        methods = re.findall(
            r'^(?:\s*)(?:public\s+)?(?:static\s+)?(?:async\s+)?func\s+([A-Za-z_][A-Za-z0-9_]*)',
            text,
            re.M,
        )
        rows.append({
            "path": str(p.relative_to(ROOT)).replace('\\', '/'),
            "classes": classes,
            "imports": imports,
            "methods": methods,
        })
    return rows


def main() -> None:
    native = parse_native_catalog()
    builtins = parse_builtin_classes()
    files = parse_stdlib_files()

    # Validate the key S0 invariant directly from source.
    runtime_text = (ROOT / "src/vm/native.cpp").read_text(encoding="utf-8")
    runtime_ids = re.findall(r'NativeId::([A-Z0-9_]+),\s*', runtime_text)
    catalog_ids = [r["native_id"] for r in native]
    assert len(catalog_ids) == len(set(catalog_ids)), "duplicate native catalog ids"
    assert len(runtime_ids) == len(set(runtime_ids)), "duplicate runtime native ids"
    assert set(catalog_ids) == set(runtime_ids), "catalog/runtime native id mismatch"

    domains: dict[str, int] = {}
    for row in native:
        domains[row["domain"]] = domains.get(row["domain"], 0) + 1

    report = {
        "schema_version": 1,
        "source_files": [
            "include/zl/compiler/native_catalog.hpp",
            "src/compiler/native_catalog.cpp",
            "src/vm/native.cpp",
            "src/compiler/builtin_library.cpp",
            "stdlib/",
            "docs/reference/STDLIB_REFERENCE.md",
        ],
        "invariants": {
            "native_catalog_entries": len(native),
            "native_runtime_bindings": len(runtime_ids),
            "native_catalog_runtime_ids_match": True,
            "builtin_class_count": len(builtins),
            "stdlib_zl_file_count": len(files),
        },
        "native_domain_counts": dict(sorted(domains.items(), key=lambda kv: (-kv[1], kv[0]))),
        "builtin_classes": builtins,
        "stdlib_files": files,
        "native_capabilities": native,
    }

    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    OUT_JSON.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# ZL Standard Library Capability Matrix",
        "",
        "This matrix is generated from the repository's compiler builtin library, native catalog, VM native table, and `stdlib/` source tree.",
        "",
        "> **S0 rule:** classify implemented capabilities from ZL source, not by analogy with other languages or by counting `.zl` files.",
        "",
        "## Source invariants",
        "",
        f"- Native catalog entries: **{len(native)}**",
        f"- VM native bindings: **{len(runtime_ids)}**",
        f"- Compiler embedded builtin classes: **{len(builtins)}**",
        f"- Visible stdlib `.zl` files: **{len(files)}**",
        "- Native catalog IDs and VM binding IDs: **exact match**",
        "",
        "## Ownership metadata finding",
        "",
        "The native catalog type supports parameter and return ownership metadata, and the ABI/runtime enforces ownership tags at the native boundary. The audited native catalog entries do not populate explicit ownership fields, so this matrix records their catalog metadata as `NONE/default` rather than guessing runtime ownership semantics. Ownership-sensitive subsystems therefore require a separate semantic audit before APIs are labeled ownership-complete.",
        "",
        "## Native capability domains",
        "",
        "| Domain | Count | Source/facade | Status |",
        "|---|---:|---|---|",
    ]
    for domain, count in sorted(domains.items(), key=lambda kv: (-kv[1], kv[0])):
        facade = next(r["public_surface"] for r in native if r["domain"] == domain)
        lines.append(f"| `{domain}` | {count} | {facade} | implemented |")

    lines += ["", "## Compiler embedded builtin classes", "", "| Class | Layer |", "|---|---|"]
    for row in builtins:
        lines.append(f"| `{row['name']}` | {row['source_layer']} |")

    lines += ["", "## Stdlib source modules", "", "| Module | Classes | Methods | Imports |", "|---|---|---|---|"]
    for row in files:
        lines.append(
            f"| `{row['path']}` | `{', '.join(row['classes']) or '—'}` | `{', '.join(row['methods']) or '—'}` | `{', '.join(row['imports']) or '—'}` |"
        )

    lines += ["", "## Native capability detail", "", "| Native ID | Qualified name | Domain | Public surface | Status | Ownership metadata |", "|---|---|---|---|---|---|"]
    for row in native:
        lines.append(
            f"| `{row['native_id']}` | `{row['qualified_name']}` | `{row['domain']}` | {row['public_surface']} | {row['status']} | {row['ownership_metadata']} |"
        )

    OUT_MD.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {OUT_MD}")
    print(f"wrote {OUT_JSON}")
    print(f"native entries: {len(native)}; runtime bindings: {len(runtime_ids)}; builtins: {len(builtins)}; stdlib files: {len(files)}")


if __name__ == "__main__":
    main()
