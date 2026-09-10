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

The differential harnesses on the example corpus are:

    tools/mir_backend_diff.sh ./build/zl_language        # backend vs reference
    tools/mir_promotion_diff.sh ./build/zl_language      # block params vs slots

The first runs every corpus program through both paths and compares exit code
and output. The second runs every program the backend *can* run through
`--mir-vm` twice - once on the memory form, once with `ZL_MIR_PROMOTE=1` - and
requires the two to be identical. Because the backend translates a block
parameter into the memory form of itself, that comparison is a real check of
`promoteSlotsToBlockParameters` against the interpreter-free path: 27 identical,
0 differing, with 32 programs the backend cannot run at all skipped (they already
fail without promotion, so they say nothing about it).

Both harnesses put every `_lib` directory on the module search path, the way
`examples/run_all.sh` does. Without that, every example that imports a sibling
module fails to *load* on both paths, and the first harness counted the
resulting double failure as a match. Three programs were passing that way
without being compared at all, and five more were being skipped by the second
harness for no reason. A comparison that cannot run is not a passing
comparison, so the first harness now reports an unloadable reference program as
an error of the harness rather than as agreement.

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

- **Registers** (parameters, slots, temps, block parameters) map to named VM
  locals; a slot or temp is bound with `DefineVar` at its defining instruction.
  SSA def/use is preserved because every value-producing instruction leaves its
  result on the stack and the `DefineVar` that materialises the result temp pops
  it.
- **Block parameters (MIR's phi nodes)** are lowered as the memory form of
  themselves, since the VM has no phi instruction: each predecessor stores the
  edge argument into that parameter's own local before transferring, and the
  block reads it. Values and slots are interchangeable in meaning, so a MIR
  module is translatable whether its merges are block parameters or store/load
  pairs. Setting `ZL_MIR_PROMOTE=1` makes `--mir-vm` promote locals to block
  parameters first, which is a differential check of the promotion: the output
  must be identical either way.
- **A call through an interface-typed receiver** is dispatched from the
  interface's own recorded signature (`InterfaceInfo::methods`), not from an
  implementing class. Dispatch slots are keyed globally by signature and every
  implementer declares the same one, so the interface's declaration is the exact
  answer; picking a class would be a guess. Interfaces are not classes in MIR -
  they get no layout and no reflection rows - so there is no class row to read
  the slot from in the first place.
- **Reflection metadata is built from MIR, and reflection is program output.**
  `Type.fields()` prints each field's visibility and `Type.methods()` lists the
  methods, so a class reflection table with the union of those omitted is a
  miscompile, not a missing feature. Fields, methods and constructors are all
  emitted, base classes first with derived declarations replacing inherited ones
  of the same name and parameter list - the same merge the reference path
  performs.
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
- Interface dispatch: a class implementing an interface, and a method called
  through an interface-typed receiver, including widening from a derived
  interface to the one it extends (`Interfaces.zl`).
- Reflection: `Type.name`/`kind`/`isData`/`fieldCount`/`fields`/`methods`, with
  field visibility and method names, return types and modifiers matching the
  reference path exactly (`Reflection.zl`).
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
- A method call on an interface-typed receiver used to be here. It is not a gap
  any more: MIR records each interface's method signatures, so the slot is
  resolved from the interface's own declaration.
- `Generics.zl` is in this list for a reason that has nothing to do with
  generics: its `main` builds a closure.

Each is a documented, localized extension: add the opcode family to the
supported set (and mark the function translatable) and it graduates from the
stub list into the differential corpus.
