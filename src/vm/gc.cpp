#include "zl/vm/gc.hpp"
#include "zl/vm/runtime_task.hpp"
#include "zl/compiler/bytecode.hpp"

#include <unordered_set>
#include <utility>
#include <type_traits>
#include <algorithm>
#include <limits>

namespace zl {
namespace {

void appendValueChildren(const Value& value, std::vector<Value>& work, std::vector<const Chunk*>& programs) {
    std::visit([&](const auto& held) {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, ListRef>) {
            if (!held) return;
            for (const auto& item : held->items) work.push_back(item);
        } else if constexpr (std::is_same_v<T, MapRef>) {
            if (!held) return;
            for (const auto& entry : held->entries) {
                work.push_back(entry.first);
                work.push_back(entry.second);
            }
        } else if constexpr (std::is_same_v<T, ObjectRef>) {
            if (!held) return;
            for (const auto& field : held->fields) work.push_back(field.second);
            if (held->atomicRefState) work.push_back(held->atomicRefState->value);
            if (held->channelState) {
                for (const auto& item : held->channelState->items) work.push_back(item);
                for (const auto& pending : held->channelState->pendingSends) {
                    work.push_back(pending.value);
                    if (pending.task) work.emplace_back(pending.task);
                }
                for (const auto& pending : held->channelState->pendingReceives)
                    if (pending.task) work.emplace_back(pending.task);
            }
        } else if constexpr (std::is_same_v<T, ClosureRef>) {
            if (!held) return;
            for (const auto& capture : held->captured) work.push_back(capture.second);
            if (held->chunk) programs.push_back(held->chunk.get());
        } else if constexpr (std::is_same_v<T, TaskRef>) {
            if (!held) return;
            held->appendGCRoots(work);
        }
    }, value);
}

const void* identity(const Value& value) {
    return std::visit([](const auto& held) -> const void* {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, ListRef> ||
                      std::is_same_v<T, MapRef> ||
                      std::is_same_v<T, ObjectRef> ||
                      std::is_same_v<T, ClosureRef> ||
                      std::is_same_v<T, TaskRef>) {
            return held.get();
        } else {
            return nullptr;
        }
    }, value);
}

class Marker {
public:
    std::unordered_set<const void*> marked;

    void trace(const GCRoots& roots) {
        auto work = roots.values;
        auto programs = roots.programs;
        while (!work.empty() || !programs.empty()) {
            if (!work.empty()) {
                Value value = std::move(work.back());
                work.pop_back();
                const auto* id = identity(value);
                if (id && marked.insert(id).second) appendValueChildren(value, work, programs);
                continue;
            }
            const auto* program = programs.back();
            programs.pop_back();
            if (!program || !programs_.insert(program).second) continue;
            work.insert(work.end(), program->constants.begin(), program->constants.end());
            const auto& storage = program->staticStorage;
            if (!storage || !statics_.insert(storage.get()).second) continue;
            // Every managed writer is stopped. Do not acquire initialization
            // locks: a parked native waiter may already have reacquired one
            // and be waiting for this trace to finish before it can resume.
            for (const auto& entry : storage->fields) {
                if (!entry.second) continue;
                work.push_back(entry.second->value);
                entry.second->failure.appendGCRoots(work);
            }
        }
    }
private:
    std::unordered_set<const Chunk*> programs_;
    std::unordered_set<const StaticRuntimeStorage*> statics_;
};

} // namespace

void appendExceptionRoots(const std::exception_ptr& error, std::vector<Value>& roots) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const ZlThrownException& exception) {
        if (exception.value()) roots.emplace_back(exception.value());
    } catch (...) {
        // Native exception objects do not contain managed ZL payloads.
    }
}

ListRef makeGCList() {
    auto& gc = TracingGC::instance();
    std::lock_guard<std::mutex> lock(gc.mutex_);
    auto box = std::make_unique<ListBox>();
    ListRef ref(box.get());
    gc.entries_.push_back(TracingGC::Entry{TracingGC::Entry::Kind::List, std::move(box), {}, {}, {}});
    ++gc.allocationsSinceCollection_;
    return ref;
}

MapRef makeGCMap() {
    auto& gc = TracingGC::instance();
    std::lock_guard<std::mutex> lock(gc.mutex_);
    auto box = std::make_unique<MapBox>();
    MapRef ref(box.get());
    gc.entries_.push_back(TracingGC::Entry{TracingGC::Entry::Kind::Map, {}, std::move(box), {}, {}});
    ++gc.allocationsSinceCollection_;
    return ref;
}

ObjectRef makeGCObject() {
    auto& gc = TracingGC::instance();
    std::lock_guard<std::mutex> lock(gc.mutex_);
    auto box = std::make_unique<ObjectBox>();
    ObjectRef ref(box.get());
    gc.entries_.push_back(TracingGC::Entry{TracingGC::Entry::Kind::Object, {}, {}, std::move(box), {}});
    ++gc.allocationsSinceCollection_;
    return ref;
}

ClosureRef makeGCClosure() {
    auto& gc = TracingGC::instance();
    std::lock_guard<std::mutex> lock(gc.mutex_);
    auto box = std::make_unique<ClosureBox>();
    ClosureRef ref(box.get());
    gc.entries_.push_back(TracingGC::Entry{TracingGC::Entry::Kind::Closure, {}, {}, {}, std::move(box)});
    ++gc.allocationsSinceCollection_;
    return ref;
}

