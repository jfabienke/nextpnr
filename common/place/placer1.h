/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Claire Xenia Wolf <claire@yosyshq.com>
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
 */
#ifndef PLACE_H
#define PLACE_H

#include <functional>
#include <vector>

#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// Optional architecture seam for detached swap evaluation. One edit of a swap:
// `bel` currently holds `expected` (nullptr when empty) at `expected_strength`
// and would hold `replacement` at `replacement_strength` afterwards.
struct Placer1SwapEdit
{
    BelId bel;
    CellInfo *expected = nullptr;
    PlaceStrength expected_strength = STRENGTH_NONE;
    CellInfo *replacement = nullptr;
    PlaceStrength replacement_strength = STRENGTH_NONE;
    // Whether the assessment certifies this bel's location after the move, as the live path's
    // isBelLocationValid would. A swap certifies both its bels; a chain move certifies the bels its
    // moved cells land on and not the ones it vacates (design section 18.1).
    bool certify = true;
};

struct Placer1SwapAssessment
{
    enum class Status
    {
        Unsupported, // the placer must use its live bind/check/revert path
        Illegal,
        Legal
    };
    Status status = Status::Unsupported;
    // Architecture-defined freshness stamp of a Legal answer; commit refuses a stale one.
    uint64_t stamp_session = 0, stamp_revision = 0;
};

struct Placer1Cfg
{
    Placer1Cfg(Context *ctx);
    float constraintWeight, netShareWeight;
    int minBelsForGridPick;
    float startTemp;
    int timingFanoutThresh;
    bool timing_driven;
    int hpwl_scale_x, hpwl_scale_y;

    // Assess a two-cell swap's legality against the live design without
    // mutating it. When set, the annealer evaluates the swap's cost delta from
    // a position overlay, draws the acceptance RNG in the original order, and
    // touches bindings only for an accepted swap through commit_swap. Cluster
    // swaps and net-share scoring stay on the live path.
    std::function<Placer1SwapAssessment(Context *, const std::vector<Placer1SwapEdit> &)> assess_swap;
    std::function<bool(Context *, const std::vector<Placer1SwapEdit> &, const Placer1SwapAssessment &)> commit_swap;
    // Also run the live path for every seam-evaluated swap and require identical
    // legality and cost deltas; the live result decides. For validation.
    bool swap_seam_shadow = false;

    // Batched refinement (requires assess_swap). Candidates are generated from
    // the state at batch start, evaluated detached on `threads` workers, and
    // consumed in order with a per-candidate acceptance stream; a candidate whose
    // read set overlaps an earlier accepted swap is re-evaluated at its turn.
    // Results depend on the seed and swap_batch, not on threads. 0 = off.
    int swap_batch = 0;
    unsigned threads = 1;
};

extern bool placer1(Context *ctx, Placer1Cfg cfg);
extern bool placer1_refine(Context *ctx, Placer1Cfg cfg);

NEXTPNR_NAMESPACE_END

#endif // PLACE_H
