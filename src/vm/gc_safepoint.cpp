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

void GCSafepointCoordinator::poll(ParticipantId id, std::vector<Value> roots) {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    auto it = s.participants.find(id);
    if (it == s.participants.end()) return;

    // Do not initiate a GC for the first few allocations. Allocation pressure
    // is sampled only at VM safe points, never in the middle of an instruction.
    if (!s.collectionRequested && !s.collectionRunning && TracingGC::instance().shouldCollect()) {
        s.collectionRequested = true;
    }

    if (!s.collectionRequested) return;

    it->second.roots = std::move(roots);
    it->second.atSafePoint = true;
    s.cv.notify_all();

    auto allAtSafePoint = [&] {
        for (const auto& [participantId, participant] : s.participants) {
            (void)participantId;
            if (!participant.atSafePoint) return false;
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
            participant.atSafePoint = false;
            participant.roots.clear();
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

} // namespace zl
