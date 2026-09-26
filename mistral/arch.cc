/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  Lofty <dan.ravensloft@gmail.com>
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
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <fstream>
#include <optional>

#include "alm_pairing.h"
#include "lab_clustering.h"
#include "log.h"
#include "nextpnr.h"
#include "register_packing.h"
#include "telemetry.h"

#include <memory>

#include "placement_coordinator.h"
#include "placement_reuse.h"
#include "placement_transaction.h"
#include "placer1.h"
#include "placer_heap.h"
#include "reuse_plan.h"
#include "route_reuse.h"
#include "router1.h"
#include "router2.h"
#include "timing.h"
#include "util.h"

#include "cyclonev.h"

NEXTPNR_NAMESPACE_BEGIN

// Design 20.1 step 2: clusters smaller than this are fragments of the growth and get no LAB hint.
static constexpr int LAB_CLUSTER_HINT_MIN = 4;

using namespace mistral;

void IdString::initialize_arch(const BaseCtx *ctx)
{
#define X(t) initialize_add(ctx, #t, ID_##t);

#include "constids.inc"

#undef X
}

CycloneV::rnode_t Arch::find_rnode(CycloneV::block_type_t bt, int x, int y, CycloneV::port_type_t port, int bi,
                                   int pi) const
{
    auto pn1 = CycloneV::pnode(bt, x, y, port, bi, pi);
    auto rn1 = cyclonev->pnode_to_rnode(pn1);
    if (rn1)
        return rn1;

    if (bt == CycloneV::GPIO) {
        auto pn2 = cyclonev->p2p_to(pn1);
        if (!pn2) {
            auto pnv = cyclonev->p2p_from(pn1);
            if (!pnv.empty())
                pn2 = pnv[0];
        }
        auto pn3 = cyclonev->hmc_get_bypass(pn2);
        auto rn2 = cyclonev->pnode_to_rnode(pn3);
        return rn2;
    }

    return 0;
}

WireId Arch::get_port(CycloneV::block_type_t bt, int x, int y, int bi, CycloneV::port_type_t port, int pi) const
{
    auto rn = find_rnode(bt, x, y, port, bi, pi);
    if (rn)
        return WireId(rn);

    log_error("Trying to connect unknown node %s\n", CycloneV::pn2s(CycloneV::pnode(bt, x, y, port, bi, pi)).c_str());
}

bool Arch::has_port(CycloneV::block_type_t bt, int x, int y, int bi, CycloneV::port_type_t port, int pi) const
{
    return find_rnode(bt, x, y, port, bi, pi) != 0;
}

Arch::Arch(ArchArgs args)
{
    this->args = args;
    lut_pin_delays_active = args.lut_pin_delays == 2; // design 20.4a, on: every timing analysis times LUT pins
    this->cyclonev = mistral::CycloneV::get_model(args.device);
    NPNR_ASSERT(this->cyclonev != nullptr);

    // Setup fast identifier maps
    for (int i = 0; i < 1024; i++) {
        IdString int_id = idf("%d", i);
        int2id.push_back(int_id);
        id2int[int_id] = i;
    }

    for (int t = int(CycloneV::NONE); t <= int(CycloneV::DCMUX); t++) {
        IdString rnode_id = id(CycloneV::rnode_type_names[t]);
        rn_t2id.push_back(rnode_id);
        id2rn_t[rnode_id] = CycloneV::rnode_type_t(t);
    }

    log_info("Initialising bels...\n");
    bels_by_tile.resize(cyclonev->get_tile_sx() * cyclonev->get_tile_sy());
    // Design 20.2: the logical-input bel pin names, interned only under the option (a new IdString shifts every
    // later index, so a default run's netlist numbering must not see them).
    if (args.lut_permutation)
        for (int k = 0; k < 5; k++)
            lperm_pins[k] = idf("LPERM%d", k);

    for (auto lab_pos : cyclonev->lab_get_pos())
        create_lab(CycloneV::pos2x(lab_pos), CycloneV::pos2y(lab_pos), /*is_mlab=*/false);

    for (auto mlab_pos : cyclonev->mlab_get_pos())
        create_lab(CycloneV::pos2x(mlab_pos), CycloneV::pos2y(mlab_pos), /*is_mlab=*/true);

    for (auto gpio_pos : cyclonev->gpio_get_pos())
        create_gpio(CycloneV::pos2x(gpio_pos), CycloneV::pos2y(gpio_pos));

    for (auto cmuxh_pos : cyclonev->cmuxh_get_pos())
        create_clkbuf(CycloneV::pos2x(cmuxh_pos), CycloneV::pos2y(cmuxh_pos));

    // G4: PLL-output clock injection bels at the global cmuxes + the (pll,counter)->INPUT_SEL map.
    for (auto cmuxh_pos : cyclonev->cmuxh_get_pos())
        create_pllclk(CycloneV::pos2x(cmuxh_pos), CycloneV::pos2y(cmuxh_pos), /*vertical=*/false);
    for (auto cmuxv_pos : cyclonev->cmuxv_get_pos())
        create_pllclk(CycloneV::pos2x(cmuxv_pos), CycloneV::pos2y(cmuxv_pos), /*vertical=*/true);
    build_pllclk_map();

    create_control(CycloneV::pos2x(cyclonev->ctrl_get_pos()[0]), CycloneV::pos2y(cyclonev->ctrl_get_pos()[0]));

    auto hps_pos = cyclonev->hps_get_pos();
    if (!hps_pos.empty()) {
        create_hps_mpu_general_purpose(CycloneV::pos2x(hps_pos[CycloneV::I_HPS_MPU_GENERAL_PURPOSE]),
                                       CycloneV::pos2y(hps_pos[CycloneV::I_HPS_MPU_GENERAL_PURPOSE]));
        if (!hps_pos.empty()) {
            create_hps_lwh2f(CycloneV::pos2x(hps_pos[CycloneV::I_HPS_HPS2FPGA_LIGHT_WEIGHT]),
                             CycloneV::pos2y(hps_pos[CycloneV::I_HPS_HPS2FPGA_LIGHT_WEIGHT]));
            create_hps_f2sdram(CycloneV::pos2x(hps_pos[CycloneV::I_HPS_FPGA2SDRAM]),
                               CycloneV::pos2y(hps_pos[CycloneV::I_HPS_FPGA2SDRAM]));
        }
    }

    for (auto m10k_pos : cyclonev->m10k_get_pos())
        create_m10k(CycloneV::pos2x(m10k_pos), CycloneV::pos2y(m10k_pos));
    for (auto dsp_pos : cyclonev->dsp_get_pos())
        create_dsp(CycloneV::pos2x(dsp_pos), CycloneV::pos2y(dsp_pos));

    for (auto fpll_pos : cyclonev->fpll_get_pos())
        create_fpll(CycloneV::pos2x(fpll_pos), CycloneV::pos2y(fpll_pos));

    // This import takes about 5s, perhaps long term we can speed it up, e.g. defer to Mistral more...
    log_info("Initialising routing graph...\n");
    int pip_count = 0;
    for (const auto &rnode : cyclonev->rnodes()) {
        WireId dst_wire(rnode.id());
        for (const auto &src : rnode.sources()) {
            WireId src_wire(src);
            wires[dst_wire].wires_uphill.push_back(src_wire);
            wires[src_wire].wires_downhill.push_back(dst_wire);
            ++pip_count;
        }
    }

    log_info("    imported %d wires and %d pips\n", int(wires.size()), pip_count);
    build_wire_slots();

    // VENDOR-LEG MIMICRY (data feeds): env VUP_BLOCK_NODES="TYPE:x,y,z;..." hard-blocks
    // arbitrary routing nodes for ALL routers (general router included -- unlike
    // VUP_IOREG_POISON_TD, which only the clock-BFS filters consult). Used to blacklist
    // our TD data-feed nodes that mismatch a vendor decompile so the router converges
    // onto the vendor-attested feeds (tools/vendor_legs.py emits the list).
    if (const char *bn = getenv("VUP_BLOCK_NODES")) {
        int blocked = 0;
        std::string s(bn);
        size_t i = 0;
        while (i < s.size()) {
            size_t c = s.find(';', i);
            if (c == std::string::npos)
                c = s.size();
            std::string tok = s.substr(i, c - i);
            i = c + 1;
            size_t col = tok.find(':');
            if (col == std::string::npos)
                continue;
            int x = -1, y = -1, z = -1;
            if (sscanf(tok.c_str() + col + 1, "%d,%d,%d", &x, &y, &z) != 3)
                continue;
            auto ty = cyclonev->rnode_type_lookup(tok.substr(0, col));
            WireId w;
            w.node = CycloneV::rnode(ty, x, y, z);
            if (wires.count(w)) {
                block_wire(w);
                blocked++;
            }
        }
        log_info("VUP_BLOCK_NODES: hard-blocked %d routing nodes\n", blocked);
    }

    // VENDOR-LEG MIMICRY (positive pinning): env VUP_RESERVE_ROUTES=
    //   "TYPE:x,y,z>BLOCK.x.y[.bi]:PORT.pi;..." reserves each dst PNODE's route so it is
    // reachable ONLY from the given src rnode -- the vendor's exact feed (one-shot, no
    // blacklist iteration). tools/vendor_legs.py --reserve emits the list from a vendor
    // decompile.
    if (const char *rr = getenv("VUP_RESERVE_ROUTES")) {
        int reserved = 0;
        std::string s(rr);
        size_t i = 0;
        while (i < s.size()) {
            size_t c = s.find(';', i);
            if (c == std::string::npos)
                c = s.size();
            std::string tok = s.substr(i, c - i);
            i = c + 1;
            size_t gt = tok.find('>');
            if (gt == std::string::npos)
                continue;
            std::string st = tok.substr(0, gt), dt = tok.substr(gt + 1);
            // src rnode: TYPE:x,y,z
            size_t col = st.find(':');
            int sx, sy, sz;
            if (col == std::string::npos || sscanf(st.c_str() + col + 1, "%d,%d,%d", &sx, &sy, &sz) != 3)
                continue;
            WireId sw;
            sw.node = CycloneV::rnode(cyclonev->rnode_type_lookup(st.substr(0, col)), sx, sy, sz);
            // dst pnode: BLOCK.x.y[.bi]:PORT.pi
            size_t dcol = dt.find(':');
            if (dcol == std::string::npos)
                continue;
            std::string blk = dt.substr(0, dcol), prt = dt.substr(dcol + 1);
            int bx = -1, by = -1, bbi = -1;
            char bname[16] = {0};
            int nf = sscanf(blk.c_str(), "%15[A-Z0-9].%d.%d.%d", bname, &bx, &by, &bbi);
            if (nf < 3)
                continue;
            char pname[32] = {0};
            int pi = -1;
            if (sscanf(prt.c_str(), "%31[A-Z0-9_].%d", pname, &pi) != 2)
                continue;
            auto pn = CycloneV::pnode(cyclonev->block_type_lookup(bname), CycloneV::xy2pos(bx, by),
                                      cyclonev->port_type_lookup(pname), nf == 4 ? bbi : -1, pi);
            auto rn = cyclonev->pnode_to_rnode(pn);
            if (!rn)
                continue;
            WireId dw;
            dw.node = rn;
            if (wires.count(sw) && wires.count(dw)) {
                reserve_route(sw, dw);
                reserved++;
            }
        }
        log_info("VUP_RESERVE_ROUTES: pinned %d vendor feeds\n", reserved);
    }

    BaseArch::init_cell_types();
    BaseArch::init_bel_buckets();
}

