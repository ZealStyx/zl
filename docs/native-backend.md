# The native backend

The native backend is the third consumer of MIR, alongside the bytecode backend
and the optimiser. Its pipeline is

    ZL source → parser → semantic/type analysis → MIR → verifyModule
              → selectModule  (native IR)
              → emitModule    (x86-64 machine code)

and the first three arrows are shared, unchanged, with `--mir-vm`. **The native
backend never reads the AST.** Every fact it acts on — the class of a value, the
operand order of a subtraction, which block a branch goes to, which function a
call names — is read out of MIR that `verifyModule` accepted. If a fact the
backend needs is not in the MIR, the fix is to put it in the MIR.

`compileMirToNative` re-runs `verifyModule` on its input and refuses to proceed
if it fails. The backend's licence to assume MIR's invariants is only sound if
something checks them, and "the caller promised" is not something.

- [Running it](#running-it)
- [Target abstraction](#target-abstraction)
- [Value classes](#value-classes)
- [Calling convention](#calling-convention)
- [Native IR](#native-ir)
- [The implemented subset](#the-implemented-subset)
- [Refusal, not guessing](#refusal-not-guessing)
- [Runtime calls](#runtime-calls)
- [Tests](#tests)
- [What is deliberately missing](#what-is-deliberately-missing)

## Running it

    zl --emit-native-ir   <output|-> <file.zl>   # stop after selection
    zl --emit-native-code <output|-> <file.zl>   # also emit machine code

Both print, on stderr, a per-function ledger: which functions were compiled
natively, and for every other function *by name and with a reason* why it was
left to the VM. The two lists always account for every function in the module.

### A worked example

    tools/native_demo.sh ./build/zl_language

walks one small program through every stage — source, MIR, native IR,
disassembled machine code — then maps the emitted bytes executable, calls them
as ordinary System V functions, and diffs the results against the VM. Given

```zl
static func sumTo(int n): int {
    var acc = 0
    var i = 1
    while (i <= n) {
        acc = acc + i
        i = i + 1
    }
    return acc
}
```

MIR keeps the mutable locals as slots and the loop as a real CFG:

```
b1 (entry):
  store slot 1('acc'), 0:int
  store slot 2('i'), 1:int
  jump b2
b2:
  %1:int = load slot 2('i')
  %2:bool = le %1:int, $0:int
  branch %2:bool, b3, b4
b3:
  ...
  jump b2
b4:
  %8:int = load slot 1('acc')
  return %8:int
```

Selection turns `le` into the class-specific `icmp.le`, MIR slots into frame
slots, and constants into `imm.i`:

```
bb2 (mir2):
  %4:int = load slot2:i
  %5:int = icmp.le %4:t1, %1:n
  branch %5:t2, bb3, bb4
```

and emission produces the loop, with the back edge resolved by relocation:

```
 55:  mov    rax,QWORD PTR [rbp-0x10]   ; load i
 6a:  mov    rcx,QWORD PTR [rbp-0x18]   ; load n (spilled from rdi)
 71:  cmp    rax,rcx
 74:  setle  al
 77:  movzx  rax,al
 89:  test   rax,rax
 8c:  je     0x115                      ; -> bb4
 92:  jmp    0x97                       ; -> bb3
...
110:  jmp    0x55                        ; back edge
```

Running `sumTo`, `biggest` and `mix` as native functions produces exactly the
VM's output, which is what the script asserts.

## Target abstraction

`include/zl/native/target.hpp` is the boundary. Above it, nothing is
target-specific; below it (`src/native/emit.cpp`) is the only code that knows
what a byte of x86-64 looks like.

A `TargetMachine` is data, not code:

| Field | Meaning |
| --- | --- |
| `arch`, `platform`, `triple` | identity |
| `pointerSize`, `stackSlotSize`, `stackGrowsDown` | memory model |
| `cc` | the calling convention (below) |
| `allocatableInt` / `allocatableFloat` | registers an allocator may hand out |
| `slotSizeFor(class)` | frame footprint of a value class |
| `encoderAvailable()` | can this build actually emit code for it |

Two targets are described: `x64SysVTarget()` and `x64WindowsTarget()`. Only
System V has an encoder, and `encoderAvailable()` says so. Emitting System V
bytes under a Windows target would miscompile every call, so the honest answer
is a refusal rather than a plausible-looking output.

Adding a target means adding a table and an encoder. It does not mean editing
instruction selection, because selection never spells a register name — it asks
the convention how many argument registers of a class exist and takes the *i*-th.

## Value classes

A value class answers "where does the machine keep this", which is a strictly
coarser question than the ZL type lattice's "what does the language mean by
this". The mapping lives in one function, `classifyType`:

| ZL / MIR type | Class | Machine representation |
| --- | --- | --- |
| `void` | `Void` | no value |
| `int` | `Integer` | 64-bit two's complement, GPR |
| `bool` | `Integer` | 0 or 1 in a full-width GPR |
| `double` / `decimal` | `Float` | IEEE-754 binary64, SSE register |
| everything else | `Reference` | pointer to a GC-visible object |

`bool` is an `Integer` because the ABI has no narrower place to put it, and
because a bool only ever arrives from a compare or a constant, both of which
already normalise to 0/1.

Two decisions worth stating:

* **Unknown fails closed toward `Reference`.** A type the mapping does not
  recognise is classified as a managed reference, never as an integer.
* **`Reference` is currently rejected, not lowered.** There is no GC map yet, so
  a reference in a register would be invisible to the collector at a safepoint —
  the one class of bug that cannot be diagnosed after the fact. The class exists
  in the model so that the day it is supported, nothing above has to change.

## Calling convention

Described as data in `CallingConvention`, covering exactly what placing and
receiving a call requires:

* `intArgRegs` / `floatArgRegs` — argument registers in order, per class;
* `intReturnReg` / `floatReturnReg` — where a result comes back;
* `callerSaved` / `calleeSaved` — the split that decides whether a value live
  across a call may stay in a register at all;
* `intScratch` / `floatScratch` — registers the encoder may always use inside
  one instruction's expansion, and which the allocator therefore never sees;
* `stackAlignment`, `shadowSpace`, `redZone`, `calleePopsArguments` — the stack
  discipline.

The test suite asserts the invariants the split exists to guarantee: no register
is both caller- and callee-saved, and no allocatable register overlaps the
encoder's scratch set.

Arguments beyond the register set are refused rather than mis-placed; stack
argument passing is a stated gap, not an unstated one.

## Native IR

`include/zl/native/lir.hpp`. One representation sits between MIR and machine
code, and it is defined by what it removes:

* **types collapse to value classes** — an instruction's meaning no longer
  depends on consulting a type table;
* **opcodes become typed** — MIR's one type-directed `Add` becomes `IAdd` or
  `FAdd` at selection, and nothing downstream asks again;
* **values become virtual registers**, unlimited, each with a class;
* **mutable MIR slots become frame slots** with explicit `Load` / `Store`;
* **MIR block parameters become parallel copies** on the predecessor edges —
  out-of-SSA happens in selection, because the copies are real instructions
  that belong in the CFG rather than a secret the emitter keeps.

And by what it keeps: a real CFG, one definition per vreg, per-instruction
source lines, and the reference/integer distinction.

A `CallRuntime` opcode exists beside `Call` for the same operation with a
runtime callee. The distinction is kept precisely so that "what did this module
fall back on?" is greppable, and `LirModule::runtimeImports` lists the answer.

## The implemented subset

Deliberately small, and complete for what it claims:

* integer and floating-point constants;
* `+ - * / %` and unary negation, on `int` and on `double`;
* the integer bitwise operations `& | ^ ~ << >> >>>`;
* all six comparisons on both classes, and boolean `not` / `and` / `or`;
* `int → double` widening;
* `Load` / `Store` of primitive locals;
* `Jump`, `Branch`, `Return`, `Unreachable`;
* direct `Call` to another function that is itself in the subset.

Correctness details that are tested rather than assumed:

* **Integer division truncates toward zero** and the remainder takes the sign of
  the dividend, matching ZL. A zero divisor branches to a trap instead of
  executing `idiv`, whose `#DE` this tier has no handler for.
* **`>>` is arithmetic and `>>>` is logical**, as in the language.
* **Float comparison is IEEE-754 ordered.** `ucomisd` sets CF, ZF and PF all to
  1 for an unordered operand, so the "less" forms additionally require PF=0,
  equality excludes unordered, and inequality includes it. `NaN != NaN` is true,
  `NaN < 1.0` is false, and `0.0 == -0.0` is true — all three are test cases,
  because a single naive `setcc` gets each of them wrong.
* **`FNeg` flips the sign bit through a GPR**, which is exact for zeros,
  infinities and NaNs alike.

Register assignment is the simplest correct policy: every vreg gets a frame
slot, and each instruction loads its operands into the convention's scratch
registers, computes, and stores the result back. That is slow code and it is
correct code under every control-flow shape without a liveness analysis, which
is the right trade for a first tier — a real allocator is an optimisation over a
working baseline, whereas a half-right allocator is a miscompile. The frame
layout, the ABI handling and the encoder are what a linear-scan pass would reuse
unchanged.

Branches are resolved by a relocation pass over the emitted buffer, so blocks
may be laid out in any order and forward edges cost nothing.

## Refusal, not guessing

Everything outside the subset is refused **by name, per function, with the MIR
opcode and the source line that caused it**; the caller runs that function on
the VM. A backend that guesses at an operation it does not implement is worse
than one that admits it.

Refusal also propagates: a function the subset accepted, which calls one it
refused, is itself dropped — there would be no native body to jump to. This is
iterated to a fixed point, so the surviving native set is closed under calls.

Currently refused, each with its own message: references and strings, objects
and field access, collections, closures and lambdas, generic templates,
constructors, `switch`, `throw` and exception handlers, async and tasks, static
fields, non-GC ownership contracts, arguments beyond the register set, and
branches into blocks with parameters (which need a split critical edge).

## Runtime calls

The escape hatch stays open. `CallRuntime` lowers to an ordinary direct call
with the target's convention, and every symbol used is collected into
`LirModule::runtimeImports`, so an operation that cannot yet be lowered directly
can be routed to the existing runtime instead of forcing the whole function back
onto the VM. `SelectionOptions::allowRuntimeCalls` gates it, because a runtime
call has an ABI the caller must honour and that should be an explicit choice per
pipeline rather than a silent fallback.

## Tests

`tests/native_backend_tests.cpp` (target `zl-native-backend-tests`) runs the
whole pipeline and — the point of the file — **executes the emitted bytes**:
they are mapped executable, direct calls are relocated, and the result is called
through a function pointer and compared against what ZL's semantics say the
program computes. A backend test that only inspects the IR proves the backend
agrees with itself.

`tools/native_demo.sh` is the same idea in shell form, for looking at rather
than asserting on.

It covers the calling-convention invariants, the value-class mapping (including
that unknown fails closed), integer and float arithmetic, all comparisons
including the NaN cases, two-way branches, a counted loop through mutable slots,
a cross-function direct call, division edge cases, refusal of unsupported
functions and of their callers, refusal of MIR that does not verify, and two
end-to-end cases that lower real ZL source through the real front end.

## What is deliberately missing

Named so that no one has to discover them by reading the code: register
allocation, stack argument passing, a GC map and safepoints, unwind tables,
object layout and field access, string and collection operations, closures,
generic instantiation, jump tables for `switch`, an object-file or JIT writer
(the emitter produces bytes plus relocations and stops there), and any target
other than x86-64 System V.
