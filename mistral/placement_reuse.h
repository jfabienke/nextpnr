/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_PLACEMENT_REUSE_H
#define MISTRAL_PLACEMENT_REUSE_H

#include <cstdint>
#include <string>

#include "reuse_plan.h"

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
    uint64_t released = 0;         // 3b: transplants released by region expansion
};

struct ReusePlan;

// Stage 5 (3a): computes the per-cell decisions without touching the design.
// Must run after packing and before placement. Fails the run on malformed
// input.
void plan_placement_reuse(Context &ctx, const std::string &path, ReusePlan &plan);

// Applies a plan: every Reuse decision becomes a hard `BEL` attribute.
PlacementReuseReport apply_placement_reuse(Context &ctx, const ReusePlan &plan);

// Plans and applies in one step (the Stage 4E entry point).
PlacementReuseReport apply_placement_reuse(Context &ctx, const std::string &path);

// Stage 5 (3b): region expansion after a failed local repair. Turns every
// Reuse decision whose previous BEL lies within Manhattan `radius` (tile
// units) of an anchor into Released, and clears its `BEL` attribute, so the
// next placer attempt is free to move it. Anchors are the previous BELs of
// changed cells and, for added cells, the previous BELs of the cells on
// their nets. A negative radius releases every transplant. Returns the
// number of cells released by this call.
unsigned release_placement_region(Context &ctx, ReusePlan &plan, int radius);

void report_placement_reuse(const PlacementReuseReport &report);

NEXTPNR_NAMESPACE_END

#endif
