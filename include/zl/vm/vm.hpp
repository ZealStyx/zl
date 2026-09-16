#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <exception>

#include "zl/compiler/bytecode.hpp"
#include "execution_state.hpp"
#include "gc_roots.hpp"
#include "runtime_scheduler.hpp"
#include "runtime_thread.hpp"

namespace zl {

class VM : public std::enable_shared_from_this<VM> {
public:
    explicit VM(std::shared_ptr<RuntimeScheduler> sharedScheduler = nullptr);
    ~VM();
    // Runs a chunk to completion (until Halt). Returns a process exit code.
    // Throws std::runtime_error on a runtime fault (undefined variable,
    // divide by zero, type mismatch, etc.) - main.cpp is expected to catch it.
    // Runs a chunk to completion (until Halt). `programArgs` becomes the
    // list<string> passed to main() if it declares a parameter (see
    // Compiler::compile / OpCode::PushProgramArgs).
    [[nodiscard]] int run(const Chunk& chunk, const std::vector<std::string>& programArgs = {});
    // Preferred entry point: same behavior, but the VM can hand closures and
    // async invocations a reference to this shared owner instead of deep-
    // copying the whole chunk on every MakeClosure / async call. The bare
    // reference overload keeps working (callers that keep the chunk alive
    // themselves), it just pays the old copy cost.
    [[nodiscard]] int run(std::shared_ptr<const Chunk> chunk, const std::vector<std::string>& programArgs = {});
    // Shared tail of both run() entry points; `owner` is the shared chunk
    // ownership when the caller had one.
    [[nodiscard]] int runImpl(const Chunk& chunk, const std::vector<std::string>& programArgs,
                              std::shared_ptr<const Chunk> owner);

    Value invokeReflectiveMethod(const Value& methodValue, const Value& receiver, const Value& argsList);
    Value invokeReflectiveConstructor(const Value& constructorValue, const Value& argsList);
    Value invokeReflectiveFunction(const Value& functionValue, const Value& argsList);
    Value invokeTaskClosure(const ClosureRef& closure);
    const Chunk* activeChunk() const noexcept { return activeChunk_; }
    // Ready async frames in this VM's scheduler. Channel blocking uses this
    // (plus the alive-worker count) to detect that nobody can ever unblock a
    // send/receive and raise a deadlock error instead of hanging forever.
    [[nodiscard]] std::size_t schedulerPendingCount() const noexcept { return scheduler_ ? scheduler_->pendingCount() : 0; }
    // Runs a single ready async frame on this VM's scheduler, if any.
    // Blocking channel operations pump this while waiting (releasing the
    // channel lock first) so an async sender/receiver queued behind the very
    // call that is about to block still runs instead of hanging the program.
    // Always own-scheduler: g_currentNativeVm is thread-local, so the pump
    // runs on the same thread that owns the scheduler.
    [[nodiscard]] bool pumpSchedulerOne() { return scheduler_ ? scheduler_->runOne() : false; }

    // Only the wait itself belongs in this scope: callbacks and managed-data
    // access must happen after reactivation, even if the native acquired a lock.
    class BlockingNativeCall {
    public:
        explicit BlockingNativeCall(VM* vm) : vm_(vm) {
            if (vm_) vm_->beginBlockingNativeCall();
        }
        ~BlockingNativeCall() { if (vm_) vm_->endBlockingNativeCall(); }
        BlockingNativeCall(const BlockingNativeCall&) = delete;
        BlockingNativeCall& operator=(const BlockingNativeCall&) = delete;
    private:
        VM* vm_;
    };


private:
    class ProgramScope {
    public:
        // `owner` is the shared ownership of `chunk`, when the caller has it.
        // A null owner inherits the enclosing scope's owner when both scopes
        // are for the same chunk object (run() -> execute() nests exactly so),
        // and is null otherwise - shareActiveChunk() then falls back to
        // copying, which is the pre-sharing behavior.
        ProgramScope(VM& vm, const Chunk& chunk, std::shared_ptr<const Chunk> owner = {});
        ~ProgramScope();
        ProgramScope(const ProgramScope&) = delete;
        ProgramScope& operator=(const ProgramScope&) = delete;
    private:
        VM& vm_;
        const Chunk* previous_;
        DeferredThreadJoins* previousJoins_;
    };
    enum class ExecuteStatus { Completed, Suspended };
    [[nodiscard]] ExecuteStatus execute(const Chunk& chunk, std::size_t startIp, bool stopAtReturn,
                              const std::vector<std::string>& programArgs, Value* returnValue,
                              std::shared_ptr<const Chunk> owner = {});
    // Handler search shared by thrown ZL exceptions and converted runtime
    // faults. Returns false when this run owns no matching handler.
    bool dispatchThrownException(const Chunk& chunk, const ObjectRef& thrown,
                                 std::size_t initialCallDepth, std::size_t& ip);
    [[nodiscard]] Value invokeFunction(const Chunk& chunk, std::size_t functionIndex,
                                       const std::vector<Value>& args,
                                       const std::optional<Value>& receiver = std::nullopt,
                                       const ClosureRef& closure = {},
                                       std::shared_ptr<const Chunk> owner = {});
    // The chunk backing every closure/async invocation made from here: the
    // active scope's shared owner when there is one, a fresh copy otherwise.
    [[nodiscard]] std::shared_ptr<const Chunk> shareActiveChunk() const;
    ExecutionState::CallFrame makeCallFrame(const Chunk& chunk, const FunctionInfo& fn,
                                           const std::vector<Value>& args,
                                           const std::optional<Value>& receiver = std::nullopt,
                                           const ClosureRef& closure = {},
                                           RuntimeTypeBindings extraBindings = {}) const;
    TaskRef scheduleAsyncInvocation(std::shared_ptr<const Chunk> chunk, std::size_t functionIndex,
                                    ExecutionState::CallFrame frame);
    void resumeAsyncInvocation();
    void pushNativeRoots(const std::vector<Value>& roots);
    void popNativeRoots();
    void appendNativeRoots(std::vector<Value>& roots) const;
    void drainThreadJoins(bool bounded = false);
    GCRoots gcRoots() const;
    void beginBlockingNativeCall();
    void endBlockingNativeCall() const;

