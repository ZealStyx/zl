# MIR

MIR is ZL's mid-level intermediate representation: a typed, SSA-shaped graph
that sits between semantic analysis and any backend. It is the layer a native
compiler, an optimiser, or a static analyser is meant to consume instead of
re-deriving structure from the AST or from bytecode.

```
.zl → Lexer → Parser → ModuleLoader → TypeChecker
                                          │
                     ┌────────────────────┴────────────────────┐
                     ▼                                         ▼
                 Compiler → Chunk → VM              zl::mir::lowerProgram
                 (the shipped path)                            │
                                                               ▼
                                                    Module → verifyModule
                                                               │
                                                               ▼
                                                        printModule / backend
```

Headers live in `include/zl/mir/`, implementations in `src/mir/`.
`include/zl/mir/mir.hpp` pulls the whole layer in, ordered by dependency.

MIR is **opt-in and additive**. The bytecode compiler and the VM are untouched
and remain the behavioural reference. Nothing in the language changed to
accommodate it.

---

## Two IRs, and why

ZL already had `zl::ir` (`include/zl/compiler/ir.hpp`). It is the input to the
native-subset backends (`zl --emit-native`, `zl --emit-machine-code`) and it
stays exactly as it is. MIR is a different layer with a different job, and the
two coexist deliberately:

| | `zl::ir` (legacy) | `zl::mir` |
|---|---|---|
| Types | `std::string` names | interned `TypeId`s in a `TypeArena` |
| Form | mutable locals, reassignable results | SSA temps plus explicit slots |
| Terminators | mixed into `instructions` | a distinct type, exactly one per block |
| Successors | a vector separate from the terminator | derived from the terminator |
| Generics / unions / function types | not representable | first-class `TypeKind`s |
| Exceptions | not representable | unwind edges and handler chains |
| Fields, indexing, methods, allocation | absent | dedicated opcodes |

`zl::ir` is not deprecated by this and has not been renamed — "MIR" already
appears in its diagnostics, and renaming it would churn user-visible strings for
no gain. When a construct only the native subset needs has to be added, add it
to `zl::mir` and lower `zl::ir` onto it later; do not grow `zl::ir` further.

---

## Core structures

**`Module`** owns everything: the `TypeArena`, the functions, the statics, the
class layouts, the constant pool, and the entry point. The arena must outlive
every `Function` that references its ids.

**`TypeArena`** interns types structurally, so a type *is* its id. Equal shapes
share an id; different shapes never do. Interning is memoisation, so the
accessors are `const` over mutable storage — which means an arena is **not**
safe to intern into from two threads at once.

**`Function`** holds parameters, slots, blocks, and the metadata a backend
needs: `returnType`, `isAsync`, `isLambda`, `typeParameters`, `ownerClass`,
`hasThisParameter`, and `incomplete`.

