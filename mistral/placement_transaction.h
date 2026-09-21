#ifndef MISTRAL_PLACEMENT_TRANSACTION_H
#define MISTRAL_PLACEMENT_TRANSACTION_H

#include <optional>
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
// True when every edited BEL is a LAB COMB/MCOMB/FF BEL, i.e. freezing would
// not report Unsupported. Reads BEL types only; no capture.
bool placement_candidate_supported(const Arch &arch, const PreparedPlacementTransaction &transaction);
FrozenPlacementCandidate freeze_placement_candidate(const Arch &arch, const PreparedPlacementTransaction &transaction);
PlacementCandidateAssessment evaluate_placement_candidate(const FrozenPlacementCandidate &candidate);
// The candidate answered by the resident Rust session without freezing it (design section 16.1):
// the edits of each LAB held in view together, legal when every LAB says so. Empty when the
// session does not cover the candidate (the legacy mode, no Rust, a bel outside a LAB, a LUTRAM
// cell, more edits in one LAB than the session holds in view, or a failed call): the caller
// then freezes and evaluates as before.
std::optional<bool> placement_candidate_resident(const Arch &arch, const PreparedPlacementTransaction &transaction);
bool placement_candidate_rust_matches(const FrozenPlacementCandidate &candidate,
                                      const PlacementCandidateAssessment &cpp);

NEXTPNR_NAMESPACE_END

#endif
