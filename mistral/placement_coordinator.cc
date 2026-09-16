/* SPDX-License-Identifier: ISC */
#include "placement_coordinator.h"

#include <algorithm>
#include <cinttypes>
#include <functional>
#include <string>

#include "arch.h"
#include "lab_frozen_batch.h"
#include "lab_v2.h"
#include "log.h"
#include "placement_pool.h"

NEXTPNR_NAMESPACE_BEGIN

struct PlacementCandidateCoordinator::WorkerTally
{
    uint64_t evaluated = 0, queries = 0, rust_batches = 0, rust_direct = 0, frozen = 0;
};

PlacementCandidateCoordinator::PlacementCandidateCoordinator(Arch &arch, unsigned workers)
        : arch_(arch), workers_(std::max(1u, workers)), pool_(std::make_unique<PlacementWorkerPool>(workers_)),
          scratch_(workers_)
{
}

PlacementCandidateCoordinator::~PlacementCandidateCoordinator() = default;

std::vector<PlacementBindingEdit>
placement_edits_for_candidate(const std::vector<std::pair<CellInfo *, BelId>> &targets,
                              const HeAPDisplacedBindings &displaced)
{
    dict<BelId, CellInfo *> replacements;
    for (const auto &target : targets)
        replacements[target.second] = target.first;
    std::vector<PlacementBindingEdit> edits;
    edits.reserve(displaced.size());
    for (const auto &entry : displaced) {
        auto replacement = replacements.find(entry.first);
        CellInfo *cell = replacement == replacements.end() ? nullptr : replacement->second;
        edits.push_back({entry.first, entry.second.cell, entry.second.strength, cell,
                         cell == nullptr ? STRENGTH_NONE : STRENGTH_STRONG});
    }
    return edits;
}

namespace {
#ifndef NO_RUST
bool rust_prefix_matches(const FrozenPlacementCandidate &candidate, const PlacementCandidateAssessment &cpp,
                         const RustFrozenLabBatchV2 &batch, std::vector<NpnrLabAssessmentV2> &scratch)
{
    if (candidate.status != FrozenPlacementStatus::Ready || cpp.status != FrozenPlacementStatus::Ready ||
        cpp.results.empty() || cpp.results.size() > candidate.queries.size())
        return false;
    const auto count = uint32_t(cpp.results.size());
    scratch.resize(count);
    if (batch.evaluate(0, count, scratch.data(), count) != NPNR_LAB_CALL_OK)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!lab_v2_result_valid(candidate.queries[i], scratch[i]) || !lab_v2_results_match(cpp.results[i], scratch[i]))
            return false;
    return true;
}
#endif
} // namespace

// Runs on the claiming worker. Pure: reads the frozen facts only.
void PlacementCandidateCoordinator::assess(unsigned worker, const FrozenPlacementCandidate &frozen,
                                           PlacementCandidateAssessment &result, uint8_t &rust_agrees,
                                           WorkerTally &tally)
{
    rust_agrees = 1;
    result = evaluate_placement_candidate(frozen);
    if (result.status != FrozenPlacementStatus::Ready)
        return; // the owner reports it
    ++tally.evaluated;
    tally.queries += frozen.queries.size();
#ifndef NO_RUST
    // Each worker owns its handle for the duration of one candidate; the Rust
    // quota (two live handles per worker id) is never approached.
    if (frozen.queries.size() <= NPNR_LAB_MAX_BATCH) {
        uint32_t status = NPNR_LAB_CALL_LIMIT;
        auto handle = RustFrozenLabBatchV2::create(frozen.queries, uint64_t(worker) + 1, status);
        if (status == NPNR_LAB_CALL_OK && handle) {
            rust_agrees = rust_prefix_matches(frozen, result, handle, scratch_.at(worker));
            ++tally.rust_batches;
            return;
        }
    }
    rust_agrees = placement_candidate_rust_matches(frozen, result);
    ++tally.rust_direct;
#else
    (void)worker;
#endif
}

