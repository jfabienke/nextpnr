#include "placement_transaction.h"

#include <set>

#include "arch.h"

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

NEXTPNR_NAMESPACE_END
