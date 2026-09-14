#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "gc_roots.hpp"

namespace zl {

class GCSafepointCoordinator {
public:
    static GCSafepointCoordinator& instance();
    using ParticipantId = std::uint64_t;

    ParticipantId registerParticipant();
    void unregisterParticipant(ParticipantId id);

    // Participate in a stop-the-world rendezvous when allocation pressure
    // requests collection. The root snapshot comes from a provider rather
    // than a by-value argument so the common case - no collection pending -
    // never pays for a full stack walk: building the snapshot is O(call
    // depth), and doing it at every safepoint made deep recursion O(n^2).
    // The provider runs only when this poll will actually publish, outside
    // the coordinator lock; a null provider publishes an empty snapshot.
    void poll(ParticipantId id, std::function<GCRoots()> rootProvider);

    // Bracket native waits that neither run bytecode nor mutate managed data.
    // Waiters publish their roots so lock holders can safely poll, including
    // inside nested execute(). Reactivation waits for an active collection to
    // finish, but not for a pending rendezvous. Pair these calls with RAII.
    void beginBlockingNative(ParticipantId id, GCRoots roots);
    void endBlockingNative(ParticipantId id);

private:
    GCSafepointCoordinator() = default;
};

} // namespace zl
