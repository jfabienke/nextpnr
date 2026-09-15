/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_PLACEMENT_COORDINATOR_H
#define MISTRAL_PLACEMENT_COORDINATOR_H

#include <cstdint>
#include <memory>
#include <vector>

#include "nextpnr.h"
#include "placement_transaction.h"
#include "placer_heap.h"

NEXTPNR_NAMESPACE_BEGIN

// Counters for the Stage 4D batch path. All are owner-thread values; workers
// never touch them.
struct PlacementBatchStats
{
    uint64_t batches = 0;
    uint64_t candidates = 0;            // candidates handed to the coordinator
    uint64_t evaluated = 0;             // candidates evaluated detached (supported prefix)
    uint64_t queries = 0;               // V2 facts evaluated by C++ (Rust evaluates the same prefix)
    uint64_t committed = 0;             // exactly one per batch that commits
    uint64_t rejected = 0;              // illegal candidates before the committed one
    uint64_t discarded = 0;             // evaluated after the committed one; never consumed
    uint64_t unsupported = 0;           // batches truncated at an unsupported candidate
    uint64_t stale = 0;                 // commits that found a changed stamp or owner (must stay 0)
    uint64_t stale_retries = 0;         // asynchronous re-evaluations of a stale proposal
    uint64_t synchronous_fallbacks = 0; // proposals re-evaluated inline by the owner
    uint64_t rust_batches = 0;          // frozen Rust handles created
    uint64_t rust_direct = 0;           // candidates cross-checked through the one-shot FFI instead
    uint64_t max_candidates = 0;
    uint64_t max_queries = 0;
};

// Owner-side scheduler for speculated HeAP cluster candidates. The owner
// prepares and freezes every candidate while it holds the live design, workers
// evaluate the frozen facts (C++ authority plus a Rust cross-check that must
// agree exactly), and the owner consumes assessments strictly in proposal
// order and commits the first legal one. Nothing but the owner ever mutates
// the design, so a stale stamp at commit time is a bug, not an expected event;
// it is still checked and, if seen, the proposal is re-evaluated synchronously.
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

    // Detached evaluation of an already frozen batch, in parallel when workers
    // are available. `rust_agrees[i]` is false when the Rust evaluator differs
    // from the C++ result for candidate i. Exposed for tests; it never mutates.
    void evaluate(const std::vector<FrozenPlacementCandidate> &frozen,
                  std::vector<PlacementCandidateAssessment> &results, std::vector<uint8_t> &rust_agrees);

  private:
    struct Pool;
    Arch &arch_;
    unsigned workers_;
    std::unique_ptr<Pool> pool_;
    PlacementBatchStats stats_;
};

// Shared with Arch::place(): convert HeAP's candidate into ordered binding edits.
std::vector<PlacementBindingEdit>
placement_edits_for_candidate(const std::vector<std::pair<CellInfo *, BelId>> &targets,
                              const HeAPDisplacedBindings &displaced);

void report_placement_batch_stats(const PlacementCandidateCoordinator &coordinator);

NEXTPNR_NAMESPACE_END

#endif
