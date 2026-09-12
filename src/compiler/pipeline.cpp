#include "zl/compiler/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <utility>

#include "zl/compiler/module_loader.hpp"
#include "zl/mir/differential.hpp"
#include "zl/parser/parser.hpp"
#include "zl/mir/ssa.hpp"
#include "zl/native/target.hpp"

namespace zl::pipeline {

namespace {

using Clock = std::chrono::steady_clock;

double elapsedMilliseconds(Clock::time_point start) {
    if (start == Clock::time_point{}) return 0.0;
    const auto delta = std::chrono::duration<double, std::milli>(Clock::now() - start);
    return delta.count();
}

std::string formatMilliseconds(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f ms", value);
    return buffer;
}

std::string plural(std::size_t count, const char* singular, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? singular : many);
}

// Joins a list into one line, capped so a report stays readable. The count is
// always reported in full even when the list is truncated - the number is the
// fact, the names are the illustration.
std::string joinCapped(const std::vector<std::string>& items, const char* separator = ", ",
                       std::size_t limit = 8) {
    std::string out;
    for (std::size_t i = 0; i < items.size() && i < limit; ++i) {
        if (i) out += separator;
        out += items[i];
    }
    if (items.size() > limit) out += " (+" + std::to_string(items.size() - limit) + " more)";
    return out;
}

// --- structural digest ----------------------------------------------------

void hashBytes(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL; // FNV-1a prime
    }
}

void hashInteger(std::uint64_t& hash, std::uint64_t value) { hashBytes(hash, &value, sizeof(value)); }

void hashString(std::uint64_t& hash, const std::string& value) {
    hashInteger(hash, value.size());
    hashBytes(hash, value.data(), value.size());
}

void hashOperand(std::uint64_t& hash, const zl::mir::Operand& operand) {
    hashInteger(hash, static_cast<std::uint64_t>(operand.kind));
    hashInteger(hash, operand.type);
    hashInteger(hash, operand.index);
}

void hashConstant(std::uint64_t& hash, const zl::mir::Constant& constant) {
    hashInteger(hash, static_cast<std::uint64_t>(constant.kind));
    hashInteger(hash, constant.boolValue);
    hashInteger(hash, static_cast<std::uint64_t>(constant.intValue));
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(constant.doubleValue));
    std::memcpy(&bits, &constant.doubleValue, sizeof(bits));
    hashInteger(hash, bits);
    hashString(hash, constant.stringValue);
    hashString(hash, constant.enumTypeName);
}

// A call instruction's callee identity. Hashing the opcode alone would make a
// module that calls `a` and one that calls `b` structurally identical, which is
// exactly the difference a backend must not miss.
void hashCallTarget(std::uint64_t& hash, const zl::mir::CallTarget& target) {
    hashInteger(hash, target.function);
    hashString(hash, target.className);
    hashString(hash, target.methodName);
    hashString(hash, target.nativeName);
    hashInteger(hash, static_cast<std::uint64_t>(target.nativeId));
    hashInteger(hash, target.argumentTypes.size());
    for (const auto type : target.argumentTypes) hashInteger(hash, type);
    hashInteger(hash, target.resultType);
    hashInteger(hash, target.isAsync);
    hashInteger(hash, target.nativeParamOwnership.size());
    for (const auto ownership : target.nativeParamOwnership)
        hashInteger(hash, static_cast<std::uint64_t>(ownership));
    hashInteger(hash, static_cast<std::uint64_t>(target.nativeReturnOwnership));
    hashString(hash, target.ffiLibrary);
    hashString(hash, target.ffiSymbol);
    hashInteger(hash, target.ffiParamTags.size());
    for (const auto tag : target.ffiParamTags) hashInteger(hash, static_cast<std::uint64_t>(tag));
    hashInteger(hash, static_cast<std::uint64_t>(target.ffiReturnTag));
}

} // namespace

// ---------------------------------------------------------------------------
// Stage metadata
// ---------------------------------------------------------------------------

