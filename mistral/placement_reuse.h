/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_PLACEMENT_REUSE_H
#define MISTRAL_PLACEMENT_REUSE_H

#include <cstdint>
#include <string>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct Context;

// Stage 4E-2: conservative placement reuse for an edited packed design.
//
// The previous run's output JSON supplies, per cell, its final BEL and a
// semantic signature (type, parameters, attributes, and port-to-net-name
// connectivity with route-through buffers folded back out). A current cell is
// reused only when a cell of the same name exists in the previous run with an
// identical signature; it then receives a hard `BEL` attribute and HeAP's
// constraint placer binds and validity-checks it before anything else. Every
// other current cell is dirty and is placed by the normal flow. Removing an
// occupant never makes the remaining occupants of a LAB illegal under the
// current rules, and changed or new cells are never constrained, so the
// transplanted set is legal whenever the previous placement was; the
// constraint placer's validity check makes any violation fatal rather than
// silent. Preparation and routing always run in full.
struct PlacementReuseReport
{
    uint64_t previous_cells = 0; // cells in the previous output, route-throughs excluded
    uint64_t previous_routethru = 0;
    uint64_t current_cells = 0;
    uint64_t matched = 0;          // same name, same signature: BEL transplanted
    uint64_t changed = 0;          // same name, different signature: dirty
    uint64_t added = 0;            // no previous cell of that name: dirty
    uint64_t removed = 0;          // previous cell with no current counterpart
    uint64_t user_constrained = 0; // current cells already bound or carrying a BEL attribute (QSF pins, user)
    uint64_t missing_bel = 0;      // matched cells whose previous BEL no longer resolves
};

// Parses `path` and annotates matching cells with `BEL`. Must run after
// packing and before placement. Fails the run on malformed input.
PlacementReuseReport apply_placement_reuse(Context &ctx, const std::string &path);

void report_placement_reuse(const PlacementReuseReport &report);

NEXTPNR_NAMESPACE_END

#endif
