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

#include "register_packing.h"
#include "pack_admission.h"

#include <algorithm>
#include <cinttypes>
#include <unordered_map>
#include <unordered_set>

#include "alm_pairing.h"
#include "lab_control_abi.h"
#include "lab_model.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

bool is_plain_lut(IdString type)
{
    return type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6);
}

// 2 and 3 are the root's half (first and second register bel), 4 and 5 the partner's.
bool is_register_child(const CellInfo *c) { return c->type == id_MISTRAL_FF && c->constr_z >= 2 && c->constr_z <= 5; }

} // namespace

bool is_alm_cluster_root(const CellInfo *root)
{
    if (root->cluster != root->name || root->constr_abs_z || root->constr_z != 0 || !is_plain_lut(root->type) ||
        root->constr_children.empty())
        return false;
    int partners = 0;
    for (const CellInfo *child : root->constr_children) {
        if (child->cluster != root->name || child->constr_x != 0 || child->constr_y != 0 || child->constr_abs_z)
            return false;
        if (is_plain_lut(child->type) && child->constr_z == 1)
            ++partners;
        else if (!is_register_child(child))
            return false;
    }
    return partners <= 1;
}

bool alm_cluster_placement(const Arch &arch, CellInfo *root, BelId root_bel,
                           std::vector<std::pair<CellInfo *, BelId>> &placement)
{
    placement.clear();
    const Loc loc = arch.getBelLocation(root_bel);
    const int half = loc.z % 6;
    if (half > 1 || !arch.isValidBelForCellType(root->type, root_bel))
        return false;
    bool has_partner = false;
    for (const CellInfo *child : root->constr_children)
        has_partner |= !is_register_child(child);
    if (has_partner && half != 0) // a pair's root seeds only from the first LUT half of an ALM
        return false;
    const int alm_base = loc.z - half;
    placement.emplace_back(root, root_bel);
    for (CellInfo *child : root->constr_children) {
        int z;
        if (!is_register_child(child)) {
            z = loc.z + 1;
        } else {
            // 2 and 3 are the root's own half, 4 and 5 the partner's, which is always the second; the odd ones
            // are the half's second register bel (design 20.3).
            const int lut_half = child->constr_z >= 4 ? 1 : half;
            z = alm_base + 2 + 2 * lut_half + (child->constr_z % 2 == 1 ? 1 : 0);
        }
        BelId bel = arch.getBelByLocation(Loc(loc.x, loc.y, z));
        if (bel == BelId() || !arch.isValidBelForCellType(child->type, bel))
            return false;
        placement.emplace_back(child, bel);
    }
    return true;
}

bool registers_share_a_lab(const std::vector<const CellInfo *> &ffs)
{
    NPNR_ASSERT(ffs.size() <= NPNR_LAB_FF_COUNT);
    NpnrLabControlsV1 input = empty_lab_controls();
    std::unordered_map<const NetInfo *, uint32_t> ids;
    auto encode = [&](ControlSig sig) {
        NpnrControlSignalV1 out{0, 0};
        if (sig.net == nullptr)
            return out;
        auto it = ids.find(sig.net);
        if (it == ids.end())
            it = ids.emplace(sig.net, uint32_t(ids.size() + 1)).first;
        out.net_id = it->second;
        out.flags = (sig.inverted ? uint32_t(NPNR_CONTROL_INVERTED) : 0u) |
                    (sig.net->is_global ? uint32_t(NPNR_CONTROL_GLOBAL) : 0u);
        return out;
    };
    for (size_t i = 0; i < ffs.size(); ++i) {
        const FFControlSet &cs = ffs[i]->ffInfo.ctrlset;
        input.ff[i].occupied = 1;
        input.ff[i].control[NPNR_CONTROL_CLK] = encode(cs.clk);
        input.ff[i].control[NPNR_CONTROL_SLOAD] = encode(cs.sload);
        input.ff[i].control[NPNR_CONTROL_SCLR] = encode(cs.sclr);
        input.ff[i].control[NPNR_CONTROL_ACLR] = encode(cs.aclr);
        input.ff[i].control[NPNR_CONTROL_ENA] = encode(cs.ena);
    }
    input.net_count = uint32_t(ids.size());
    return evaluate_lab_controls_cpp(input).status == NPNR_CONTROL_LEGAL;
}

