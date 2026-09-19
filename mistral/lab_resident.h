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

#ifndef MISTRAL_LAB_RESIDENT_H
#define MISTRAL_LAB_RESIDENT_H

#include <array>
#include <cstdint>
#include <thread>
#include <vector>

#include "lab_v2_abi.h"
#include "nextpnr.h"
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct Arch;

// Stage 6 (parity): the owner of a resident LAB snapshot session for the Rust legality modes.
//
// The capture path built and validated a whole-LAB record for every query; on the full core the
// legaliser asks tens of millions of times and the capture was 62% of each query, the Rust
// validation of the record another 19%. The resident session keeps every LAB's facts in Rust and
// receives, per query, only the ALMs whose bindings or facts changed since its last query (the
// arch marks them in `Arch::lab_alm_dirty` from the same hooks that version LABs for reuse), so a
// query costs one small patch, an incremental validation, and an evaluation over cached counts.
// Net ids travel as run-stable keys (the net name's index plus one); the rules compare them for
// equality only. Created on the first dispatch of a non-legacy mode and owned by the arch; one
// thread, which is the placer's owner thread by construction.
struct ResidentLabLegality
{
    explicit ResidentLabLegality(const Arch &arch);
    ~ResidentLabLegality();
    ResidentLabLegality(const ResidentLabLegality &) = delete;
    ResidentLabLegality &operator=(const ResidentLabLegality &) = delete;

    bool available() const { return handle_ != nullptr; }
    // Sends the LAB's dirty ALMs (after a reset if it was never sent), evaluates the query, and
    // returns the call status; `out` is written only on NPNR_LAB_CALL_OK.
    // With `recompute` the session recomputes the ALM input counts from the facts (the harness
    // modes); without it the arch's counts travel with the patches as facts.
    uint32_t evaluate(const Arch &arch, uint32_t lab, NpnrLabQueryV2 query, uint32_t query_alm, bool recompute,
                      NpnrLabVerdictV2 &out);

    uint64_t evaluations = 0, resets = 0, trials = 0, commits = 0, restored = 0;

  private:
    NpnrLabResidentV2 *handle_ = nullptr;
    std::thread::id owner_;
    // Per LAB bel (ten ALMs, two LUT halves then four registers each): where the arch keeps the
    // bel's occupant, the occupant the committed facts were built from (an occupant back to it,
    // with no facts change reported since, means the committed facts without a rebuild), the
    // committed facts, the last trial sent, and whether a trial is pending. The legaliser binds a
    // candidate, asks, and usually unbinds; the annealer swaps, asks, and usually reverts. So a
    // changed bel travels as a trial the session holds in view for that evaluation, and is
    // committed only when the next query of the LAB finds it unchanged.
    std::vector<CellInfo *const *> bound_;
    std::vector<const CellInfo *> occupant_;
    std::vector<NpnrLabLutV2> committed_lut_, pending_lut_;
    std::vector<NpnrLabFfV2> committed_ff_, pending_ff_;
    std::vector<uint8_t> has_pending_;
    std::array<NpnrBelPatchV2, 60> batch_{};
};

NEXTPNR_NAMESPACE_END

#endif
