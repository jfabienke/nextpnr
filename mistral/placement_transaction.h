#ifndef MISTRAL_PLACEMENT_TRANSACTION_H
#define MISTRAL_PLACEMENT_TRANSACTION_H

#include <vector>

#include "lab_v2.h"
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

enum class FrozenPlacementStatus
{
    Ready,
    Unsupported,
    Stale,
    Malformed
};

struct FrozenPlacementCandidate
{
    PlacementRevisionStamp stamp;
    FrozenPlacementStatus status = FrozenPlacementStatus::Malformed;
    std::vector<NpnrLabFactsV2> queries;
};

struct PlacementCandidateAssessment
{
    FrozenPlacementStatus status = FrozenPlacementStatus::Malformed;
    bool legal = false;
    std::vector<NpnrLabAssessmentV2> results;
};

PreparedPlacementTransaction prepare_placement_transaction(const Arch &arch, std::vector<PlacementBindingEdit> edits);
PlacementCommitOutcome commit_placement_transaction(Arch &arch, PreparedPlacementTransaction &&transaction);
FrozenPlacementCandidate freeze_placement_candidate(const Arch &arch, const PreparedPlacementTransaction &transaction);
PlacementCandidateAssessment evaluate_placement_candidate(const FrozenPlacementCandidate &candidate);
bool placement_candidate_rust_matches(const FrozenPlacementCandidate &candidate,
                                      const PlacementCandidateAssessment &cpp);

NEXTPNR_NAMESPACE_END

#endif
