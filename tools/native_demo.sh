#!/usr/bin/env bash
# Walks one ZL program through every stage of the native pipeline and proves
# the result: ZL -> MIR -> native IR -> x86-64 machine code, then *executes*
# the emitted bytes and diffs them against what the VM produces.
#
#   tools/native_demo.sh [path/to/zl_language]
#
# Needs objdump (for the disassembly listing) and a C compiler (to call the
# emitted bytes). Both are optional: the script degrades to whichever stages
# it can run.

set -uo pipefail

ZL="${1:-./build/zl_language}"
if [ ! -x "$ZL" ]; then
    echo "usage: $0 [path/to/zl_language]   (built binary not found at '$ZL')" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/Demo.zl" <<'ZLEOF'
class Demo {
    // A counted loop over mutable locals: exercises slots, a loop header,
    // a back edge, and a comparison.
    static func sumTo(int n): int {
        var acc = 0
        var i = 1
        while (i <= n) {
            acc = acc + i
            i = i + 1
        }
        return acc
    }

    // A two-way branch.
    static func biggest(int a, int b): int {
        if (a > b) {
            return a
        }
        return b
    }

    // Floating point: a different value class, a different register bank.
    static func mix(double x, double y): double {
        return x * y + 1.5
    }

    func main(): void {
        log("sumTo(10)    = " + Demo.sumTo(10))
        log("sumTo(0)     = " + Demo.sumTo(0))
        log("sumTo(100)   = " + Demo.sumTo(100))
        log("biggest(3,9) = " + Demo.biggest(3, 9))
        log("biggest(9,3) = " + Demo.biggest(9, 3))
        log("mix(2.0,4.0) = " + Demo.mix(2.0, 4.0))
        log("mix(-1.5,2.0)= " + Demo.mix(-1.5, 2.0))
    }
}
ZLEOF

rule() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

rule "0. the program"
cat "$WORK/Demo.zl"

rule "1. MIR (what the native backend consumes - verified, never the AST)"
"$ZL" --emit-mir - "$WORK/Demo.zl" 2>/dev/null |
    awk '/^func Demo\.(sumTo|biggest|mix)/,/^$/'

rule "2. native IR (types collapsed to value classes, opcodes now typed)"
"$ZL" --emit-native-ir - "$WORK/Demo.zl" 2>/dev/null | awk '/^func Demo\./,/^}/'

rule "3. the per-function ledger (every function is native or VM, by name)"
"$ZL" --emit-native-ir - "$WORK/Demo.zl" 2>&1 >/dev/null | grep -E '^native:|Demo\.'

rule "4. machine code"
"$ZL" --emit-native-code "$WORK/out.txt" "$WORK/Demo.zl" 2>/dev/null

python3 - "$WORK" <<'PYEOF'
import os, re, subprocess, sys
work = sys.argv[1]
section = open(os.path.join(work, "out.txt")).read().split("; machine code\n", 1)[1]
os.makedirs(os.path.join(work, "bin"), exist_ok=True)
have_objdump = subprocess.run(["which", "objdump"], capture_output=True).returncode == 0
for name, frame, count, hexbytes in re.findall(
        r"; (\S+) frame=(\d+) bytes=(\d+)\n((?:[0-9a-f]{2}[ \n])+)", section):
    if not name.startswith("Demo."):
        continue
    data = bytes(int(b, 16) for b in hexbytes.split())
    assert len(data) == int(count), (name, len(data), count)
    path = os.path.join(work, "bin", re.sub(r"\W", "_", name) + ".bin")
    open(path, "wb").write(data)
    print(f"\n--- {name}   frame={frame} bytes={count} ---")
    if not have_objdump:
        print("(objdump unavailable; raw bytes written to " + path + ")")
        continue
    out = subprocess.run(
        ["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel", path],
        capture_output=True, text=True).stdout
    for line in out.splitlines():
        if re.match(r"^\s+[0-9a-f]+:", line):
            print(line)
PYEOF

rule "5. execute the emitted bytes and compare against the VM"
cat > "$WORK/run.c" <<'CEOF'
// Nothing here knows anything about ZL. It maps the emitted bytes executable
// and calls them as ordinary System V functions, which is the whole claim.
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
static void* load(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void* m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fread(m, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(1); }
    fclose(f); mprotect(m, 4096, PROT_READ | PROT_EXEC); return m;
}
static void* at(const char* dir, const char* file) {
    static char p[512];
    snprintf(p, sizeof p, "%s/bin/%s", dir, file);
    return load(p);
}
int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: run <workdir>\n"); return 2; }
    long   (*sumTo)(long)           = at(argv[1], "Demo_sumTo_int_.bin");
    long   (*biggest)(long, long)   = at(argv[1], "Demo_biggest_int_int_.bin");
    double (*mix)(double, double)   = at(argv[1], "Demo_mix_double_double_.bin");
    printf("sumTo(10)    = %ld\n", sumTo(10));
    printf("sumTo(0)     = %ld\n", sumTo(0));
    printf("sumTo(100)   = %ld\n", sumTo(100));
    printf("biggest(3,9) = %ld\n", biggest(3, 9));
    printf("biggest(9,3) = %ld\n", biggest(9, 3));
    printf("mix(2.0,4.0) = %g\n", mix(2.0, 4.0));
    printf("mix(-1.5,2.0)= %g\n", mix(-1.5, 2.0));
    return 0;
}
CEOF

CC="$(command -v cc || command -v gcc || command -v clang || true)"
if [ -z "$CC" ]; then
    echo "no C compiler found; skipping the execution comparison" >&2
    exit 0
fi

"$CC" -O1 -o "$WORK/run" "$WORK/run.c" || exit 1
"$ZL" "$WORK/Demo.zl" > "$WORK/vm.txt" 2>&1
"$WORK/run" "$WORK" > "$WORK/native.txt" 2>&1

echo "--- VM (reference) ---";              cat "$WORK/vm.txt"
echo "--- native (executed bytes) ---";     cat "$WORK/native.txt"

if diff -u "$WORK/vm.txt" "$WORK/native.txt" > "$WORK/diff.txt"; then
    printf '\n\033[1mIDENTICAL: the emitted machine code matches the VM.\033[0m\n'
    exit 0
fi
printf '\nMISMATCH between the VM and the native code:\n'
cat "$WORK/diff.txt"
exit 1