int Arch::getTileBelDimZ(int x, int y) const
{
    // This seems like a reasonable upper bound
    return 256;
}

BelId Arch::getBelByName(IdStringList name) const
{
    BelId bel;
    NPNR_ASSERT(name.size() == 4);
    int x = id2int.at(name[1]);
    int y = id2int.at(name[2]);
    int z = id2int.at(name[3]);

    bel.pos = CycloneV::xy2pos(x, y);
    bel.z = z;

    NPNR_ASSERT(name[0] == getBelType(bel));

    return bel;
}

IdStringList Arch::getBelName(BelId bel) const
{
    int x = CycloneV::pos2x(bel.pos);
    int y = CycloneV::pos2y(bel.pos);
    int z = bel.z & 0xFF;

    std::array<IdString, 4> ids{
            getBelType(bel),
            int2id.at(x),
            int2id.at(y),
            int2id.at(z),
    };

    return IdStringList(ids);
}

// Is the bel that models the SAME physical global-clock output, under the other bel type, in use?
// HOT under the placer (isBelLocationValid on CLKENA/PLLCLK): the sibling relation is
// static, so build it once instead of an allocating tile scan per query (profiled: the
// scan consumed ~60% of placement wall time at ~24k cells and stalled both placers).
bool Arch::global_sibling_occupied(BelId bel, IdString other_type) const
{
    if (!clk_sibling_cache_built) {
        dict<uint64_t, std::vector<BelId>> by_key;
        for (BelId b : getBels()) {
            auto &bd = bel_data(b);
            if (bd.type != id_MISTRAL_CLKENA && bd.type != id_MISTRAL_PLLCLK)
                continue;
            Loc l = getBelLocation(b);
            uint64_t key = (uint64_t(l.x) << 32) | (uint64_t(l.y) << 16) | uint64_t(bd.block_index & 0xFFFF);
            by_key[key].push_back(b);
        }
        for (auto &kv : by_key)
            for (BelId b : kv.second) {
                auto &sibs = clk_sibling_cache[b];
                for (BelId o : kv.second)
                    if (o != b)
                        sibs.push_back(o);
            }
        clk_sibling_cache_built = true;
    }
    auto it = clk_sibling_cache.find(bel);
    if (it == clk_sibling_cache.end())
        return false;
    for (BelId b : it->second) {
        if (bel_data(b).type != other_type)
            continue;
        if (getBoundBelCell(b) != nullptr)
            return true;
    }
    return false;
}

bool Arch::isBelLocationValid(BelId bel, bool explain_invalid) const
{
    auto &data = bel_data(bel);
    if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB)) {
        if (args.lab_legality != LabLegalityMode::Legacy)
            return dispatch_lab_legality(*this, data.lab_data.lab, NPNR_LAB_QUERY_COMB_BEL, data.lab_data.alm);
        return is_alm_legal(data.lab_data.lab, data.lab_data.alm) && lab_level_legal(*this, data.lab_data.lab, false);
    } else if (data.type == id_MISTRAL_FF) {
        if (args.lab_legality != LabLegalityMode::Legacy)
            return dispatch_lab_legality(*this, data.lab_data.lab, NPNR_LAB_QUERY_FF_BEL, data.lab_data.alm);
        return is_alm_legal(data.lab_data.lab, data.lab_data.alm) && lab_level_legal(*this, data.lab_data.lab, true);
    } else if (data.type == id_MISTRAL_CLKENA) {
        // A CLKBUF bel and a PLLCLK bel at the same (tile, index) drive the SAME physical
        // CMUX*G CLKOUT -- create_clkbuf and create_pllclk both bind that port. They are two models
        // of one wire, so at most one may be used. Without this the placer happily binds both and
        // global routing then aborts trying to bind that wire to a second net
        // (w2n_entry == nullptr), which is what stopped two PLLs from routing.
        // An EMPTY bel is always valid -- the question is only whether what is bound here conflicts.
        if (getBoundBelCell(bel) == nullptr)
            return true;
        return !global_sibling_occupied(bel, id_MISTRAL_PLLCLK);
    } else if (data.type == id_MISTRAL_PLLCLK) {
        if (getBoundBelCell(bel) != nullptr && global_sibling_occupied(bel, id_MISTRAL_CLKENA))
            return false;
        // A PLLCLK bel is legal only where the dedicated PLL->cmux wiring exists for its source
        // PLL counter (pllclk_sel_map). If the source PLL is not placed yet, defer (valid); the
        // pair converges as the placer binds both.
        CellInfo *ci = getBoundBelCell(bel);
        if (ci == nullptr)
            return true;
        if (!ci->attrs.count(id_PLLCLK_PLL) || !ci->attrs.count(id_PLLCLK_COUNTER))
            return true; // not a pack-inserted pllclk (shouldn't happen); nothing to check
        auto pll_it = cells.find(id(ci->attrs.at(id_PLLCLK_PLL).as_string()));
        if (pll_it == cells.end() || pll_it->second->bel == BelId())
            return true;
        Loc pl = getBelLocation(pll_it->second->bel);
        int counter = int(ci->attrs.at(id_PLLCLK_COUNTER).as_int64());
        bool okp = pllclk_lookup(uint32_t(CycloneV::xy2pos(pl.x, pl.y)), counter, uint32_t(bel.pos),
                                 data.block_index) >= 0;
        if (!okp && explain_invalid)
            log_info("  [pllclk] %s invalid: its PLL '%s' is at FPLL(%d,%d) counter C%d, no dedicated "
                     "wiring to cmux(%d,%d) gclk %d\n",
                     ci->name.c_str(this), pll_it->second->name.c_str(this), pl.x, pl.y, counter,
                     CycloneV::pos2x(CycloneV::pos_t(bel.pos)), CycloneV::pos2y(CycloneV::pos_t(bel.pos)),
                     data.block_index);
        return okp;
    }
    return true;
}

void Arch::update_bel(BelId bel)
{
    auto &data = bel_data(bel);
    if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF)) {
        update_alm_input_count(data.lab_data.lab, data.lab_data.alm);
    }
}

WireId Arch::getWireByName(IdStringList name) const
{
    // non-mistral wires
    auto found_npnr = npnr_wirebyname.find(name);
    if (found_npnr != npnr_wirebyname.end())
        return found_npnr->second;
    // mistral wires
    NPNR_ASSERT(name.size() == 4);
    CycloneV::rnode_type_t ty = id2rn_t.at(name[0]);
    int x = id2int.at(name[1]);
    int y = id2int.at(name[2]);
    int z = id2int.at(name[3]);
    return WireId(CycloneV::rnode(ty, x, y, z));
}

