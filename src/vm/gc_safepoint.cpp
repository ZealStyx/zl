#include "zl/vm/gc_safepoint.hpp"
#include "zl/vm/gc.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace zl {
namespace {

struct CoordinatorState {
    struct Participant {
        bool parked{false};
        std::vector<Value> roots;
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<GCSafepointCoordinator::ParticipantId, Participant> participants;
    GCSafepointCoordinator::ParticipantId nextId{1};
    bool collectionRequested{false};
    bool collectionRunning{false};

    bool allParked() const {
        return std::all_of(participants.begin(), participants.end(),
                           [](const auto& entry) { return entry.second.parked; });
    }
};

CoordinatorState& state() {
    static CoordinatorState s;
    return s;
}

} // namespace

GCSafepointCoordinator& GCSafepointCoordinator::instance() {
    static GCSafepointCoordinator coordinator;
    return coordinator;
}

GCSafepointCoordinator::ParticipantId GCSafepointCoordinator::registerParticipant() {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    // A pending rendezvous must not stop an active participant from creating
    // another VM. Only an actual trace/sweep excludes new mutators.
    s.cv.wait(lock, [&] { return !s.collectionRunning; });
    const auto id = s.nextId++;
    s.participants.emplace(id, CoordinatorState::Participant{});
    return id;
}

void GCSafepointCoordinator::unregisterParticipant(ParticipantId id) {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    s.cv.wait(lock, [&] { return !s.collectionRunning; });
    s.participants.erase(id);
    // The last running participant may exit rather than poll. Waiting pollers
    // must re-evaluate the rendezvous and elect a collector in that case too.
    s.cv.notify_all();
}

void GCSafepointCoordinator::poll(ParticipantId id, std::vector<Value> roots) {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.participants.count(id)) return;
    if (!s.collectionRequested && TracingGC::instance().shouldCollect()) {
        s.collectionRequested = true;
    }
    if (!s.collectionRequested) return;

    auto& participant = s.participants.at(id);
    participant.roots = std::move(roots);
    participant.parked = true;
    s.cv.notify_all();
    while (s.collectionRequested) {
        if (s.collectionRunning || !s.allParked()) {
            s.cv.wait(lock);
            continue;
        }
        std::exception_ptr failure;
        try {
            std::vector<Value> allRoots;
            for (const auto& entry : s.participants) {
                const auto& snapshot = entry.second.roots;
                allRoots.insert(allRoots.end(), snapshot.begin(), snapshot.end());
            }
            s.collectionRunning = true;
            lock.unlock();
            (void)TracingGC::instance().collect(allRoots);
        } catch (...) {
            failure = std::current_exception();
        }
        if (!lock.owns_lock()) lock.lock();
        s.collectionRunning = false;
        s.collectionRequested = false;
        s.cv.notify_all();
        if (failure) {
            participant.parked = false;
            participant.roots.clear();
            std::rethrow_exception(failure);
        }
    }
    // A collector never clears another participant's snapshot or parked flag.
    // Until that participant actually resumes, its roots must also survive a
    // second collection started by a faster participant.
    participant.parked = false;
    participant.roots.clear();
}

void GCSafepointCoordinator::beginBlockingNative(ParticipantId id, std::vector<Value> roots) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.participants.find(id);
    if (it == s.participants.end()) return;
    it->second.roots = std::move(roots);
    it->second.parked = true;
    s.cv.notify_all();
}

void GCSafepointCoordinator::endBlockingNative(ParticipantId id) {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    // The native operation may have completed during tracing. Its thread must
    // not touch managed memory until the collector is finished. Do not wait
    // for a merely requested collection: the native may have acquired a lock
    // needed by a participant that has not yet parked.
    s.cv.wait(lock, [&] { return !s.collectionRunning; });
    auto it = s.participants.find(id);
    if (it == s.participants.end()) return;
    it->second.parked = false;
    it->second.roots.clear();
    s.cv.notify_all();
}

} // namespace zl
