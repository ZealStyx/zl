#include "zl/vm/gc.hpp"
#include "zl/vm/runtime_task.hpp"

#include <unordered_set>
#include <utility>
#include <type_traits>
#include <algorithm>
#include <limits>

namespace zl {
namespace {

void appendValueChildren(const Value& value, std::vector<Value>& work) {
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
                for (const auto& pending : held->channelState->pendingSends) work.push_back(pending.value);
            }
        } else if constexpr (std::is_same_v<T, ClosureRef>) {
            if (!held) return;
            for (const auto& capture : held->captured) work.push_back(capture.second);
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

void markValue(const Value& root, std::unordered_set<const void*>& marked) {
    std::vector<Value> work{root};
    while (!work.empty()) {
        Value value = std::move(work.back());
        work.pop_back();
        const void* id = identity(value);
        if (!id || !marked.insert(id).second) continue;
        appendValueChildren(value, work);
    }
}

} // namespace

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

TracingGC::Stats TracingGC::collect(const std::vector<Value>& roots) {
    // The coordinator must stop all mutators before entering this method.
    // Serializing collectors alone does not make tracing concurrent-safe.
    std::lock_guard<std::mutex> collectLock(collectMutex_);

    std::unordered_set<const void*> protectedIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : protectedRoots_) protectedIds.insert(entry.first);
    }

    std::unordered_set<const void*> marked;
    for (const auto& root : roots) markValue(root, marked);

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
    for (const auto& root : protectedValues) markValue(root, marked);

    std::lock_guard<std::mutex> lock(mutex_);
    Stats stats;
    stats.tracked = entries_.size();

    std::vector<Entry> survivors;
    survivors.reserve(entries_.size());
    for (auto& entry : entries_) {
        const void* id = nullptr;
        switch (entry.kind) {
            case Entry::Kind::List: id = entry.list.get(); break;
            case Entry::Kind::Map: id = entry.map.get(); break;
            case Entry::Kind::Object: id = entry.object.get(); break;
            case Entry::Kind::Closure: id = entry.closure.get(); break;
        }
        if (marked.count(id)) {
            ++stats.reachable;
            survivors.push_back(std::move(entry));
            continue;
        }

        ++stats.unreachable;
        if (entry.kind == Entry::Kind::List) entry.list->items.clear();
        else if (entry.kind == Entry::Kind::Map) entry.map->entries.clear();
        else if (entry.kind == Entry::Kind::Object) {
            entry.object->fields.clear();
            if (entry.object->atomicRefState) entry.object->atomicRefState->value = Value{};
            if (entry.object->channelState) {
                entry.object->channelState->items.clear();
                entry.object->channelState->pendingSends.clear();
                entry.object->channelState->pendingReceives.clear();
            }
        } else entry.closure->captured.clear();
        ++stats.reclaimed;
    }
    entries_.swap(survivors);
    stats.tracked = entries_.size();
    stats.reachable = marked.empty() ? 0 : stats.reachable;
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
    return stats;
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
