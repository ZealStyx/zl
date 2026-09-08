#!/usr/bin/env python3
"""Focused assignment/union regression: python3 tests/type_boundaries.py /path/to/zl.

One end-to-end program verifies stores, flow and lexical binding identity. Negative
variants must fail in --check, not merely trip a VM assertion later. Generated
sources live in one temporary directory, not in the examples corpus.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

PROGRAM = r'''
data Packet { code: int }
class Cell<T> {
    public func replace(T initial, unknown replacement): T {
        try { initial = replacement } catch error { log("generic rejected") }
        return initial
    }
    public func optional(T|nil value): T|nil { return value }
    public func use(func(T): T f, T value): T { return f(value) }
}
class TypeBoundaries {
    static func dynamic(unknown value): unknown { return value }
    static func label(int|string|bool value): string {
        return match value {
            int n when n < 0 => "negative"
            int _ => "number " + (value + 1)
            string _ => String.upper(value)
            bool flag => "flag " + flag
            null => "nil"
        }
    }
    static func boolWord(bool flag): string {
        if flag { return "yes" }
        return "no"
    }
    static func rest(int|bool value): string {
        return match value { int n => "int", other => TypeBoundaries.boolWord(other) }
    }
    static func relay(int|string value): int|string { return value }
    static func snapshot(int|string value): string {
        var text = match value {
            int n when (value = "changed") == "" => "never"
            int n => "snapshot " + n
            string s => s
            null => "nil"
        }
        return text + "/" + value
    }
    func main(): void {
        int number = 7
        try { number = TypeBoundaries.dynamic("wrong") } catch error { log("typed rejected") }
        log("preserved " + number)
        var inferred = 8
        try { inferred = TypeBoundaries.dynamic(false) } catch error { log("inferred rejected") }
        log("inferred " + inferred)
        int|string choice = 9
        try { choice = TypeBoundaries.dynamic(false) } catch error { log("union rejected") }
        choice = "new"
        log("union " + choice)
        double wide = 1.5
        wide = TypeBoundaries.dynamic(3)
        log("wide " + wide)
        func(int): int identity = func(x) => x
        try { identity = TypeBoundaries.dynamic(func(string x) => String.length(x)) }
        catch error { log("callable rejected") }
        identity = func(x) => x + 1
        log("callable " + identity(4))
        var inferredFunction = func(int x) => x
        inferredFunction = func(x) => x + 2
        log("inferred context " + inferredFunction(4))
        var apply = func(func(int): int f, int x) => f(x)
        log("typed callback " + apply(func(int x) => x * 2, 6))
        List<int> items = [1]
        items = [2, 3]
        log("literal " + items.last())
        var inferredItems = new List<int>()
        inferredItems = [4, 5]
        log("inferred literal " + inferredItems.last())
        array[2]<int> fixed = [1, 2]
        try { fixed = TypeBoundaries.dynamic([3]) } catch error { log("array rejected") }
        log("fixed " + fixed)
        log("generic " + new Cell<int>().replace(11, "wrong"))
        log("generic callback " + new Cell<int>().use(identity, 7))
        List<int|bool> variants = [1, true]
        List<bool|int> reordered = variants
        log("canonical union " + reordered.length())
        int|func(int): int operation = func(int x) => x + 2
        var callResult = match operation {
            func(int): int invoke => invoke(4)
            int n => n
            null => 0
        }
        log("function union " + callResult)
        Packet|int packet = Packet { code: 7 }
        var packetText = match packet {
            Packet { code: n } => "packet " + (packet.code + n)
            int n => "number " + n
            null => "missing"
        }
        log(packetText)
        int|nil optional = new Cell<int>().optional(12)
        log("optional " + optional)
        log(TypeBoundaries.label(-1))
        log(TypeBoundaries.label(4))
        log(TypeBoundaries.label("ok"))
        log(TypeBoundaries.label(true))
        log(TypeBoundaries.label(null))
        log("remaining " + TypeBoundaries.rest(false))
        var alias = TypeBoundaries.relay("alias")
        log(TypeBoundaries.label(alias))
        log(TypeBoundaries.snapshot(5))
        int|string stable = 3
        var escaped = match stable {
            int _ => func() => stable + 1
            _ => func() => 0
        }
        stable = "later"
        log("captured " + escaped())
        int shadow = 10
        { string shadow = "inner"
          log(shadow) }
        log("outer " + shadow)
        int|bool subject = 6
        var copy = match subject { int subject => (subject = 12), _ => 0 }
        log("pattern copy " + copy + "/" + subject)
        int i = 90
        var worker = Task.spawn(func() {
            var total = 0
            for i in 0..3 { total = total + i }
            return total
        })
        log("local loop " + worker.block() + "/" + i)
    }
}
'''

EXPECTED = '''typed rejected
preserved 7
inferred rejected
inferred 8
union rejected
union new
wide 3
callable rejected
callable 5
inferred context 6
typed callback 12
literal 3
inferred literal 5
array rejected
fixed [1, 2]
generic rejected
generic 11
generic callback 8
canonical union 2
function union 6
packet 14
optional 12
negative
number 5
OK
flag true
nil
remaining no
ALIAS
snapshot 5/changed
captured 4
inner
outer 10
pattern copy 12/6
local loop 3/90
'''

INVALID = {
    "union local needs narrowing": 'int|string u = 1\nint result = u',
    "union alias retains alternatives": 'int|string u = 1\nvar alias = u\nlog(String.length(alias))',
    "native alternatives check every union member": 'int|string u = 1\nlog(Collection.length(u))',
    "union arithmetic is not dynamic": 'int|string u = 1\nlog(u * 2.0)',
    "generic union contract is not erased": 'int|bool u = true\nvar bad = new Some<int|nil>(u)',
    "union assignment checks every alternative": 'int|bool source = true\nint|string target = source',
    "guard is not exhaustive": 'int|bool u = 1\nlog(match u { int n when n < 0 => 0, bool b => 1 })',
    "nullable alternative remains uncovered": 'int|string u = 1\nlog(match u { int n => 0, string s => 1 })',
    "wildcard binding keeps remaining union": 'int|string|bool u = 1\nlog(match u { int n => 0, other => String.length(other) })',
    "pattern view preserves constness": 'int|bool initial = 1\nlet u = initial\nlog(match u { int n => (u = 2), _ => 0 })',
    "assigned subject loses its refinement": 'int|string u = 1\nlog(match u { int n when (u = "changed") == "changed" => u + 1, _ => 0 })',
    "later arm cannot narrow a mutated snapshot source": 'int|string u = 1\nlog(match u { int n when (u = "changed") == "" => 0, int _ => u + 1, _ => 0 })',
    "capture writes invalidate entry assumptions": 'int|string u = 1\nvar f = match u { int _ => func() { var n = u + 1\nu = "later"\nreturn n }, _ => func() => 0 }',
    "typed lambda parameter checks its callable argument": 'var apply = func(func(int): int f) => f("wrong")',
    "callable reassignment checks signature": 'func(int): int f = func(x) => x\nf = func(string x) => String.length(x)',
    "shadowed binding does not replace outer contract": 'int x = 1\n{ string x = "inner" }\nx = "wrong"',
}


def run(binary, source, path, check=False):
    path.write_text(source)
    command = [str(binary)] + (["--check"] if check else [])
    command += ["--root", str(ROOT / "stdlib"), str(path)]
    try:
        return subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("test did not complete; timeout requires diagnosis, not a pass/fail verdict") from error


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__.splitlines()[0])
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="zl-type-boundaries-") as directory:
        source = pathlib.Path(directory) / "TypeBoundaries.zl"
        result = run(binary, PROGRAM, source)
        if result.returncode or result.stdout != EXPECTED or result.stderr:
            raise AssertionError(f"end-to-end contract regression ({result.returncode})\n{result.stdout}\n{result.stderr}")
        for name, body in INVALID.items():
            result = run(binary, "class TypeBoundaries { func main(): void {\n" + body + "\n} }\n", source, check=True)
            if result.returncode != 1 or "type error" not in result.stderr:
                raise AssertionError(f"{name}: expected a semantic rejection, got {result.returncode}\n{result.stdout}\n{result.stderr}")
        print(f"type boundaries: PASS (end-to-end + {len(INVALID)} semantic rejections)")


if __name__ == "__main__":
    main()