TracingGC& TracingGC::instance() {
    static TracingGC gc;
    return gc;
}

// These registration methods remain as a compatibility boundary for embedders.
// Managed runtime allocation must use makeGC* so the collector retains physical
// ownership. A handle already owned by this collector is simply observed here.
void TracingGC::track(const ListRef& value) {
    if (!value) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) if (entry.kind == Entry::Kind::List && entry.list.get() == value.get()) return;
}

void TracingGC::track(const MapRef& value) {
    if (!value) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) if (entry.kind == Entry::Kind::Map && entry.map.get() == value.get()) return;
}

void TracingGC::track(const ObjectRef& value) {
    if (!value) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) if (entry.kind == Entry::Kind::Object && entry.object.get() == value.get()) return;
}

void TracingGC::track(const ClosureRef& value) {
    if (!value) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) if (entry.kind == Entry::Kind::Closure && entry.closure.get() == value.get()) return;
}

void TracingGC::track(const Value& value) {
    std::visit([this](const auto& held) {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, ListRef>) track(held);
        else if constexpr (std::is_same_v<T, MapRef>) track(held);
        else if constexpr (std::is_same_v<T, ObjectRef>) track(held);
        else if constexpr (std::is_same_v<T, ClosureRef>) track(held);
        else if constexpr (std::is_same_v<T, NativeBufferView> || std::is_same_v<T, NativeStructView> || std::is_same_v<T, NativeHandleRef> || std::is_same_v<T, NativeCallbackRef>) { /* external resources are not GC objects */ }
    }, value);
}

TracingGC::Collection TracingGC::collect(const GCRoots& roots) {
    // The coordinator must stop all mutators before entering this method.
    // Serializing collectors alone does not make tracing concurrent-safe.
    std::lock_guard<std::mutex> collectLock(collectMutex_);

    std::unordered_set<const void*> protectedIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : protectedRoots_) protectedIds.insert(entry.first);
    }

    Marker marker;
    marker.trace(roots);

    // Convert protected identities into temporary non-owning Value roots while
    // the registry is stable. Marking itself happens outside the registry lock
    // to avoid lock-order inversions with runtime state such as Tasks.
    std::vector<Value> protectedValues;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const void* protectedId : protectedIds) {
            for (const auto& entry : entries_) {
                switch (entry.kind) {
                    case Entry::Kind::List:
                        if (entry.list.get() == protectedId) protectedValues.emplace_back(ListRef(entry.list.get()));
                        break;
                    case Entry::Kind::Map:
                        if (entry.map.get() == protectedId) protectedValues.emplace_back(MapRef(entry.map.get()));
                        break;
                    case Entry::Kind::Object:
                        if (entry.object.get() == protectedId) protectedValues.emplace_back(ObjectRef(entry.object.get()));
                        break;
                    case Entry::Kind::Closure:
                        if (entry.closure.get() == protectedId) protectedValues.emplace_back(ClosureRef(entry.closure.get()));
                        break;
                }
            }
        }
    }
    marker.trace(GCRoots{std::move(protectedValues)});

    std::lock_guard<std::mutex> lock(mutex_);
    Collection result;
    auto& stats = result.stats;
    stats.tracked = entries_.size();

    std::vector<Entry> survivors;
    survivors.reserve(entries_.size());
    result.retired_.reserve(entries_.size());
    // Complete allocations before mutating the registry. On failure the
    // coordinator can safely resume without a partially detached heap.
    static_assert(std::is_nothrow_move_constructible<Entry>::value, "GC retirement must not throw");
    for (auto& entry : entries_) {
        const void* id = nullptr;
        switch (entry.kind) {
            case Entry::Kind::List: id = entry.list.get(); break;
            case Entry::Kind::Map: id = entry.map.get(); break;
            case Entry::Kind::Object: id = entry.object.get(); break;
            case Entry::Kind::Closure: id = entry.closure.get(); break;
        }
        if (marker.marked.count(id)) {
            ++stats.reachable;
            survivors.push_back(std::move(entry));
            continue;
        }

        ++stats.unreachable;
        result.retired_.push_back(std::move(entry));
        ++stats.reclaimed;
    }
    entries_.swap(survivors);
    stats.tracked = entries_.size();
    stats.deferred = 0;

    // Adapt the next collection threshold to the live heap instead of using
    // only a fixed allocation count. A small floor avoids overly frequent
    // collections for tiny heaps, while allowing stable larger heaps to grow
    // proportionally between collections.
    constexpr std::size_t kMinimumAllocationThreshold = 128;
    const std::size_t liveHeapBudget = entries_.size() > (std::numeric_limits<std::size_t>::max() / 2)
        ? std::numeric_limits<std::size_t>::max()
        : entries_.size() * 2;
    allocationThreshold_ = std::max(kMinimumAllocationThreshold, liveHeapBudget);
    allocationsSinceCollection_ = 0;
    return result;
}

void TracingGC::protect(const void* identity) {
    if (!identity) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++protectedRoots_[identity];
}

void TracingGC::unprotect(const void* identity) {
    if (!identity) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = protectedRoots_.find(identity);
    if (it != protectedRoots_.end() && --it->second == 0) protectedRoots_.erase(it);
}

bool TracingGC::shouldCollect() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocationsSinceCollection_ >= allocationThreshold_;
}

std::size_t TracingGC::trackedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void appendGCRoots(const std::vector<Value>& values, std::vector<Value>& roots) {
    roots.insert(roots.end(), values.begin(), values.end());
}

} // namespace zl
