// Garbage collector regressions: the allocation gateway, reachability from
// explicit roots (values, program graphs, tasks, in-flight exceptions),
// collection/reclamation accounting, external protections, and a repeated
// allocation stress. The collector is process-global, so every assertion
// measures deltas around a fresh collection rather than absolute counts.
#include "zl/compiler/bytecode.hpp"
#include "zl/vm/gc.hpp"
#include "zl/vm/gc_roots.hpp"
#include "zl/vm/runtime_task.hpp"
#include "zl/vm/value.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "GC regression: " << message << '\n';
    ++failures;
}

using zl::TracingGC;
using zl::GCRoots;

std::size_t baseline() {
    TracingGC::instance().collect({}).reclaim();
    return TracingGC::instance().trackedCount();
}

void testAllocationGateway() {
    const std::size_t start = baseline();
    auto list = zl::makeGCList();
    auto map = zl::makeGCMap();
    auto object = zl::makeGCObject();
    auto closure = zl::makeGCClosure();
    require(TracingGC::instance().trackedCount() == start + 4,
            "every gateway allocation is registered exactly once");
    require(list && map && object && closure, "gateway handles are valid");
    object->className = "Box";
    list->items.push_back(zl::Value(1));
    TracingGC::instance().collect(GCRoots{list, map, object, closure}).reclaim();
    require(TracingGC::instance().trackedCount() == start + 4, "rooted boxes survive a collection");
    // Drop the roots: everything becomes garbage in one sweep.
    TracingGC::instance().collect({}).reclaim();
    require(TracingGC::instance().trackedCount() == start, "unrooted boxes are reclaimed");
}

void testReachabilityThroughGraphs() {
    const std::size_t start = baseline();

    // A root list -> object -> map chain: only the root is named, the rest
    // must survive by reachability.
    auto leaf = zl::makeGCMap();
    auto middle = zl::makeGCObject();
    middle->className = "Middle";
    middle->fields["leaf"] = zl::Value(leaf);
    auto root = zl::makeGCList();
    root->items.push_back(zl::Value(middle));

    TracingGC::instance().collect(GCRoots{root}).reclaim();
    require(TracingGC::instance().trackedCount() == start + 3,
            "transitively reachable boxes survive without being named");

    // Unreachable siblings of the root are reclaimed in the same sweep.
    auto garbage = zl::makeGCList();
    (void)garbage;
    TracingGC::instance().collect(GCRoots{root}).reclaim();
    require(TracingGC::instance().trackedCount() == start + 3,
            "an unreachable sibling is reclaimed");
    TracingGC::instance().collect({}).reclaim();
}

void testCollectionAccounting() {
    const std::size_t start = baseline();
    std::vector<zl::Value> roots;
    for (int i = 0; i < 40; ++i) {
        auto box = zl::makeGCList();
        if (i % 2 == 0) roots.push_back(zl::Value(box));
    }
    auto collection = TracingGC::instance().collect(GCRoots{roots});
    require(collection.stats.tracked == start + 20, "post-collection count is the survivor set");
    require(collection.stats.reachable == start + 20, "reachable count matches the roots' closure");
    require(collection.stats.unreachable == 20, "every unrooted box is reported unreachable");
    require(collection.stats.reclaimed == 20, "detached boxes are counted for reclamation");
    collection.reclaim();
    require(TracingGC::instance().trackedCount() == start + 20, "reclaim() settles the registry");
    TracingGC::instance().collect({}).reclaim();
}

void testExternalProtection() {
    const std::size_t start = baseline();
    auto box = zl::makeGCObject();
    const void* identity = box.get();

    TracingGC::instance().protect(identity);
    TracingGC::instance().collect({}).reclaim();
    require(TracingGC::instance().trackedCount() == start + 1,
            "a protected identity survives without being rooted");

    // Protections are counted: one unprotect leaves one hold in place.
    TracingGC::instance().unprotect(identity);
    TracingGC::instance().protect(identity);
    TracingGC::instance().unprotect(identity);
    TracingGC::instance().unprotect(identity);
    TracingGC::instance().collect({}).reclaim();
    require(TracingGC::instance().trackedCount() == start,
            "the last unprotect releases the hold");

    // RAII form: a queued native callback holds its payload across a sweep.
    auto payload = zl::makeGCObject();
    const void* payloadId = payload.get();
    {
        zl::ProtectedGCRoot guard(payloadId);
        TracingGC::instance().collect({}).reclaim();
        require(TracingGC::instance().trackedCount() == start + 1,
                "ProtectedGCRoot keeps the payload live inside its scope");
    }
    TracingGC::instance().collect({}).reclaim();
    require(TracingGC::instance().trackedCount() == start,
            "leaving the scope drops the protection");
}