void PlacementCandidateCoordinator::run_jobs(size_t count, const std::function<void(unsigned, size_t)> &job)
{
    if (count == 0)
        return;
    ++stats_.pool_runs;
    const auto failure = pool_->run(count, job);
    stats_.pool_spin_wakes = pool_->spin_wakes();
    stats_.pool_block_wakes = pool_->block_wakes();
    if (!failure.empty())
        log_error("A placement worker failed: %s\n", failure.c_str());
}

void PlacementCandidateCoordinator::freeze_and_evaluate(const std::vector<PreparedPlacementTransaction> &prepared,
                                                        std::vector<FrozenPlacementCandidate> &frozen,
                                                        std::vector<PlacementCandidateAssessment> &results,
                                                        std::vector<uint8_t> &rust_agrees)
{
    frozen.assign(prepared.size(), {});
    results.assign(prepared.size(), {});
    rust_agrees.assign(prepared.size(), 1);
    std::vector<WorkerTally> tallies(workers_);
    run_jobs(prepared.size(), [&](unsigned worker, size_t i) {
        frozen[i] = freeze_placement_candidate(arch_, prepared[i]);
        ++tallies[worker].frozen;
        if (frozen[i].status == FrozenPlacementStatus::Ready)
            assess(worker, frozen[i], results[i], rust_agrees[i], tallies[worker]);
        else
            results[i].status = frozen[i].status;
    });
    for (unsigned w = 0; w < workers_; ++w) {
        stats_.evaluated += tallies[w].evaluated;
        stats_.queries += tallies[w].queries;
        stats_.rust_batches += tallies[w].rust_batches;
        stats_.rust_direct += tallies[w].rust_direct;
        if (w != 0)
            stats_.frozen_by_workers += tallies[w].frozen;
    }
}

void PlacementCandidateCoordinator::evaluate(const std::vector<FrozenPlacementCandidate> &frozen,
                                             std::vector<PlacementCandidateAssessment> &results,
                                             std::vector<uint8_t> &rust_agrees)
{
    results.assign(frozen.size(), {});
    rust_agrees.assign(frozen.size(), 1);
    std::vector<WorkerTally> tallies(workers_);
    run_jobs(frozen.size(), [&](unsigned worker, size_t i) {
        if (frozen[i].status == FrozenPlacementStatus::Ready)
            assess(worker, frozen[i], results[i], rust_agrees[i], tallies[worker]);
        else
            results[i].status = frozen[i].status;
    });
    for (const auto &tally : tallies) {
        stats_.evaluated += tally.evaluated;
        stats_.queries += tally.queries;
        stats_.rust_batches += tally.rust_batches;
        stats_.rust_direct += tally.rust_direct;
    }
}

