#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include <memory>
#include <unordered_map>

#include "gc_roots.hpp"

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
    struct Entry {
        enum class Kind : std::uint8_t { List, Map, Object, Closure };
        Kind kind;
        std::unique_ptr<ListBox> list;
        std::unique_ptr<MapBox> map;
        std::unique_ptr<ObjectBox> object;
        std::unique_ptr<ClosureBox> closure;
    };

public:
    struct Stats {
        std::size_t tracked{0};
        std::size_t reachable{0};
        std::size_t unreachable{0};
        std::size_t reclaimed{0};
        std::size_t deferred{0};
    };

    // Trace/sweep only detaches garbage. Keep this result until mutators have
    // resumed, then reclaim: C++ ownership inside an unreachable box can join
    // threads, unregister VMs, or destroy a program's static storage.
    class Collection {
    public:
        Collection() = default;
        Collection(Collection&&) noexcept = default;
        Collection& operator=(Collection&&) noexcept = default;
        Collection(const Collection&) = delete;
        Collection& operator=(const Collection&) = delete;
        Stats stats;
        void reclaim() noexcept { retired_.clear(); }
    private:
        friend class TracingGC;
        std::vector<Entry> retired_;
    };

    static TracingGC& instance();

    void track(const ListRef& value);
    void track(const MapRef& value);
    void track(const ObjectRef& value);
    void track(const ClosureRef& value);
    void track(const Value& value);

    [[nodiscard]] Collection collect(const GCRoots& roots);
    [[nodiscard]] std::size_t trackedCount() const;
    [[nodiscard]] bool shouldCollect() const;

    // Protect an externally-managed runtime object (such as a raw Thread
    // closure) from reclamation while code outside ExecutionState is using it.
    // Protections are counted: several workers may retain the same closure.
    void protect(const void* identity);
    void unprotect(const void* identity);

private:
    friend ListRef makeGCList();
    friend MapRef makeGCMap();
    friend ObjectRef makeGCObject();
    friend ClosureRef makeGCClosure();


    mutable std::mutex mutex_;
    mutable std::mutex collectMutex_;
    std::vector<Entry> entries_;
    std::unordered_map<const void*, std::size_t> protectedRoots_;
    std::size_t allocationsSinceCollection_{0};
    std::size_t allocationThreshold_{128};
};

// Retain a root across a queued/running native callback, including enqueue or
// thread-start failure. Share this token when the callback must be copyable.
class ProtectedGCRoot {
public:
    explicit ProtectedGCRoot(const void* identity) : identity_(identity) {
        TracingGC::instance().protect(identity_);
    }
    ~ProtectedGCRoot() { TracingGC::instance().unprotect(identity_); }
    ProtectedGCRoot(const ProtectedGCRoot&) = delete;
    ProtectedGCRoot& operator=(const ProtectedGCRoot&) = delete;
private:
    const void* identity_;
};

// Explicit root collection for a VM execution state. The VM calls this at
// safe points; callers must provide additional roots owned outside the
// ExecutionState (for example suspended async invocation arguments/results).
void appendGCRoots(const std::vector<Value>& values, std::vector<Value>& roots);

} // namespace zl
