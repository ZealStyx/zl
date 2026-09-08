#pragma once

#include <cstdint>
#include <vector>

#include "value.hpp"

namespace zl {

class GCSafepointCoordinator {
public:
    static GCSafepointCoordinator& instance();

    using ParticipantId = std::uint64_t;

    ParticipantId registerParticipant();
    void unregisterParticipant(ParticipantId id);

    // Publish a complete VM root snapshot at a safe point. If a collection has
    // been requested by allocation pressure, this call participates in the
    // stop-the-world rendezvous and blocks until the collection finishes.
    void poll(ParticipantId id, std::vector<Value> roots);

    // A thread that is about to block inside a native call running no ZL
    // bytecode (e.g. Thread.join, awaiting a condition) is AT a safepoint for
    // GC purposes: it cannot touch GC-managed objects until it unblocks. Mark
    // it so an in-progress rendezvous does not wait on a participant that is
    // idle in native code (which would deadlock: that native call is itself
    // waiting for the worker threads GC is parked). endBlockingNative clears
    // the mark once the thread resumes running bytecode.
    void beginBlockingNative(ParticipantId id, std::vector<Value> roots);
    void endBlockingNative(ParticipantId id);

    // Publish a fresh root snapshot WITHOUT joining/blocking on a rendezvous.
    // Used by a nested execute() that is holding an application-level lock
    // (e.g. the closure inside Mutex.withLock): such a thread must not block
    // on a GC rendezvous, because other participants are blocked on the lock
    // it holds and can never reach their own safepoints (a lock-order
    // inversion). It keeps executing; the roots it publishes are the ones the
    // eventual leader trace keeps alive.
    void publishRoots(ParticipantId id, std::vector<Value> roots);

private:
    GCSafepointCoordinator() = default;
};

} // namespace zl