**`BasicBlock`** holds value-producing instructions and exactly one
`Terminator`. `Instruction` and `Terminator` are separate types; that separation
is the single most load-bearing decision in the design (see
[Invariants](#invariants)).

**`Operand`** is a typed reference to a value: `{kind, type, index}`, where
`kind` is one of `None`, `Const`, `Temp`, `Param`, `Static`. Every non-`None`
operand carries a `TypeId`, so no consumer ever has to ask "what type is this?"
and re-derive it from context.

---

## Invariants

The verifier enforces all of these. They are the contract a backend may rely on.

### Types

1. **Every value has a type.** Operands, temps, parameters, slots, and
   instruction results all carry a `TypeId`. `kNoType` (0) means "absent", and
   appears only where a value legitimately is (a void call, a bare `return`).
2. **Types are interned.** Two `TypeId`s compare equal exactly when the types are
   structurally equal. A backend may use `==` on ids and never inspect shapes.
3. **Nullability is derived, never stored.** A stored flag would be a copy of
   an invariant and could disagree with the type. It is answered by
   `TypeArena::isNullable(id)`, not by the top-level kind alone: `string` is
   nullable even though the VM holds it inline rather than behind a collector
   handle, and a union is nullable when any member is, so `int|string` admits
   `null` while `int|double` does not.
4. **A generic parameter is a `TypeParam`, not an erasure.** `T` inside
   `class Set<T>` is a real type with a name. Nothing is boxed to `object` to
   make the first implementation simpler.
5. **`name` is set for every nominal type** — `Object`, `Task`, `Shared`,
   `TypeParam`. Rendering never needed it (the kind supplies `"Task"`), but every
   consumer asking "which class is this?" does.
6. **An array's size is optional.** `array<int>` is dynamic; `array[10]<int>` is
   fixed. Interning must not normalise the former into the latter.

### SSA and storage

7. **A temp is defined exactly once.** The verifier collects every definition
   and rejects a second one.
8. **A temp is dominated by its definition.** Uses are checked against the
   dominator tree, not against textual order.
9. **Mutable locals are slots, not temps.** `Load`/`Store` name a `SlotId`. This
   is what keeps SSA and ZL's reassignment semantics from fighting each other.
10. **Parameters stay SSA unless the body writes, moves, or borrows them.** A
    read-only parameter is a `Param` operand. Promoting every parameter to a
    slot would force `this` — and every read-only binding — through storage that
    has to be *mutable* to exist at all, contradicting the binding's own
    immutability.
11. **A `let` that is never moved is an SSA value**, bound directly to the
    operand its initialiser produced. Giving it a slot would mean storing into
    storage the language says can never be written.
12. **A borrow has no value of its own.** `Borrow` writes to a slot; it does not
    produce a temp. A borrow that produced a temp could not be identified by the
    ownership flow, so dropping one would be undetectable.

### Control flow

13. **`blocks[0]` is the entry block**, and block ids are 1-based and match
    position. `rebuildEdges()` maintains that.
14. **Every block ends in exactly one terminator.** `TerminatorKind::None` is
    rejected. Value-producing instructions never transfer control, and
    terminators never produce values.
15. **Every block is reachable.** Unreachable blocks are an error by default;
    `VerifierOptions::unreachableBlocksAreErrors` demotes them to warnings for
    hand-written MIR under construction.
16. **Predecessor relationships are consistent.** A block named as a successor
    must list the edge, and vice versa. The verifier checks both directions.
17. **The entry block has no predecessors.**
18. **A branch condition is `bool`.** Where semantic analysis left a condition's
    type unknown — a call through a bare `func` — the lowerer records the
    required type with an explicit `Refine` rather than branching on an untyped
    value.

### Calls and generics

19. **A function is a template, never a copy.** `class Set<T>`'s methods are
    lowered once with `this: Set<T>`. The *call site* records which
    instantiation it selected in `Instruction::typeArguments`, one `TypeId` per
    entry in the callee's `typeParameters`. That is the only place the
    substitution is knowable, so it is where it is stored.
20. **An enum member's static type is the enum.** `Color.RED` is a string at
    runtime, but typed as `string` in MIR every comparison and every call taking
    an enum member would read as a mismatch. `ConstKind::EnumMember` carries the
    enum's name so the type survives.
21. **A bare `func` has no signature.** `FunctionSignature::hasSignature` is
    false, and the verifier defers arity checking to the call boundary.
    Recording it as `func(): void` would invent an arity the source never stated
    and reject calls the type checker accepted.
22. **A call's result may be absent.** Calls and `await` are value-producing
    opcodes with an *optional* result: a void callee and an `await` of
    `Task<void>` define no temp. `OpcodeShape::optionalResult` records this, so
    "does this opcode produce a value?" has one answer in the table rather than
    a special case in the verifier.

### Ownership

23. **Move and borrow state is tracked per slot** and checked as a dataflow
    problem over the reverse postorder: use after move, move while borrowed,
    drop of a borrow, and `EndBorrow` without `Borrow` are all rejected.
24. **Only owned storage can be moved or dropped.** Ownership kinds on slots and
    parameters are preserved from the source, not inferred.

### Exceptions

25. **A catch block has no normal predecessors.** It is reachable only along
    unwind edges. A catch block that control can also fall into is invalid.
26. **Handler order is search order**, innermost last, matching how the chain is
    installed dynamically as blocks are created.

---

## Instruction taxonomy

54 opcodes, grouped by what they do. `opcodeShape(op)` answers, for any opcode,
how many operands it takes, whether it produces a result, whether it may throw,
and whether it has side effects. `opcodeUsesSlot` and `opcodeIsCall` are the
other two shared predicates; the verifier and the printer both use them so they
cannot disagree about which fields an opcode reads.

- **Arithmetic / bitwise:** `Add Sub Mul Div Mod Pow Neg`, `BitAnd BitOr BitXor
  BitNot Shl Shr Ushr`
- **Comparison:** `Eq Ne Lt Le Gt Ge`
- **Logic:** `Not And Or`. `&&` and `||` are **not** opcodes — they short-circuit
  in ZL, so they lower to branches.
- **Conversion:** `Widen` (the language's implicit `int → double`), `Refine`,
  `TypeTest`, `NullCheck`, `IsNull`
- **Locals:** `Load Store Move Borrow EndBorrow Drop`
- **Aggregate / static access:** `FieldLoad FieldStore IndexLoad IndexStore
  StaticLoad StaticStore`
- **Construction:** `Alloc`, `NewCollection`
- **Calls:** `Call InvokeMethod InvokeSuper InvokeStatic CallIndirect
  CallNative MakeClosure`
- **Async:** `Await TaskCreate`
- **Misc:** `Nop RangeInBounds Log`

Terminators: `Return Jump Branch Switch Throw Unreachable`.

Arithmetic typing is delegated to `zl::OperatorRules`, the same table the
existing compiler uses, so MIR cannot disagree with the language about what
`int + string` means (concatenation, producing `string`).

`Refine` and `TypeTest` are the two halves of a runtime type narrowing.
`Refine` asserts and retypes, raising when the value does not match — the typed
form of the VM's `AssertType`. `TypeTest` asks and produces a `bool` — the typed
form of the VM's `MatchType` — because a `match` arm has to survive a "no" and
fall through to the next arm. Its tested type lives in
`Instruction::testedType`, since `resultType` there is always `bool`.

---

## What the builder guarantees

`FunctionBuilder::finish()` establishes two invariants before anything inspects
the function:

- every block ends in exactly one terminator — an unterminated block becomes
  `Unreachable`;
- no block is unreachable — `pruneUnreachableBlocks` removes blocks nothing can
  reach, following **both** normal and unwind edges, renumbers the survivors
  densely, and rewrites every reference to them.

This is why a function that was only *partially* lowered is still structurally
valid MIR. The semantic caveat travels separately, in `Function::incomplete`.

Unwind edges matter here in a way that is easy to get wrong: a block reached
along an unwind edge is ordinary code from there on, so a catch handler's jump
to the join block keeps everything downstream of it reachable. Following only
unwind edges in that traversal makes the join block look dead, prunes it, and
leaves the handler jumping nowhere.

---

## Lowering

`zl::mir::lowerProgram(program, checker)` runs four passes over the flat
`Program` the `ModuleLoader` produces:

0. **Class shapes and layouts** — type parameters, parents, fields, statics.
1. **Declare every function**, so a call can name its target regardless of
   declaration order. Lambdas are discovered in the same pass, because a closure
   body is a real MIR function that other instructions reference by id.
2. **Declare lambda bodies** — flags and capture placeholders only. Capture
   *types* are not knowable yet; declaring them early would mean declaring them
   `unknown` and throwing the information away.
3. **Lower function bodies.**
4. **Lower lambda bodies**, in discovery order, so an outer body fills an inner
   closure's capture types before that closure's parameters are added.

Ordering is real, not incidental: the lambda list is a vector, not the hash map
that indexes it, because a hash map's iteration order would let an inner closure
be lowered before the outer body that types its captures.

Notable choices:

- **Function names** are the checker's own: `ownerClass + "." +
  dispatchSignatureForFunction(...).describe()`. Lookups fall back up the parent
  chain, so inherited methods resolve the way dispatch does.
- **`this` is parameter 0**, with `Function::hasThisParameter` set. It is not a
  hidden side channel. Inside a generic class it is the *self-parameterized*
  form (`Set<T>`, not bare `Set`).
- **Closures are functions with leading capture parameters**, so a closure's own
  arity is `parameters.size() - captures.size()`. Captures are copied into slots
  at body entry. The environment is explicit rather than implicit.
- **`this` is captured by name, like any other local.** Semantic analysis puts
  `"this"` in `captureStorageNames`, and a capture is resolved by looking the
  name up in scope — so the enclosing body binds `this` under its own name as
  well as holding it in the receiver field. Inside the closure the captured slot
  then becomes the body's receiver, because `this` in a body is spelled `ThisExpr`
  and answered from the receiver, not from the scope. Capturing it without that
  second step lowers the closure but rejects every `this` inside it.
- **A closure inside a generic class is generic over the class's parameters.**
  It closes over `this`, whose type mentions them, and an unsubstituted type
  parameter is only legal inside a template. So the closure's MIR function is
  declared as a template over the same parameters its owner class has; without
  that, the captured `this: Box<T>` is a type parameter in a concrete function
  and the verifier rejects it.
- **`coerce()` makes implicit conversions explicit.** Where the result type is
  `double` and an operand is `int`, a `Widen` is emitted, so MIR arithmetic is
  homogeneous and a backend never has to re-derive the promotion rule.
- **A `data` record literal is an allocation plus a write per field.** A record
  has no constructor, so `Point { x: 10, y: 20 }` must not go looking for one —
  unlike `new C(...)`, it never consults the function table.
- **A collection has two spellings and both must work everywhere.** The
  lowercase keyword forms (`list<T>`, `map<K,V>`, `set<T>`) arrive as the
  List/Map/Set kinds; the capitalised class forms (`List<T>`, `Map<K,V>`,
  `Set<T>`) are real generic classes, so they arrive as an `Object` carrying
  that name with the same arguments. Inside a generic class the class spelling
  is the only one available, since `list<T>` cannot be parameterised by a type
  variable. `isCollectionType` is the single answer to "is this a collection",
  used by both construction and indexing, so a spelling cannot end up
  constructible but not indexable or the reverse. A literal is built as
  `NewCollection` plus one `IndexStore` per element, which is why a set has to
  accept positional indexing even though the language has no positional read for
  one.
- **Unsupported constructs do not crash.** `unsupported()` marks the function
  incomplete, records a note, and returns. The module still verifies.

- **`match` lowers to a chain of two-way branches**, one test block per arm, in
  source order. The subject is evaluated exactly once before the first test, so
  a subject with a side effect cannot run once per arm and a guard that
  reassigns the subject's variable cannot change what a later arm compares
  against. Arm bodies store into a result slot the join block reads back — the
  same join mechanism `try` uses, since this IR has no phi. A type pattern emits
  `TypeTest` and then `Refine`s both the arm's own binding and the subject
  identifier the body keeps spelling, which is what narrows `int|string` to
  `int` inside an `int n =>` arm. The non-exhaustive fallthrough raises
  `Exception("non-exhaustive match")`, as the bytecode reference does; when the
  last arm is a wildcard that block is unreachable and `finish()` prunes it.

### What is not lowered yet

`try/finally`, `try` with no `catch`, structural (`data`/list/map) match
patterns, `data` copy-update (`base with { ... }`), function references,
object-typed collection literals, and a method call whose receiver semantic
analysis could not resolve to a class (which happens where a generic's own type
parameter comes back out, as with `Shared<T>.get()`). Each produces a note and an
incomplete function rather than malformed MIR.

Most of what is left is one root cause rather than several: a bare `func`
parameter carries no signature, so semantic analysis types its body's
expressions UNKNOWN and MIR records that faithfully instead of inventing a type.
Fixing it means inferring lambda parameter types at call sites, in the checker —
not guessing in the lowerer.

---

## Verifier

`verifyModule(module)` checks a function in this order, because later stages
assume earlier ones held:

1. `checkSignature` — parameters, return type, `this`, type parameters.
2. `collectDefinitions` — SSA uniqueness and temp→slot provenance.
3. `checkControlFlow` — terminators, block references, predecessor consistency,
   reachability, entry-block shape.
4. `checkInstructions` — operand counts, operand types, result types, opcode
   shapes, call signatures, field and index access, ownership rules.
5. `checkDominance` — every temp use is dominated by its definition.
6. `checkOwnershipFlow` — a fixpoint over reverse postorder tracking
   `{moved, borrows}`.

Module level, it additionally checks for duplicate function names, positional id
consistency, the entry point, and static field layout.

The verifier has its own `assignable(from, to)`. `TypeChecker::isAssignable` is
private, and the two are not identical: MIR's does not model ZL's collection
conversion rules (`Set<T>` against bare `set`, list/set interchangeability when
generics agree). That is a known gap, not an oversight — widening it means
exposing the checker's rules at the right layer rather than duplicating them.

An **unsubstituted generic parameter is compatible with anything**. Inside a
template, `T` stands for some type that is not knowable at verification time, so
demanding a match would reject correct MIR. Recording the instantiation at the
call site is what makes the check precise where it can be.

---

## Textual form

`printModule` produces a readable dump, used by `zl --emit-mir`:

```
func Hello.main()($0 this:Hello): void
  at line 2
  slots:
    s1 $local383:int
    s2 __for_end:int
  b1 (entry):
    store slot 1('$local383'), 0:int  ; line 3
    jump b2  ; line 4
  b2:
    %1:int = load slot 1('$local383')  ; line 5
    %2:bool = gt %1:int, 5:int  ; line 7
    branch %2:bool, b4, b3  ; line 7
  b3:
    log "small":string  ; line 10
    jump b5  ; line 7
  b4:
    log "big":string  ; line 8
    jump b5  ; line 7
  b5:
    return  ; line 2
  edges: b1->b2 b2->b3 b2->b4 b3->b5 b4->b5
```

`$n` is a parameter, `%n` a temp, `sn` a slot, `bn` a block. Generic templates
are marked `; generic-template`, constructors `; constructor`, and an
unlowered body `; incomplete`.

---

## Command line

```bash
zl --emit-mir out.mir program.zl   # write the textual MIR
zl --emit-mir - program.zl         # print it to stdout
```

Exit codes: `0` verified, `2` bad usage, `3` stdlib version mismatch, `4`
verification failed (the module is not written), `5` the output could not be
written, `1` a front-end failure. Lowering notes go to stderr; they are not
errors, and a module with notes can still verify.

---

## Tests

- `tests/mir_tests.cpp` (`zl-mir-tests`) — 47 regressions. Builds MIR by hand
  and checks the verifier rejects each class of malformed module. A
  lowering-only test could never reach most of these shapes, because the builder
  refuses to produce them.
- `tests/mir_lowering_tests.cpp` (`zl-mir-lowering-tests`) — 29 regressions.
  Drives the real pipeline on small programs and asserts on the MIR that comes
  out — including the specific properties an earlier lowerer got wrong.

Lowering is also exercised across `examples/`: every file is lowered and
verified. 54 of the 57 lower and verify completely; the other 3 verify with
notes. That count is the measure of coverage.

The 8 remaining notes are all one root cause: a bare `func` parameter.
`applyTwice(func f, int x)` carries no signature, so semantic analysis types a
lambda's parameter as `unknown`, and MIR records `unknown` rather than inventing
a type. That is the honest answer — the language genuinely does not know — and
inventing `int` to make the count look better would be exactly the erasure this
IR exists to avoid.
