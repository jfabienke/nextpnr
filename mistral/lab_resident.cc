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

#include "lab_resident.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "lab_v2.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
constexpr int kBelsPerLab = 60;
constexpr uint64_t kAllBelsDirty = (uint64_t(1) << kBelsPerLab) - 1;
constexpr uint64_t kNeedsReset = uint64_t(1) << kBelsPerLab;
} // namespace

ResidentLabLegality::ResidentLabLegality(const Arch &arch) : owner_(std::this_thread::get_id())
{
#ifndef NO_RUST
    NpnrLabResidentV2 *handle = nullptr;
    const uint32_t status =
            npnr_mistral_resident_v2_create(uint32_t(arch.labs.size()), resolved_lab_input_limit(), &handle);
    if (status != NPNR_LAB_CALL_OK || handle == nullptr)
        log_error("LAB legality: the resident snapshot session could not be created (status %u).\n", status);
    handle_ = handle;
    const size_t bels = arch.labs.size() * kBelsPerLab;
    arch.lab_bel_dirty.assign(arch.labs.size(), kAllBelsDirty | kNeedsReset);
    arch.lab_bel_refacts.assign(arch.labs.size(), kAllBelsDirty);
    bound_.resize(bels);
    for (size_t lab = 0; lab < arch.labs.size(); ++lab)
        for (int alm = 0; alm < 10; ++alm) {
            const ALMInfo &info = arch.labs[lab].alms[alm];
            for (int i = 0; i < 2; ++i)
                bound_[lab * kBelsPerLab + alm * 6 + i] = &arch.bel_data(info.lut_bels[i]).bound;
            for (int i = 0; i < 4; ++i)
                bound_[lab * kBelsPerLab + alm * 6 + 2 + i] = &arch.bel_data(info.ff_bels[i]).bound;
        }
    occupant_.assign(bels, nullptr);
    committed_lut_.assign(arch.labs.size() * 20, NpnrLabLutV2{});
    pending_lut_.assign(arch.labs.size() * 20, NpnrLabLutV2{});
    committed_ff_.assign(arch.labs.size() * 40, NpnrLabFfV2{});
    pending_ff_.assign(arch.labs.size() * 40, NpnrLabFfV2{});
    has_pending_.assign(bels, 0);
#else
    (void)arch;
#endif
}

ResidentLabLegality::~ResidentLabLegality()
{
#ifndef NO_RUST
    if (handle_ != nullptr)
        npnr_mistral_resident_v2_destroy(handle_);
#endif
}

