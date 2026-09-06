# ZL Standard Library

The standard library is deliberately hybrid: public APIs live in ZL packages, while the
VM, OS access, parsing engines, and storage primitives remain small native primitives.
See [native.md](native.md) for the boundary rule.

- [`zl.lang` (auto-imported)](#zllang-auto-imported)
- [Math](#math)
- [`zl.test`](#zltest)
- [`zl.logging`](#zllogging)
- [`zl.text`](#zltext)
- [`zl.serialize`](#zlserialize)
- [`zl.time`](#zltime)
- [`zl.crypto`](#zlcrypto)
- [Concurrency](#concurrency)

## `zl.lang` (auto-imported)

`zl.lang` is available everywhere with no `import` statement. It contains everything
that existed as a flat native before packages did: `Math`, `String`, `IO`, `Collection`,
`FileSystem`, `Time`, `System`, and `Int`/`Double`/`Bool` parsing.

Everything else requires an explicit `import` — see [packages.md](packages.md).

Beyond `zl.lang`, the bundled packages include reference queue and stack utilities
(`zl.util.Queue`, `zl.util.Stack`), text and time helpers, task/channel/thread facades,
serialization, filesystem access, and a small portable DNS network facade.

## Math

Constants are accessed without parentheses:

```zl
log(Math.PI)
log(Math.E)
log(Math.TAU)
```

Available operations:

- `sqrt`, `abs`, `pow`, `floor`, `ceil`, `round`, `min`, `max`
- `clamp`, `sign`, `cbrt`, `trunc`
- `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`
- `log`, `log10`, `exp`
- `lerp`, `degrees`, `radians`
- `random`, `randomInt(min, max)`, `randomFloat(min, max)`

Domain errors are reported for `asin`/`acos` outside `[-1, 1]` and for `log`/`log10` on
non-positive inputs. `Math.exp` reports finite-input overflow. `Math.clamp`,
`Math.randomInt`, and `Math.randomFloat` reject a minimum greater than the maximum. The
random functions use a process-local pseudorandom generator and are **not**
cryptographically secure.

High-level helpers (`clamp`, `sign`, `lerp`, `degrees`, `radians`) are ZL-owned static
methods; genuinely primitive operations (`sqrt`, `sin`, `cbrt`, random generation) stay
native. The public `Math.*` API is stable across that boundary.

## `zl.test`

```zl
import zl.test.Test
import zl.test.Runner

class ExampleTests {
    func main(): void {
        var tester = new Test()
        tester.assertThrows(func() { throw "expected" })

        var runner = new Runner("example")
        runner.run("truth", func() { Test.assertTrue(true) })
        runner.run("math", func() { Test.assertEqual(2 + 2, 4) })
        Test.assertEqual(runner.report(), 0)
    }
}
```

Assertions: `Test.assertTrue`, `Test.assertFalse`, `Test.assertEqual`,
`Test.assertNotEqual`, `Test.assertNear(actual, expected, epsilon)`, `Test.fail`,
and `Test.assertThrows`.

`Runner` provides ZL-native grouping, pass/fail accounting, and reporting.

## `zl.logging`

```zl
import zl.logging.Log

Log.info("server started")
Log.warn("cache miss")
Log.error("request failed")
```

The package is `zl.logging` rather than `zl.log` because `log` is a reserved keyword.

## `zl.text`

```zl
import zl.text.Text

Text.upper("zl")
Text.startsWith("hello", "he")
Text.endsWith("hello", "lo")
Text.join(new List<string>(), ",")

Text.format("hello {0}", {"ZL"})
Text.regexMatches("abc123", "[0-9]+")
Text.regexFindAll("a12 b34", "[0-9]+")
Text.regexReplace("a1b2", "[0-9]", "X")
```

Convenience and composition methods are ZL-owned. `String.*` remains the low-level
native string boundary; the regex and formatting engines stay native.

## `zl.serialize`

```zl
import zl.serialize.Serialize

var json = Serialize.stringify({"name": "ZL", "version": 15})
var value = Serialize.parse(json)
var answer = Serialize.number(42)
```

`encode`/`decode` and the low-level typed conversion primitives remain available behind
the facade. The JSON parser and encoder are native, and support null, booleans, numbers,
strings, arrays/lists, and string-keyed objects/maps.

## `zl.time`

Provides `Date`, `DateTime`, `TimeOfDay`, and `Duration` value objects over the native
clock primitives. Higher-level operations — `Time.nowDateTime()`, `Time.today()`,
`Date.addDays()`, `DateTime.addHours()`, `DateTime.difference()` — are implemented in ZL.

## `zl.crypto`

```zl
import zl.crypto.Crypto

Crypto.sha256("abc")
Crypto.crc32("123456789")
```

SHA-256 and CRC32 are native primitives; ZL owns the public package API. Cryptographic
primitives are deliberately kept native for correctness and performance.

## Concurrency

The runtime supports native asynchronous operations that return `Task<T>` and complete
from worker threads. The reference operation is `Time.sleepAsync(milliseconds)`, which
can be awaited from an `async func` without blocking the VM scheduler.

`Shared<T>` is an explicit generic wrapper. Construct it with `new Shared<T>(value)` and
access it with `get()` / `setValue()`. CPU-worker and raw-thread closures may carry only
`Shared`-wrapped object captures; ordinary captures are rejected.

`Shared<T>` does **not** imply thread safety. The top-level `share()` helper and
compile-time confinement checks remain pending.
