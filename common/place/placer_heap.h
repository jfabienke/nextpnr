/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  gatecat <gatecat@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  [[cite]] HeAP
 *  Analytical Placement for Heterogeneous FPGAs, Marcel Gort and Jason H. Anderson
 *  https://janders.eecg.utoronto.ca/pdfs/marcelfpl12.pdf
 *
 *  [[cite]] SimPL
 *  SimPL: An Effective Placement Algorithm, Myung-Chul Kim, Dong-Jin Lee and Igor L. Markov
 *  http://www.ece.umich.edu/cse/awards/pdfs/iccad10-simpl.pdf
 */

#ifndef PLACER_HEAP_H
#define PLACER_HEAP_H
#include "log.h"
#include "nextpnr.h"
#include "placer1.h"

#include <functional>

NEXTPNR_NAMESPACE_BEGIN

// State displaced by a provisional clustered HeAP move. Rollback must preserve
// placement strength as well as occupancy because strength affects later rip-up
// eligibility and therefore the search trajectory.
struct HeAPDisplacedBinding
{
    CellInfo *cell = nullptr;
    PlaceStrength strength = STRENGTH_NONE;
};
using HeAPDisplacedBindings = dict<BelId, HeAPDisplacedBinding>;

enum class HeAPClusterTransactionOutcome
{
    Committed,
    Rejected,
    Unsupported
};

// One speculative clustered move: the cells to bind and the complete set of
// bindings it displaces. Candidates in a batch are generated serially under one
// unchanged placement and are independent until one of them commits.
struct HeAPClusterCandidate
{
    std::vector<std::pair<CellInfo *, BelId>> targets;
    HeAPDisplacedBindings displaced;
};

enum class HeAPClusterBatchStatus
{
    Committed,   // candidate `index` was committed; later candidates were discarded
    Unsupported, // candidate `index` needs the live bind/check/revert path; earlier ones were illegal
    NoneLegal    // every candidate was evaluated and rejected
};

struct HeAPClusterBatchOutcome
{
    HeAPClusterBatchStatus status = HeAPClusterBatchStatus::NoneLegal;
    size_t index = 0;
};

struct PlacerHeapCfg
{
    PlacerHeapCfg(Context *ctx);

    float alpha, beta;
    float criticalityExponent;
    float timingWeight;
    bool timing_driven;
    float solverTolerance;
    bool placeAllAtOnce;
    float netShareWeight;
    bool parallelRefine;
    bool chainRipup;
    int cell_placement_timeout;

    int hpwl_scale_x, hpwl_scale_y;
    int spread_scale_x, spread_scale_y;

    // These cell types will be randomly locked to prevent singular matrices
    pool<IdString> ioBufTypes;
    // These cell types are part of the same unit (e.g. slices split into
    // components) so will always be spread together
    std::vector<pool<BelBucketId>> cellGroups;

    // this is an optional callback to prioritise certain cells/clusters for legalisation
    std::function<float(Context *, CellInfo *)> get_cell_legalisation_weight = [](Context *, CellInfo *) { return 1; };

    // Optional architecture-owned frozen transaction. Unsupported candidates
    // use HeAP's original bind/check/revert path.
    std::function<HeAPClusterTransactionOutcome(Context *, const std::vector<std::pair<CellInfo *, BelId>> &,
                                                const HeAPDisplacedBindings &)>
            place_cluster_transaction;

    // Optional batch form. With clusterLookahead > 0 the legaliser generates up to
    // that many candidates ahead of evaluation, in exactly the serial search
    // order, and the callback must evaluate them detached and commit the first
    // legal one in sequence order. Search state (RNG, radius, counters) is
    // restored to the point just after the committed candidate, so the result is
    // identical to the serial trajectory. Requires place_cluster_transaction for
    // the Unsupported fallback. Set programmatically by the architecture; it is
    // deliberately not a settings key so that enabling it interns no IdString.
    std::function<HeAPClusterBatchOutcome(Context *, const std::vector<HeAPClusterCandidate> &)>
            place_cluster_transactions;
    int clusterLookahead = 0;

    // Passed to the simulated-annealing refinement (see Placer1Cfg).
    std::function<Placer1SwapAssessment(Context *, const std::vector<Placer1SwapEdit> &)> assess_swap;
    std::function<bool(Context *, const std::vector<Placer1SwapEdit> &, const Placer1SwapAssessment &)> commit_swap;
    bool swap_seam_shadow = false;
    int swap_batch = 0;
    unsigned swap_threads = 1;

    bool disableCtrlSet;

    /*
    Control set API
    HeAP legalisation can be sped up by directly searching for nearby tiles to place an FF with a compatible control
    set. Only one shared control set is currently supported, however, as a full validity check is always performed too,
    this doesn't need to encompass every possible incompatibility (this is only for performance/QoR not correctness)

    ff_bel_bucket is the bel bucket ID for the flipflop (or logic cell if combined with LUT) bel type

    ff_control_set_groups contains the Z-location of flipflops in a control set group.
    Each entry in this represents a SLICE, i.e. the set of flipflops that share the control set. In XC7 this would be
    the two SLICEs in a tile.

    get_cell_control_set should return a unique index for every control set possibility. i.e. if this function returns
    the same value the flipflops could be placed in the same group.
    */

    BelBucketId ff_bel_bucket = BelBucketId();
    std::vector<std::vector<int>> ff_control_set_groups;

    // ctrl_set_max_radius is specified as a schedule per iteration, in general this should decrease over time
    std::vector<int> ctrl_set_max_radius;

    // TODO: control sets might have a hierarchy, like ultrascale+ CE vs CLK/SR
    std::function<int32_t(Context *, const CellInfo *)> get_cell_control_set = [](Context *, const CellInfo *) {
        return -1;
    };
};

void restore_heap_cluster_bindings(Context *ctx, const HeAPDisplacedBindings &bindings);

extern bool placer_heap(Context *ctx, PlacerHeapCfg cfg);
NEXTPNR_NAMESPACE_END
#endif
