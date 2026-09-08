#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <exception>

#include "zl/compiler/bytecode.hpp"
#include "execution_state.hpp"
#include "runtime_scheduler.hpp"

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

    Value invokeReflectiveMethod(const Value& methodValue, const Value& receiver, const Value& argsList);
    Value invokeReflectiveConstructor(const Value& constructorValue, const Value& argsList);
    Value invokeReflectiveFunction(const Value& functionValue, const Value& argsList);
    void invokeThreadClosure(const ClosureRef& closure);
    Value invokeTaskClosure(const ClosureRef& closure);

    // Mark this VM thread as blocked inside a native call running no bytecode
    // (Thread.join, a condition wait). While blocked it counts as at a GC
    // safepoint so a rendezvous triggered by a worker does not deadlock
    // waiting on this participant; unblock when the native call returns.
    void beginBlockingNativeCall();
    void endBlockingNativeCall() const;


private:
    enum class ExecuteStatus { Completed, Suspended };
    [[nodiscard]] ExecuteStatus execute(const Chunk& chunk, std::size_t startIp, bool stopAtReturn,
                              const std::vector<std::string>& programArgs, Value* returnValue);
    [[nodiscard]] Value invokeFunction(const Chunk& chunk, std::size_t functionIndex,
                                       const std::vector<Value>& args,
                                       const std::optional<Value>& receiver = std::nullopt);
    void beginAsyncInvocation(std::shared_ptr<const Chunk> chunk, std::size_t functionIndex,
                              std::vector<Value> args, std::optional<Value> receiver, TaskRef task);
    void resumeAsyncInvocation();
    void pushNativeRoots(const std::vector<Value>& roots);
    void popNativeRoots();
    void appendNativeRoots(std::vector<Value>& roots) const;

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
        std::vector<Value> args;
        std::optional<Value> receiver;
        TaskRef task;
        std::size_t resumeIp{0};
        bool started{false};
    };
    std::optional<AsyncInvocation> asyncInvocation_;
    // When `main` itself is an async func, the top-level call yields a Task that
    // nothing holds. Remember it so VM::run can wait for it instead of tearing
    // the VM down while a worker is still going to resume it.
    TaskRef entryTask_;
    std::exception_ptr pendingResumeException_;
    std::vector<std::vector<Value>> nativeRootFrames_;
    const Chunk* activeChunk_{nullptr};
    std::uint64_t gcParticipantId_{0};
    // Per-thread nesting depth of execute(). A nested run driven from a native
    // (Mutex.withLock, reflection invoke) may hold an application lock; it must
    // not block on a GC safepoint (other participants are parked on that lock),
    // so nested runs publish roots non-blockingly while the outermost run
    // performs the rendezvous.
    int executeDepth_{0};
};

} // namespace zl
