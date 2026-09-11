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
`promoteSlotsToBlockParameters` against the interpreter-free path: all 50
runnable programs run on the backend today, all 50 are identical with and
without promotion, and the skip count is 0 (the `_lib` module sources are
excluded from the listing rather than counted as permanent skips).

Both harnesses put every `_lib` directory on the module search path, the way
`examples/run_all.sh` does. Without that, every example that imports a sibling
module fails to *load* on both paths, and the first harness counted the
resulting double failure as a match. Three programs were passing that way
without being compared at all, and five more were being skipped by the second
harness for no reason. A comparison that cannot run is not a passing
comparison, so the first harness now reports an unloadable reference program as
an error of the harness rather than as agreement.

## Design

### Type semantics are translated, not assumed

The three opcodes that carry the type system's runtime meaning lower to the
VM's own instructions, so a boundary the MIR records is a boundary the
interpreter enforces:

- `Refine` → `AssertType "<rendered type>"`. A refine is the explicit
  dynamic-to-static assertion (see mir.md, invariant 8), so the backend emits a
  real check. Only an identity refinement — operand type already equals the
  asserted type — stays a no-op, because there the MIR checker proved the fact
  the assertion would re-check. The rendered name is the canonical source
  spelling (`int`, `list<int>`, `Option<int>`, `int|string`), which
  `RuntimeTypeCheck` parses with the language's own type-name grammar, so
  nested generics assert nested.
- `TypeTest` → `MatchType "<rendered type>"` — the non-raising partner, the
  bool a `match` arm branches on.
- `IsNull` → `value == null` in the reference's own spelling.

Index reads go through the VM's `GetIndex` (what the reference compiler emits
for `c[i]`), which accepts a raw native list and a typed `List`/`Set` object
alike by unwrapping the object's `__native` storage; only maps read through
`Collection.mapGet`. Collection classification reads the MIR type structurally
— both the lowercase keyword kinds and the capitalised class spellings — rather
than matching a rendered-name prefix.

### Ownership events become the runtime's own lifetime opcodes

The backend does not reinvent lifetime management; it emits the same opcodes
the reference compiler emits for the same source constructs:

- `Move` → `MoveVar <slot>`. The VM's `MoveVar` loads the local, clears it,
  and leaves the value on the stack — the transfer the source's `move`
  spelled, with a later read of the source failing the same way the
  reference's does.
- `Drop` (either spelling) → `DropVar <local>`. The slot form names the
  slot's local; the value form releases the parameter or temp local its value
  lives in. `DropVar` erases the local, which is the reference's deterministic
  release.
- `Borrow` → the owner's value bound to the borrow's own local. The bytecode
  has no aliasing to maintain, so the borrow's lifetime rules are enforced by
  the MIR verifier, not by the interpreter.
- `EndBorrow` → `DropVar` on the borrow's local: the deterministic release a
  native borrow view exists for; for ordinary values it only retires the
  name, which is unobservable.

Every function translated from MIR also registers its owned locals and
parameters in `ownedLocalNames`, so the VM's frame teardown erases them on
*any* frame exit — ordinary return, early return, exception — exactly as the
reference compiler's `ownedLocalNames` does. The two paths release the same
storage at the same points; nothing depends on reaching the cleanup at the
end of the body.

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
- Every concurrency operation lowers to the same runtime call the reference
  compiler emits for the same source, so the bytes are identical: `task_spawn`/
  `task_block`/`task_ignore`/`task_cancel` to the `Task.*` natives,
  `thread_start`/`join`/`is_alive` to `Thread.*`, the channel family to
  `Channel.*` (including the `sendAsync`/`receiveAsync` task constructors),
  the scoped locks to `Mutex.withLock`/`RwLock.withRead`/`withWrite`/
  `Shared.withLock`, every atomic lane to its `Atomic.*` native, semaphores
  and conditions to theirs, and `shared_create`/`get`/`set` to
  `share`/`Shared.get`/`Shared.setValue`. Awaiting a `Task<void>` into an
  `unknown` temp (the spelling the checker leaves for some void-awaits)
  emits the same `Await`+`Pop` the reference path does.

Every runnable example in the tree — `examples/basics/*`, the whole of
`examples/intermediate/*`, and `examples/advanced/*` — is the corpus the
harness enforces (`examples/*/*.zl`; `_lib` directories hold importable
module sources, not programs).

## Known gaps (fail closed)

None today: the differential corpus is every runnable example in the tree —
`examples/basics/*`, `examples/intermediate/*` and `examples/advanced/*`
(50 programs, the `_lib` module sources are imports, not programs), each
byte-identical on both paths — including async tasks, threads, channels and
mutexes from `advanced/`, closures/lambdas with indirect calls,
`try`/`catch`/`finally` with typed and rethrow handlers, static fields with
lazy initializers, generics end to end (generic classes, projected
inheritance, `Shared<T>`, reflective `Method.invoke` on generic receivers),
and the collection algorithms built on all of that. No example reaches a
stub: a tree-wide run of `--mir-vm` reports zero stub warnings, so the
fail-closed path is never exercised by the corpus — every function the
examples touch is faithfully translated.

The one family that still fails closed by design is FFI: `ffi_call` and the
handle/callback lifecycle have no bytecode spelling, so any function using
them is stubbed rather than mistranslated. No example uses FFI, so the corpus
never exercises that path.

Other constructs that would still fail closed (the gate rejects the function
before it can misbehave) have no example coverage left; when one turns up,
name it here and in `KNOWN_GAPS` in `tools/mir_backend_diff.sh`. The mechanism is
unchanged: a construct the backend cannot translate faithfully stubs the
functions that use it, and the affected programs raise a loud runtime error
under `--mir-vm` rather than silently diverge. The last gap to graduate was
the interface-receiver method call, resolved from the interface's own
declaration; before that, runtime *type* narrowing (`TypeTest` → `MatchType`,
`Refine` → `AssertType`).

Each future gap is a documented, localized extension: add the opcode family to
the supported set (and mark the function translatable) and it graduates from
the stub list into the differential corpus.
