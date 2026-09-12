#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zl/compiler/module_loader.hpp"
#include "zl/compiler/type_checker.hpp"
#include "zl/mir/lowering.hpp"
#include "zl/mir/passes.hpp"
#include "zl/mir/verifier.hpp"
#include "zl/mir/vm_backend.hpp"
#include "zl/native/pipeline.hpp"

// ---------------------------------------------------------------------------
// The compiler pipeline: the one path from ZL source to a selected backend
// ---------------------------------------------------------------------------
//
//     ZL source
//       -> lexer/parser (ModuleLoader)
//       -> semantic/type analysis (TypeChecker)
//       -> typed lowering
//       -> MIR
//       -> MIR verification
//       -> MIR optimisation
//       -> selected backend
//
// This header is that sentence as an API. It exists for three reasons:
//
//   1. **MIR is the compiler boundary.** Every backend - the shipped bytecode
//      backend, the native/AOT backend, and anything added later - is handed
//      the *same* verified `zl::mir::Module` and nothing else. No backend
//      re-reads the AST, and none of them re-derive what an expression meant;
//      the AST and the type checker decide language semantics once, typed
//      lowering records those decisions in MIR, and a backend only translates
//      them into a target format.
//   2. **The stages are named, ordered and checkable.** Before this, each CLI
//      command hand-rolled its own copy of "load, check, lower, verify, ...",
//      so the pipeline was a convention rather than a component. Here the
//      stages are a `Stage` enum, each stage has a stated invariant (see
//      `stageInvariant`), and each stage records what it did.
//   3. **Selecting a backend cannot change what a program means.** The backend
//      is an option, and the option is applied strictly after MIR: the module a
//      backend receives is byte-identical (same digest) whichever backend runs,
//      which is what makes the choice a pure code-generation decision. See
//      `Result::mirDigest()` and `docs/pipeline.md`.
//
// Deliberately *not* here: lexing, parsing, module resolution and type checking
// internals (they belong to the front end), any IR construction (that is
// `zl::mir`), and any target-specific encoding (that is each backend). A
// pipeline that knew about any of those would be a second implementation of
// them rather than a composition of them.