IdStringList Arch::getWireName(WireId wire) const
{
    if (wire.is_nextpnr_created()) {
        // non-mistral wires
        std::array<IdString, 4> ids{
                id_WIRE,
                int2id.at(CycloneV::rn2x(wire.node)),
                int2id.at(CycloneV::rn2y(wire.node)),
                wires.at(wire).name_override,
        };
        return IdStringList(ids);
    } else {
        std::array<IdString, 4> ids{
                rn_t2id.at(CycloneV::rn2t(wire.node)),
                int2id.at(CycloneV::rn2x(wire.node)),
                int2id.at(CycloneV::rn2y(wire.node)),
                int2id.at(CycloneV::rn2z(wire.node)),
        };
        return IdStringList(ids);
    }
}

PipId Arch::getPipByName(IdStringList name) const
{
    WireId src = getWireByName(name.slice(0, 4));
    WireId dst = getWireByName(name.slice(4, 8));
    NPNR_ASSERT(src != WireId());
    NPNR_ASSERT(dst != WireId());
    return PipId(src.node, dst.node);
}

IdStringList Arch::getPipName(PipId pip) const
{
    return IdStringList::concat(getWireName(getPipSrcWire(pip)), getWireName(getPipDstWire(pip)));
}

std::vector<BelId> Arch::getBelsByTile(int x, int y) const
{
    // This should probably be redesigned, but it's a hack.
    std::vector<BelId> bels;
    if (x >= 0 && x < cyclonev->get_tile_sx() && y >= 0 && y < cyclonev->get_tile_sy()) {
        for (size_t i = 0; i < bels_by_tile.at(pos2idx(x, y)).size(); i++)
            bels.push_back(BelId(CycloneV::xy2pos(x, y), i));
    }

    return bels;
}

IdString Arch::getBelType(BelId bel) const { return bel_data(bel).type; }

std::vector<IdString> Arch::getBelPins(BelId bel) const
{
    std::vector<IdString> pins;
    for (auto &p : bel_data(bel).pins)
        pins.push_back(p.first);
    return pins;
}

bool Arch::isValidBelForCellType(IdString cell_type, BelId bel) const
{
    // Any combinational cell type can - theoretically - be placed at a combinational ALM bel
    // The precise legality mechanics will be dealt with in isBelLocationValid.
    IdString bel_type = getBelType(bel);
    // G4/B2: the FPLL position is load-bearing — the MCNT feedback loop closes only where the cmux
    // feedback mapping is ground-truth-known (currently (0,0)). The heap placer relocates cells
    // regardless of bind strength (MEASURED: pack chose (0,0), the bitstream emitted (89,0)), so the
    // constraint must live HERE: fast_bels builds the placer's candidate list from this predicate,
    // so an unmapped position is never even a candidate.
    // (An attempt to pin the FPLL to (0,0) here made placement unreachable — the heap placer's
    // radius search never offers that corner tile. Position pinning therefore remains OPEN; see
    // PLL_OUTCLK_DESIGN.md B2. The emission adapts to wherever the placer lands instead.)
    if (bel_type == id_MISTRAL_COMB)
        return is_comb_cell(cell_type);
    else if (bel_type == id_MISTRAL_MCOMB)
        return is_comb_cell(cell_type) || (cell_type == id_MISTRAL_MLAB);
    else if (bel_type == id_MISTRAL_IO)
        return is_io_cell(cell_type);
    else if (bel_type == id_MISTRAL_CLKENA)
        return is_clkbuf_cell(cell_type);
    else
        return bel_type == cell_type;
}

BelBucketId Arch::getBelBucketForCellType(IdString cell_type) const
{
    if (is_comb_cell(cell_type) || cell_type == id_MISTRAL_MLAB)
        return id_MISTRAL_COMB;
    else if (is_io_cell(cell_type))
        return id_MISTRAL_IO;
    else if (is_clkbuf_cell(cell_type))
        return id_MISTRAL_CLKENA;
    else
        return cell_type;
}

BelBucketId Arch::getBelBucketForBel(BelId bel) const
{
    IdString bel_type = getBelType(bel);
    if (bel_type == id_MISTRAL_MCOMB)
        return id_MISTRAL_COMB;
    return bel_type;
}

BelId Arch::bel_by_block_idx(int x, int y, IdString type, int block_index) const
{
    auto &bels = bels_by_tile.at(pos2idx(x, y));
    for (size_t i = 0; i < bels.size(); i++) {
        auto &bel_data = bels.at(i);
        if (bel_data.type == type && bel_data.block_index == block_index)
            return BelId(CycloneV::xy2pos(x, y), i);
    }
    return BelId();
}

BelId Arch::add_bel(int x, int y, IdString name, IdString type)
{
    auto &bels = bels_by_tile.at(pos2idx(x, y));
    BelId id = BelId(CycloneV::xy2pos(x, y), bels.size());
    all_bels.push_back(id);
    bels.emplace_back();
    auto &bel = bels.back();
    bel.name = name;
    bel.type = type;
    // TODO: buckets (for example LABs and MLABs in the same bucket)
    bel.bucket = type;
    return id;
}

WireId Arch::add_wire(int x, int y, IdString name, uint64_t flags)
{
    std::array<IdString, 4> ids{
            id_WIRE,
            int2id.at(x),
            int2id.at(y),
            name,
    };
    IdStringList full_name(ids);
    auto existing = npnr_wirebyname.find(full_name);
    if (existing != npnr_wirebyname.end()) {
        // Already exists, don't create anything
        return existing->second;
    } else {
        // Determine a unique ID for the wire
        int z = 0;
        WireId id;
        while (wires.count(id = WireId(CycloneV::rnode(CycloneV::rnode_type_t((z >> 10) + 128), x, y, (z & 0x3FF)))))
            z++;
        NPNR_ASSERT(wire_routes.empty()); // wires are numbered into slots once, after the last one
        wires[id].name_override = name;
        if (flags != 0)
            pending_wire_flags.emplace_back(id, flags);
        npnr_wirebyname[full_name] = id;
        return id;
    }
}

void Arch::block_wire(WireId w)
{
    const int slot = wire_slot(w);
    if (slot < 0 || wire_infos[slot] == nullptr)
        return;
    wire_routes[slot].flags |= WireInfo::BLOCKED;
    placement_revision.note_mutation(PlacementMutation::Routing);
}

void Arch::reserve_route(WireId src, WireId dst)
{
    auto &dst_data = wires.at(dst);
    int idx = -1;

    for (int i = 0; i < int(dst_data.wires_uphill.size()); i++) {
        if (dst_data.wires_uphill.at(i) == src) {
            idx = i;
            break;
        }
    }

    NPNR_ASSERT(idx != -1);

    set_wire_flags(dst, WireInfo::RESERVED_ROUTE | unsigned(idx));
    placement_revision.note_mutation(PlacementMutation::Routing);
}

bool Arch::wires_connected(WireId src, WireId dst) const
{
    PipId pip(src.node, dst.node);
    return getBoundPipNet(pip) != nullptr;
}

void Arch::build_wire_slots()
{
    // Groups of one rnode type in one tile, each numbered densely by z from its first slot, in
    // (type, x, y) order so that the wires of a tile sit together.
    NPNR_ASSERT(wire_routes.empty());
    std::array<bool, 256> present{};
    for (auto &wire : wires) {
        const auto node = wire.first.node;
        present[CycloneV::rn2t(node)] = true;
        wire_slot_dim_x = std::max(wire_slot_dim_x, int(CycloneV::rn2x(node)) + 1);
        wire_slot_dim_y = std::max(wire_slot_dim_y, int(CycloneV::rn2y(node)) + 1);
    }
    int types = 0;
    for (int t = 0; t < 256; t++)
        if (present[t])
            wire_slot_type[t] = uint8_t(++types);
    NPNR_ASSERT(types < 256);
    auto group_of = [&](CycloneV::rnode_t node) {
        return (size_t(wire_slot_type[CycloneV::rn2t(node)] - 1) * wire_slot_dim_x + CycloneV::rn2x(node)) *
                       wire_slot_dim_y +
               CycloneV::rn2y(node);
    };
    wire_slot_groups.assign(size_t(types) * wire_slot_dim_x * wire_slot_dim_y, {0, 0});
    for (auto &wire : wires) {
        auto &count = wire_slot_groups[group_of(wire.first.node)].second;
        count = std::max(count, CycloneV::rn2z(wire.first.node) + 1);
    }
    uint32_t next = 0;
    for (auto &group : wire_slot_groups) {
        group.first = next;
        next += group.second;
    }
    wire_routes.assign(next, WireRoute());
    wire_infos.assign(next, nullptr);
    for (auto &wire : wires) {
        const int slot = wire_slot(wire.first);
        NPNR_ASSERT(slot >= 0 && wire_infos[slot] == nullptr);
        wire_infos[slot] = &wire.second;
    }
    for (auto &pending : pending_wire_flags)
        set_wire_flags(pending.first, pending.second);
    pending_wire_flags.clear();
    log_info("    numbered %d wires into %u slots in %zu groups\n", int(wires.size()), next, wire_slot_groups.size());
}

PipId Arch::add_pip(WireId src, WireId dst)
{
    NPNR_ASSERT(wire_routes.empty()); // pips are added before the wires are numbered into slots
    wires[src].wires_downhill.push_back(dst);
    wires[dst].wires_uphill.push_back(src);
    return PipId(src.node, dst.node);
}

