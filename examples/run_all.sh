#!/usr/bin/env bash
#
# examples/run_all.sh - run every example and diff its real stdout against the
# "// Expected output" block embedded at the bottom of the same file.
#
# Usage:
#   examples/run_all.sh [path/to/zl_language]
#
# With no argument it looks for ../build/zl_language, then for `zl_language`
# and `zl` on PATH. Exit status is 0 only if every example ran cleanly AND
# matched its expected block byte-for-byte.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ZL=""
if [[ $# -ge 1 ]]; then
    ZL="$1"
elif [[ -x "$ROOT/build/zl_language" ]]; then
    ZL="$ROOT/build/zl_language"
elif command -v zl_language >/dev/null 2>&1; then
    ZL="$(command -v zl_language)"
elif command -v zl >/dev/null 2>&1; then
    ZL="$(command -v zl)"
fi

if [[ -z "$ZL" || ! -x "$ZL" ]]; then
    echo "error: cannot find the zl_language runtime" >&2
    echo "hint: build it first, or pass the path: $0 /path/to/zl_language" >&2
    exit 2
fi

PY="${PYTHON:-python3}"
command -v "$PY" >/dev/null 2>&1 || { echo "error: python3 is required to diff expected output" >&2; exit 2; }

total=0
passed=0
failed=()
skipped=0

# Directories named `_lib` hold importable module sources rather than runnable
# examples: they declare an interface, a data record, or a class with no main().
# Skip them as examples, but expose each one to the importer via --root so the
# examples that `import` them still resolve.
ROOTS=()
while IFS= read -r libdir; do
    [[ -d "$libdir" ]] && ROOTS+=("--root" "$libdir")
done < <(find "$SCRIPT_DIR" -type d -name '_lib' | sort)

# Collect examples in a stable order: directory name, then file name.
while IFS= read -r file; do
    rel="${file#"$ROOT"/}"
    total=$((total + 1))

    actual="$("$ZL" ${ROOTS[@]+"${ROOTS[@]}"} "$file" 2>&1)"
    status=$?

    expected="$("$PY" - "$file" <<'PYEOF'
import sys
BEGIN = "// --- Expected output (verified by examples/run_all.sh) ---"
END = "// --- End expected output ---"
lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
try:
    start = lines.index(BEGIN)
    stop = lines.index(END)
except ValueError:
    sys.exit(3)
out = []
for line in lines[start + 1:stop]:
    out.append(line[3:] if line.startswith("// ") else (line[2:] if line == "//" else line))
print("\n".join(out))
PYEOF
)"
    parse=$?

    if [[ $parse -eq 3 ]]; then
        echo "SKIP  $rel  (no Expected output block)"
        skipped=$((skipped + 1))
        total=$((total - 1))
        continue
    fi

    if [[ $status -ne 0 ]]; then
        echo "FAIL  $rel  (runtime exited $status)"
        echo "$actual" | sed 's/^/        | /'
        failed+=("$rel")
        continue
    fi

    if [[ "$actual" == "$expected" ]]; then
        echo "PASS  $rel"
        passed=$((passed + 1))
    else
        echo "FAIL  $rel  (output differs from Expected output block)"
        diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") | sed 's/^/        /'
        failed+=("$rel")
    fi
done < <(find "$SCRIPT_DIR" -name '*.zl' -type f -not -path '*/_lib/*' | sort)

echo
echo "------------------------------------------------------------"
if [[ ${#failed[@]} -eq 0 ]]; then
    echo "$passed/$total PASS"
    [[ $skipped -gt 0 ]] && echo "($skipped example(s) have no Expected output block and were skipped)"
    exit 0
fi
echo "$passed/$total PASS, ${#failed[@]} FAIL"
for f in "${failed[@]}"; do echo "  - $f"; done
exit 1