namespace zl::pipeline {

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

// The canonical stage order, top to bottom. `Stage` values are ordered: the
// pipeline runs them in exactly this order and refuses to run one out of turn
// (`ErrorKind::Invariant`), because a stage's postcondition is the next
// stage's precondition.
enum class Stage : std::uint8_t {
    // 1. Lexing, parsing and import resolution -> one merged `zl::Program`.
    Load,
    // 2. Semantic analysis: `TypeChecker::check`. This is where ZL's language
    //    semantics are decided - types, overloads, visibility, mutability,
    //    ownership and capture rules - and recorded per AST node.
    SemanticAnalysis,
    // 3. Typed lowering: AST + recorded types -> `zl::mir::Module`. The lowerer
    //    performs no inference of its own; unsupported constructs mark a
    //    *function* incomplete rather than producing a guess.
    TypedLowering,
    // 4. MIR verification: every invariant a backend is entitled to assume.
    MirVerification,
    // 5. MIR optimisation: the pass manager, over verified MIR, verifying
    //    again between passes and rolling back a pass that breaks the module.
    MirOptimization,
    // 6. Code generation: exactly one selected backend translates the verified
    //    module into a target format.
    CodeGeneration,
};

inline constexpr std::size_t kStageCount = 6;

[[nodiscard]] const char* stageName(Stage stage) noexcept;
// One line stating what the stage guarantees to the next one. Printed by
// `zl --pipeline-report`, and the thing a test names when it fails.
[[nodiscard]] const char* stageInvariant(Stage stage) noexcept;

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

// Which code generator consumes the verified MIR module.
//
// Both are consumers of the same boundary and neither is privileged by the
// language: `Bytecode` targets the existing stack VM (`compileModuleToBytecode`
// -> `zl::Chunk`), `Native` targets machine code through the native IR
// (`compileMirToNative` -> native IR -> x86-64). A third backend would be a new
// enumerator plus a new arm in `Pipeline::generate`, and nothing else in the
// compiler would move.
enum class Backend : std::uint8_t {
    Bytecode,
    Native,
};

[[nodiscard]] const char* backendName(Backend backend) noexcept;
// Parses "bytecode"/"native" (case-insensitive). Returns false for anything
// else, so an unknown spelling is a usage error rather than a silent default.
[[nodiscard]] bool parseBackend(const std::string& text, Backend& out) noexcept;

// What the process does with the generated code.
enum class Execution : std::uint8_t {
    // Generate and stop (the `--emit-*` commands).
    None,
    // Generate, then run the program on the VM. The executed artifact is the
    // bytecode translation of the *same* verified MIR the selected backend
    // consumed, because the VM is the only execution driver in this phase (see
    // "Compatibility with the VM" in docs/pipeline.md). Selecting `Backend::Native`
    // therefore still executes MIR-derived bytecode: the native module is code
    // generation, not yet a runtime.
    Vm,
};

// ---------------------------------------------------------------------------
// Failure reporting
// ---------------------------------------------------------------------------

// Which kind of failure ended the pipeline. The kind chooses the message the
// CLI prints and the exit code, so both stay stable while the plumbing moves.
enum class ErrorKind : std::uint8_t {
    None,
    // Import resolution, file naming rules, duplicate declarations.
    Module,
    // Lexer/parser rejection.
    Syntax,
    // `TypeChecker::check` rejection: the program is not a ZL program.
    TypeCheck,
    // MIR contract violation. A backend must not see this module.
    Verification,
    // A construct the language has but MIR (or the lowerer) cannot represent
    // yet, reported rather than guessed at.
    Unsupported,
    // The optimiser could not produce a trustworthy module.
    Optimization,
    // The selected backend refused, or an encoder failed.
    Backend,
    // Something failed before any of the above could classify it: an I/O error,
    // an unclassified exception, a missing file.
    Environment,
    // The pipeline itself was driven incorrectly, or an internal stage
    // postcondition did not hold. Always a compiler defect, never user error.
    Invariant,
    // The caller asked for something the command line cannot express.
    Usage,
};

[[nodiscard]] const char* errorKindName(ErrorKind kind) noexcept;
// The message prefix the CLI prints: "module error", "syntax error",
// "compile error", ...
[[nodiscard]] const char* errorKindLabel(ErrorKind kind) noexcept;
// 0 for None; otherwise the process exit code this failure maps to:
// 1 front-end/backend failure, 2 usage, 4 verification or backend refusal,
// 6 unsupported lowering, 7 environment. (3, a stdlib version mismatch, is
// decided by the caller before the pipeline starts; 5, an unwritable output,
// is decided after it finishes.)
[[nodiscard]] int exitCodeFor(ErrorKind kind) noexcept;

struct Failure {
    ErrorKind kind{ErrorKind::None};
    // The stage that stopped the pipeline.
    Stage stage{Stage::Load};
    // One line: what failed.
    std::string message;
    // Optional multi-line detail (verifier diagnostics, backend ledger).
    std::string detail;

    [[nodiscard]] bool ok() const noexcept { return kind == ErrorKind::None; }
};

// ---------------------------------------------------------------------------
// Stage records
// ---------------------------------------------------------------------------

struct StageRecord {
    Stage stage{Stage::Load};
    // False for a stage the options skipped (optimisation and slot promotion
    // are opt-in). A skipped stage is still *recorded*, so "the optimiser did
    // not run" is visible in a report instead of being an absence.
    bool ran{false};
    bool ok{true};
    // One line describing what the stage produced or why it stopped.
    std::string detail;
    // A stage-specific count: declarations loaded, functions lowered, passes
    // run, functions stubbed, backend functions compiled.
    std::size_t count{0};
    double milliseconds{0.0};

    [[nodiscard]] std::string describe() const;
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    // Which backend translates MIR. Default: the shipped bytecode backend.
    Backend backend{Backend::Bytecode};
    Execution execution{Execution::Vm};

    bool requireMain{true};

