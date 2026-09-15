#ifndef MISTRAL_PLACEMENT_TRANSACTION_H
#define MISTRAL_PLACEMENT_TRANSACTION_H

#include <vector>

#include "nextpnr.h"
#include "placement_revision.h"

NEXTPNR_NAMESPACE_BEGIN

struct PlacementBindingEdit
{
    BelId bel;
    CellInfo *expected = nullptr;
    PlaceStrength expected_strength = STRENGTH_NONE;
    CellInfo *replacement = nullptr;
    PlaceStrength replacement_strength = STRENGTH_NONE;
};

enum class PlacementCommitOutcome
{
    Committed,
    Stale,
    Mismatched,
    Malformed
};

class PreparedPlacementTransaction
{
  public:
    PreparedPlacementTransaction() = default;
    PreparedPlacementTransaction(const PreparedPlacementTransaction &) = delete;
    PreparedPlacementTransaction &operator=(const PreparedPlacementTransaction &) = delete;
    PreparedPlacementTransaction(PreparedPlacementTransaction &&) = default;
    PreparedPlacementTransaction &operator=(PreparedPlacementTransaction &&) = default;

    explicit operator bool() const { return valid_; }
    PlacementRevisionStamp stamp() const { return stamp_; }
    const std::vector<PlacementBindingEdit> &edits() const { return edits_; }

  private:
    friend PreparedPlacementTransaction prepare_placement_transaction(const Arch &, std::vector<PlacementBindingEdit>);
    friend PlacementCommitOutcome commit_placement_transaction(Arch &, PreparedPlacementTransaction &&);
    PlacementRevisionStamp stamp_;
    std::vector<PlacementBindingEdit> edits_;
    bool valid_ = false;
};

PreparedPlacementTransaction prepare_placement_transaction(const Arch &arch, std::vector<PlacementBindingEdit> edits);
PlacementCommitOutcome commit_placement_transaction(Arch &arch, PreparedPlacementTransaction &&transaction);

NEXTPNR_NAMESPACE_END

#endif
