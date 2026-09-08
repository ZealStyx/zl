// Focused program-root and post-safepoint retirement regression.
#include "zl/compiler/bytecode.hpp"
#include "zl/vm/gc.hpp"
#include "zl/vm/gc_safepoint.hpp"
#include "zl/vm/runtime_task.hpp"
#include "zl/vm/runtime_thread.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
using namespace zl;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "GC lifetime regression: " << message << '\n'; std::abort(); }
}

std::mutex mutex;
std::condition_variable cv;
bool finished = false;
std::atomic<const char*> stage{"program roots"};

void programGraph() {
    auto& gc = TracingGC::instance();
    gc.collect({}).reclaim();
    require(gc.trackedCount() == 0, "test begins with an unexpected live heap");
    {
        Chunk program;
        auto constant = makeGCObject();
        constant->className = "TypeLiteral";
        program.constants.emplace_back(constant);
        auto slot = std::make_shared<StaticFieldState>();
        auto first = makeGCList();
        first->items.emplace_back(std::int64_t{17});
        slot->value = first;
        program.staticStorage->fields["Example.items"] = slot;
        auto failure = std::make_shared<StaticFieldState>();
        auto exception = makeGCObject();
        exception->fields["message"] = std::string("cached initializer failure");
        failure->failure = StoredException(std::make_exception_ptr(ZlThrownException(exception)));
        program.staticStorage->fields["Example.bad"] = failure;
        GCRoots parked;
        parked.programs.push_back(&program);
        auto firstPass = gc.collect(parked);
        require(firstPass.stats.reachable == 3, "constants, static value and cached failure were not traced");
        firstPass.reclaim();

        // Reuse the exact parked root set. It must observe shared static state
        // at collection time, not retain a stale flattened list of values.
        auto latest = makeGCList();
        latest->items.emplace_back(std::int64_t{29});
        slot->value = latest;
        auto secondPass = gc.collect(parked);
        require(secondPass.stats.reachable == 3 && secondPass.stats.reclaimed == 1,
                "parked program root did not follow the latest static value");
        secondPass.reclaim();
        require(std::get<std::int64_t>(latest->items.front()) == 29, "latest static payload was reclaimed");

        // A reachable closure owns its program even with no active VM roots.
        auto closure = makeGCClosure();
        closure->chunk = std::make_shared<Chunk>(program);
        auto throughClosure = gc.collect({Value{closure}});
        require(throughClosure.stats.reachable == 4, "closure did not retain its program graph");
        throughClosure.reclaim();

        // A static closure pointing back to this same program must NOT form a
        // permanent protected-root cycle when all external roots disappear.
        auto callback = std::make_shared<StaticFieldState>();
        callback->value = closure;
        program.staticStorage->fields["Example.callback"] = callback;
        exception->fields["callback"] = closure; // failure -> closure -> program -> failure
    }
    auto deadProgram = gc.collect({});
    require(deadProgram.stats.reclaimed == 4 && deadProgram.stats.tracked == 0,
            "unreachable program/static/closure cycle stayed pinned");
    deadProgram.reclaim();
}

void pendingOperation() {
    auto& gc = TracingGC::instance();
    auto channel = makeGCObject();
    channel->channelState = std::make_shared<ObjectBox::ChannelState>();
    auto payload = makeGCList();
    payload->items.emplace_back(std::int64_t{42});
    auto pending = std::make_shared<RuntimeTaskState>("void", std::vector<Value>{channel});
    channel->channelState->pendingSends.push_back({payload, pending});
    auto live = gc.collect({Value{pending}});
    require(live.stats.reachable == 2, "pending native task lost its operation owner/payload");
    live.reclaim();
    pending->cancel();
    auto completed = gc.collect({Value{pending}});
    require(completed.stats.reclaimed == 2, "terminal native task kept its operation rooted");
    completed.reclaim();
}

void inFlightFailure() {
    auto& gc = TracingGC::instance();
    auto object = makeGCObject();
    object->fields["message"] = std::string("unwinding");
    try {
        throw ZlThrownException(object);
    } catch (const ZlThrownException& error) {
        auto collection = gc.collect({});
        require(collection.stats.reachable == 1, "in-flight exception was not a root");
        collection.reclaim();
        require(std::get<std::string>(error.value()->fields.at("message")) == "unwinding", "unwinding lost its payload");
    }
    auto released = gc.collect({});
    require(released.stats.reclaimed == 1, "in-flight exception pin outlived its exception");
    released.reclaim();
}

void taskDiagnostic() {
    auto& gc = TracingGC::instance();
    auto exception = makeGCObject(); // deliberately retired before its task owner
    exception->fields["message"] = std::string("original task failure");
    auto task = std::make_shared<RuntimeTaskState>("int");
    task->start();
    task->fail(std::make_exception_ptr(ZlThrownException(exception)));
    auto owner = makeGCList();
    owner->items.emplace_back(task);
    task.reset();
    std::ostringstream output;
    auto* saved = std::cerr.rdbuf(output.rdbuf());
    gc.collect({}).reclaim();
    std::cerr.rdbuf(saved);
    require(output.str() == "unobserved task failure: original task failure\n",
            "task destruction accessed a retired exception instead of its owned diagnostic");
}

void retirementRendezvous() {
    stage = "retirement must permit native reactivation and another collection";
    auto& gc = TracingGC::instance();
    auto& coordinator = GCSafepointCoordinator::instance();
    const auto collector = coordinator.registerParticipant();
    const auto worker = coordinator.registerParticipant();
    std::promise<void> parked;
    bool retireStarted = false;
    bool secondCollection = false;
    auto thread = std::shared_ptr<RuntimeThreadState>(new RuntimeThreadState(), [&](RuntimeThreadState* state) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            retireStarted = true;
        }
        cv.notify_all();
        delete state; // joins the parked worker: tracing must already be over
    });
    thread->startWith([&] {
        coordinator.beginBlockingNative(worker, {});
        parked.set_value();
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return retireStarted; });
        }
        coordinator.endBlockingNative(worker);
        for (int i = 0; i < 160; ++i) (void)makeGCList();
        coordinator.poll(worker, {});
        secondCollection = true;
        coordinator.unregisterParticipant(worker);
    });
    auto deadOwner = makeGCList();
    deadOwner->items.emplace_back(thread);
    thread.reset();
    parked.get_future().wait();
    for (int i = 0; i < 160; ++i) (void)makeGCList();
    coordinator.poll(collector, {});
    coordinator.unregisterParticipant(collector);
    require(secondCollection, "blocking reclamation prevented another mutator from collecting");
    require(gc.trackedCount() == 0, "retirement left garbage in the registry");
}
} // namespace

int main() {
    std::thread watchdog([] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, 15s, [] { return finished; })) {
            std::cerr << "GC lifetime transition stalled: " << stage.load() << '\n';
            std::abort();
        }
    });
    programGraph();
    stage = "task diagnostics after managed reclamation";
    taskDiagnostic();
    inFlightFailure();
    pendingOperation();
    retirementRendezvous();
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
    }
    cv.notify_all();
    watchdog.join();
    std::cout << "GC program lifetimes and retirement: PASS\n";
}
