/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_PLACEMENT_COORDINATOR_H
#define MISTRAL_PLACEMENT_COORDINATOR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "nextpnr.h"
#include "placement_pool.h"
#include "placement_transaction.h"
#include "placer_heap.h"

NEXTPNR_NAMESPACE_BEGIN

// Counters for the Stage 4D batch path. Owner-thread values; worker tallies
// are folded in after each parallel section.
struct PlacementBatchStats
{
    uint64_t batches = 0;
    uint64_t candidates = 0;            // candidates handed to the coordinator
    uint64_t evaluated = 0;             // candidates frozen and evaluated detached (supported prefix)
    uint64_t queries = 0;               // V2 facts evaluated by C++ (Rust evaluates the same prefix)
    uint64_t committed = 0;             // exactly one per batch that commits
    uint64_t rejected = 0;              // illegal candidates before the committed one
    uint64_t discarded = 0;             // evaluated after the committed one; never consumed
    uint64_t unsupported = 0;           // batches truncated at an unsupported candidate
    uint64_t stale = 0;                 // commits that found a changed stamp or owner (must stay 0)
    uint64_t stale_retries = 0;         // asynchronous re-evaluations of a stale proposal
    uint64_t synchronous_fallbacks = 0; // proposals re-evaluated inline by the owner
    uint64_t rust_batches = 0;          // frozen Rust handles created (one per candidate, by its worker)
    uint64_t rust_direct = 0;           // candidates cross-checked through the one-shot FFI instead
    uint64_t frozen_by_workers = 0;     // candidates whose overlay capture ran on a worker (owner = worker 0)
    uint64_t pool_runs = 0;             // parallel sections
    uint64_t pool_spin_wakes = 0;       // worker wake-ups served by the spin phase, no blocking
    uint64_t pool_block_wakes = 0;      // worker wake-ups that had to block on the condition variable
    uint64_t max_candidates = 0;
    uint64_t max_queries = 0;
};

// Owner-side scheduler for speculated HeAP cluster candidates. The owner
// prepares every candidate (binding edits plus revision stamp) and decides the
// supported prefix; workers then freeze the candidate's whole-LAB facts with
// its occupancy overlay, create their own Rust-owned handle, evaluate the
// detached C++ reference, and require Rust agreement. The owner consumes
// assessments strictly in proposal order and commits the first legal one.
// Workers read the live design only while the owner is blocked in the
// parallel section, so the design is immutable for them; nothing they touch
// interns an IdString or updates a mutable cache.
class PlacementCandidateCoordinator
{
  public:
    static constexpr unsigned MAX_STALE_RETRIES = 2;

    PlacementCandidateCoordinator(Arch &arch, unsigned workers);
    ~PlacementCandidateCoordinator();
    PlacementCandidateCoordinator(const PlacementCandidateCoordinator &) = delete;
    PlacementCandidateCoordinator &operator=(const PlacementCandidateCoordinator &) = delete;

    unsigned workers() const { return workers_; }
    const PlacementBatchStats &stats() const { return stats_; }

    // HeAP batch callback. Candidates are in proposal order.
    HeAPClusterBatchOutcome place(const std::vector<HeAPClusterCandidate> &candidates);

    // Parallel freeze plus evaluation of prepared transactions: frozen[i] is
    // captured by whichever worker claims i. Never mutates. `rust_agrees[i]`
    // is false when the Rust evaluator differs from the C++ result.
    void freeze_and_evaluate(const std::vector<PreparedPlacementTransaction> &prepared,
                             std::vector<FrozenPlacementCandidate> &frozen,
                             std::vector<PlacementCandidateAssessment> &results, std::vector<uint8_t> &rust_agrees);

    // Detached evaluation of already frozen candidates (tests and the stale
    // retry path). Never mutates.
    void evaluate(const std::vector<FrozenPlacementCandidate> &frozen,
                  std::vector<PlacementCandidateAssessment> &results, std::vector<uint8_t> &rust_agrees);

  private:
    struct WorkerTally;
    void assess(unsigned worker, const FrozenPlacementCandidate &frozen, PlacementCandidateAssessment &result,
                uint8_t &rust_agrees, WorkerTally &tally);
    void run_jobs(size_t count, const std::function<void(unsigned, size_t)> &job);
    Arch &arch_;
    unsigned workers_;
    std::unique_ptr<PlacementWorkerPool> pool_;
    std::vector<std::vector<NpnrLabAssessmentV2>> scratch_;
    PlacementBatchStats stats_;
};

// Stage 5 (1c): the annealer's swap seam. assess evaluates the live LAB rules
// under a BelOverlay of the edits (no binding, no serialisation; about the
// cost of the live checks) and stamps a Legal answer with the current
// revision; commit rechecks the stamp and every expected owner, then applies
// the edits. Unsupported when any edited BEL is not a LAB BEL. Parity with
// the live path is validated by the annealer's shadow mode.
Placer1SwapAssessment mistral_assess_swap(Context *ctx, const std::vector<Placer1SwapEdit> &edits);
bool mistral_commit_swap(Context *ctx, const std::vector<Placer1SwapEdit> &edits,
                         const Placer1SwapAssessment &assessment);

// Shared with Arch::place(): convert HeAP's candidate into ordered binding edits.
std::vector<PlacementBindingEdit>
placement_edits_for_candidate(const std::vector<std::pair<CellInfo *, BelId>> &targets,
                              const HeAPDisplacedBindings &displaced);

void report_placement_batch_stats(const PlacementCandidateCoordinator &coordinator);

NEXTPNR_NAMESPACE_END

#endif