RegisterPackingReport pack_registers(Context &ctx)
{
    RegisterPackingReport r;
    // Deterministic: registers in name order.
    std::vector<CellInfo *> ffs;
    for (auto &cell : ctx.cells)
        if (cell.second->type == id_MISTRAL_FF)
            ffs.push_back(cell.second.get());
    std::sort(ffs.begin(), ffs.end(),
              [](const CellInfo *a, const CellInfo *b) { return a->name.index < b->name.index; });
    r.registers = ffs.size();
    // LUTs by the register slots taken: one, or two under --alm-both-registers (design 20.3), where a LUT's second
    // register takes the second register bel of its half (not beside a six-input LUT, which the rule refuses).
    std::unordered_map<const CellInfo *, int> slots_used;
    const bool both = ctx.args.alm_both_registers;
    // The control model below is the search's filter; what admits a register into a cluster is the
    // authority the placer will ask, on a clean ALM, before it is committed (design section 13).
    PackAdmission gate(ctx, /*assign_facts=*/false);
    for (CellInfo *ff : ffs) {
        const NetInfo *d = ff->getPort(id_DATAIN);
        if (d == nullptr || d->driver.cell == nullptr || d->driver.port != id_Q || !is_plain_lut(d->driver.cell->type))
            continue;
        ++r.lut_driven;
        CellInfo *lut = d->driver.cell;
        if (ff->cluster != ClusterId() || !ff->constr_children.empty()) {
            ++r.ff_constrained;
            continue;
        }
        CellInfo *root = nullptr;
        int base = 0;
        enum
        {
            Single,
            PairRoot,
            PairChild
        } kind = Single;
        if (lut->cluster == ClusterId()) {
            if (!lut->constr_children.empty() || lut->constr_abs_z) {
                ++r.lut_clustered;
                continue;
            }
            root = lut;
            base = 2;
        } else if (lut->cluster == lut->name && (is_alm_pair_root(lut) || is_alm_cluster_root(lut))) {
            root = lut;
            base = 2;
            for (const CellInfo *child : lut->constr_children)
                if (!is_register_child(child))
                    kind = PairRoot;
        } else if (lut->cluster != lut->name && lut->constr_z == 1 && !lut->constr_abs_z) {
            CellInfo *candidate = ctx.cells.at(lut->cluster).get();
            if (!is_alm_pair_root(candidate) && !is_alm_cluster_root(candidate)) {
                ++r.lut_clustered;
                continue;
            }
            root = candidate;
            base = 4;
            kind = PairChild;
        } else {
            ++r.lut_clustered;
            continue;
        }
        const int used = slots_used.count(lut) ? slots_used.at(lut) : 0;
        if (used >= (both && lut->type != id_MISTRAL_ALUT6 ? 2 : 1)) {
            ++r.lut_full;
            continue;
        }
        if (used == 1)
            ++base; // the half's second register bel
        // The cluster's registers land in one LAB; the control model must admit them together.
        std::vector<const CellInfo *> together;
        for (const CellInfo *child : root->constr_children)
            if (is_register_child(child))
                together.push_back(child);
        together.push_back(ff);
        if (!registers_share_a_lab(together)) {
            ++r.control_conflict;
            continue;
        }
        const ClusterId root_cluster = root->cluster, ff_cluster = ff->cluster;
        const int root_z = root->constr_z, ff_x = ff->constr_x, ff_y = ff->constr_y, ff_z = ff->constr_z;
        const bool root_abs_z = root->constr_abs_z, ff_abs_z = ff->constr_abs_z;
        if (root->cluster == ClusterId()) {
            root->cluster = root->name;
            root->constr_abs_z = false;
            root->constr_z = 0;
        }
        ff->cluster = root->name;
        ff->constr_x = 0;
        ff->constr_y = 0;
        ff->constr_abs_z = false;
        ff->constr_z = base;
        root->constr_children.push_back(ff);
        if (!gate.admits(root)) {
            root->constr_children.pop_back();
            root->cluster = root_cluster;
            root->constr_z = root_z;
            root->constr_abs_z = root_abs_z;
            ff->cluster = ff_cluster;
            ff->constr_x = ff_x;
            ff->constr_y = ff_y;
            ff->constr_z = ff_z;
            ff->constr_abs_z = ff_abs_z;
            ++r.refused;
            continue;
        }
        slots_used[lut] = used + 1;
        ++r.packed;
        r.second_registers += used == 1;
        if (kind == Single)
            ++r.onto_single;
        else if (kind == PairRoot)
            ++r.onto_pair_root;
        else
            ++r.onto_pair_child;
    }
    report_pack_admission(ctx, gate, "register-packing");
    return r;
}

void report_register_packing(const RegisterPackingReport &r)
{
    log_info("Register packing: %" PRIu64 " registers, %" PRIu64 " driven by a plain LUT, %" PRIu64
             " packed with it (%" PRIu64 " onto a single LUT, %" PRIu64 " onto a pair's root, %" PRIu64
             " onto a pair's partner; %" PRIu64 " on a second register bel); not packed: %" PRIu64
             " LUT slots already taken, %" PRIu64 " LUTs in another cluster, %" PRIu64
             " registers already constrained, %" PRIu64 " control-set conflicts inside the cluster.\n",
             r.registers, r.lut_driven, r.packed, r.onto_single, r.onto_pair_root, r.onto_pair_child,
             r.second_registers, r.lut_full, r.lut_clustered, r.ff_constrained, r.control_conflict);
}

NEXTPNR_NAMESPACE_END
