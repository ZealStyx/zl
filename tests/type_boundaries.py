#!/usr/bin/env python3
"""Focused assignment/union regression: python3 tests/type_boundaries.py /path/to/zl.

Two end-to-end programs verify stores, heap contracts and lexical flow. Negative
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

HEAP_PROGRAM = r'''
import zl.util.Queue
import zl.util.Stack

data HeapEntry { count: int }
class HeapHolder {
    public int count
    public list<int> values
    func HeapHolder(): void {
        this.count = 7
        this.values = [1]
    }
}
class HeapBase<T> { public T payload }
class HeapChild<A,B> extends HeapBase<B> {
    func HeapChild(B value): void { this.payload = value }
}
class TypeBoundaries {
    static int total = 4
    static func dynamic(unknown value): unknown { return value }
    static func append(unknown target, unknown value): void { Collection.push(target, value) }
    static func setAt(unknown target, int index, unknown value): void { Collection.set(target, index, value) }
    static func put(unknown target, unknown key, unknown value): void { Collection.mapSet(target, key, value) }
    static func add(unknown target, unknown value): void { Collection.setAdd(target, value) }
    static func accept(list<int> values, int count): void {}
    func main(): void {
        list<int> numbers = [1]
        try { TypeBoundaries.append(numbers, "bad") } catch error { log("alias push rejected") }
        try { TypeBoundaries.setAt(numbers, 0, "bad") } catch error { log("alias set rejected") }
        log("preserved " + numbers)
        var inferred = [2]
        try { TypeBoundaries.append(inferred, "bad") } catch error { log("inferred heap rejected") }
        list<double> wide = [1]
        TypeBoundaries.append(wide, 2.5)
        log("wide " + wide)
        try { list<double> wrongView = TypeBoundaries.dynamic(numbers) }
        catch error { log("invariant view rejected") }

        var child = Collection.newList()
        TypeBoundaries.append(child, 1)
        list<list<int>> nested = [child]
        try { TypeBoundaries.append(child, "bad") } catch error { log("old child alias rejected") }
        var inserted = Collection.newList()
        Collection.push(nested, inserted)
        try { TypeBoundaries.append(inserted, "bad") } catch error { log("new child alias rejected") }
        Collection.push(inserted, 3)
        log("nested " + nested)

        var first = Collection.newList()
        try { TypeBoundaries.accept(first, TypeBoundaries.dynamic("bad")) }
        catch error { log("argument batch rejected") }
        TypeBoundaries.append(first, "free")
        log("unbound after failure " + first)
        var okayChild = Collection.newList()
        var badChild = Collection.newList()
        TypeBoundaries.append(okayChild, 1)
        TypeBoundaries.append(badChild, "bad")
        var rawParent = Collection.newList()
        TypeBoundaries.append(rawParent, okayChild)
        TypeBoundaries.append(rawParent, badChild)
        try { list<list<int>> failed = TypeBoundaries.dynamic(rawParent) }
        catch error { log("nested batch rejected") }
        TypeBoundaries.append(okayChild, "free")
        log("nested rollback " + okayChild)

        map<string,int> scores = Collection.newMap()
        Collection.mapSet(scores, "one", 1)
        try { TypeBoundaries.put(scores, "one", "bad") } catch error { log("map value rejected") }
        try { TypeBoundaries.put(scores, 2, 2) } catch error { log("map key rejected") }
        log("map " + scores)
        var keys = Collection.mapKeys(scores)
        try { TypeBoundaries.append(keys, 3) } catch error { log("projected keys rejected") }
        log("key " + String.upper(Collection.get(keys, 0)))
        map<list<int>,int> index = Collection.newMap()
        var rawKey = Collection.newList()
        try { TypeBoundaries.put(index, rawKey, "bad") } catch error { log("map batch rejected") }
        TypeBoundaries.append(rawKey, "free")
        log("key rollback " + rawKey)

        set<string> names = ["a"]
        try { TypeBoundaries.add(names, 7) } catch error { log("set rejected") }
        var copiedNames = Collection.setItems(names)
        try { TypeBoundaries.append(copiedNames, 7) } catch error { log("copied set rejected") }
        array[2]<int> fixed = [4, 5]
        var arrayAlias = TypeBoundaries.dynamic(fixed)
        try { TypeBoundaries.append(arrayAlias, 6) } catch error { log("array grow rejected") }
        try { Queue.dequeue(arrayAlias) } catch error { log("array shrink rejected") }
        TypeBoundaries.setAt(arrayAlias, 0, 9)
        log("array " + fixed)
        list<int> queue = Queue.newQueue()
        Queue.enqueue(queue, 1)
        log("dequeue " + Queue.dequeue(queue))
        var queueAlias = TypeBoundaries.dynamic(queue)
        try { Queue.enqueue(queueAlias, "bad") } catch error { log("queue rejected") }
        try { Stack.push(queueAlias, "bad") } catch error { log("stack rejected") }
        Stack.push(queueAlias, 2)
        log("stack " + Stack.pop(queueAlias))

        var holder = new HeapHolder()
        try { holder.count = TypeBoundaries.dynamic("bad") } catch error { log("field rejected") }
        try { holder.values = TypeBoundaries.dynamic(["bad"]) } catch error { log("field container rejected") }
        log("holder " + holder.count + "/" + holder.values)
        var inherited = new HeapChild<string,int>(8)
        try { inherited.payload = TypeBoundaries.dynamic("bad") } catch error { log("inherited field rejected") }
        log("inherited " + inherited.payload)
        try { var invalid = HeapEntry { count: TypeBoundaries.dynamic("bad") } }
        catch error { log("record rejected") }
        var record = HeapEntry { count: 9 }
        try { var invalidCopy = record with { count: TypeBoundaries.dynamic("bad") } }
        catch error { log("record update rejected") }
        log("record " + record.count)
        log("static initial " + TypeBoundaries.total)
        try { TypeBoundaries.total = TypeBoundaries.dynamic("bad") } catch error { log("static rejected") }
        log("static preserved " + TypeBoundaries.total)
        var cell = new Shared<int>(10)
        try { Shared.__set(cell, TypeBoundaries.dynamic("bad")) } catch error { log("native field rejected") }
        log("cell " + cell.get())

        var words = String.split("one,two", ",")
        log("split " + String.upper(Collection.get(words, 1)))
        try { TypeBoundaries.append(words, 3) } catch error { log("native result rejected") }
        var cycle = Collection.newList()
        TypeBoundaries.append(cycle, cycle)
        try { list<list<int>> invalidCycle = TypeBoundaries.dynamic(cycle) }
        catch error { log("cycle rejected") }
        TypeBoundaries.append(cycle, "still unbound")
        log("cycle length " + Collection.length(cycle))
        var untyped = Collection.newList()
        var matched = match untyped {
            list<int> items => true
            _ => false
        }
        try { TypeBoundaries.append(untyped, "bad") } catch error { log("pattern alias rejected") }
        log("matched " + matched)
    }
}
'''

HEAP_EXPECTED = '''alias push rejected
alias set rejected
preserved [1]
inferred heap rejected
wide [1, 2.5]
invariant view rejected
old child alias rejected
new child alias rejected
nested [[1], [3]]
argument batch rejected
unbound after failure ["free"]
nested batch rejected
nested rollback [1, "free"]
map value rejected
map key rejected
map {"one": 1}
projected keys rejected
key ONE
map batch rejected
key rollback ["free"]
set rejected
copied set rejected
array grow rejected
array shrink rejected
array [9, 5]
dequeue 1
queue rejected
stack rejected
stack 2
field rejected
field container rejected
holder 7/[1]
inherited field rejected
inherited 8
record rejected
record update rejected
record 9
static initial 4
static rejected
static preserved 4
native field rejected
cell 10
split TWO
native result rejected
cycle rejected
cycle length 2
pattern alias rejected
matched true
'''

INVALID = {
    "split keeps native element type": 'list<int> wrong = String.split("a,b", ",")',
    "native get keeps element type": 'var words = String.split("a,b", ",")\nint wrong = Collection.get(words, 0)',
    "native write checks element": 'list<int> xs = [1]\nCollection.push(xs, "wrong")',
    "native map write checks value": 'map<string,int> xs = Collection.newMap()\nCollection.mapSet(xs, "key", false)',
    "native projection keeps key type": 'map<string,int> xs = Collection.newMap()\nlist<int> wrong = Collection.mapKeys(xs)',
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


INVALID_HEAP_PROGRAM = r"""
class HeapParent { public int value }
class TypeBoundaries extends HeapParent {
    public string value
    func main(): void {}
}
"""


ASYNC_FAILURE_PROGRAM = r"""
class TypeBoundaries {
    static func pressure(): void {
        for i in 0..1200 { var garbage = [i, i + 1] }
    }
    async func main(): void {
        await Time.sleepAsync(1)
        var child = Thread.start(func() { TypeBoundaries.pressure() })
        throw new Exception("async entry failed safely")
    }
}
"""


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
        for program, expected in [(PROGRAM, EXPECTED), (HEAP_PROGRAM, HEAP_EXPECTED)]:
            result = run(binary, program, source)
            if result.returncode or result.stdout != expected or result.stderr:
                raise AssertionError(f"end-to-end contract regression ({result.returncode})\n{result.stdout}\n{result.stderr}")
        for name, body in INVALID.items():
            result = run(binary, "class TypeBoundaries { func main(): void {\n" + body + "\n} }\n", source, check=True)
            if result.returncode != 1 or "type error" not in result.stderr:
                raise AssertionError(f"{name}: expected a semantic rejection, got {result.returncode}\n{result.stdout}\n{result.stderr}")
        result = run(binary, INVALID_HEAP_PROGRAM, source, check=True)
        if result.returncode != 1 or "cannot redeclare inherited field" not in result.stderr:
            raise AssertionError("inherited field contract collision was not rejected: " + result.stderr)
        result = run(binary, ASYNC_FAILURE_PROGRAM, source)
        if result.returncode != 1 or "async entry failed safely" not in result.stderr or "AddressSanitizer" in result.stderr:
            raise AssertionError("async entry failure/unwind lost its result: " + result.stdout + result.stderr)
        print(f"type boundaries: PASS (two end-to-end workflows + {len(INVALID) + 1} semantic rejections + async failure propagation)")


if __name__ == "__main__":
    main()