void testProgramGraphRoots() {
    const std::size_t start = baseline();

    // A program's constants are read at trace time: the constant object is
    // alive while the chunk is a root, even though no Value names it.
    auto constant = zl::makeGCObject();
    constant->className = "Const";
    zl::Chunk chunk;
    chunk.constants.push_back(zl::Value(constant));
    auto staticValue = zl::makeGCObject();
    staticValue->className = "Static";
    auto field = std::make_shared<zl::StaticFieldState>();
    field->value = zl::Value(staticValue);
    chunk.staticStorage->fields["seed"] = field;

    GCRoots programRoots;
    programRoots.programs.push_back(&chunk);
    TracingGC::instance().collect(programRoots).reclaim();
    require(TracingGC::instance().trackedCount() == start + 2,
            "program constants and statics are collected as roots");
    TracingGC::instance().collect({}).reclaim();
    require(TracingGC::instance().trackedCount() == start, "dropping the program drops its roots");
}

void testTaskAndExceptionRoots() {
    const std::size_t start = baseline();

    // A task pins its result and its exception payload.
    auto result = zl::makeGCObject();
    auto task = std::make_shared<zl::RuntimeTaskState>("");
    task->start();
    task->succeed(zl::Value(result));
    zl::Value taskValue = task;
    TracingGC::instance().collect(GCRoots{taskValue}).reclaim();
    // The task itself is not a GC box - only its result payload is tracked.
    require(TracingGC::instance().trackedCount() == start + 1,
            "a task's result stays reachable through the task");
    TracingGC::instance().collect({}).reclaim();
    require(TracingGC::instance().trackedCount() == start, "an unrooted task releases its result");

    // An in-flight ZL exception pins its object; the pin ends with the throw.
    auto payload = zl::makeGCObject();
    try {
        throw zl::ZlThrownException(payload);
    } catch (const zl::ZlThrownException& exception) {
        (void)TracingGC::instance().collect({}).reclaim();
        require(TracingGC::instance().trackedCount() == start + 1,
                "an in-flight exception keeps its payload alive");
    }
    TracingGC::instance().collect({}).reclaim();
    require(TracingGC::instance().trackedCount() == start,
            "the pin dies with the in-flight exception");
}

void testAllocationThreshold() {
    TracingGC::instance().collect({}).reclaim();
    require(!TracingGC::instance().shouldCollect(), "a fresh collector does not need a sweep");
    std::vector<zl::Value> roots;
    roots.reserve(128);
    for (int i = 0; i < 128; ++i) {
        roots.push_back(zl::Value(zl::makeGCList()));
    }
    require(TracingGC::instance().shouldCollect(), "allocation pressure trips the threshold");
    TracingGC::instance().collect(GCRoots{roots}).reclaim();
    require(!TracingGC::instance().shouldCollect(), "a collection resets the pressure counter");
    TracingGC::instance().collect({}).reclaim();
}

void testAllocationStress() {
    const std::size_t start = baseline();
    // Four generations of nested boxes; only the last root set is kept. The
    // collector must settle on exactly that set, not drift or leak.
    std::vector<zl::Value> roots;
    for (int generation = 0; generation < 4; ++generation) {
        std::vector<zl::Value> previous = std::move(roots);
        roots.clear();
        for (int i = 0; i < 512; ++i) {
            auto outer = zl::makeGCList();
            auto inner = zl::makeGCObject();
            inner->className = "Stress";
            outer->items.push_back(zl::Value(inner));
            if (generation == 3 && i % 4 == 0) roots.push_back(zl::Value(outer));
        }
        auto collection = TracingGC::instance().collect(GCRoots{roots});
        collection.reclaim();
        require(collection.stats.tracked == (generation == 3 ? start + 2 * roots.size() : start),
                "generation " + std::to_string(generation) + " reclaims its predecessors");
    }
    // And again, to prove the steady state is stable across consecutive runs.
    auto settled = TracingGC::instance().collect(GCRoots{roots});
    settled.reclaim();
    require(TracingGC::instance().trackedCount() == start + 2 * roots.size(),
            "steady state holds across consecutive collections");
    TracingGC::instance().collect({}).reclaim();
    require(TracingGC::instance().trackedCount() == start, "the stress set releases cleanly");
}

} // namespace

int main() {
    testAllocationGateway();
    testReachabilityThroughGraphs();
    testCollectionAccounting();
    testExternalProtection();
    testProgramGraphRoots();
    testTaskAndExceptionRoots();
    testAllocationThreshold();
    testAllocationStress();

    if (failures != 0) {
        std::cerr << failures << " GC regression(s) failed\n";
        return 1;
    }
    std::cout << "all GC regressions passed\n";
    return 0;
}
