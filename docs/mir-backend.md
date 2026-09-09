# MIR → Bytecode backend

The ZL compiler's existing path is `source → parser → type analysis → AST→Chunk
→ VM`. This backend adds a sibling that finally makes MIR a *used* IR:

    ZL source → parser → semantic/type analysis → MIR → verifyModule
              → compileModuleToBytecode → Chunk → VM

The VM and its bytecode remain the behavioural reference; nothing in this
backend changes them. It lowers a verified `zl::mir::Module` into the same
`zl::Chunk` the existing stack VM executes, so the two backends can be run
against each other as a differential pair: any program that the MIR backend
claims to support must print the same output and exit the same way on both.

The original `Compiler` (AST → Chunk) path is kept untouched purely as that
reference.

## Running it

    ./build/zl_language --mir-vm <file.zl> [program args...]

This runs the whole pipeline above and returns the VM's exit code. On stdout it
is identical to the reference path; it differs only by reporting (on stderr)
how many of the module's functions were *stubbed* because the backend does not
yet translate them.

The differential harness that checks the backend against the reference on the
example corpus is `tools/mir_backend_diff.sh`.

## Design

### Fail closed, never miscompile

Every construct the backend cannot yet translate faithfully is rejected for the
*function that contains it*: that function's `Chunk` entry is pointed at a
single shared stub that raises a loud runtime error if it is ever reached. A
program that never reaches an unsupported function behaves identically to the
reference; a program that reaches one fails loudly instead of producing subtly
wrong output. This mirrors the stance `lowerProgram` takes toward constructs MIR
itself cannot represent.

A module whose *whole* translation is impossible (e.g. no valid entry point)
records hard errors and returns `ok() == false` rather than producing a partial
chunk.

### Structure

- `include/zl/mir/vm_backend.hpp` — public API: `BytecodeResult`
  (`chunk`, `stubbed`, `stubbedFunctions`, `errors`, `ok()`) and
  `compileModuleToBytecode(const Module&)`.
- `src/mir/vm_backend.cpp` — the `MirBytecodeTranslator`.
- `src/main.cpp` — the `--mir-vm` command that wires the pipeline into the CLI.

Each supported MIR function body is translated straight-line with block-level
control flow patched into VM `Jump`/`JumpIfFalse` addresses:

- **Registers** (parameters, slots, temps) map to named VM locals; a slot or
  temp is bound with `DefineVar` at its defining instruction. SSA def/use is
  preserved because every value-producing instruction leaves its result on the
  stack and the `DefineVar` that materialises the result temp pops it.
- **Dispatch** slots and per-class vtables are derived from the module's own
  instance methods/constructors, so subclass overrides and base methods share a
  slot.
- **Generic allocation** records the concrete instantiated type name (e.g.
  `List<string>`) on `NewObject`, which is what lets the runtime substitute a
  generic method body's type parameter (`T`) when it is later type-checked.
- **Raw native collections** (`list<T>`, `map<K,V>`, `set<T>`) reach the VM's
  `Collection.*` natives: literal building via `Collection.push`/`mapSet`, and
  element reads via `Collection.get`/`Collection.mapGet`.

## Verified coverage

The backend reproduces the reference byte-for-byte (see
`tools/mir_backend_diff.sh`) for, at minimum:

- Arithmetic, bitwise, comparison, boolean, string concat, int/double widening.
- Conditionals, `for`-range with `step`, `while`, `do-while`, `break`/`continue`.
- Functions, recursion, `static func` calls (`InvokeStatic`), mutual calls.
- Classes with fields and constructors, instance methods and virtual dispatch
  (`InvokeMethod`), `super` calls (`InvokeSuper`), inheritance, interfaces.
- Generics on user classes (including method dispatch on parameterized
  receivers), enums, `data` records, operator overloading, encapsulation,
  static *methods*.
- Raw and typed `List`/`Map`/`Set`/`Array` literals and element access, and the
  standard `basics/` programs (HelloWorld, Operators, Variables, Conditions,
  Loops, Functions, DataTypes, Arrays, Lists, Maps, Sets).
- Native calls (`log`, `Math.*`, `Collection.*`, ...).

`examples/basics/*` and the passing `examples/intermediate/*` set are the corpus
the harness enforces.

## Known gaps (fail closed)

These constructs still stub the functions that use them, so the affected
example programs raise a loud runtime error under `--mir-vm` rather than match:

- Closures / lambdas and indirect calls (`MakeClosure`, `CallIndirect`, value
  capture), and the collection algorithms that are built on them.
- Exceptions (`try`/`catch`/`finally`) and `throw` with exception-object values
  through handler chains; `match` expressions with runtime type narrowing
  (`TypeTest`/`Refine` on union members) and non-constant patterns.
- Static *fields* (`StaticLoad`/`StaticStore`, lazy-init initializer functions).
- Nested generic collections and reflection-driven generic runtime checks.

Each is a documented, localized extension: add the opcode family to the
supported set (and mark the function translatable) and it graduates from the
stub list into the differential corpus.
