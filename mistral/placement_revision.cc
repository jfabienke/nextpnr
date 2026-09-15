#include "placement_revision.h"

#include <atomic>

NEXTPNR_NAMESPACE_BEGIN

namespace {
std::atomic<uint64_t> next_placement_session{1};
}

PlacementRevisionState::PlacementRevisionState(uint64_t fixed_session, uint64_t initial_revision)
        : session_{fixed_session == 0 ? next_placement_session.fetch_add(1, std::memory_order_relaxed) : fixed_session},
          revision_(initial_revision), valid_(session_.value != 0 && initial_revision != 0)
{
}

bool PlacementRevisionState::note_mutation(PlacementMutation mutation)
{
    const size_t index = static_cast<size_t>(mutation);
    if (!valid_ || index >= mutation_counts_.size() || revision_ == std::numeric_limits<uint64_t>::max()) {
        valid_ = false;
        return false;
    }
    ++mutation_counts_[index];
    ++revision_;
    return true;
}

NEXTPNR_NAMESPACE_END