void Arch::add_bel_pin(BelId bel, IdString pin, PortType dir, WireId wire)
{
    auto &b = bel_data(bel);
    NPNR_ASSERT(!b.pins.count(pin));
    b.pins[pin].dir = dir;
    b.pins[pin].wire = wire;

    BelPin bel_pin;
    bel_pin.bel = bel;
    bel_pin.pin = pin;
    wires[wire].bel_pins.push_back(bel_pin);
}

void Arch::assign_default_pinmap(CellInfo *cell)
{
    if (cell->type == id_MISTRAL_M10K || cell->type == id_MISTRAL_M10K_DC)
        return; // M10Ks always have a custom pinmap (set in setup_m10ks; the frontend calls
                // assignArchInfo at LOAD time, so a default same-name map assigned here would
                // linger ahead of the real pins and break routing)
    for (auto &port : cell->ports) {
        auto &pinmap = cell->pin_data[port.first].bel_pins;
        if (!pinmap.empty())
            continue; // already mapped
        if (is_comb_cell(cell->type) && comb_pinmap.count(port.first))
            pinmap.push_back(comb_pinmap.at(port.first)); // default comb mapping for placer purposes
        else
            pinmap.push_back(port.first); // default: assume bel pin named the same as cell pin
    }
}

void Arch::assignArchInfo()
{
    // Design 19.11: a net the global router carries (driven by a clock buffer, a clock enable block, or a PLL
    // counter) reaches a LAB's registers through the LAB's clock input, not a DATAIN line (Quartus's fit of the same
    // three-enable LAB agrees, tracker 2026-09-25). Flagged global, the control model stops reserving DATAIN[0] for
    // it, and a third enable may take that line. Runs after packing and on every checkpoint restore.
    if (args.lab_global_clocks)
        for (auto &net : nets) {
            const CellInfo *driver = net.second->driver.cell;
            if (driver && driver->type.in(id_MISTRAL_CLKENA, id_MISTRAL_CLKBUF, id_MISTRAL_PLLCLK))
                net.second->is_global = true;
        }
    for (auto &cell : cells) {
        CellInfo *ci = cell.second.get();
        // Route-through buffers (MISTRAL_BUF, created by lab_pre_route) sit
        // at LUT BELs and carry comb facts too; a context restored after
        // routing preparation holds them, and a buffer left with a
        // zero-filled union reads as MLAB group 0, which turns its whole LAB
        // into LUTRAM in the bitstream.
        if (is_comb_cell(ci->type) || ci->type == id_MISTRAL_MLAB || ci->type == id_MISTRAL_BUF)
            assign_comb_info(ci);
        else if (ci->type == id_MISTRAL_FF)
            assign_ff_info(ci);
        assign_default_pinmap(ci);
    }
}

BoundingBox Arch::getRouteBoundingBox(WireId src, WireId dst) const
{
    BoundingBox bounds;
    int src_x = CycloneV::rn2x(src.node);
    int src_y = CycloneV::rn2y(src.node);
    int dst_x = CycloneV::rn2x(dst.node);
    int dst_y = CycloneV::rn2y(dst.node);
    bounds.x0 = std::min(src_x, dst_x);
    bounds.y0 = std::min(src_y, dst_y);
    bounds.x1 = std::max(src_x, dst_x);
    bounds.y1 = std::max(src_y, dst_y);
    return bounds;
}

void Arch::lab_reuse_begin()
{
    lab_versions.assign(labs.size(), 0);
    lab_assessments.assign(labs.size(), LabAssessmentEntry{});
    lab_prepared.assign(labs.size(), LabStamp{});
    lab_routed_epoch = 0;
    lab_content_cache.clear();
    lab_reuse_stats = LabReuseStats{};
    lab_reuse_effective = args.lab_reuse;
    // Reuse is validated against the plain legacy query only. Modes with their
    // own comparison or sampling semantics keep every query live.
    if (lab_reuse_effective != LabReuseMode::Off &&
        (args.lab_legality != LabLegalityMode::Legacy || args.lab_controls != LabControlMode::Legacy ||
         args.verify_lab_controls || !args.lab_control_profile_path.empty())) {
        log_warning("LAB assessment reuse disabled: it requires legacy LAB modes without verification or profiling.\n");
        lab_reuse_effective = LabReuseMode::Off;
    }
    if (lab_reuse_effective != LabReuseMode::Off)
        log_info("LAB assessment reuse: %s (%zu LABs).\n", lab_reuse_mode_name(lab_reuse_effective), labs.size());
    lab_reuse_active = true;
}

void Arch::lab_reuse_end()
{
    lab_reuse_active = false;
    report_lab_reuse_stats(*this);
    if (lab_reuse_effective == LabReuseMode::Shadow && lab_reuse_stats.mismatches != 0)
        log_error("LAB assessment reuse shadow found %" PRIu64 " mismatches.\n", lab_reuse_stats.mismatches);
    lab_assessments.clear();
    lab_content_cache.clear();
    lab_content_cache.shrink_to_fit();
}

void Arch::report_lab_states() const
{
    if (lab_versions.empty())
        return;
    std::array<uint64_t, 4> counts{};
    for (uint32_t lab = 0; lab < labs.size(); ++lab)
        ++counts[size_t(lab_reuse_state(*this, lab))];
    log_info("LAB states: dirty=%" PRIu64 ", evaluated=%" PRIu64 ", prepared=%" PRIu64 ", routed=%" PRIu64
             " (routing epoch %" PRIu64 ")\n",
             counts[0], counts[1], counts[2], counts[3], lab_routing_epoch);
}

bool Arch::place()
{
    monitor_phase(NPNR_MONITOR_PHASE_PLACE);
    // Stage 5 (3a): adopt the context in the phase this step needs and run
    // the typed transition; the phase check catches a second placement or a
    // placement of an unpacked design at the legacy boundary.
    const auto started = std::chrono::steady_clock::now();
    if (args.lab_clustering) {
        // Design 20.1, step 1: the clusters and their statistics; placement from them is step 2.
        LabClusteringCfg cfg;
        cfg.fill_target = args.lab_cluster_fill;
        cfg.line_price = args.lab_cluster_line_price;
        lab_clusters = run_lab_clustering(getCtx(), cfg);
    }
    place_build(Build<BuildPhase::Packed>::adopt(*getCtx()));
    telemetry_placement_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    write_mistral_telemetry(*this, "placed");
    return true;
}

bool Arch::getClusterPlacement(ClusterId cluster, BelId root_bel,
                               std::vector<std::pair<CellInfo *, BelId>> &placement) const
{
    CellInfo *root = getCtx()->cells.at(cluster).get();
    if (!is_alm_pair_root(root) && !is_alm_cluster_root(root))
        return BaseArch::getClusterPlacement(cluster, root_bel, placement);
    return alm_cluster_placement(*this, root, root_bel, placement); // Stage 6 (6b, 6g)
}

// Stage 6 (6e): RUDY over the current placement. Every net with two or more placed endpoints
// spreads (width + height) / area of its bounding box over the tiles it covers; a tile above
// the mean of the used tiles inflates the units of the cells in it by that ratio, up to four.
void Arch::rebuild_spread_inflation(const std::function<Loc(const CellInfo *)> &loc_of)
{
    const int w = getGridDimX() + 1, h = getGridDimY() + 1;
    spread_inflation_w = w;
    spread_inflation_h = h;
    std::vector<float> density(size_t(w) * h, 0.0f);
    for (auto &net_pair : getCtx()->nets) {
        const NetInfo *net = net_pair.second.get();
        int x0 = w, y0 = h, x1 = -1, y1 = -1, endpoints = 0;
        auto extend = [&](const CellInfo *cell) {
            if (cell == nullptr)
                return;
            Loc l = loc_of(cell);
            if (l.x < 0 || l.y < 0)
                return;
            x0 = std::min(x0, l.x);
            y0 = std::min(y0, l.y);
            x1 = std::max(x1, l.x);
            y1 = std::max(y1, l.y);
            endpoints++;
        };
        extend(net->driver.cell);
        for (auto &usr : net->users)
            extend(usr.cell);
        if (endpoints < 2 || x1 < 0)
            continue;
        x1 = std::min(x1, w - 1);
        y1 = std::min(y1, h - 1);
        const float bw = float(x1 - x0 + 1), bh = float(y1 - y0 + 1);
        const float per_tile = (bw + bh) / (bw * bh);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                density[size_t(y) * w + x] += per_tile;
    }
    double sum = 0;
    size_t used = 0;
    for (float d : density)
        if (d > 0) {
            sum += d;
            used++;
        }
    // Only tiles well above the mean count as congested: at low utilisation everything is
    // "above average" somewhere and inflating it only pulls a placement apart. The multiple is
    // sweepable (MISTRAL_SPREAD_CONGESTION_K, default 2) so the threshold is measured, not guessed.
    static const float k =
            getenv("MISTRAL_SPREAD_CONGESTION_K") ? float(atof(getenv("MISTRAL_SPREAD_CONGESTION_K"))) : 2.0f;
    const float mean = used ? float(sum / used) : 1.0f;
    const float threshold = std::max(1e-6f, k * mean);
    spread_inflation.assign(density.size(), 1.0f);
    for (size_t i = 0; i < density.size(); i++)
        spread_inflation[i] = std::max(1.0f, std::min(3.0f, density[i] / threshold));
}

