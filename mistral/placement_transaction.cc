#include "placement_transaction.h"

#include <set>

#include "arch.h"
#include "lab_dispatch.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
bool binding_matches(const Arch &arch, const PlacementBindingEdit &edit)
{
    CellInfo *bound = arch.getBoundBelCell(edit.bel);
    return bound == edit.expected &&
           (bound == nullptr ? edit.expected_strength == STRENGTH_NONE : bound->belStrength == edit.expected_strength);
}
} // namespace

PreparedPlacementTransaction prepare_placement_transaction(const Arch &arch, std::vector<PlacementBindingEdit> edits)
{
    PreparedPlacementTransaction prepared;
    prepared.stamp_ = arch.placement_revision.stamp();
    if (!prepared.stamp_ || edits.empty())
        return prepared;

    pool<BelId> bels;
    std::set<CellInfo *> replacements;
    std::set<CellInfo *> displaced;
    for (const auto &edit : edits) {
        if (edit.bel == BelId() || !bels.insert(edit.bel).second || !binding_matches(arch, edit) ||
            (edit.replacement == nullptr) != (edit.replacement_strength == STRENGTH_NONE) ||
            (edit.replacement != nullptr && !replacements.insert(edit.replacement).second))
            return prepared;
        if (edit.expected != nullptr)
            displaced.insert(edit.expected);
    }
    for (const auto &edit : edits) {
        if (edit.replacement != nullptr && edit.replacement->bel != BelId() && !displaced.count(edit.replacement))
            return prepared;
    }
    prepared.edits_ = std::move(edits);
    prepared.valid_ = true;
    return prepared;
}

PlacementCommitOutcome commit_placement_transaction(Arch &arch, PreparedPlacementTransaction &&transaction)
{
    if (!transaction.valid_)
        return PlacementCommitOutcome::Malformed;
    if (!arch.placement_revision.is_current(transaction.stamp_))
        return PlacementCommitOutcome::Stale;
    for (const auto &edit : transaction.edits_)
        if (!binding_matches(arch, edit))
            return PlacementCommitOutcome::Mismatched;

    for (const auto &edit : transaction.edits_)
        if (edit.expected != nullptr)
            arch.unbindBel(edit.bel);
    for (const auto &edit : transaction.edits_)
        if (edit.replacement != nullptr)
            arch.bindBel(edit.bel, edit.replacement, edit.replacement_strength);
    transaction.valid_ = false;
    return PlacementCommitOutcome::Committed;
}

FrozenPlacementCandidate freeze_placement_candidate(const Arch &arch, const PreparedPlacementTransaction &transaction)
{
    FrozenPlacementCandidate frozen;
    frozen.stamp = transaction.stamp();
    if (!transaction || !frozen.stamp) {
        frozen.status = FrozenPlacementStatus::Malformed;
        return frozen;
    }
    if (!arch.placement_revision.is_current(frozen.stamp)) {
        frozen.status = FrozenPlacementStatus::Stale;
        return frozen;
    }

    dict<BelId, CellInfo *> occupancy;
    for (const auto &edit : transaction.edits())
        occupancy[edit.bel] = edit.replacement;

    frozen.queries.reserve(transaction.edits().size());
    uint64_t request = 0;
    for (const auto &edit : transaction.edits()) {
        const auto &data = arch.bel_data(edit.bel);
        NpnrLabQueryV2 query;
        if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB))
            query = NPNR_LAB_QUERY_COMB_BEL;
        else if (data.type == id_MISTRAL_FF)
            query = NPNR_LAB_QUERY_FF_BEL;
        else {
            frozen.queries.clear();
            frozen.status = FrozenPlacementStatus::Unsupported;
            return frozen;
        }
        frozen.queries.push_back(capture_lab_v2_overlay(arch, data.lab_data.lab, query, data.lab_data.alm, occupancy,
                                                        ++request, frozen.stamp.revision));
    }
    frozen.status = FrozenPlacementStatus::Ready;
    return frozen;
}

PlacementCandidateAssessment evaluate_placement_candidate(const FrozenPlacementCandidate &candidate)
{
    PlacementCandidateAssessment assessment;
    assessment.status = candidate.status;
    if (candidate.status != FrozenPlacementStatus::Ready)
        return assessment;
    assessment.results.reserve(candidate.queries.size());
    assessment.legal = true;
    for (const auto &query : candidate.queries) {
        auto result = evaluate_lab_v2_cpp(query);
        if (!lab_v2_result_valid(query, result)) {
            assessment.status = FrozenPlacementStatus::Malformed;
            assessment.legal = false;
            assessment.results.clear();
            return assessment;
        }
        assessment.legal &= result.status == NPNR_LAB_V2_LEGAL;
        assessment.results.push_back(result);
        if (!assessment.legal)
            break;
    }
    return assessment;
}

bool placement_candidate_rust_matches(const FrozenPlacementCandidate &candidate,
                                      const PlacementCandidateAssessment &cpp)
{
    if (candidate.status != FrozenPlacementStatus::Ready || cpp.status != FrozenPlacementStatus::Ready ||
        cpp.results.empty() || cpp.results.size() > candidate.queries.size())
        return false;
#ifndef NO_RUST
    for (size_t i = 0; i < cpp.results.size(); ++i) {
        NpnrLabAssessmentV2 rust{};
        if (npnr_mistral_eval_lab_v2(&candidate.queries[i], 1, &rust, 1) != NPNR_LAB_CALL_OK ||
            !lab_v2_result_valid(candidate.queries[i], rust) || !lab_v2_results_match(cpp.results[i], rust))
            return false;
    }
#endif
    return true;
}

NEXTPNR_NAMESPACE_END
