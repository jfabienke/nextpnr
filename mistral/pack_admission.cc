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

#include "pack_admission.h"

#include <cinttypes>

#include "lab_control_abi.h"
#include "lab_v2.h"
#include "log.h"
#include "register_packing.h"

NEXTPNR_NAMESPACE_BEGIN

PackAdmission::PackAdmission(Context &ctx, bool assign_facts) : ctx_(ctx), assign_facts_(assign_facts)
{
    for (uint32_t lab = 0; lab < ctx.labs.size(); ++lab) {
        const auto &info = ctx.labs[lab];
        if (info.is_mlab)
            continue;
        bool clean = true;
        auto free_bel = [&](BelId bel) { return bel != BelId() && ctx.checkBelAvail(bel); };
        for (const auto &alm : info.alms) {
            for (BelId bel : alm.lut_bels)
                clean = clean && free_bel(bel);
            for (BelId bel : alm.ff_bels)
                clean = clean && free_bel(bel);
        }
        if (clean) {
            lab_ = lab;
            seed_ = info.alms[0].lut_bels[0];
            return;
        }
    }
}

bool PackAdmission::admits(CellInfo *root)
{
    if (!available())
        return true;
    ++checked;
    std::vector<std::pair<CellInfo *, BelId>> placement;
    if (!alm_cluster_placement(ctx_, root, seed_, placement) || placement.size() > BelOverlay::MAX) {
        ++refused;
        return false;
    }
    if (assign_facts_)
        for (auto &member : placement) {
            if (member.first->type == id_MISTRAL_FF)
                ctx_.assign_ff_info(member.first);
            else
                ctx_.assign_comb_info(member.first);
        }
    const LabLegalityMode mode = ctx_.args.lab_legality;
    auto cpp_answer = [&]() {
        BelOverlay overlay;
        for (auto &member : placement)
            overlay.add(member.second, member.first);
        return ctx_.overlay_bels_legal(overlay);
    };
    bool admitted = false;
#ifdef NO_RUST
    admitted = cpp_answer();
#else
    if (mode == LabLegalityMode::Legacy) {
        admitted = cpp_answer();
    } else {
        dict<BelId, CellInfo *> occupancy;
        for (auto &member : placement)
            occupancy[member.second] = member.first;
        const NpnrLabFactsV2 facts =
                capture_lab_v2_overlay(ctx_, lab_, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX, occupancy);
        NpnrLabAssessmentV2 result{};
        const uint32_t call = npnr_mistral_eval_lab_v2(&facts, 1, &result, 1);
        const bool valid = call == NPNR_LAB_CALL_OK && lab_v2_result_valid(facts, result);
        const bool rust = valid && result.status == NPNR_LAB_V2_LEGAL;
        if (!valid)
            ++errors;
        if (mode == LabLegalityMode::Rust) {
            admitted = rust; // an invalid answer refuses: the authority fails closed
        } else {
            const bool cpp = cpp_answer();
            if (!valid || cpp != rust) {
                ++mismatches;
                if (mode == LabLegalityMode::Verify)
                    log_error("Pack admission: the C++ rules %s cluster '%s' and the Rust evaluator %s.\n",
                              cpp ? "admit" : "refuse", ctx_.nameOf(root),
                              !valid ? "gave no valid answer"
                              : rust ? "admits it"
                                     : "refuses it");
                if (mismatches <= 4)
                    log_warning("Pack admission: the C++ rules %s cluster '%s' and the Rust evaluator %s.\n",
                                cpp ? "admit" : "refuse", ctx_.nameOf(root),
                                !valid ? "gave no valid answer"
                                : rust ? "admits it"
                                       : "refuses it");
            }
            admitted = mode == LabLegalityMode::Verify ? rust : cpp;
        }
    }
#endif
    (void)mode;
    if (!admitted)
        ++refused;
    return admitted;
}

void report_pack_admission(const Context &ctx, const PackAdmission &gate, const char *packer)
{
    if (!gate.available()) {
        log_warning("Pack admission %s: no clean LAB to evaluate on; the packer's own rule stands.\n", packer);
        return;
    }
    log_info("Pack admission %s: authority=%s checked=%" PRIu64 " refused=%" PRIu64 " mismatches=%" PRIu64
             " errors=%" PRIu64 ".\n",
             packer, lab_legality_mode_name(ctx.args.lab_legality), gate.checked, gate.refused, gate.mismatches,
             gate.errors);
}

NEXTPNR_NAMESPACE_END