HeAPClusterBatchOutcome PlacementCandidateCoordinator::place(const std::vector<HeAPClusterCandidate> &candidates)
{
    ++stats_.batches;
    stats_.candidates += candidates.size();
    stats_.max_candidates = std::max<uint64_t>(stats_.max_candidates, candidates.size());

    // 1. Prepare on the owner, in proposal order, while the live design is
    //    stable. The supported prefix is decided from BEL types alone; the
    //    first unsupported candidate truncates the batch (HeAP replays it live)
    //    and everything after it is never proposed.
    std::vector<PreparedPlacementTransaction> prepared;
    prepared.reserve(candidates.size());
    size_t supported = candidates.size();
    for (size_t i = 0; i < candidates.size(); ++i) {
        auto transaction = prepare_placement_transaction(
                arch_, placement_edits_for_candidate(candidates[i].targets, candidates[i].displaced));
        if (!transaction)
            log_error("Failed to preflight a detached HeAP cluster candidate (batch %" PRIu64 ", index %zu).\n",
                      stats_.batches, i);
        if (!placement_candidate_supported(arch_, transaction)) {
            supported = i;
            ++stats_.unsupported;
            break;
        }
        prepared.push_back(std::move(transaction));
    }

    // 2. Parallel freeze and detached evaluation. The owner is blocked here, so
    //    the design is immutable for every worker.
    const uint64_t queries_before = stats_.queries;
    std::vector<FrozenPlacementCandidate> frozen;
    std::vector<PlacementCandidateAssessment> results;
    std::vector<uint8_t> rust_agrees;
    freeze_and_evaluate(prepared, frozen, results, rust_agrees);
    stats_.max_queries = std::max(stats_.max_queries, stats_.queries - queries_before);

    // 3. Consume strictly in proposal order.
    for (size_t i = 0; i < prepared.size(); ++i) {
        if (frozen[i].status != FrozenPlacementStatus::Ready)
            log_error("Failed to freeze a detached HeAP cluster candidate (batch %" PRIu64 ", index %zu).\n",
                      stats_.batches, i);
        if (results[i].status != FrozenPlacementStatus::Ready)
            log_error("Failed to evaluate a detached HeAP cluster candidate (batch %" PRIu64 ", index %zu).\n",
                      stats_.batches, i);
        if (!rust_agrees[i])
            log_error("Rust disagrees with detached C++ for a HeAP cluster candidate (batch %" PRIu64 ", index %zu).\n",
                      stats_.batches, i);
        if (!results[i].legal) {
            ++stats_.rejected;
            continue;
        }
        auto outcome = commit_placement_transaction(arch_, std::move(prepared[i]));
        bool rejected_on_retry = false;
        for (unsigned retry = 0; !rejected_on_retry && (outcome == PlacementCommitOutcome::Stale ||
                                                        outcome == PlacementCommitOutcome::Mismatched);
             ++retry) {
            // Only the owner mutates, so this cannot happen in the current
            // design; keep the documented policy anyway: re-evaluate against
            // the current state, asynchronously up to MAX_STALE_RETRIES times,
            // then synchronously.
            ++stats_.stale;
            auto again = prepare_placement_transaction(
                    arch_, placement_edits_for_candidate(candidates[i].targets, candidates[i].displaced));
            if (!again)
                log_error("Failed to preflight a stale HeAP cluster candidate.\n");
            PlacementCandidateAssessment assessment;
            if (retry < MAX_STALE_RETRIES) {
                ++stats_.stale_retries;
                std::vector<PreparedPlacementTransaction> one;
                one.push_back(std::move(again));
                std::vector<FrozenPlacementCandidate> one_frozen;
                std::vector<PlacementCandidateAssessment> one_result;
                std::vector<uint8_t> one_agrees;
                freeze_and_evaluate(one, one_frozen, one_result, one_agrees);
                if (one_frozen[0].status != FrozenPlacementStatus::Ready)
                    log_error("Failed to refreeze a stale HeAP cluster candidate.\n");
                if (!one_agrees[0])
                    log_error("Rust disagrees with detached C++ for a stale HeAP cluster candidate.\n");
                assessment = std::move(one_result[0]);
                again = std::move(one[0]);
            } else {
                ++stats_.synchronous_fallbacks;
                auto refrozen = freeze_placement_candidate(arch_, again);
                if (refrozen.status != FrozenPlacementStatus::Ready)
                    log_error("Failed to refreeze a stale HeAP cluster candidate.\n");
                assessment = evaluate_placement_candidate(refrozen);
                if (!placement_candidate_rust_matches(refrozen, assessment))
                    log_error("Rust disagrees with detached C++ for a synchronous HeAP cluster candidate.\n");
            }
            if (assessment.status != FrozenPlacementStatus::Ready)
                log_error("Failed to re-evaluate a stale HeAP cluster candidate.\n");
            if (!assessment.legal)
                rejected_on_retry = true;
            else
                outcome = commit_placement_transaction(arch_, std::move(again));
        }
        if (outcome == PlacementCommitOutcome::Committed) {
            ++stats_.committed;
            stats_.discarded += prepared.size() - i - 1;
            return {HeAPClusterBatchStatus::Committed, i};
        }
        ++stats_.rejected;
    }
    if (supported < candidates.size())
        return {HeAPClusterBatchStatus::Unsupported, supported};
    return {HeAPClusterBatchStatus::NoneLegal, 0};
}

