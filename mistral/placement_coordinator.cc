/* SPDX-License-Identifier: ISC */
#include "placement_coordinator.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "arch.h"
#include "lab_frozen_batch.h"
#include "lab_v2.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

// A persistent pool of (workers - 1) threads plus the calling owner thread. Each
// job is a range of indices claimed dynamically; results are addressed by index,
// so claim order never influences what the owner sees.
struct PlacementCandidateCoordinator::Pool
{
    using Job = std::function<void(unsigned worker, size_t index)>;

    explicit Pool(unsigned threads)
    {
        for (unsigned i = 0; i < threads; ++i)
            workers.emplace_back([this, i] { worker_loop(i + 1); });
    }
    ~Pool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        for (auto &thread : workers)
            thread.join();
    }

    void run(size_t count, const Job &fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            job = &fn;
            job_count = count;
            next.store(0, std::memory_order_relaxed);
            finished = 0;
            ++generation;
        }
        wake.notify_all();
        claim(0, fn, count);
        std::unique_lock<std::mutex> lock(mutex);
        done.wait(lock, [&] { return finished == workers.size(); });
        job = nullptr;
    }

  private:
    void claim(unsigned worker, const Job &fn, size_t count)
    {
        for (size_t i = next.fetch_add(1, std::memory_order_relaxed); i < count;
             i = next.fetch_add(1, std::memory_order_relaxed))
            fn(worker, i);
    }

    void worker_loop(unsigned worker)
    {
        uint64_t seen = 0;
        while (true) {
            const Job *fn;
            size_t count;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stop || generation != seen; });
                if (stop)
                    return;
                seen = generation;
                fn = job;
                count = job_count;
            }
            claim(worker, *fn, count);
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++finished;
            }
            done.notify_all();
        }
    }

    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable wake, done;
    const Job *job = nullptr;
    size_t job_count = 0;
    std::atomic<size_t> next{0};
    size_t finished = 0;
    uint64_t generation = 0;
    bool stop = false;
};

PlacementCandidateCoordinator::PlacementCandidateCoordinator(Arch &arch, unsigned workers)
        : arch_(arch), workers_(std::max(1u, workers))
{
    if (workers_ > 1)
        pool_ = std::make_unique<Pool>(workers_ - 1);
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

// Where candidate i's queries live inside the flattened Rust batches. A
// candidate whose queries do not fit one 64-record handle is checked through the
// one-shot FFI instead (`handle == SIZE_MAX`).
struct RustSlice
{
    size_t handle = SIZE_MAX;
    uint32_t offset = 0;
};

#ifndef NO_RUST
bool rust_prefix_matches(const FrozenPlacementCandidate &candidate, const PlacementCandidateAssessment &cpp,
                         const RustFrozenLabBatchV2 &batch, uint32_t offset, std::vector<NpnrLabAssessmentV2> &scratch)
{
    if (candidate.status != FrozenPlacementStatus::Ready || cpp.status != FrozenPlacementStatus::Ready ||
        cpp.results.empty() || cpp.results.size() > candidate.queries.size())
        return false;
    const auto count = uint32_t(cpp.results.size());
    scratch.resize(count);
    if (batch.evaluate(offset, count, scratch.data(), count) != NPNR_LAB_CALL_OK)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!lab_v2_result_valid(candidate.queries[i], scratch[i]) || !lab_v2_results_match(cpp.results[i], scratch[i]))
            return false;
    return true;
}
#endif

} // namespace

void PlacementCandidateCoordinator::evaluate(const std::vector<FrozenPlacementCandidate> &frozen,
                                             std::vector<PlacementCandidateAssessment> &results,
                                             std::vector<uint8_t> &rust_agrees)
{
    results.assign(frozen.size(), {});
    rust_agrees.assign(frozen.size(), 1);
    if (frozen.empty())
        return;

    // Publish the frozen facts to Rust-owned handles before any worker starts.
    // Handles are created here, on the owner, and outlive the parallel section.
    std::vector<RustSlice> slices(frozen.size());
    std::vector<RustFrozenLabBatchV2> handles;
#ifndef NO_RUST
    {
        std::vector<NpnrLabFactsV2> pending;
        std::vector<size_t> pending_candidates;
        auto flush = [&] {
            if (pending.empty())
                return;
            uint32_t status = NPNR_LAB_CALL_LIMIT;
            auto handle = RustFrozenLabBatchV2::create(pending, uint64_t(handles.size()), status);
            if (status == NPNR_LAB_CALL_OK && handle) {
                for (size_t c : pending_candidates)
                    slices[c].handle = handles.size();
                handles.push_back(std::move(handle));
                ++stats_.rust_batches;
            } else {
                // Quota or validation refusal: these candidates use the one-shot path.
                for (size_t c : pending_candidates)
                    slices[c].handle = SIZE_MAX;
            }
            pending.clear();
            pending_candidates.clear();
        };
        for (size_t c = 0; c < frozen.size(); ++c) {
            const auto &queries = frozen[c].queries;
            if (frozen[c].status != FrozenPlacementStatus::Ready || queries.empty() ||
                queries.size() > NPNR_LAB_MAX_BATCH)
                continue;
            if (pending.size() + queries.size() > NPNR_LAB_MAX_BATCH)
                flush();
            slices[c].offset = uint32_t(pending.size());
            pending.insert(pending.end(), queries.begin(), queries.end());
            pending_candidates.push_back(c);
        }
        flush();
    }
#endif

    // Worker 0 is the owner thread; each worker owns one scratch buffer and
    // writes only results[i] / rust_agrees[i] for the indices it claims.
    std::vector<std::vector<NpnrLabAssessmentV2>> scratch(workers_);
    auto job = [&](unsigned worker, size_t i) {
        auto &mine = scratch.at(worker);
        results[i] = evaluate_placement_candidate(frozen[i]);
#ifndef NO_RUST
        if (results[i].status == FrozenPlacementStatus::Ready) {
            if (slices[i].handle != SIZE_MAX)
                rust_agrees[i] =
                        rust_prefix_matches(frozen[i], results[i], handles[slices[i].handle], slices[i].offset, mine);
            else
                rust_agrees[i] = placement_candidate_rust_matches(frozen[i], results[i]);
        }
#else
        (void)mine;
#endif
    };
    if (pool_)
        pool_->run(frozen.size(), job);
    else
        for (size_t i = 0; i < frozen.size(); ++i)
            job(0, i);

#ifndef NO_RUST
    for (size_t c = 0; c < frozen.size(); ++c)
        if (frozen[c].status == FrozenPlacementStatus::Ready && slices[c].handle == SIZE_MAX)
            ++stats_.rust_direct;
#endif
}