float Arch::spread_inflation_at(int x, int y) const
{
    if (spread_inflation.empty() || x < 0 || y < 0 || x >= spread_inflation_w || y >= spread_inflation_h)
        return 1.0f;
    return spread_inflation[size_t(y) * spread_inflation_w + x];
}

// Stage 6 (6a): what the strict legaliser ran into. LAB input lines and ALM pairing are the
// capacities HeAP's spreading does not see; this names how full they are when it stalls.
void Arch::report_legalisation_stall(const std::vector<CellInfo *> &stuck) const
{
    const int limit = resolved_lab_input_limit();
    int labs_total = 0, labs_near_limit = 0, labs_at_limit = 0, alms_total = 0, alms_paired = 0, alms_single = 0;
    long inputs_total = 0;
    for (uint32_t lab = 0; lab < labs.size(); lab++) {
        const auto &ld = labs.at(lab);
        int inputs = args.lab_input_nets && !ld.is_mlab ? ld.net_demand : 0; // design 19.10
        for (int i = 0; i < 10; i++) {
            const auto &alm = ld.alms.at(i);
            if (!args.lab_input_nets || ld.is_mlab)
                inputs += alm.unique_input_count;
            const CellInfo *l0 = getBoundBelCell(alm.lut_bels[0]);
            const CellInfo *l1 = getBoundBelCell(alm.lut_bels[1]);
            alms_total++;
            if (l0 != nullptr && l1 != nullptr)
                alms_paired++;
            else if (l0 != nullptr || l1 != nullptr)
                alms_single++;
        }
        labs_total++;
        inputs_total += inputs;
        if (inputs >= limit)
            labs_at_limit++;
        else if (inputs >= limit - 4)
            labs_near_limit++;
    }
    dict<IdString, std::pair<int, long>> stuck_inputs; // type -> (count, unique input nets)
    for (const CellInfo *ci : stuck) {
        std::unordered_set<const NetInfo *> nets;
        for (auto &port : ci->ports)
            if (port.second.type == PORT_IN && port.second.net != nullptr)
                nets.insert(port.second.net);
        auto &e = stuck_inputs[ci->type];
        e.first++;
        e.second += long(nets.size());
    }
    log_info("LAB input lines (limit %d per LAB): %d of %d LABs at the limit, %d within 4 of it, %.1f inputs "
             "per LAB on average.\n",
             limit, labs_at_limit, labs_total, labs_near_limit, labs_total ? double(inputs_total) / labs_total : 0.0);
    log_info("ALM occupancy: %d of %d ALMs hold two LUTs, %d hold one (%.2f LUTs per used ALM).\n", alms_paired,
             alms_total, alms_single,
             (alms_paired + alms_single) ? double(2 * alms_paired + alms_single) / (alms_paired + alms_single) : 0.0);
    for (auto &e : stuck_inputs)
        log_info("Stuck %s: %d cells, %.1f unique input nets each.\n", e.first.c_str(getCtx()), e.second.first,
                 e.second.first ? double(e.second.second) / e.second.first : 0.0);
}

