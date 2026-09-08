#pragma once

#include <cstdint>
#include <vector>

#include "gc_roots.hpp"

namespace zl {

class GCSafepointCoordinator {
public:
    static GCSafepointCoordinator& instance();
    using ParticipantId = std::uint64_t;

    ParticipantId registerParticipant();
    void unregisterParticipant(ParticipantId id);

    // Publish a complete root snapshot and participate in a stop-the-world
    // rendezvous when allocation pressure requests collection.
    void poll(ParticipantId id, GCRoots roots);

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