    // Stage 4. On by default and not meant to be turned off; the switch exists
    // so a test can prove that stage 6 refuses to run without it.
    bool verify{true};
    // Re-verify immediately before handing the module to a backend. Cheap
    // insurance against anything mutating the module between stages 4 and 6,
    // and the reason a backend can trust its input at all. With this off, code
    // generation still refuses to run unless stage 4 verified the module.
    bool verifyBeforeCodegen{true};

    // Run the code-generation stage. Commands that only inspect MIR
    // (`--emit-mir`, `--emit-ssa`, `--mir-opt-check`, `--safety-check`) turn it
    // off: their result is the MIR boundary, and a library file with no entry
    // point has no code to generate in the first place.
    bool codeGeneration{true};

    // Stage 5. Opt-in per invocation, like every other compile-time cost in
    // this project: the optimiser is proven observationally equivalent by
    // tools/mir_opt_diff.sh and tools/mir_opt_check_all.sh, and those harnesses
    // measure it by running both ways, which they can only do while both ways
    // are reachable.
    bool optimize{false};
    // Run `compareModules` on the pre/post optimisation pair and fail the stage
    // if the two do not observe the same events. Off by default (it is a
    // checker, not a compiler feature); the harnesses and `--mir-opt-check`
    // turn it on.
    bool checkOptimization{false};
    // Empty means "default", the curated pipeline.
    std::string optimizerPipeline;

    // Promote mutable locals to block parameters before code generation. An IR
    // *form* choice, not an optimisation: both forms mean the same thing, and
    // both backends must accept either.
    bool promoteSlots{false};

    // Refuse to finish when the native backend left any function to the VM.
    bool strictNative{false};

    // Print the stage ledger to stderr as the pipeline runs.
    bool verbose{false};
    // Print the optimiser's per-pass trace (`ZL_MIR_OPT_VERBOSE`). Separate
    // from `verbose` so that "show me the stages" does not mean "show me every
    // pass on every function".
    bool traceOptimization{false};
    // Collect one line per slot the SSA promotion declined
    // (`ZL_MIR_SSA_VERBOSE`).
    bool reportPromotionSkips{false};

    zl::mir::LoweringOptions lowering{};
    zl::mir::VerifierOptions verification{};
    zl::mir::OptimizationOptions optimization{};
    zl::native::PipelineOptions native{};
};

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

// What the SSA-form promotion did, when it was asked for. Both MIR forms mean
// the same thing, so this is a form report rather than a metric: the number of
// slots that stayed in memory form, and why, is the interesting part.
struct SlotPromotion {
    bool ran{false};
    std::size_t promotedSlots{0};
    std::size_t blockParameters{0};
    std::size_t loadsRemoved{0};
    std::size_t storesRemoved{0};
    std::size_t changedFunctions{0};
    // One line per slot the promotion declined (only collected when
    // `Options::verbose` is set).
    std::vector<std::string> notes;
};

struct Result {
    Options options{};
    Failure failure{};
    std::vector<StageRecord> stages;
    // The entry file the load stage was pointed at.
    std::filesystem::path entry;

    // Stage 1 output. Kept because lowering diagnostics point back into it and
    // because a caller may want to re-lower; MIR itself does not reference it.
    std::unique_ptr<zl::Program> program;
    // Stage 2. Only valid while `program` is alive.
    std::unique_ptr<zl::TypeChecker> checker;

    // Stages 3-5 output: MIR, the compiler boundary. The module a backend
    // receives is this one, after optimisation if the stage ran.
    zl::mir::Module module;
    std::vector<std::string> loweringDiagnostics;
    // Functions that are not a complete translation of their source body.
    // Reported, never fatal: an incomplete function is still well-formed MIR.
    std::vector<std::string> incompleteFunctions;

    SlotPromotion promotion;
    zl::mir::VerificationReport verification;
    std::size_t optimizationPasses{0};
    std::string optimizationPipeline;