uint32_t ResidentLabLegality::evaluate(const Arch &arch, uint32_t lab, NpnrLabQueryV2 query, uint32_t query_alm,
                                       bool recompute, NpnrLabVerdictV2 &out)
{
#ifndef NO_RUST
    NPNR_ASSERT(handle_ != nullptr && std::this_thread::get_id() == owner_ && lab < arch.labs.size());
    uint64_t &dirty = arch.lab_bel_dirty[lab];
    uint64_t &refacts = arch.lab_bel_refacts[lab];
    const LABInfo &lab_info = arch.labs[lab];
    if (dirty & kNeedsReset) {
        const uint32_t status = npnr_mistral_resident_v2_reset(handle_, lab, lab_info.is_mlab ? 1u : 0u);
        if (status != NPNR_LAB_CALL_OK)
            return status;
        ++resets;
        dirty = kAllBelsDirty;
        refacts = kAllBelsDirty;
        std::fill_n(committed_lut_.begin() + size_t(lab) * 20, 20, NpnrLabLutV2{});
        std::fill_n(committed_ff_.begin() + size_t(lab) * 40, 40, NpnrLabFfV2{});
        std::fill_n(has_pending_.begin() + size_t(lab) * kBelsPerLab, kBelsPerLab, uint8_t(0));
    }
    // The batch lives in a member buffer: a 60-record array on the stack cost a stack probe per query.
    std::array<NpnrBelPatchV2, kBelsPerLab> &batch = batch_;
    uint32_t count = 0;
    uint64_t still_dirty = 0;
    const size_t base = size_t(lab) * kBelsPerLab;
    // Just reset, or more changed bels than the session holds in view (a chain bound before any
    // query): the state travels as commits, since it is the state.
    const bool resync = (refacts == kAllBelsDirty && dirty == kAllBelsDirty) ||
                        __builtin_popcountll(dirty) > NPNR_LAB_RESIDENT_MAX_TRIALS;
    for (uint64_t bits = dirty; bits != 0; bits &= bits - 1) {
        const int bel = __builtin_ctzll(bits);
        const uint64_t bit = uint64_t(1) << bel;
        const uint8_t alm = uint8_t(bel / 6), slot = uint8_t(bel % 6);
        const CellInfo *now = *bound_[base + bel];
        if (!(refacts & bit) && now == occupant_[base + bel]) {
            ++restored;
            has_pending_[base + bel] = 0;
            continue;
        }
        NpnrBelPatchV2 &patch = batch[count];
        capture_bel_v2_keyed(arch, lab, alm, slot, patch);
        const bool is_lut = slot < 2;
        void *committed = is_lut ? static_cast<void *>(&committed_lut_[size_t(lab) * 20 + alm * 2 + slot])
                                 : static_cast<void *>(&committed_ff_[size_t(lab) * 40 + alm * 4 + (slot - 2)]);
        void *pending = is_lut ? static_cast<void *>(&pending_lut_[size_t(lab) * 20 + alm * 2 + slot])
                               : static_cast<void *>(&pending_ff_[size_t(lab) * 40 + alm * 4 + (slot - 2)]);
        const void *fresh = is_lut ? static_cast<const void *>(&patch.lut) : static_cast<const void *>(&patch.ff);
        const size_t size = is_lut ? sizeof(NpnrLabLutV2) : sizeof(NpnrLabFfV2);
        if (std::memcmp(fresh, committed, size) == 0) {
            ++restored;
            has_pending_[base + bel] = 0;
            occupant_[base + bel] = now;
            refacts &= ~bit;
            continue;
        }
        patch.alm = alm;
        patch.slot = slot;
        patch.alm_inputs = lab_info.alms[alm].unique_input_count;
        patch.commit = resync || (has_pending_[base + bel] && std::memcmp(fresh, pending, size) == 0) ? 1u : 0u;
        if (!patch.commit) {
            std::memcpy(pending, fresh, size);
            has_pending_[base + bel] = 1;
            still_dirty |= bit; // rebuilt at the next query, to be committed if unchanged
        }
        ++count;
    }
    const uint32_t status =
            npnr_mistral_resident_v2_evaluate(handle_, lab, count ? batch.data() : nullptr, count, query, query_alm,
                                              recompute ? NPNR_LAB_RESIDENT_RECOMPUTE_COUNTS : 0u, &out);
    ++evaluations;
    if (status == NPNR_LAB_CALL_OK && out.status != NPNR_LAB_V2_MALFORMED) {
        dirty = still_dirty;
        for (uint32_t i = 0; i < count; ++i) {
            const NpnrBelPatchV2 &patch = batch[i];
            const int bel = int(patch.alm) * 6 + int(patch.slot);
            if (patch.commit) {
                if (patch.slot < 2)
                    committed_lut_[size_t(lab) * 20 + patch.alm * 2 + patch.slot] = patch.lut;
                else
                    committed_ff_[size_t(lab) * 40 + patch.alm * 4 + (patch.slot - 2)] = patch.ff;
                has_pending_[base + bel] = 0;
                occupant_[base + bel] = *bound_[base + bel];
                refacts &= ~(uint64_t(1) << bel);
                ++commits;
            } else {
                ++trials;
            }
        }
    }
    return status;
#else
    (void)arch;
    (void)lab;
    (void)query;
    (void)query_alm;
    (void)recompute;
    (void)out;
    return NPNR_LAB_CALL_BAD_SNAPSHOT;
#endif
}

NEXTPNR_NAMESPACE_END