const char* stageName(Stage stage) noexcept {
    switch (stage) {
        case Stage::Load: return "load";
        case Stage::SemanticAnalysis: return "semantic analysis";
        case Stage::TypedLowering: return "typed lowering";
        case Stage::MirVerification: return "MIR verification";
        case Stage::MirOptimization: return "MIR optimization";
        case Stage::CodeGeneration: return "code generation";
    }
    return "unknown";
}

const char* stageInvariant(Stage stage) noexcept {
    switch (stage) {
        case Stage::Load:
            return "the program is the complete merged translation unit: every declaration of the "
                   "entry file and of every transitively imported file, exactly once";
        case Stage::SemanticAnalysis:
            return "the program is a well-formed ZL program, and the checker's recorded expression "
                   "types are the only source of type truth downstream";
        case Stage::TypedLowering:
            return "every source function is present as MIR; a function that cannot be represented "
                   "is marked incomplete rather than guessed at";
        case Stage::MirVerification:
            return "the module satisfies every MIR invariant (docs/mir.md), which is exactly what "
                   "entitles a backend to assume them without re-checking";
        case Stage::MirOptimization:
            return "the module still verifies and still observes the same events; a pass that "
                   "breaks either is rolled back and the stage fails";
        case Stage::CodeGeneration:
            return "the selected backend produced its artifact from this exact verified module, "
                   "and re-derived no language semantics from the AST";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Backends, failures
// ---------------------------------------------------------------------------

const char* backendName(Backend backend) noexcept {
    switch (backend) {
        case Backend::Bytecode: return "bytecode";
        case Backend::Native: return "native";
    }
    return "unknown";
}

bool parseBackend(const std::string& text, Backend& out) noexcept {
    std::string lowered;
    lowered.reserve(text.size());
    for (char c : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lowered == "bytecode" || lowered == "vm" || lowered == "mir") {
        out = Backend::Bytecode;
        return true;
    }
    if (lowered == "native" || lowered == "aot" || lowered == "machine") {
        out = Backend::Native;
        return true;
    }
    return false;
}

const char* errorKindName(ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::None: return "none";
        case ErrorKind::Module: return "module";
        case ErrorKind::Syntax: return "syntax";
        case ErrorKind::TypeCheck: return "type-check";
        case ErrorKind::Verification: return "verification";
        case ErrorKind::Unsupported: return "unsupported";
        case ErrorKind::Optimization: return "optimization";
        case ErrorKind::Backend: return "backend";
        case ErrorKind::Environment: return "environment";
        case ErrorKind::Invariant: return "compiler-invariant";
        case ErrorKind::Usage: return "usage";
    }
    return "unknown";
}

const char* errorKindLabel(ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::None: return "";
        case ErrorKind::Module: return "module error";
        case ErrorKind::Syntax: return "syntax error";
        case ErrorKind::TypeCheck: return "compile error";
        case ErrorKind::Verification: return "MIR verification error";
        case ErrorKind::Unsupported: return "unsupported construct";
        case ErrorKind::Optimization: return "MIR optimization error";
        case ErrorKind::Backend: return "backend error";
        case ErrorKind::Environment: return "error";
        case ErrorKind::Invariant: return "compiler invariant violated";
        case ErrorKind::Usage: return "usage error";
    }
    return "error";
}

