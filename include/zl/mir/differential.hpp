#pragma once

// ---------------------------------------------------------------------------
// Differential validation: does the optimised MIR observe the same thing?
// ---------------------------------------------------------------------------
//
// An optimiser's only product is a program that behaves the same while costing
// less. Saying "the verifier passes" does not establish that: the verifier
// checks that the MIR obeys the MIR's own rules, and a program can obey every
// rule while doing something different. This is the second half of the
// argument.
//
// `compareModules` takes the module as it was lowered and the module as the
// optimiser left it, and asks whether anything a *program* can observe has
// changed. It is a static check - it reads the two modules, it does not run
// them - and it is built on one definition of "observable":
//
//   an instruction is observable when its effect set contains something the
//   outside world, another thread, or the language's lifetime discipline can
//   see. Output, calls, heap writes, task and thread operations, synchronisation,
//   volatile reads, native-resource events, ownership events and suspension are
//   observable. Pure computation, allocation, and reading are not.
//
// Two consequences are deliberate, and both are conservatism in the right
// direction:
//
//   * a runtime *check* (`refine`, `null_check`, `field_load`) is not counted.
//     The optimiser is allowed to delete a check it has proved cannot fail, and
//     counting it would make every correct removal look like a divergence.
//
//   * a write to a local slot is not counted either. It is observable only
//     through a later read of that slot, and deciding whether a given store can
//     be observed is the same work the optimiser does to remove it - so
//     counting stores would make this comparison either blind or circular.
//     Slot writes get a weaker rule instead: one may be removed, never added.
//
// What is left is the part of the program that is the point of the program:
// what it prints, what it calls, what it writes, what it starts, what it
// releases. If that sequence changes, the optimiser is broken, and this says so
// before a backend ever sees the module.

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include "zl/mir/function.hpp"

namespace zl::mir {

struct DifferentialOptions {
    // Compare the module's shape: the same functions in the same order, with
    // the same signatures, the same classes and statics, and the same entry
    // point.
    bool compareStructure{true};
    // Compare each function's observable events (see the header comment).
    bool compareObservableEvents{true};
    // Judge the optimised module against the unoptimised one *after* running
    // the same pipeline over it, so that deleting code which cannot run is not
    // reported as a divergence while deleting code that can is.
    //
    // This is the one place the comparison leans on the optimiser, and it says
    // so rather than hiding it: deciding which events a program can actually
    // perform is the optimiser's job, so a comparison that refused to use it
    // could only ever report "something was removed". Turning this off makes
    // the event comparison strict against the raw original, which is useful
    // when the pipeline under test *is* the default one and the question is
    // whether it is idempotent.
    bool normalizeBefore{true};
    // The pipeline used to build that normalised reference. Empty means the
    // default one.
    //
    // It has to be the same pipeline the `after` module was built with. The
    // event comparison asks "did the optimiser remove anything it should not
    // have", and it answers that by measuring `after` against a reference that
    // has had the same removal opportunities. Judge a module built by one
    // pass against a reference built by eight and every event the other seven
    // would have deleted is reported as a divergence - which is a statement
    // about the comparison, not about the module.
    std::string referencePipeline;
    // Optimisation may not make the program bigger. A pass that adds
    // instructions is either wrong or not finished; either way, say so.
    bool requireNoGrowth{true};
    std::size_t maxMismatches{32};

    [[nodiscard]] std::string describe() const;
    // The options with `referencePipeline` set to `spec`, so a caller that
    // optimised with a named pipeline asks for a reference built the same way.
    [[nodiscard]] DifferentialOptions withReferencePipeline(std::string spec) const;
};

struct DifferentialMismatch {
    // Empty for a module-level mismatch.
    std::string function;
    std::string kind;
    std::string detail;

    [[nodiscard]] std::string describe() const;
};

struct DifferentialResult {
    bool equivalent{true};
    std::vector<DifferentialMismatch> mismatches;
    // Notes are not failures: they record a difference that is allowed but
    // worth seeing, such as an event sequence whose order changed while its
    // contents did not.
    std::vector<std::string> notes;
    std::size_t functionsCompared{0};
    std::size_t instructionsBefore{0};
    std::size_t instructionsAfter{0};
    std::size_t eventsCompared{0};
    bool truncated{false};

    [[nodiscard]] bool ok() const noexcept { return equivalent; }
    [[nodiscard]] std::string describe() const;
};

// The observable events of one function, in reverse post-order over the blocks
// reachable from its entry. Each entry is a stable rendering - opcode and
// target identity, never a value - so two modules can be compared by string.
[[nodiscard]] std::vector<std::string> observableEventsOf(const Module& module, const Function& function,
                                                          const std::unordered_set<SlotId>& observedSlots);

// Compares two versions of one module: the unoptimised `before` and the
// optimised `after`. Both are expected to verify; this answers the question the
// verifier does not ask.
[[nodiscard]] DifferentialResult compareModules(const Module& before, const Module& after,
                                               const DifferentialOptions& options = {});

} // namespace zl::mir
