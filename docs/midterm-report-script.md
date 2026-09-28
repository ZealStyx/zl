# Report Script — 3 minutes

**Parts:** Risk Profile & Mitigation · Agile Execution Cycle
Read aloud at a normal pace this runs ~3:10. Cut marks at the end bring it to 2:00.
**Bold = slow down and land it.**

---

### Opening — 0:15

Good morning. I'm covering two parts of our ZL project: our risk profile, and
how our Agile cycle actually runs.

Both come down to one fact about compilers. **A compiler that crashes is safe.
A compiler that lies is dangerous** — it hands you a program that runs, prints
an answer, and the answer is quietly wrong.

---

### Part A — Risk Profile & Mitigation — 1:15

Our risk level is **medium to high**. Nothing in ZL is life-critical, but three
risks are real.

**First, silent wrong results.** We have two backends — a bytecode VM and a
native x86-64 backend. Two translations of the same program can disagree. Our
mitigation is structural: both backends are handed the *same verified MIR*, our
typed intermediate representation, and five differential harnesses run the same
programs through both on every push and require identical output. For floating
point that means bit-for-bit identical, not "close enough."

**Second, memory and native-code crashes.** Native frames can't safely enter our
garbage-collected runtime. So the native driver **refuses, by name and reason**,
anything it cannot honestly execute — unsupported signatures, unsupported hosts.
References and objects are blocked until the GC map exists. We'd rather refuse
loudly than guess.

**Third, architecture drift** — three people editing one shared core. A backend
that quietly starts reading the AST still compiles; it just now has its own
opinion about the language. So the boundary is machine-checked: `boundary_lint`
reads our source and fails the build if a backend reaches past MIR.

The point is that **none of these mitigations is a promise. Each one is a
command that returns an error code.**

---

### Transition — 0:10

And that's what connects to my second part — because those checks are exactly
what an iteration has to pass before we call anything finished.

---

### Part B — Agile Execution Cycle — 1:20

Our loop is: backlog, iteration planning, implementation, **verification gate**,
review and document, working increment — then back to the backlog.

Two arrows matter. A **red gate sends work back to implementation** — it never
advances. And writing our examples and standard library *in ZL itself* keeps
exposing compiler gaps, which feed straight back into the backlog. We generate
our own requirements by using our own language.

Here's one real iteration — our native execution driver, September 20th.

**Backlog:** the native backend had been emitting machine code that nothing ever
called. **Planning:** we set the exit criterion before coding — a named program
must run end to end with a *measured* wall time against the VM. **Implementation:**
the driver, plus a benchmark program with a VM twin. **Gate:** a parity test
requiring both tiers to agree exactly. **Review:** the changelog entry with the
number — **6 milliseconds of machine code against 870 on the VM**, about 140
times. **Increment:** everything green — tests, regressions, all 52 examples,
native gate pass.

And the finding we could only get by measuring: the gap between the two tiers is
**subset coverage, not the quality of the code we emit.**

---

### Close — 0:10

So: the risks are the kind that stay hidden, and the cycle is built to make them
visible before a task can close. Thank you.

---

## Cut to 2:00

- Drop **risk three** (architecture drift) and the sentence after it.
- Drop the transition; go straight into "Our loop is…".
- In Part B, drop the backlog/planning/implementation breakdown — keep only:
  *"One example: our native backend emitted code nothing ever called. The exit
  criterion was a measured end-to-end run — 6 milliseconds against 870 on the
  VM — and it closed with every gate green."*

## If asked

- **Why not Waterfall?** Language behaviour is discovered by running programs; testing only at the end would let wrong-code bugs pile up.
- **Why not Spiral?** A formal risk review every loop is too heavy for three students. We keep its best idea: risky features start with a design and a measured baseline.
- **Biggest weakness?** The native backend covers about 1.7% of a realistic module. It's written in our task list under P1-6 — a limitation you've written down is a task; one you haven't is a surprise.

*Longer version with the full risk table and Q&A prep: `docs/midterm-report-guide.md`.*