Placer1SwapAssessment mistral_assess_swap(Context *ctx, const std::vector<Placer1SwapEdit> &edits)
{
    Placer1SwapAssessment assessment;
    const Arch &arch = *ctx;
    BelOverlay overlay;
    for (const auto &edit : edits) {
        if (edit.bel == BelId() || overlay.count >= BelOverlay::MAX)
            return assessment;
        const auto &data = arch.bel_data(edit.bel);
        if (!data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF))
            return assessment; // Unsupported: the annealer uses its live path
        if (arch.getBoundBelCell(edit.bel) != edit.expected)
            log_error("Detached swap assessment saw an unexpected occupant on '%s'.\n", ctx->nameOfBel(edit.bel));
        overlay.add(edit.bel, edit.replacement);
    }
    const auto stamp = arch.placement_revision.stamp();
    assessment.stamp_session = stamp.session.value;
    assessment.stamp_revision = stamp.revision;
    assessment.status = arch.overlay_bels_legal(overlay) ? Placer1SwapAssessment::Status::Legal
                                                         : Placer1SwapAssessment::Status::Illegal;
    return assessment;
}

bool mistral_commit_swap(Context *ctx, const std::vector<Placer1SwapEdit> &edits,
                         const Placer1SwapAssessment &assessment)
{
    Arch &arch = *ctx;
    if (assessment.status != Placer1SwapAssessment::Status::Legal)
        return false;
    const auto stamp = arch.placement_revision.stamp();
    if (!stamp || stamp.session.value != assessment.stamp_session || stamp.revision != assessment.stamp_revision)
        return false;
    for (const auto &edit : edits) {
        const CellInfo *bound = arch.getBoundBelCell(edit.bel);
        if (bound != edit.expected || (bound != nullptr && bound->belStrength != edit.expected_strength))
            return false;
    }
    for (const auto &edit : edits)
        if (edit.expected != nullptr)
            arch.unbindBel(edit.bel);
    for (const auto &edit : edits)
        if (edit.replacement != nullptr)
            arch.bindBel(edit.bel, edit.replacement, edit.replacement_strength);
    return true;
}

void report_placement_batch_stats(const PlacementCandidateCoordinator &coordinator)
{
    const auto &s = coordinator.stats();
    log_info("Cluster candidate batches: workers=%u, batches=%" PRIu64 ", candidates=%" PRIu64 " (evaluated=%" PRIu64
             ", max/batch=%" PRIu64 "), queries=%" PRIu64 " (max/batch=%" PRIu64 ")\n",
             coordinator.workers(), s.batches, s.candidates, s.evaluated, s.max_candidates, s.queries, s.max_queries);
    log_info("  committed=%" PRIu64 ", rejected=%" PRIu64 ", discarded=%" PRIu64 ", unsupported=%" PRIu64
             ", stale=%" PRIu64 ", stale-retries=%" PRIu64 ", synchronous-fallbacks=%" PRIu64 "\n",
             s.committed, s.rejected, s.discarded, s.unsupported, s.stale, s.stale_retries, s.synchronous_fallbacks);
    log_info("  rust frozen handles=%" PRIu64 ", one-shot cross-checks=%" PRIu64 ", frozen by workers=%" PRIu64
             "; pool runs=%" PRIu64 ", spin wakes=%" PRIu64 ", blocking wakes=%" PRIu64 "\n",
             s.rust_batches, s.rust_direct, s.frozen_by_workers, s.pool_runs, s.pool_spin_wakes, s.pool_block_wakes);
}

NEXTPNR_NAMESPACE_END