bool Arch::run_placement()
{
    std::string placer = str_or_default(settings, id_placer, defaultPlacer);
    // Stage 5 (3a): plan reuse before applying it; (3b): retry with a
    // growing released region if the placer cannot repair locally.
    ReusePlan *plan = nullptr;
    if (!args.reuse_placement_path.empty()) {
        reuse_plan_ = std::make_shared<ReusePlan>();
        plan = reuse_plan_.get();
        plan_placement_reuse(*getCtx(), args.reuse_placement_path, *plan);
        if (!args.reuse_plan_path.empty())
            write_reuse_plan(*plan, args.reuse_plan_path);
        if (args.reuse_dry_run)
            log_info("Reuse dry run: the placement plan is not applied.\n");
        else
            report_placement_reuse(apply_placement_reuse(*getCtx(), *plan));
    }

    auto run_placer = [&]() -> bool {
        lab_reuse_begin();

        if (placer == "heap") {
            PlacerHeapCfg cfg(getCtx());
            cfg.ioBufTypes.insert(id_MISTRAL_IO);
            cfg.ioBufTypes.insert(id_MISTRAL_IB);
            cfg.ioBufTypes.insert(id_MISTRAL_OB);
            cfg.cellGroups.emplace_back();
            cfg.cellGroups.back().insert({id_MISTRAL_COMB});
            cfg.cellGroups.back().insert({id_MISTRAL_FF});

            // The Cyclone V is asymmetrical enough that it's somewhat beneficial to prefer connecting things
            // horizontally.
            cfg.hpwl_scale_x = 1;
            cfg.hpwl_scale_y = 2;
            cfg.cluster_units_by_bucket = args.register_packing; // Stage 6 (6g): LUT+register clusters
            if (args.row_cost > 0) { // Stage 6 (6h): the fabric enters LABs through row wires
                cfg.hpwl_scale_y = args.row_cost;
                cfg.anisotropic = true;
            }
            cfg.sa_row_weight = args.sa_row_weight; // Design 19.2: rows and LAB entries per net
            cfg.sa_entry_weight = args.sa_entry_weight;
            cfg.sa_timing_lambda = args.sa_timing_lambda; // Design 19.7
            cfg.sa_timing_crit_exp = args.sa_crit_exp;
            cfg.lab_affinity_weight = args.heap_lab_affinity; // Design 19.6: LAB affinity when legalising
            cfg.lab_affinity_reach = args.heap_lab_reach;
            cfg.skip_refine = args.no_sa_refine; // design 20.0
            if (!args.lab_hint_path.empty()) {
                // Design 20.0: `cell x y` per line; the cell's (or its cluster root's) LAB is tried first.
                auto hints = std::make_shared<dict<IdString, Loc>>();
                std::ifstream in(args.lab_hint_path);
                if (!in)
                    log_error("cannot read --lab-hint file %s\n", args.lab_hint_path.c_str());
                std::string name;
                int x, y, unknown = 0;
                while (in >> name >> x >> y) {
                    IdString id = getCtx()->id(name);
                    if (!cells.count(id)) {
                        ++unknown;
                        continue;
                    }
                    (*hints)[id] = Loc(x, y, 0);
                }
                log_info("LAB hints: %d cells from %s (%d names not in the design).\n", int(hints->size()),
                         args.lab_hint_path.c_str(), unknown);
                cfg.lab_hint = [hints](Context *, const CellInfo *cell, Loc &loc) {
                    auto found = hints->find(cell->name);
                    if (found == hints->end())
                        return false;
                    loc = found->second;
                    return true;
                };
            } else if (args.lab_clustering) {
                if (args.lab_cluster_pull > 0) {
                    cfg.pull_weight = args.lab_cluster_pull;
                    for (const auto &cluster : lab_clusters.clusters)
                        if (int(cluster.size()) >= LAB_CLUSTER_HINT_MIN)
                            cfg.pull_groups.push_back(cluster);
                }
                // Design 20.1 step 2: a cell goes to the tile holding most of its cluster's legalised members; the
                // cluster's first member takes the normal search. The legaliser unbinds every cell it solves before
                // it starts, so members bound now were placed in this legalisation.
                cfg.lab_hint = [this](Context *, const CellInfo *cell, Loc &loc) {
                    auto found = lab_clusters.cluster_of.find(cell->name);
                    if (found == lab_clusters.cluster_of.end())
                        return false;
                    const auto &members = lab_clusters.clusters.at(found->second);
                    if (int(members.size()) < LAB_CLUSTER_HINT_MIN)
                        return false;
                    std::vector<std::pair<Loc, int>> tiles;
                    for (const CellInfo *member : members) {
                        if (member == cell || member->bel == BelId())
                            continue;
                        Loc at = getBelLocation(member->bel);
                        at.z = 0;
                        auto tile = std::find_if(tiles.begin(), tiles.end(),
                                                 [&](const std::pair<Loc, int> &t) { return t.first == at; });
                        if (tile == tiles.end())
                            tiles.emplace_back(at, 1);
                        else
                            ++tile->second;
                    }
                    if (tiles.empty())
                        return false;
                    loc = std::max_element(tiles.begin(), tiles.end(), [](const auto &a, const auto &b) {
                              return a.second < b.second;
                          })->first;
                    return true;
                };
            }
            cfg.report_infeasible = [this](Context *, const std::vector<CellInfo *> &stuck) {
                report_legalisation_stall(stuck);
            };
            if (args.spread_demand > 0 || args.spread_congestion) {
                // Stage 6 (6c): a comb cell occupies as many units as it has unique input nets, a bel
                // offers four, so a region of 5-input LUTs and pairs spreads thinner than one of
                // 2-input LUTs; everything else keeps one bel's worth. Stage 6 (6e): the units are
                // then inflated by the wire-density estimate of the tile the cell currently sits in.
                cfg.spread_units_per_bel = 4;
                const bool by_inputs = args.spread_demand > 0, by_congestion = args.spread_congestion;
                if (by_congestion)
                    cfg.on_spread_begin = [this](Context *, const std::function<Loc(const CellInfo *)> &loc_of) {
                        rebuild_spread_inflation(loc_of);
                    };
                cfg.get_cell_spread_units = [this, by_inputs, by_congestion](Context *, const CellInfo *ci, int x,
                                                                             int y) {
                    int base = 4;
                    if (by_inputs && is_comb_cell(ci->type)) {
                        std::unordered_set<const NetInfo *> nets;
                        for (auto &port : ci->ports)
                            if (port.second.type == PORT_IN && port.second.net != nullptr && port.first != id_CI)
                                nets.insert(port.second.net);
                        base = std::max(1, std::min(8, int(nets.size())));
                    }
                    // Inflation only for LAB cells: the other buckets are a handful of bels each and
                    // cannot absorb an inflated demand at all.
                    if (by_congestion && (is_comb_cell(ci->type) || ci->type == id_MISTRAL_FF))
                        base = int(std::lround(base * spread_inflation_at(x, y)));
                    return std::max(1, base);
                };
            }

            cfg.beta = 0.5; // TODO: find a good value of beta for sensible ALM spreading
            // EXPERIMENTAL (routing-congestion mitigation): beta caps the ALM-slot
            // utilisation the cut-spreader tolerates per region; lowering it spreads
            // cells across more of the (mostly-empty) die, distributing routing-wire
            // demand. Sweepable via env to probe whether ALM-spreading relieves the
            // router2 overuse floor before building a routing-demand-aware inflator.
            if (const char *beta_env = getenv("MISTRAL_HEAP_BETA")) {
                cfg.beta = float(atof(beta_env));
                log_info("MISTRAL_HEAP_BETA override: cut-spreader beta = %.3f\n", cfg.beta);
            }
            cfg.criticalityExponent = args.heap_crit_exp;
            // Stage 6 (tile scan, design section 14): in the Rust legality modes the strict legaliser
            // asks for the first legal bel of a tile in one call after a first refusal; the comparing
            // modes skip nothing and check every prediction against the live answer.
            if (args.lab_legality != LabLegalityMode::Legacy && args.lab_tile_scan) {
                cfg.scan_tile_first_legal = [](Context *owner, CellInfo *cell, const std::vector<BelId> &bels,
                                               int &first_legal) {
                    return scan_lab_tile(*owner, cell, bels, first_legal);
                };
                cfg.scan_tile_advisory = args.lab_legality != LabLegalityMode::Rust;
                cfg.scan_tile_observed = [](Context *owner, bool predicted_legal, bool live_legal) {
                    note_lab_tile_prediction(*owner, predicted_legal, live_legal);
                };
            }
            cfg.place_cluster_transaction = [](Context *owner, const std::vector<std::pair<CellInfo *, BelId>> &targets,
                                               const HeAPDisplacedBindings &displaced) {
                // Design 19.10: the detached evaluators count LAB inputs per ALM; under the distinct-net
                // model a cluster takes the live path, which binds and asks isBelLocationValid.
                if (owner->args.lab_input_nets)
                    return HeAPClusterTransactionOutcome::Unsupported;
                auto prepared =
                        prepare_placement_transaction(*owner, placement_edits_for_candidate(targets, displaced));
                if (!prepared)
                    log_error("Failed to preflight a detached HeAP cluster candidate.\n");
                // Design section 16.1: in the Rust legality modes the resident session answers the
                // candidate from the LAB it already holds, without freezing whole-LAB records. In
                // rust mode that answer decides; in shadow and verify the frozen evaluation below
                // decides as before and the two are compared. What the session does not cover (a
                // carry chain's many edits, a LUTRAM cell) takes the frozen path in every mode.
                static const bool debug_reject = getenv("MISTRAL_DEBUG_CLUSTER_REJECT") != nullptr;
                const LabLegalityMode legality = owner->args.lab_legality;
                std::optional<bool> resident;
                if (legality != LabLegalityMode::Legacy && owner->args.lab_tile_scan)
                    resident = placement_candidate_resident(*owner, prepared);
                if (resident && legality == LabLegalityMode::Rust && !debug_reject) {
                    if (!*resident)
                        return HeAPClusterTransactionOutcome::Rejected;
                    if (commit_placement_transaction(*owner, std::move(prepared)) != PlacementCommitOutcome::Committed)
                        log_error("Failed to commit a detached HeAP cluster candidate.\n");
                    return HeAPClusterTransactionOutcome::Committed;
                }
                auto frozen = freeze_placement_candidate(*owner, prepared);
                if (frozen.status == FrozenPlacementStatus::Unsupported)
                    return HeAPClusterTransactionOutcome::Unsupported;
                if (frozen.status != FrozenPlacementStatus::Ready)
                    log_error("Failed to freeze a detached HeAP cluster candidate.\n");
                auto assessment = evaluate_placement_candidate(frozen);
                if (assessment.status != FrozenPlacementStatus::Ready)
                    log_error("Failed to evaluate a detached HeAP cluster candidate.\n");
                if (!placement_candidate_rust_matches(frozen, assessment))
                    log_error("Rust disagrees with detached C++ for a HeAP cluster candidate.\n");
                if (resident && *resident != assessment.legal) {
                    owner->lab_legality_stats.mismatches.fetch_add(1, std::memory_order_relaxed);
                    if (legality == LabLegalityMode::Verify)
                        log_error("The resident session %s a HeAP cluster candidate the frozen evaluation %s.\n",
                                  *resident ? "admits" : "refuses", assessment.legal ? "admits" : "refuses");
                }
                if (!assessment.legal) {
                    static int debug_left = getenv("MISTRAL_DEBUG_CLUSTER_REJECT")
                                                    ? std::max(20, atoi(getenv("MISTRAL_DEBUG_CLUSTER_REJECT")))
                                                    : 0;
                    if (debug_left > 0) {
                        --debug_left;
                        for (const auto &res : assessment.results)
                            if (res.status != NPNR_LAB_V2_LEGAL)
                                log_info("[cluster-reject] root %s: status %u reason %u query %u alm %u slot %u "
                                         "observed %d limit %d (frozen %s)\n",
                                         owner->nameOf(targets.front().first), res.status, res.reason, res.query,
                                         res.failing_alm, res.failing_slot, res.observed, res.limit,
                                         frozen.status == FrozenPlacementStatus::Ready ? "ready" : "other");
                        for (const auto &t : targets)
                            if (t.first->type == id_MISTRAL_FF)
                                log_info("[cluster-reject]   %s at %s: clk %s ena %s aclr %s sclr %s sload %s\n",
                                         owner->nameOf(t.first), owner->nameOfBel(t.second),
                                         owner->nameOf(t.first->ffInfo.ctrlset.clk.net),
                                         owner->nameOf(t.first->ffInfo.ctrlset.ena.net),
                                         owner->nameOf(t.first->ffInfo.ctrlset.aclr.net),
                                         owner->nameOf(t.first->ffInfo.ctrlset.sclr.net),
                                         owner->nameOf(t.first->ffInfo.ctrlset.sload.net));
                        // The LAB's registers as they stand, with their control nets.
                        static const bool debug_occupants = getenv("MISTRAL_DEBUG_CLUSTER_REJECT_LAB") != nullptr;
                        if (debug_occupants) {
                            const auto &lab = owner->labs.at(owner->bel_data(targets.front().second).lab_data.lab);
                            for (const auto &alm : lab.alms)
                                for (BelId b : alm.ff_bels)
                                    if (const CellInfo *ff = owner->getBoundBelCell(b))
                                        log_info("[cluster-reject]     occupant %s: clk %s ena %s aclr %s sclr %s "
                                                 "sload %s\n",
                                                 owner->nameOfBel(b), owner->nameOf(ff->ffInfo.ctrlset.clk.net),
                                                 owner->nameOf(ff->ffInfo.ctrlset.ena.net),
                                                 owner->nameOf(ff->ffInfo.ctrlset.aclr.net),
                                                 owner->nameOf(ff->ffInfo.ctrlset.sclr.net),
                                                 owner->nameOf(ff->ffInfo.ctrlset.sload.net));
                            for (const auto &t : targets)
                                log_info("[cluster-reject]     target %s (%s) at %s\n", owner->nameOf(t.first),
                                         t.first->type.c_str(owner), owner->nameOfBel(t.second));
                        }
                    }
                    return HeAPClusterTransactionOutcome::Rejected;
                }
                auto outcome = commit_placement_transaction(*owner, std::move(prepared));
                if (outcome != PlacementCommitOutcome::Committed)
                    log_error("Failed to commit a detached HeAP cluster candidate.\n");
                return HeAPClusterTransactionOutcome::Committed;
            };
            // Stage 4D: speculate candidate locations and evaluate them detached on
            // `--threads` workers. Off unless --placer-lookahead is given; the serial
            // per-candidate callback above remains the fallback for unsupported moves.
            cfg.clusterLookahead = std::max(0, args.placer_lookahead);
            const int lookahead = cfg.clusterLookahead;
            const int threads = std::max(1, getCtx()->setting<int>("threads", 1));
            std::unique_ptr<PlacementCandidateCoordinator> coordinator;
            if (lookahead > 0) {
                coordinator = std::make_unique<PlacementCandidateCoordinator>(*this, unsigned(threads));
                log_info("Cluster lookahead enabled: %d candidates per batch, %u workers.\n", lookahead,
                         coordinator->workers());
                cfg.place_cluster_transactions = [&coordinator](Context *,
                                                                const std::vector<HeAPClusterCandidate> &candidates) {
                    return coordinator->place(candidates);
                };
            }
            if (args.sa_seam != SwapSeamMode::Off || args.sa_batch > 0) {
                cfg.assess_swap = mistral_assess_swap;
                cfg.commit_swap = mistral_commit_swap;
                cfg.swap_seam_shadow = args.sa_seam == SwapSeamMode::Shadow;
                cfg.swap_batch = std::max(0, args.sa_batch);
                cfg.swap_threads = unsigned(threads);
                if (cfg.swap_batch > 0 && cfg.swap_seam_shadow)
                    log_error("--sa-batch cannot be combined with --sa-seam shadow.\n");
                log_info("Annealer swap seam: %s%s.\n", cfg.swap_seam_shadow ? "shadow" : "on",
                         cfg.swap_batch > 0 ? " (batched)" : "");
            }
            const bool ok = placer_heap(getCtx(), cfg);
            if (coordinator)
                report_placement_batch_stats(*coordinator);
            lab_reuse_end();
            if (!ok)
                return false;
        } else if (placer == "sa") {
            Placer1Cfg sa_cfg(getCtx());
            sa_cfg.row_weight = args.sa_row_weight;
            sa_cfg.entry_weight = args.sa_entry_weight;
            sa_cfg.timing_lambda = args.sa_timing_lambda;
            sa_cfg.timing_crit_exp = args.sa_crit_exp;
            if (args.sa_seam != SwapSeamMode::Off) {
                sa_cfg.assess_swap = mistral_assess_swap;
                sa_cfg.commit_swap = mistral_commit_swap;
                sa_cfg.swap_seam_shadow = args.sa_seam == SwapSeamMode::Shadow;
            }
            const bool ok = placer1(getCtx(), sa_cfg);
            lab_reuse_end();
            if (!ok)
                return false;
        } else {
            log_error("Mistral architecture does not support placer '%s'\n", placer.c_str());
        }
        return true;
    };

    bool placed = false;
    if (plan == nullptr || args.reuse_dry_run) {
        placed = run_placer();
    } else {
        // The ladder: on a placer failure, release the transplants within a
        // growing radius of the dirty cells (their previous BELs, or those of
        // their neighbours for added cells), unbind everything the placer
        // bound, restore the pre-placement RNG state, and try again; the last
        // rung releases every transplant, which is the clean placement.
        const uint64_t rng_before = getCtx()->rngstate;
        static const int radii[] = {2, 5, 12, -1};
        unsigned forced = 0;
        if (const char *f = getenv("MISTRAL_PLACEMENT_REUSE_FORCE_FALLBACK"))
            forced = unsigned(atoi(f));
        for (unsigned attempt = 0;; ++attempt) {
            plan->placement_attempts = attempt + 1;
            bool failed = false;
            if (attempt < forced) {
                log_warning(
                        "Placement reuse fallback forced for attempt %u by MISTRAL_PLACEMENT_REUSE_FORCE_FALLBACK.\n",
                        attempt + 1);
                failed = true;
            } else {
                try {
                    placed = run_placer();
                } catch (log_execution_error_exception &) {
                    lab_reuse_end();
                    placed = false;
                }
                failed = !placed;
            }
            if (!failed)
                break;
            if (attempt >= sizeof(radii) / sizeof(radii[0]))
                log_error("Placement reuse: the placer failed after every transplant was released.\n");
            const int radius = radii[attempt];
            const unsigned released = release_placement_region(*getCtx(), *plan, radius);
            size_t unbound = 0;
            for (auto &cell : cells) {
                CellInfo *ci = cell.second.get();
                if (ci->bel != BelId() && ci->belStrength != STRENGTH_LOCKED) {
                    unbindBel(ci->bel);
                    ++unbound;
                }
            }
            getCtx()->rngstate = rng_before;
            if (radius < 0)
                log_warning("Placement reuse: attempt %u failed; every transplant released (%u), %zu cells unbound; "
                            "placing from scratch from the pre-placement RNG state.\n",
                            attempt + 1, released, unbound);
            else
                log_warning("Placement reuse: attempt %u failed; released %u transplants within %d tiles of the dirty "
                            "cells, %zu cells unbound; retrying from the pre-placement RNG state.\n",
                            attempt + 1, released, radius, unbound);
        }
        if (!args.reuse_plan_path.empty())
            write_reuse_plan(*plan, args.reuse_plan_path); // with the attempts and releases
    }
    if (!placed)
        return false;

    // G4/B2: the placer migrates the PLL off the pack-chosen bel no matter how it is constrained
    // (four approaches measured as failures - see PLL_OUTCLK_DESIGN.md). So DERIVE the assignment
    // from where it actually landed, here, after placement and before routing.
    fixup_pllclk_placement();

    telemetry_checksum = getCtx()->checksum();
    getCtx()->attrs[id_step] = std::string("place");
    archInfoToAttributes();
    return true;
}

