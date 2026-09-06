#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include <memory>
#include <unordered_set>

#include "value.hpp"

namespace zl {

// Single allocation gateway for GC-managed runtime boxes. Every runtime
// allocation of a ListBox, MapBox, ObjectBox, or ClosureBox should enter
// through one of these helpers so registration cannot be accidentally omitted.
[[nodiscard]] ListRef makeGCList();
[[nodiscard]] MapRef makeGCMap();
[[nodiscard]] ObjectRef makeGCObject();
[[nodiscard]] ClosureRef makeGCClosure();


// Tracing GC boundary for ZL's hybrid memory model.
//
// Runtime GC allocation is centralized through the makeGC* gateway helpers.
// The collector holds one strong registry reference for each managed box,
// discovers roots explicitly, marks the managed graph, and performs the
// current tracing sweep. Physical storage is collector-owned and released from
// the registry when an allocation is unreachable.
class TracingGC {
public:
    struct Stats {
        std::size_t tracked{0};
        std::size_t reachable{0};
        std::size_t unreachable{0};
        std::size_t reclaimed{0};
        std::size_t deferred{0};
    };

    static TracingGC& instance();

    void track(const ListRef& value);
    void track(const MapRef& value);
    void track(const ObjectRef& value);
    void track(const ClosureRef& value);
    void track(const Value& value);

    [[nodiscard]] Stats collect(const std::vector<Value>& roots);
    [[nodiscard]] std::size_t trackedCount() const;
    [[nodiscard]] bool shouldCollect() const;

    // Protect an externally-managed runtime object (such as a raw Thread
    // closure) from reclamation while code outside ExecutionState is using it.
    void protect(const void* identity);
    void unprotect(const void* identity);

private:
    friend ListRef makeGCList();
    friend MapRef makeGCMap();
    friend ObjectRef makeGCObject();
    friend ClosureRef makeGCClosure();

    struct Entry {
        enum class Kind : std::uint8_t { List, Map, Object, Closure };
        Kind kind;
        std::unique_ptr<ListBox> list;
        std::unique_ptr<MapBox> map;
        std::unique_ptr<ObjectBox> object;
        std::unique_ptr<ClosureBox> closure;
    };

    mutable std::mutex mutex_;
    mutable std::mutex collectMutex_;
    std::vector<Entry> entries_;
    std::unordered_set<const void*> protectedRoots_;
    std::size_t allocationsSinceCollection_{0};
    std::size_t allocationThreshold_{128};
};

// Explicit root collection for a VM execution state. The VM calls this at
// safe points; callers must provide additional roots owned outside the
// ExecutionState (for example suspended async invocation arguments/results).
void appendGCRoots(const std::vector<Value>& values, std::vector<Value>& roots);

} // namespace zl
