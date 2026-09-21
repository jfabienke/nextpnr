#include "placement_transaction.h"

#include <array>
#include <set>

#include "arch.h"
#include "lab_dispatch.h"
#include "lab_resident.h"

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

bool placement_candidate_supported(const Arch &arch, const PreparedPlacementTransaction &transaction)
{
    if (!transaction)
        return false;
    for (const auto &edit : transaction.edits())
        if (!arch.bel_data(edit.bel).type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF))
            return false;
    return true;
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

std::optional<bool> placement_candidate_resident(const Arch &arch, const PreparedPlacementTransaction &transaction)
{
#ifdef NO_RUST
    (void)arch;
    (void)transaction;
    return std::nullopt;
#else
    if (arch.args.lab_legality == LabLegalityMode::Legacy || !transaction || transaction.edits().empty())
        return std::nullopt;
    // The edits grouped by LAB, in the order the transaction lists them. A cluster of one ALM is
    // one group; a carry chain is several and usually over the budget.
    struct Group
    {
        uint32_t lab;
        uint32_t count = 0;
        std::array<NpnrBelPatchV2, NPNR_LAB_RESIDENT_MAX_TRIALS> edits{};
    };
    std::array<Group, 4> groups{};
    size_t group_count = 0;
    for (const auto &edit : transaction.edits()) {
        const auto &data = arch.bel_data(edit.bel);
        const bool is_ff = data.type == id_MISTRAL_FF;
        if (!is_ff && !data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB))
            return std::nullopt;
        if (edit.replacement != nullptr && edit.replacement->type == id_MISTRAL_MLAB)
            return std::nullopt; // LUTRAM write reservations are host-owned
        size_t g = 0;
        while (g < group_count && groups[g].lab != data.lab_data.lab)
            ++g;
        if (g == group_count) {
            if (group_count == groups.size())
                return std::nullopt;
            groups[group_count++].lab = data.lab_data.lab;
        }
        Group &group = groups[g];
        if (group.count == group.edits.size())
            return std::nullopt;
        NpnrBelPatchV2 &patch = group.edits[group.count++];
        patch = NpnrBelPatchV2{};
        if (edit.replacement != nullptr)
            capture_cell_v2_keyed(*edit.replacement, is_ff, patch);
        patch.alm = data.lab_data.alm;
        patch.slot = uint32_t((is_ff ? 2 : 0) + data.lab_data.idx);
    }
    if (!arch.lab_resident)
        std::atomic_store(&arch.lab_resident, std::make_shared<ResidentLabLegality>(arch));
    for (size_t g = 0; g < group_count; ++g) {
        bool legal = false;
        const uint32_t call = arch.lab_resident->edits(arch, groups[g].lab, groups[g].edits.data(), groups[g].count,
                                                       arch.args.lab_legality != LabLegalityMode::Rust, legal);
        if (call == NPNR_LAB_CALL_BAD_SNAPSHOT)
            return std::nullopt; // over the budget with the LAB's pending trials: not covered
        if (call != NPNR_LAB_CALL_OK) {
            arch.lab_legality_stats.errors.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        if (!legal)
            return false;
    }
    return true;
#endif
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
