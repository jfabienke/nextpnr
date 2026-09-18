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

#ifndef MISTRAL_REGISTER_PACKING_H
#define MISTRAL_REGISTER_PACKING_H

#include <cstdint>
#include <utility>
#include <vector>

#include "nextpnr.h"
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

// Stage 6 (6g): pack a register with the LUT that drives it into one ALM.
//
// A Cyclone V ALM half feeds its register from its LUT output directly (the checker admits one
// register per half; the second is marked unusable in lab.cc); a register whose data comes from
// anywhere else costs a LAB input line, a fabric route, and either the half's unused LUT slot (a
// route-through) or the half's E/F path. Quartus packs 95% of LUT-driven registers with their
// LUT; HeAP, placing registers and LUTs as independent cells, lands 7 to 16% (unit 6f). The
// packer attaches one such register to its LUT's cluster as a child at relative z 2 (the root's
// half) or 4 (a pair partner's half), and the arch's cluster placement
// (Arch::getClusterPlacement) puts it on the register bel of the half the LUT lands on. A LUT
// already in a carry chain or an MLAB group is left alone. Off by default (`--register-packing`).
struct RegisterPackingReport
{
    uint64_t registers = 0;        // MISTRAL_FF cells seen
    uint64_t lut_driven = 0;       // whose data input is a plain LUT's output
    uint64_t packed = 0;           // clustered with that LUT
    uint64_t onto_single = 0;      // the LUT became a cluster root for it
    uint64_t onto_pair_root = 0;   // the LUT was a pair's root
    uint64_t onto_pair_child = 0;  // the LUT was a pair's partner
    uint64_t lut_full = 0;         // the LUT's register slot was already taken
    uint64_t lut_clustered = 0;    // the LUT sits in a chain or another cluster
    uint64_t ff_constrained = 0;   // the register already has a cluster or children
    uint64_t control_conflict = 0; // its control set cannot share a LAB with the cluster's other registers
};

// A plain-LUT root whose children are a pair partner at relative z 1 and/or registers at 2 or 4.
bool is_alm_cluster_root(const CellInfo *root);

// Places a pair or register cluster on one ALM from the root's bel: the partner on the other
// LUT half, each register on a register bel of its LUT's half. False when the bel cannot host
// it (a pair's root seeds only from the first half).
bool alm_cluster_placement(const Arch &arch, CellInfo *root, BelId root_bel,
                           std::vector<std::pair<CellInfo *, BelId>> &placement);

// The LAB control model's verdict on these registers alone in a LAB (clock, enable, clears and
// load lines): a cluster whose own registers conflict could never be placed.
bool registers_share_a_lab(const std::vector<const CellInfo *> &ffs);

// Runs after Arch::assignArchInfo, which fills the registers' control sets.
RegisterPackingReport pack_registers(Context &ctx);
void report_register_packing(const RegisterPackingReport &r);

NEXTPNR_NAMESPACE_END

#endif
