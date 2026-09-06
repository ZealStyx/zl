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

private:
    GCSafepointCoordinator() = default;
};

} // namespace zl