    Value binaryArith(OpCode op, const Value& a, const Value& b) const;
    Value binaryBitwise(OpCode op, const Value& a, const Value& b) const;
    Value binaryCompare(OpCode op, const Value& a, const Value& b) const;
    Value binaryLogical(OpCode op, const Value& a, const Value& b) const;
    Value unary(OpCode op, const Value& a) const;
    bool rangeContinue(const Value& current, const Value& end, const Value& step) const;
    // Cooperatively pump ready async frames until `task` settles. A native
    // async op may complete on a worker thread and enqueue our continuation
    // later, so after draining what is ready now we briefly sleep and retry
    // rather than returning while owning the scheduler that must resume the
    // task. Used by both Task.block() and the top-level async entry point.
    void pumpSchedulerUntilTerminal(const TaskRef& task);

    ExecutionState state_;
    // Shared ownership is deliberate. A child VM created for an async
    // invocation can outlive the root VM - the task continuation holds it - so
    // a raw pointer to the root's scheduler would dangle and a worker thread
    // would then enqueue into freed memory.
    std::shared_ptr<RuntimeScheduler> ownedScheduler_;
    std::shared_ptr<RuntimeScheduler> scheduler_;

    struct AsyncInvocation {
        std::shared_ptr<const Chunk> chunk;
        std::size_t functionIndex{0};
        ExecutionState::CallFrame frame;
        TaskRef task;
        std::size_t resumeIp{0};
        bool started{false};
        TaskRef awaitedTask;
    };
    std::optional<AsyncInvocation> asyncInvocation_;
    // When `main` itself is an async func, the top-level call yields a Task that
    // nothing holds. Remember it so VM::run can wait for it instead of tearing
    // the VM down while a worker is still going to resume it.
    TaskRef entryTask_;
    std::exception_ptr pendingResumeException_;
    std::vector<std::vector<Value>> nativeRootFrames_;
    const Chunk* activeChunk_{nullptr};
    std::vector<const Chunk*> activePrograms_;
    // Parallel to activePrograms_: each scope's shared chunk owner (may be
    // null). Keeping it as a shared_ptr here is what pins the chunk for as
    // long as any scope - or any closure handed out from under it - lives.
    std::vector<std::shared_ptr<const Chunk>> activeChunkOwners_;
    DeferredThreadJoins threadJoins_;
    std::uint64_t gcParticipantId_{0};
    // Nested execute() activations on this VM (native->ZL callbacks such as
    // Mutex.withLock, reflective invocation, async resumption while blocked).
    // ZL call frames live on the heap and are capped separately at 100000,
    // but each nested execute()activation also consumes C++ stack, which
    // overflows far earlier - so re-entry gets its own, much lower budget.
    // 1000 activations stay comfortably inside an 8MB stack while no sane
    // program nests anywhere near that many blocking/callback levels.
    std::size_t nestedExecuteDepth_{0};
    static constexpr std::size_t kMaxNestedExecuteDepth = 1000;

};

} // namespace zl
