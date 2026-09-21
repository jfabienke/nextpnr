/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ALM_PAIRING_H
#define MISTRAL_ALM_PAIRING_H

#include <cstdint>

#include "nextpnr.h"
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

// Stage 6 (6b): pair plain LUTs into ALMs before placement.
//
// A Cyclone V ALM holds two LUTs only when they fit its shared-input structure: 64 LUT bits
// between them, eight unique inputs, and only the A and B lines shareable, so a 5-input LUT
// pairs with another 5-input LUT only when they share two nets, with a 4-input LUT when they
// share one, and with anything of three inputs or fewer freely; a 6-input LUT never pairs.
// Carry chains already put two arithmetic cells per ALM. HeAP spreads by bel count and pairs
// plain LUTs only where its random search happens to land a compatible partner, which on the
// full core came to 1.4 cells per ALM against Quartus's 1.92 and a legaliser with no room.
//
// The packer forms pairs greedily and emits each as a two-cell cluster whose placement the
// arch overrides (Arch::getClusterPlacement) to the two LUT halves of one ALM, so HeAP places
// the pair atomically and the legality checks see both cells together. Off by default
// (`--alm-pairing N`): level 1 pairs LUTs that share an input net, level 2 also pairs a LUT
// with one it drives or is driven by, level 3 pairs whatever is left and compatible.
struct AlmPairingReport
{
    uint64_t eligible = 0;    // plain LUTs that could take a partner
    uint64_t pairs = 0;       // pairs formed
    uint64_t by_shared = 0;   // pairs sharing at least one input net
    uint64_t by_link = 0;     // pairs connected through an output (level 2)
    uint64_t by_any = 0;      // pairs with no relation (level 3)
    uint64_t unpaired = 0;    // eligible LUTs left single
    uint64_t shared_nets = 0; // input nets shared inside pairs, summed
    uint64_t refused = 0;     // pairs the search chose and the LAB legality authority refused (design 13)
};

// Declared input count of a pairable LUT type, or -1 when the type never pairs.
int alm_pair_lut_inputs(IdString type);
// The ALM rule: 64 bits, eight unique inputs with two shareable lines.
bool alm_pair_compatible(const CellInfo *a, const CellInfo *b);
// Structural test used by the arch's cluster placement: a root with one child at relative z 1.
bool is_alm_pair_root(const CellInfo *root);
AlmPairingReport pair_alm_luts(Context &ctx, int level);
void report_alm_pairing(const AlmPairingReport &r);

NEXTPNR_NAMESPACE_END

#endif
