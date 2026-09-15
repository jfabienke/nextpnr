#ifndef MISTRAL_PLACEMENT_REVISION_H
#define MISTRAL_PLACEMENT_REVISION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct PlacementSessionId
{
    uint64_t value = 0;
    explicit operator bool() const { return value != 0; }
    bool operator==(const PlacementSessionId &other) const { return value == other.value; }
    bool operator!=(const PlacementSessionId &other) const { return !(*this == other); }
};

template <typename Tag> struct PlacementEntityKey
{
    PlacementSessionId session;
    uint64_t value = 0;
    explicit operator bool() const { return bool(session) && value != 0; }
    bool operator==(const PlacementEntityKey &other) const { return session == other.session && value == other.value; }
    bool operator!=(const PlacementEntityKey &other) const { return !(*this == other); }
};

struct PlacementCellTag;
struct PlacementNetTag;
struct PlacementBelTag;
struct PlacementLabTag;
using PlacementCellKey = PlacementEntityKey<PlacementCellTag>;
using PlacementNetKey = PlacementEntityKey<PlacementNetTag>;
using PlacementBelKey = PlacementEntityKey<PlacementBelTag>;
using PlacementLabKey = PlacementEntityKey<PlacementLabTag>;

struct PlacementRevisionStamp
{
    PlacementSessionId session;
    uint64_t revision = 0;
    explicit operator bool() const { return bool(session) && revision != 0; }
};

enum class PlacementMutation : uint8_t
{
    BelBinding,
    Connectivity,
    CellFacts,
    NetFacts,
    Constraints,
    GeneratedObjects,
    Routing,
    Count
};

class PlacementRevisionState
{
  public:
    explicit PlacementRevisionState(uint64_t fixed_session = 0, uint64_t initial_revision = 1);

    PlacementSessionId session() const { return session_; }
    uint64_t revision() const { return revision_; }
    bool valid() const { return valid_; }
    PlacementRevisionStamp stamp() const
    {
        return valid_ ? PlacementRevisionStamp{session_, revision_} : PlacementRevisionStamp{};
    }
    bool is_current(PlacementRevisionStamp stamp) const
    {
        return valid_ && stamp.session == session_ && stamp.revision == revision_;
    }

    bool note_mutation(PlacementMutation mutation);
    uint64_t mutation_count(PlacementMutation mutation) const
    {
        return mutation_counts_.at(static_cast<size_t>(mutation));
    }

    PlacementCellKey issue_cell_key() { return issue_key<PlacementCellKey>(); }
    PlacementNetKey issue_net_key() { return issue_key<PlacementNetKey>(); }
    PlacementBelKey issue_bel_key() { return issue_key<PlacementBelKey>(); }
    PlacementLabKey issue_lab_key() { return issue_key<PlacementLabKey>(); }

  private:
    template <typename Key> Key issue_key()
    {
        if (!valid_ || next_entity_ == std::numeric_limits<uint64_t>::max()) {
            valid_ = false;
            return {};
        }
        return Key{session_, next_entity_++};
    }

    PlacementSessionId session_;
    uint64_t revision_ = 1;
    uint64_t next_entity_ = 1;
    bool valid_ = true;
    std::array<uint64_t, static_cast<size_t>(PlacementMutation::Count)> mutation_counts_{};
};

NEXTPNR_NAMESPACE_END

#endif