    // Stage 6 artifacts. Exactly one of these matches `options.backend`;
    // `chunk` is additionally populated under `Backend::Native` when
    // `options.execution == Execution::Vm`, because the VM is the execution
    // driver in this phase.
    std::optional<zl::Chunk> chunk;
    std::optional<zl::native::PipelineResult> native;
    std::string nativeTargetTriple;
    // Bytecode-backend refusals: functions that were stubbed rather than
    // mis-translated. A program that never reaches one behaves normally; one
    // that does raises at runtime, loudly.
    std::vector<std::string> stubbedFunctions;
    std::vector<std::string> stubbedReasons;

    [[nodiscard]] bool ok() const noexcept { return failure.ok(); }
    // True when every lowered function is a complete translation.
    [[nodiscard]] bool complete() const { return incompleteFunctions.empty(); }
    [[nodiscard]] const StageRecord* stage(Stage stage) const;
    // The stage ledger, one line per stage, in order.
    [[nodiscard]] std::string describe() const;
    // A stable digest of the MIR handed to the backend. Two runs that produce
    // the same digest handed their backend the same module, which is the
    // property `--pipeline-report` checks across backends.
    [[nodiscard]] std::string mirDigest() const;
};

// A structural digest of a module: function names, shapes, opcodes, operand
// kinds, types, callee identities, and the constant/static/class/interface
// pools the operands index into - in order, so two modules differ whenever
// anything a backend can observe differs. Independent of the printer and cheap
// enough to run on every compile.
[[nodiscard]] std::string moduleDigest(const zl::mir::Module& module);

// ---------------------------------------------------------------------------
// The pipeline
// ---------------------------------------------------------------------------

class Pipeline {
public:
    explicit Pipeline(Options options = {});

    // Runs every stage from `Load` to `CodeGeneration`, stopping at the first
    // failure. `extraRoots` are the module search roots after the project's own
    // source root (dependency roots, then the stdlib root).
    [[nodiscard]] bool run(const std::filesystem::path& entry,
                           const std::vector<std::filesystem::path>& extraRoots = {});

    // The stages, one at a time. `run` is exactly these calls in this order;
    // they are public because a test (or a future incremental driver) should be
    // able to stop between two stages and look at what the boundary holds.
    [[nodiscard]] bool load(const std::filesystem::path& entry,
                            const std::vector<std::filesystem::path>& extraRoots = {});
    [[nodiscard]] bool analyze();
    [[nodiscard]] bool lowerToMir();
    [[nodiscard]] bool verifyMir();
    [[nodiscard]] bool optimizeMir();
    [[nodiscard]] bool generate();
    // Records the code-generation stage as deliberately not requested.
    [[nodiscard]] bool skipCodeGeneration();

    [[nodiscard]] const Result& result() const noexcept { return result_; }
    [[nodiscard]] Result& result() noexcept { return result_; }

private:
    // Records a stage outcome. Every stage goes through this, which is what
    // keeps the ledger complete.
    StageRecord& beginStage(Stage stage);
    void finishStage(StageRecord& record, bool ok, std::string detail, std::size_t count = 0,
                     std::chrono::steady_clock::time_point started = {});
    // True when the stage is allowed to run now, and false (with an
    // `ErrorKind::Invariant` failure recorded) when it is out of turn.
    [[nodiscard]] bool expectStage(Stage stage);
    [[nodiscard]] bool fail(Stage stage, ErrorKind kind, std::string message, std::string detail = {});
    void reportStage(const StageRecord& record) const;

    Options options_;
    Result result_;
    // Index of the next stage in `stages`, i.e. the pipeline's program counter.
    std::size_t nextStage_{0};
    // Set by the verification stage (or by the pre-codegen re-check).
    bool verified_{false};
    // The stage whose ledger line is still open, so a failure can close it.
    StageRecord* current_{nullptr};
    std::chrono::steady_clock::time_point stageStarted_{};
};

// One-shot form of `Pipeline::run`, for callers that do not need the stages.
[[nodiscard]] Result compile(const std::filesystem::path& entry,
                             const std::vector<std::filesystem::path>& extraRoots = {},
                             Options options = {});

} // namespace zl::pipeline