// Stage 5 (2b): everything the router depends on that is not the router. A
// route-prepared checkpoint is written after this and resumed before the
// router, so nothing the router needs may be left to state this half and the
// checkpoint both omit.
void Arch::prepare_route()
{
    lab_pre_route();
    route_globals();
}

bool Arch::route()
{
    monitor_phase(NPNR_MONITOR_PHASE_ROUTE_PREPARE);
    Context &ctx = *getCtx();
    Build<BuildPhase::RoutePrepared> prepared = [&]() {
        if (checkpoint_phase_ == "route-prepared") {
            log_info("Routing preparation restored from the checkpoint; running the router.\n");
            return Build<BuildPhase::RoutePrepared>::adopt(ctx);
        }
        return prepare_build(Build<BuildPhase::Placed>::adopt(ctx));
    }();
    if (args.route_prepare_only) {
        log_info("Routing preparation complete; the router is skipped (--route-prepare-only).\n");
        return true;
    }
    route_build(std::move(prepared));
    // Design 19 diagnostic: MISTRAL_DUMP_ARC_DELAYS=file writes every routed arc's driver and sink tiles and
    // its routing delay (the timing analyser's own), to relate an arc's delay to its span.
    if (const char *e = getenv("MISTRAL_DUMP_ARC_DELAYS")) {
        FILE *f = fopen(e, "w");
        if (!f)
            log_error("cannot write %s\n", e);
        fprintf(f, "net,dx0,dy0,sx,sy,delay_ns\n");
        for (auto &net : ctx.nets) {
            const NetInfo *ni = net.second.get();
            if (!ni->driver.cell || ni->driver.cell->bel == BelId())
                continue;
            Loc d = getBelLocation(ni->driver.cell->bel);
            for (auto &usr : ni->users) {
                if (!usr.cell || usr.cell->bel == BelId())
                    continue;
                Loc s = getBelLocation(usr.cell->bel);
                fprintf(f, "%s,%d,%d,%d,%d,%.4f\n", ni->name.c_str(&ctx), d.x, d.y, s.x, s.y,
                        ctx.getDelayNS(ctx.getNetinfoRouteDelayQuad(ni, usr).maxDelay()));
            }
        }
        fclose(f);
    }
    return true;
}

