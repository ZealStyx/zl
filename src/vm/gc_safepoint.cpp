#include "zl/vm/gc_safepoint.hpp"
#include "zl/vm/gc.hpp"

#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace zl {
namespace {

struct CoordinatorState {
    struct Participant {
        bool atSafePoint{false};
        bool blockedInNative{false};
        std::vector<Value> roots;
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<GCSafepointCoordinator::ParticipantId, Participant> participants;
    GCSafepointCoordinator::ParticipantId nextId{1};
    bool collectionRequested{false};
    bool collectionRunning{false};
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
    s.cv.wait(lock, [&] { return !s.collectionRunning && !s.collectionRequested; });
    const auto id = s.nextId++;
    s.participants.emplace(id, CoordinatorState::Participant{});
    return id;
}

void GCSafepointCoordinator::unregisterParticipant(ParticipantId id) {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    auto it = s.participants.find(id);
    if (it == s.participants.end()) return;
    s.participants.erase(it);
    if (s.collectionRequested) s.cv.notify_all();
}

namespace {

// Shared rendezvous: mark the participant at a safe point and, if a collection
// is requested, either lead it (when this participant is the last one to
// arrive) or wait until it completes. A thread blocking inside a native call
// (beginBlockingNative) arrives here with no fresh roots, since it cannot touch
// GC objects while parked; the native call's arguments are already kept alive
// by the calling VM's native root frames.
void arriveSafepoint(GCSafepointCoordinator::ParticipantId id, std::vector<Value> roots) {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    auto it = s.participants.find(id);
    if (it == s.participants.end()) return;

    // Do not initiate a GC for the first few allocations. Allocation pressure
    // is sampled only at VM safe points, never in the middle of an instruction.
    if (!s.collectionRequested && !s.collectionRunning && TracingGC::instance().shouldCollect()) {
        s.collectionRequested = true;
    }

    if (!s.collectionRequested) {
        return;
    }

    // Publish the participant's latest root snapshot. A thread blocked in a
    // native call keeps its roots (set by beginBlockingNative) across the
    // rendezvous; a transient poller supplies fresh roots each arrival.
    if (!it->second.blockedInNative) {
        it->second.roots = std::move(roots);
    }
    it->second.atSafePoint = true;
    s.cv.notify_all();

    // A participant counts as at the rendezvous when it has arrived at a bytecode
    // safe point OR is blocked inside a native call running no bytecode.
    auto allAtSafePoint = [&] {
        for (const auto& [participantId, participant] : s.participants) {
            (void)participantId;
            if (!participant.atSafePoint && !participant.blockedInNative) return false;
        }
        return true;
    };

    if (allAtSafePoint()) {
        std::vector<Value> allRoots;
        for (auto& [participantId, participant] : s.participants) {
            (void)participantId;
            allRoots.insert(allRoots.end(), participant.roots.begin(), participant.roots.end());
        }
        s.collectionRunning = true;
        lock.unlock();

        (void)TracingGC::instance().collect(allRoots);

        lock.lock();
        s.collectionRequested = false;
        s.collectionRunning = false;
        for (auto& [participantId, participant] : s.participants) {
            (void)participantId;
            // A thread blocked inside a native call remains parked across this
            // collection: keep it flagged (with its roots) so the next
            // rendezvous does not wait on it; only transient pollers reset.
            if (participant.blockedInNative) {
                participant.atSafePoint = true;
            } else {
                participant.atSafePoint = false;
                participant.roots.clear();
            }
        }
        s.cv.notify_all();
        return;
    }

    s.cv.wait(lock, [&] {
        return !s.collectionRequested || s.collectionRunning;
    });
    // If another participant became collector leader, remain blocked until the
    // collection has fully completed and state is released.
    s.cv.wait(lock, [&] { return !s.collectionRunning; });
}

} // namespace

void GCSafepointCoordinator::poll(ParticipantId id, std::vector<Value> roots) {
    arriveSafepoint(id, std::move(roots));
}

void GCSafepointCoordinator::publishRoots(ParticipantId id, std::vector<Value> roots) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.participants.find(id);
    if (it == s.participants.end()) return;
    // Refresh this participant's root snapshot without ever blocking. A nested
    // execute() running inside a held application lock cannot join a
    // stop-the-world rendezvous (other participants are parked on that lock);
    // the leader's trace keeps alive whatever roots we publish here.
    it->second.roots = std::move(roots);
}

void GCSafepointCoordinator::beginBlockingNative(ParticipantId id, std::vector<Value> roots) {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    auto it = s.participants.find(id);
    if (it == s.participants.end()) return;
    // The thread is about to run no ZL bytecode (Thread.join, a condition
    // wait). It counts as at a safe point for a rendezvous racing the parked
    // window, and publishes its roots so the leader's trace keeps the objects
    // the native frame still references.
    it->second.blockedInNative = true;
    it->second.roots = std::move(roots);
    s.cv.notify_all();

    // If a collection is requested while we arrive, help complete it here
    // (same rendezvous as a normal poll) before parking.
    if (s.collectionRequested) {
        lock.unlock();
        arriveSafepoint(id, {});
        return;
    }
}

void GCSafepointCoordinator::endBlockingNative(ParticipantId id) {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    auto it = s.participants.find(id);
    if (it == s.participants.end()) return;
    // The thread is about to resume executing bytecode: it is no longer parked
    // in native code. Clear the blocked flag and take the next safepoint poll
    // as usual (which will join any in-flight rendezvous). Do NOT wait here:
    // beginBlockingNative already settled any rendezvous that was requested
    // during the parked window, and blocking again could stall after the
    // joined workers are gone.
    it->second.blockedInNative = false;
    it->second.atSafePoint = false;
    it->second.roots.clear();
    s.cv.notify_all();
}

} // namespace zl