int exitCodeFor(ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::None: return 0;
        case ErrorKind::Module:
        case ErrorKind::Syntax:
        case ErrorKind::TypeCheck:
        case ErrorKind::Invariant:
        case ErrorKind::Environment: return 1;
        case ErrorKind::Usage: return 2;
        case ErrorKind::Verification:
        case ErrorKind::Optimization:
        case ErrorKind::Backend: return 4;
        case ErrorKind::Unsupported: return 6;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Stage records and result reporting
// ---------------------------------------------------------------------------

std::string StageRecord::describe() const {
    std::ostringstream out;
    out << (ran ? "ok  " : "--  ") << stageName(stage);
    const std::size_t nameLength = std::string(stageName(stage)).size();
    for (std::size_t i = nameLength; i < 19; ++i) out << ' ';
    out << (ok ? "ok" : "FAIL");
    if (ran) {
        out << "  " << formatMilliseconds(milliseconds);
    } else {
        out << "      (skipped)";
    }
    if (!detail.empty()) out << "  " << detail;
    return out.str();
}

const StageRecord* Result::stage(Stage stage) const {
    for (const auto& record : stages) {
        if (record.stage == stage) return &record;
    }
    return nullptr;
}

std::string Result::describe() const {
    std::ostringstream out;
    out << "compiler pipeline: " << entry.string() << " (backend " << backendName(options.backend)
        << ")\n";
    for (const auto& record : stages) out << "  " << record.describe() << "\n";
    out << "  " << "mir boundary      " << mirDigest() << "  ("
        << plural(module.functions.size(), "function", "functions") << ", backend "
        << backendName(options.backend) << ")";
    return out.str();
}

std::string moduleDigest(const zl::mir::Module& module) {
    std::uint64_t hash = 1469598103934665603ULL; // FNV-1a offset basis
    hashString(hash, module.name);
    hashInteger(hash, module.entryPoint);
    hashInteger(hash, module.functions.size());
    hashInteger(hash, module.statics.size());
    hashInteger(hash, module.classes.size());
    hashInteger(hash, module.interfaces.size());
    hashInteger(hash, module.constants.size());
    for (const auto& function : module.functions) {
        hashString(hash, function.name);
        hashString(hash, function.ownerClass);
        hashInteger(hash, function.hasThisParameter);
        hashInteger(hash, function.returnType);
        hashInteger(hash, static_cast<std::uint64_t>(function.returnOwnership));
        hashInteger(hash, function.isAsync);
        hashInteger(hash, function.isLambda);
        hashInteger(hash, function.isConstructor);
        hashInteger(hash, function.isStatic);
        hashInteger(hash, function.isNative);
        hashInteger(hash, function.isOperator);
        hashInteger(hash, function.isGenericTemplate);
        hashInteger(hash, function.incomplete);
        for (const auto& parameter : function.typeParameters) hashString(hash, parameter);
        for (const auto argument : function.genericArguments) hashInteger(hash, argument);
        hashInteger(hash, static_cast<std::uint64_t>(function.exceptionBehavior));
        hashInteger(hash, function.captures.size());
        for (const auto& capture : function.captures) {
            hashString(hash, capture.name);
            hashString(hash, capture.storage);
            hashInteger(hash, capture.type);
            hashInteger(hash, capture.usesThis);
        }
        for (const auto& parameter : function.parameters) {
            hashString(hash, parameter.name);
            hashInteger(hash, parameter.type);
            hashInteger(hash, static_cast<std::uint64_t>(parameter.ownership));
            hashString(hash, parameter.borrowSource);
        }
        for (const auto& slot : function.slots) {
            hashString(hash, slot.name);
            hashInteger(hash, slot.type);
            hashInteger(hash, slot.isMutable);
            hashInteger(hash, slot.isCatchBinding);
            hashInteger(hash, static_cast<std::uint64_t>(slot.ownership));
        }
        for (const auto& block : function.blocks) {
            hashInteger(hash, block.id);
            hashInteger(hash, static_cast<std::uint64_t>(block.kind));
            hashInteger(hash, block.parameters.size());
            for (const auto& parameter : block.parameters) {
                hashInteger(hash, parameter.id);
                hashInteger(hash, parameter.type);
            }
            for (const auto& instruction : block.instructions) {
                hashInteger(hash, static_cast<std::uint64_t>(instruction.opcode));
                hashInteger(hash, instruction.result);
                hashInteger(hash, instruction.resultType);
                hashInteger(hash, instruction.operands.size());
                for (const auto& operand : instruction.operands) hashOperand(hash, operand);
                hashString(hash, instruction.name);
                hashString(hash, instruction.className);
                for (const auto argument : instruction.typeArguments) hashInteger(hash, argument);
                hashCallTarget(hash, instruction.target);
                hashInteger(hash, instruction.testedType);
                hashInteger(hash, instruction.slot);
            }
            const auto& terminator = block.terminator;
            hashInteger(hash, static_cast<std::uint64_t>(terminator.kind));
            hashOperand(hash, terminator.value);
            hashInteger(hash, terminator.target);
            hashInteger(hash, terminator.elseBlock);
            hashInteger(hash, terminator.cases.size());
            for (const auto& target : terminator.cases) {
                hashInteger(hash, static_cast<std::uint64_t>(target.value));
                hashInteger(hash, target.block);
            }
            hashInteger(hash, terminator.edgeArguments.size());
            for (const auto& edges : terminator.edgeArguments) {
                hashInteger(hash, edges.size());
                for (const auto& operand : edges) hashOperand(hash, operand);
            }
            for (const auto& handler : block.exceptionHandlers) {
                hashInteger(hash, handler.catchType);
                hashInteger(hash, handler.block);
                hashInteger(hash, handler.catchSlot);
                hashInteger(hash, handler.isFinally);
            }
        }
    }
    // The pools a backend resolves ids against. Instruction operands name
    // constants, statics and classes by index, so hashing the instructions
    // without these would let two modules differ only in what those indexes
    // point at and still digest the same.
    for (const auto& constant : module.constants) hashConstant(hash, constant);
    for (const auto& field : module.statics) {
        hashInteger(hash, field.id);
        hashString(hash, field.className);
        hashString(hash, field.name);
        hashInteger(hash, field.type);
        hashInteger(hash, field.initializer);
    }
    for (const auto& layout : module.classes) {
        hashString(hash, layout.name);
        for (const auto& parameter : layout.typeParameters) hashString(hash, parameter);
        hashString(hash, layout.parent);
        hashString(hash, layout.parentTypeName);
        for (const auto& interfaceName : layout.interfaces) hashString(hash, interfaceName);
        hashInteger(hash, layout.fields.size());
        for (const auto& field : layout.fields) {
            hashString(hash, field.name);
            hashInteger(hash, field.type);
            hashInteger(hash, static_cast<std::uint64_t>(field.ownership));
            hashInteger(hash, field.isStatic);
            hashInteger(hash, static_cast<std::uint64_t>(field.access));
        }
        hashInteger(hash, layout.isData);
        hashInteger(hash, layout.isEnum);
        for (const auto& member : layout.enumMembers) hashString(hash, member);
    }
    for (const auto& interface : module.interfaces) {
        hashString(hash, interface.name);
        for (const auto& base : interface.bases) hashString(hash, base);
        hashInteger(hash, interface.methods.size());
        for (const auto& method : interface.methods) {
            hashString(hash, method.name);
            hashInteger(hash, method.parameterTypes.size());
            for (const auto type : method.parameterTypes) hashInteger(hash, type);
            hashInteger(hash, method.returnType);
        }
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

std::string Result::mirDigest() const { return moduleDigest(module); }

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

Pipeline::Pipeline(Options options) : options_(std::move(options)) {
    result_.options = options_;
    result_.stages.reserve(kStageCount);
}

StageRecord& Pipeline::beginStage(Stage stage) {
    result_.stages.push_back(StageRecord{});
    StageRecord& record = result_.stages.back();
    record.stage = stage;
    record.ran = true;
    current_ = &record;
    stageStarted_ = Clock::now();
    return record;
}

void Pipeline::finishStage(StageRecord& record, bool ok, std::string detail, std::size_t count,
                           Clock::time_point started) {
    record.ok = ok;
    record.detail = std::move(detail);
    record.count = count;
    record.milliseconds = elapsedMilliseconds(started);
    if (current_ == &record) current_ = nullptr;
    if (options_.verbose) reportStage(record);
}

void Pipeline::reportStage(const StageRecord& record) const {
    std::fprintf(stderr, "pipeline: %s\n", record.describe().c_str());
}

bool Pipeline::fail(Stage stage, ErrorKind kind, std::string message, std::string detail) {
    // A stage that failed is still a stage that ran, so close its ledger line
    // here: the alternative is a `--pipeline-report` that shows the pipeline
    // stopping between two stages, with the failure itself in none of them.
    if (current_ != nullptr) {
        current_->ok = false;
        current_->detail = message;
        current_->milliseconds = elapsedMilliseconds(stageStarted_);
        if (options_.verbose) reportStage(*current_);
        current_ = nullptr;
    }
    result_.failure.kind = kind;
    result_.failure.message = std::move(message);
    result_.failure.detail = std::move(detail);
    result_.failure.stage = stage;
    return false;
}

bool Pipeline::expectStage(Stage stage) {
    const auto index = static_cast<std::size_t>(stage);
    if (index == nextStage_) return true;
    const std::string what = index < nextStage_
        ? std::string("stage '") + stageName(stage) + "' ran after '" + stageName(static_cast<Stage>(nextStage_ - 1)) + "'"
        : std::string("stage '") + stageName(stage) + "' ran before '" + stageName(static_cast<Stage>(nextStage_)) + "'";
    (void)fail(stage, ErrorKind::Invariant,
               "the compiler pipeline was driven out of order: " + what,
               std::string("stage invariant: ") + stageInvariant(stage));
    return false;
}

bool Pipeline::load(const std::filesystem::path& entry,
                    const std::vector<std::filesystem::path>& extraRoots) {
    if (!expectStage(Stage::Load)) return false;
    ++nextStage_;
    StageRecord& record = beginStage(Stage::Load);
    const auto started = Clock::now();
    result_.entry = entry;
    try {
        zl::ModuleLoader loader(entry, extraRoots);
        result_.program = loader.load();
    } catch (const zl::ParseError& error) {
        return fail(Stage::Load, ErrorKind::Syntax, error.what());
    } catch (const zl::ModuleError& error) {
        return fail(Stage::Load, ErrorKind::Module, error.what());
    } catch (const std::exception& error) {
        // The lexer throws std::runtime_error rather than a typed error; it is
        // still a front-end rejection, and misreporting it as a compiler
        // invariant would credit the compiler with a bug it does not have.
        return fail(Stage::Load, ErrorKind::Environment, error.what());
    }

    // Postcondition: a merged program with at least one declaration. An empty
    // program cannot be the output of a successful load, so seeing one means
    // the loader returned something it should not have.
    if (!result_.program || result_.program->declarations.empty()) {
        return fail(Stage::Load, ErrorKind::Invariant,
                    "load produced no declarations",
                    std::string("stage invariant: ") + stageInvariant(Stage::Load));
    }
    std::size_t functions = 0;
    for (const auto& declaration : result_.program->declarations) {
        if (declaration->kind == NodeKind::ClassDecl) {
            const auto* cls = static_cast<const ClassDecl*>(declaration.get());
            functions += cls->members.size();
        } else if (declaration->kind == NodeKind::DataDecl) {
            const auto* data = static_cast<const DataDecl*>(declaration.get());
            functions += data->members.size();
        }
    }
    finishStage(record, true,
                plural(result_.program->declarations.size(), "declaration", "declarations") + ", " +
                    plural(functions, "member", "members"),
                result_.program->declarations.size(), started);
    return true;
}

bool Pipeline::analyze() {
    if (!expectStage(Stage::SemanticAnalysis)) return false;
    ++nextStage_;
    StageRecord& record = beginStage(Stage::SemanticAnalysis);
    const auto started = Clock::now();
    // The checker's recorded expression types are read by the lowering stage,
    // so this must be the same checker instance that saw this program.
    result_.checker = std::make_unique<zl::TypeChecker>();
    try {
        result_.checker->check(*result_.program, options_.requireMain);
    } catch (const zl::TypeCheckError& error) {
        return fail(Stage::SemanticAnalysis, ErrorKind::TypeCheck, error.what());
    }

    const std::size_t recorded = result_.checker->expressionTypes().size();
    finishStage(record, true,
                plural(recorded, "expression type", "expression types") + " recorded", recorded,
                started);
    return true;
}

bool Pipeline::lowerToMir() {
    if (!expectStage(Stage::TypedLowering)) return false;
    ++nextStage_;
    StageRecord& record = beginStage(Stage::TypedLowering);
    const auto started = Clock::now();
    zl::mir::LoweringResult lowered = zl::mir::lowerProgram(*result_.program, *result_.checker,
                                                            options_.lowering);
    result_.module = std::move(lowered.module);
    result_.loweringDiagnostics = std::move(lowered.diagnostics);
    result_.incompleteFunctions = std::move(lowered.incompleteFunctions);

    // Postcondition: lowering may report incomplete *functions*, but it must
    // not report failure - it never throws for a program the type checker
    // accepted, and a module that failed to lower is not a module.
    if (!lowered.success) {
        return fail(Stage::TypedLowering, ErrorKind::Unsupported,
                    "typed lowering did not complete",
                    joinCapped(result_.loweringDiagnostics));
    }

    std::size_t blocks = 0;
    for (const auto& function : result_.module.functions) blocks += function.blocks.size();
    std::string detail = plural(result_.module.functions.size(), "function", "functions") + ", " +
                         plural(blocks, "block", "blocks");

    // The MIR form. Promotion is a rewrite of stage 3's output by stage 3, so
    // that stage 4 verifies exactly the form the backend will receive; both
    // forms mean the same thing, and every backend accepts either.
    if (options_.promoteSlots) {
        result_.promotion.ran = true;
        for (auto& function : result_.module.functions) {
            const zl::mir::SsaPromotionReport promotion =
                zl::mir::promoteSlotsToBlockParameters(function);
            result_.promotion.promotedSlots += promotion.promotedSlots;
            result_.promotion.blockParameters += promotion.parametersAdded;
            result_.promotion.loadsRemoved += promotion.loadsRemoved;
            result_.promotion.storesRemoved += promotion.storesRemoved;
            if (promotion.changed) ++result_.promotion.changedFunctions;
            if (options_.reportPromotionSkips) {
                for (const auto& reason : promotion.skipped) {
                    result_.promotion.notes.push_back(function.name + ": " + reason);
                }
            }
        }
        detail += ", ssa form: " +
                  plural(result_.promotion.promotedSlots, "slot promoted", "slots promoted") +
                  " into " + plural(result_.promotion.blockParameters, "block parameter",
                                    "block parameters");
    }

    if (!result_.incompleteFunctions.empty()) {
        detail += ", " + plural(result_.incompleteFunctions.size(), "incomplete function",
                                "incomplete functions");
    }
    finishStage(record, true, std::move(detail), result_.module.functions.size(), started);
    return true;
}

bool Pipeline::verifyMir() {
    if (!expectStage(Stage::MirVerification)) return false;
    ++nextStage_;
    StageRecord& record = beginStage(Stage::MirVerification);

    if (!options_.verify) {
        // Recorded, not silent: a disabled stage is still visible, and stage 6
        // refuses to run without a verification unless it performs its own.
        record.ran = false;
        finishStage(record, true, "disabled by options (code generation will re-verify)", 0, {});
        return true;
    }

    const auto started = Clock::now();
    result_.verification = zl::mir::verifyModule(result_.module, options_.verification);
    if (!result_.verification.ok()) {
        return fail(Stage::MirVerification, ErrorKind::Verification,
                    plural(result_.verification.errorCount(), "MIR contract violation",
                           "MIR contract violations") + " - a backend must not consume this module",
                    result_.verification.describe());
    }
    verified_ = true;
    const std::size_t warnings = result_.verification.warningCount();
    std::string detail = "0 errors";
    if (warnings != 0) detail += ", " + plural(warnings, "warning", "warnings");
    finishStage(record, true, std::move(detail), warnings, started);
    return true;
}

bool Pipeline::optimizeMir() {
    if (!expectStage(Stage::MirOptimization)) return false;
    ++nextStage_;
    StageRecord& record = beginStage(Stage::MirOptimization);

    if (!options_.optimize) {
        record.ran = false;
        finishStage(record, true, "skipped (opt-in: options.optimize / ZL_MIR_OPT=1)");
        return true;
    }
    // The optimiser's input must be MIR the verifier accepted; otherwise the
    // stage would be measuring the lowerer instead of itself.
    if (!verified_ && options_.verify) {
        return fail(Stage::MirOptimization, ErrorKind::Invariant,
                    "optimisation ran on a module that was not verified",
                    std::string("stage invariant: ") + stageInvariant(Stage::MirVerification));
    }

    const auto started = Clock::now();
    std::string error;
    const std::string spec = options_.optimizerPipeline.empty() ? "default" : options_.optimizerPipeline;
    zl::mir::PassManager manager = zl::mir::PassManager::namedPipeline(spec, error);
    if (!error.empty()) {
        std::string message = error;
        for (const auto& name : zl::mir::PassRegistry::instance().passNames()) {
            message += "\n  available pass: " + name;
        }
        return fail(Stage::MirOptimization, ErrorKind::Usage, message);
    }
    result_.optimizationPipeline = spec;

    const zl::mir::Module unoptimized = options_.checkOptimization ? result_.module : zl::mir::Module{};
    const zl::mir::OptimizationReport report = manager.run(result_.module, options_.optimization);
    result_.optimizationPasses = report.runs.size();
    if (options_.traceOptimization) std::fprintf(stderr, "%s\n", report.describeTrace().c_str());

    if (report.rollbacks != 0) {
        return fail(Stage::MirOptimization, ErrorKind::Optimization,
                    plural(report.rollbacks, "pass produced invalid MIR and was rolled back",
                           "passes produced invalid MIR and were rolled back"),
                    report.describe());
    }
    if (report.verifiedAtEnd && !report.finalVerification.ok()) {
        return fail(Stage::MirOptimization, ErrorKind::Optimization,
                    "the optimised module does not verify", report.finalVerification.describe());
    }
    // Optimisation is invisible by contract, so when asked to check it, check
    // it: the pre/post pair must observe the same events.
    if (options_.checkOptimization) {
        const auto comparison = zl::mir::compareModules(
            unoptimized, result_.module,
            zl::mir::DifferentialOptions{}.withReferencePipeline(spec));
        if (!comparison.equivalent) {
            std::string detail = comparison.describe();
            for (const auto& mismatch : comparison.mismatches) {
                detail += "\n  " + mismatch.describe();
            }
            return fail(Stage::MirOptimization, ErrorKind::Optimization,
                        "the optimised module is not observationally equivalent", std::move(detail));
        }
    }
    verified_ = report.verifiedAtEnd && report.trustworthy();

    std::string detail = "pipeline '" + spec + "', " +
                         plural(report.runs.size(), "pass run", "pass runs") + ", " +
                         plural(report.instructionsRemoved(), "instruction removed",
                                "instructions removed");
    if (report.changed()) detail += ", changed";
    else detail += ", no change";
    finishStage(record, true, std::move(detail), report.runs.size(), started);
    return true;
}

bool Pipeline::generate() {
    if (!expectStage(Stage::CodeGeneration)) return false;
    ++nextStage_;
    StageRecord& record = beginStage(Stage::CodeGeneration);
    const auto started = Clock::now();

    // Invariant: no backend ever consumes unverified MIR. This is enforced at
    // the code-generation boundary itself, whatever the other stages did: either
    // stage 4 verified this exact module, or this stage verifies it now. A
    // module mutated between the two - by a pass, by a future stage, by a bug -
    // is caught here rather than by a backend noticing something odd later.
    if (options_.verifyBeforeCodegen || !verified_) {
        // Both checks off is an explicit decision not to verify the module at
        // all. Refuse instead of quietly verifying something the caller turned
        // off: an unrequested verification would make `verify=false` a lie, and
        // a backend consuming unverified MIR is the thing this pipeline exists
        // to prevent.
        if (!options_.verifyBeforeCodegen && !verified_) {
            return fail(Stage::CodeGeneration, ErrorKind::Invariant,
                        "code generation refused: the module was never verified",
                        std::string("stage invariant: ") + stageInvariant(Stage::MirVerification));
        }
        const auto verification = zl::mir::verifyModule(result_.module, options_.verification);
        if (!verification.ok()) {
            return fail(Stage::CodeGeneration, ErrorKind::Verification,
                        "the module does not verify immediately before code generation",
                        verification.describe());
        }
        result_.verification = verification;
        verified_ = true;
    }

    std::string detail;
    switch (options_.backend) {
        case Backend::Bytecode: {
            zl::mir::BytecodeResult backend = zl::mir::compileModuleToBytecode(result_.module);
            if (!backend.ok()) {
                return fail(Stage::CodeGeneration, ErrorKind::Backend,
                            std::string("the ") + backendName(options_.backend) +
                                " backend could not translate the module",
                            joinCapped(backend.errors, "\n", 20));
            }
            result_.stubbedFunctions = backend.stubbedFunctions;
            result_.stubbedReasons = backend.stubbedReasons;
            result_.chunk = std::move(backend.chunk);
            detail = std::string("backend ") + backendName(options_.backend) + ": " +
                     plural(result_.module.functions.size() - backend.stubbed, "function",
                            "functions") +
                     " translated";
            if (backend.stubbed != 0) {
                detail += ", " + plural(backend.stubbed, "function stubbed (fail-closed)",
                                        "functions stubbed (fail-closed)");
            }
            break;
        }
        case Backend::Native: {
            const auto& target = zl::native::hostTarget();
            zl::native::PipelineResult native =
                zl::native::compileMirToNative(result_.module, target, options_.native);
            if (!native.ok()) {
                return fail(Stage::CodeGeneration, ErrorKind::Backend,
                            "the native backend refused the module", native.describe());
            }
            result_.nativeTargetTriple = target.triple;
            const std::size_t nativeCount = native.nativeFunctions.size();
            const std::size_t vmCount = native.vmFunctions.size();
            if (options_.strictNative && vmCount != 0) {
                // Strict mode is all-or-nothing, so the useful detail is the
                // refusals themselves - not the whole per-function ledger.
                std::string detail = "the native tier's subset is deliberately partial; these "
                                     "functions were left to the VM:";
                for (std::size_t i = 0; i < native.vmFunctions.size() && i < 8; ++i) {
                    detail += "\n  " + native.vmFunctions[i].function + " - " +
                              native.vmFunctions[i].reason;
                }
                if (vmCount > 8) {
                    detail += "\n  (+" + std::to_string(vmCount - 8) +
                              " more; --emit-native-ir prints the whole ledger)";
                }
                return fail(Stage::CodeGeneration, ErrorKind::Backend,
                            "strict native mode: " + plural(vmCount, "function", "functions") +
                                " of " + std::to_string(nativeCount + vmCount) +
                                " could not be compiled natively",
                            std::move(detail));
            }
            detail = std::string("backend ") + backendName(options_.backend) + ": " +
                     plural(nativeCount, "function", "functions") + " native, " +
                     plural(vmCount, "function", "functions") + " left to the VM";
            result_.native = std::move(native);
            break;
        }
    }

    // The execution driver. The VM is the only one in this phase, and it runs
    // the bytecode translation of the *same* module, whatever backend was
    // selected - which is what makes the backend choice invisible to the
    // language. See `Execution` in the header.
    if (options_.execution == Execution::Vm && !result_.chunk) {
        zl::mir::BytecodeResult driver = zl::mir::compileModuleToBytecode(result_.module);
        if (!driver.ok()) {
            return fail(Stage::CodeGeneration, ErrorKind::Backend,
                        "the execution driver (bytecode for the VM) could not translate the module",
                        joinCapped(driver.errors, "\n", 20));
        }
        result_.chunk = std::move(driver.chunk);
        result_.stubbedFunctions = driver.stubbedFunctions;
        result_.stubbedReasons = driver.stubbedReasons;
        detail += "; execution driver: VM (bytecode translation of the same MIR)";
    }

    finishStage(record, true, std::move(detail), result_.module.functions.size(), started);
    return true;
}

bool Pipeline::skipCodeGeneration() {
    if (!expectStage(Stage::CodeGeneration)) return false;
    ++nextStage_;
    StageRecord& record = beginStage(Stage::CodeGeneration);
    record.ran = false;
    finishStage(record, true, "skipped: this command stops at MIR (options.codeGeneration=false)");
    return true;
}

bool Pipeline::run(const std::filesystem::path& entry,
                   const std::vector<std::filesystem::path>& extraRoots) {
    if (!load(entry, extraRoots) || !analyze() || !lowerToMir() || !verifyMir() || !optimizeMir()) {
        return false;
    }
    return options_.codeGeneration ? generate() : skipCodeGeneration();
}

Result compile(const std::filesystem::path& entry,
               const std::vector<std::filesystem::path>& extraRoots, Options options) {
    Pipeline pipeline(std::move(options));
    (void)pipeline.run(entry, extraRoots);
    return std::move(pipeline.result());
}

} // namespace zl::pipeline
