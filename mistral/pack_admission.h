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

#ifndef MISTRAL_PACK_ADMISSION_H
#define MISTRAL_PACK_ADMISSION_H

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// Pack-time admission (design section 13): before a packer commits a cluster that must sit on
// one ALM (an ALM pair of unit 6b, a LUT with its registers of unit 6g), the cluster is laid on
// a clean ALM through the cluster placement the placer uses and evaluated by the run's LAB
// legality authority, through the seams the placer's own paths have: the bel overlay for the
// C++ rules, the overlay capture for the Rust evaluator. Nothing is bound. The packers' own
// rules remain as search filters; this is what admits the result, so no cluster leaves the
// packer that the placer's authority would refuse at every ALM.
struct PackAdmission
{
    // `assign_facts`: assign the members' arch facts before evaluating, for a packer that runs
    // before assignArchInfo (the pairing does; register packing runs after it).
    PackAdmission(Context &ctx, bool assign_facts);
    // Whether the cluster rooted at `root`, with its children attached, is legal on a clean ALM.
    // Without a clean LAB to ask on, the packer's own rule stands.
    bool admits(CellInfo *root);
    bool available() const { return seed_ != BelId(); }

    uint64_t checked = 0, refused = 0, mismatches = 0, errors = 0;

  private:
    Context &ctx_;
    const bool assign_facts_;
    uint32_t lab_ = 0;
    BelId seed_; // the first LUT bel of a clean, non-MLAB LAB
};

// `Pack admission <packer>: authority=... checked=... refused=... mismatches=... errors=...`
void report_pack_admission(const Context &ctx, const PackAdmission &gate, const char *packer);

NEXTPNR_NAMESPACE_END

#endif