bool Arch::run_router_phase()
{
    monitor_phase(NPNR_MONITOR_PHASE_ROUTE);
    const auto started = std::chrono::steady_clock::now();
    // Stage 6 (6f) diagnostic: MISTRAL_DUMP_LAB_LINES=x,y prints, for LAB (x,y), every input line's
    // sources by wire type and every LAB output's destinations by type, with the column wires named.
    if (const char *e = getenv("MISTRAL_DUMP_LAB_LINES")) {
        int lx = 0, ly = 0;
        sscanf(e, "%d,%d", &lx, &ly);
        std::string td = stringf("TD.%d.%d.", lx, ly), gin = stringf("GIN.%d.%d.", lx, ly);
        std::string ld = stringf("LD.%d.%d.", lx, ly), gout = stringf("GOUT.%d.%d.", lx, ly);
        for (WireId w : getWires()) {
            std::string n = nameOfWire(w);
            if (n.compare(0, td.size(), td) == 0) {
                std::map<std::string, int> by_type;
                std::string cols;
                for (PipId p : getPipsUphill(w)) {
                    std::string sname = nameOfWire(getPipSrcWire(p));
                    by_type[sname.substr(0, sname.find('.'))]++;
                    if (sname[0] == 'V')
                        cols += " " + sname;
                }
                std::string t;
                for (auto &kv : by_type)
                    t += stringf(" %s=%d", kv.first.c_str(), kv.second);
                log_info("[lab-lines] %s sources:%s | columns:%s\n", n.c_str(), t.c_str(), cols.c_str());
            } else if (n.compare(0, gin.size(), gin) == 0) {
                std::map<std::string, int> by_type;
                std::string cols;
                for (PipId p : getPipsDownhill(w)) {
                    std::string dname = nameOfWire(getPipDstWire(p));
                    by_type[dname.substr(0, dname.find('.'))]++;
                    if (dname[0] == 'V')
                        cols += " " + dname;
                }
                std::string t;
                for (auto &kv : by_type)
                    t += stringf(" %s=%d", kv.first.c_str(), kv.second);
                log_info("[lab-lines] %s drives:%s | columns:%s\n", n.c_str(), t.c_str(), cols.c_str());
            } else if (n.compare(0, ld.size(), ld) == 0 || n.compare(0, gout.size(), gout) == 0) {
                // Design 20.0: a local line's reach (downhill) and an ALM input's feeds (uphill), by wire type.
                const bool is_ld = n[0] == 'L';
                std::map<std::string, int> by_type;
                if (is_ld)
                    for (PipId p : getPipsDownhill(w)) {
                        std::string dname = nameOfWire(getPipDstWire(p));
                        by_type[dname.substr(0, dname.find('.'))]++;
                    }
                else
                    for (PipId p : getPipsUphill(w)) {
                        std::string sname = nameOfWire(getPipSrcWire(p));
                        by_type[sname.substr(0, sname.find('.'))]++;
                    }
                std::string t;
                for (auto &kv : by_type)
                    t += stringf(" %s=%d", kv.first.c_str(), kv.second);
                log_info("[lab-lines] %s %s:%s\n", n.c_str(), is_ld ? "drives" : "fed by", t.c_str());
            }
        }
    }
    std::string router = str_or_default(settings, id_router, defaultRouter);
    bool result = false;
    auto run_router = [&]() {
        if (router == "router1") {
            result = router1(getCtx(), Router1Cfg(getCtx()));
        } else if (router == "router2") {
            Router2Cfg cfg(getCtx());
            cfg.prerouted_hist_cost = args.reuse_routes_history;
            // Design 17.3: router2 reaches its index of a wire through the arch's wire slot.
            cfg.wire_slot = [this](WireId wire) { return wire_slot(wire); };
            cfg.wire_slot_count = int(wire_routes.size());
            // Stage 6 (6f): the measured negotiation knobs travel in ArchArgs (resume-safe: nothing is
            // interned before the netlist); the rest of router2's cost terms can be overridden from the
            // environment for experiments from a checkpoint, where the common --router2-* options cannot
            // be used because they intern their settings key before the checkpoint replays its table.
            cfg.reroute_period = args.router2_reroute;
            cfg.reroute_contested_only = args.router2_reroute_contested;
            cfg.repair_rounds = args.router2_repair_rounds; // Design 19.9
            cfg.repair_crit = args.router2_repair_crit;
            if (args.router2_crit_cost) {
                // Design 19.3: the unit cost for arcs that are not critical, the delay cost for those that are,
                // blended by router2's own weight w = max(floor, 1 - crit^2). A wire costs w + (1 - w) * delay / U,
                // where U is the mean pip delay: one unit, as under the unit cost, for an arc with no criticality.
                // Arcs below the criticality threshold keep the unit cost outright (19.3b).
                double sum = 0;
                size_t count = 0;
                for (PipId pip : getPips()) {
                    sum += getCtx()->getDelayNS(getPipDelay(pip).maxDelay() + getDelayEpsilon());
                    ++count;
                }
                const float unit = count ? float(sum / double(count)) : 1.0f;
                const float threshold = args.router2_crit_threshold;
                log_info("router2 criticality cost: unit %.3f ns (mean over %zu pips), threshold %.3f\n", unit, count,
                         threshold);
                cfg.get_base_cost = [unit, threshold](Context *ctx, WireId wire, PipId pip, float crit_weight) {
                    const float crit = std::sqrt(std::max(0.0f, 1.0f - crit_weight));
                    if (crit < threshold)
                        return 1.0f;
                    return crit_weight + (1.0f - crit_weight) * default_base_cost(ctx, wire, pip, crit_weight) / unit;
                };
            } else if (args.router2_unit_cost) {
                cfg.get_base_cost = [](Context *, WireId, PipId, float) { return 1.0f; };
            }
            if (const char *e = getenv("MISTRAL_R2_PRESENT_FLOOR"))
                cfg.present_cong_floor = float(atof(e));
            if (const char *e = getenv("MISTRAL_R2_CRIT_FLOOR"))
                cfg.crit_weight_floor = float(atof(e));
            if (const char *e = getenv("MISTRAL_R2_CONG_MULT"))
                cfg.curr_cong_mult = float(atof(e));
            if (const char *e = getenv("MISTRAL_R2_INIT_CONG"))
                cfg.init_curr_cong_weight = float(atof(e));
            if (const char *e = getenv("MISTRAL_R2_HIST"))
                cfg.hist_cong_weight = float(atof(e));
            if (const char *e = getenv("MISTRAL_R2_ESTIMATE"))
                cfg.estimate_weight = float(atof(e));
            if (const char *e = getenv("MISTRAL_R2_MAX_ITER"))
                cfg.max_iter = atoi(e);
            if (const char *e = getenv("MISTRAL_R2_HEATMAP"))
                cfg.heatmap = e;
            if (getenv("MISTRAL_R2_NO_TMDRIV"))
                getCtx()->settings[getCtx()->id("timing_driven")] = false;
            router2(getCtx(), cfg);
            result = true;
        } else {
            log_error("Mistral architecture does not support router '%s'\n", router.c_str());
        }
    };
    if (args.reuse_routes_path.empty()) {
        run_router();
    } else {
        // Stage 5 (3c): preserved routes are seeds the router may rip up; if
        // it fails anyway, drop them all and route from scratch from the same
        // RNG state, so the fallback is the uninterrupted run's routing.
        PreviousRoutes previous = load_previous_routes(args.reuse_routes_path);
        if (!reuse_plan_)
            reuse_plan_ = std::make_shared<ReusePlan>();
        plan_route_reuse(*getCtx(), previous, *reuse_plan_);
        if (!args.reuse_plan_path.empty())
            write_reuse_plan(*reuse_plan_, args.reuse_plan_path);
        RouteReuseReport reuse;
        if (args.reuse_dry_run)
            log_info("Reuse dry run: the route plan is not applied.\n");
        else
            reuse = apply_route_reuse(*getCtx(), previous, *reuse_plan_);
        const uint64_t rng_before = getCtx()->rngstate;
        bool fallback = getenv("MISTRAL_ROUTE_REUSE_FORCE_FALLBACK") != nullptr;
        getCtx()->router_gave_up = false;
        if (!fallback) {
            try {
                run_router();
            } catch (log_execution_error_exception &) {
                log_warning("Router failed with %" PRIu64 " reused routes; dropping them and routing from scratch.\n",
                            reuse.reused);
                fallback = true;
            }
            // router2 does not fail at its iteration cap: it gives up and
            // router1 legalises whatever is left. With reused routes in the
            // way that is a different routing, not the uninterrupted one, so
            // treat it as the failure it is.
            if (!fallback && reuse.reused > 0 && getCtx()->router_gave_up) {
                log_warning("Router gave up with %" PRIu64 " reused routes; dropping them and routing from scratch.\n",
                            reuse.reused);
                fallback = true;
            }
        } else {
            log_warning("Route reuse fallback forced by MISTRAL_ROUTE_REUSE_FORCE_FALLBACK.\n");
        }
        if (fallback) {
            drop_reused_routes(*getCtx(), reuse);
            // A router that gave up leaves router1's legalised routing on
            // every net; from scratch means only the globals remain.
            const size_t unrouted = unroute_below_locked(*getCtx());
            log_info("Route reuse fallback: %zu nets unrouted; routing again from the pre-router RNG state.\n",
                     unrouted);
            getCtx()->rngstate = rng_before;
            getCtx()->router_gave_up = false;
            run_router();
        } else if (!args.reuse_dry_run) {
            // What the router left of the applied routes; the plan is
            // rewritten so each net's decision carries the answer.
            measure_route_survival(*getCtx(), reuse, reuse_plan_.get());
            if (!args.reuse_plan_path.empty())
                write_reuse_plan(*reuse_plan_, args.reuse_plan_path);
        }
        report_route_reuse(reuse);
    }
    if (args.lut_permutation && result)
        lut_permutation_fixup(); // design 20.2: before signoff and the bitstream see the routing
    if (args.lut_pin_delays == 1 && result) {
        // Design 20.4a, report: routed under the logical table; the timing result the report writes is redone by pin.
        lut_pin_delays_active = true;
        log_info("LUT pin delays (report): timing the routed design by physical LUT pin.\n");
        timing_analysis(getCtx(), false, true, false, false, true);
    }
    telemetry_checksum = getCtx()->checksum();
    note_routing_complete();
    report_lab_states();
    getCtx()->attrs[id_step] = std::string("route");
    archInfoToAttributes();
    telemetry_routing_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    write_mistral_telemetry(*this, "routed");
    return result;
}

const std::string Arch::defaultPlacer = "heap";

const std::vector<std::string> Arch::availablePlacers = {"sa", "heap"};

const std::string Arch::defaultRouter = "router2";
const std::vector<std::string> Arch::availableRouters = {"router1", "router2"};

NEXTPNR_NAMESPACE_END