HeAPClusterBatchOutcome PlacementCandidateCoordinator::place(const std::vector<HeAPClusterCandidate> &candidates)
{
    ++stats_.batches;
    stats_.candidates += candidates.size();
    stats_.max_candidates = std::max<uint64_t>(stats_.max_candidates, candidates.size());

    // 1. Prepare and freeze on the owner, in proposal order, while the live
    //    design is stable. Stop at the first unsupported candidate: HeAP handles
    //    it with the live path and everything after it is never proposed.
    std::vector<PreparedPlacementTransaction> prepared;
    std::vector<FrozenPlacementCandidate> frozen;
    prepared.reserve(candidates.size());
    frozen.reserve(candidates.size());
    size_t supported = candidates.size();
    uint64_t batch_queries = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        auto transaction = prepare_placement_transaction(
                arch_, placement_edits_for_candidate(candidates[i].targets, candidates[i].displaced));
        if (!transaction)
            log_error("Failed to preflight a detached HeAP cluster candidate (batch %" PRIu64 ", index %zu).\n",
                      stats_.batches, i);
        auto candidate = freeze_placement_candidate(arch_, transaction);
        if (candidate.status == FrozenPlacementStatus::Unsupported) {
            supported = i;
            ++stats_.unsupported;
            break;
        }
        if (candidate.status != FrozenPlacementStatus::Ready)
            log_error("Failed to freeze a detached HeAP cluster candidate (batch %" PRIu64 ", index %zu).\n",
                      stats_.batches, i);
        batch_queries += candidate.queries.size();
        prepared.push_back(std::move(transaction));
        frozen.push_back(std::move(candidate));
    }
    stats_.evaluated += frozen.size();
    stats_.queries += batch_queries;
    stats_.max_queries = std::max(stats_.max_queries, batch_queries);

    // 2. Detached evaluation.
    std::vector<PlacementCandidateAssessment> results;
    std::vector<uint8_t> rust_agrees;
    evaluate(frozen, results, rust_agrees);

    // 3. Consume strictly in proposal order.
    for (size_t i = 0; i < frozen.size(); ++i) {
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
            auto refrozen = freeze_placement_candidate(arch_, again);
            if (refrozen.status != FrozenPlacementStatus::Ready)
                log_error("Failed to refreeze a stale HeAP cluster candidate.\n");
            PlacementCandidateAssessment assessment;
            if (retry < MAX_STALE_RETRIES) {
                ++stats_.stale_retries;
                std::vector<FrozenPlacementCandidate> one;
                one.push_back(std::move(refrozen));
                std::vector<PlacementCandidateAssessment> one_result;
                std::vector<uint8_t> one_agrees;
                evaluate(one, one_result, one_agrees);
                if (!one_agrees[0])
                    log_error("Rust disagrees with detached C++ for a stale HeAP cluster candidate.\n");
                assessment = std::move(one_result[0]);
            } else {
                ++stats_.synchronous_fallbacks;
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
            stats_.discarded += frozen.size() - i - 1;
            return {HeAPClusterBatchStatus::Committed, i};
        }
        ++stats_.rejected;
    }
    if (supported < candidates.size())
        return {HeAPClusterBatchStatus::Unsupported, supported};
    return {HeAPClusterBatchStatus::NoneLegal, 0};
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
    log_info("  rust frozen handles=%" PRIu64 ", one-shot cross-checks=%" PRIu64 "\n", s.rust_batches, s.rust_direct);
}

NEXTPNR_NAMESPACE_END
