/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The Mistral LAB work
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

// Design 20.1: LAB clustering before placement. Decide which cells share a LAB from connectivity, admitting every
// addition through the arch's own LAB rules on an empty scratch LAB, before HeAP places anything.

#ifndef MISTRAL_LAB_CLUSTERING_H
#define MISTRAL_LAB_CLUSTERING_H

#include <vector>

#include "nextpnr_types.h"

NEXTPNR_NAMESPACE_BEGIN

struct Context;

struct LabClusteringCfg
{
    int fill_target = 16;       // stop growing a cluster at this many cells
    float line_price = 0.1f;    // attraction lost per new external input net a unit brings
    float attraction_floor = 0; // close a cluster when the best candidate's attraction is at or below this
    int tries_per_step = 32;    // candidates tried, best first, before a cluster is closed
};

struct LabClustering
{
    std::vector<std::vector<CellInfo *>> clusters; // each cluster's cells
    dict<IdString, int> cluster_of;                // cell name -> cluster index
    // Statistics over the clusters: cells, nets with every pin inside, distinct nets entering from outside.
    int cells = 0, absorbed_nets = 0;
    long external_nets = 0;
};

// Runs after packing, before placement, with no cell of the LAB kinds placed. Leaves nothing bound.
LabClustering run_lab_clustering(Context *ctx, const LabClusteringCfg &cfg);

NEXTPNR_NAMESPACE_END

#endif
