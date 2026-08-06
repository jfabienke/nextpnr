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

#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

// Cyclone V fractional PLL (FPLL) bel.
//
// The frontend keys off the yosys `altera_pll` blackbox cell (emitted by
// synth_intel_alm), so the bel *type* is id_altera_pll to make the default
// bel-bucket / isValidBelForCellType matching (bel_type == cell_type) place
// the cell here directly - no dedicated MISTRAL_PLL cell is invented.
//
// Which FPLL ports the Mistral p2r model actually exposes to the routing graph
// was established empirically (dump of pnode_to_rnode + source/target sets for
// every port type at every FPLL position on 5CSEBA6U23I7):
//
//   coreclk0    -> PMUX  (30 SCLK sources, 0 targets)  : reference-clock INPUT
//   fbclk_in_r0 -> DCMUX ( 2 sources,      0 targets)  : feedback-clock INPUT
//   lock0       -> GIN   ( 0 sources,      N targets)  : locked-status OUTPUT
//   (nreset0/dprio*/... -> GOUT)                       : control INPUTs
//
// The important correction over the first cut: coreclk0 is *not* a clock
// output. The PMUX node it maps to is fed by the sector-clock (SCLK) network
// and drives nothing - it is the per-PLL reference-clock selector, i.e. the
// PLL's reference-clock INPUT. So the logical `refclk` maps here.
//
// The PLL's generated output clocks (C0..C8 / outclk[]) are deliberately NOT
// modelled as FPLL routing ports by Mistral: physically they reach the global
// clock network only through the clock control block (CMUX) via the dedicated
// CMUX_PLLIN selection, which libmistral represents as a bitstream mux and not
// as routing-graph edges. There is consequently no routing node anywhere in
// the graph that carries a PLL output, and (verified) the clock network is
// only ever driven by clock-type nodes - never by a GIN/GOUT/data node. Hence
// no bel pin can legitimately drive a PLL output onto the clock tree; routing
// outclk -> loads is unmodelled. See globals.cc and the FPLL notes there.
void Arch::create_fpll(int x, int y)
{
    BelId bel = add_bel(x, y, id_MISTRAL_FPLL, id_altera_pll);
    bel_data(bel).block_index = 0;

    // Reference clock input: the PMUX (coreclk0) node, selected from SCLK.
    if (has_port(CycloneV::FPLL, x, y, -1, CycloneV::CORECLK0))
        add_bel_pin(bel, id_REFCLK, PORT_IN, get_port(CycloneV::FPLL, x, y, -1, CycloneV::CORECLK0));

    // Locked status output.
    if (has_port(CycloneV::FPLL, x, y, -1, CycloneV::LOCK0))
        add_bel_pin(bel, id_LOCKED, PORT_OUT, get_port(CycloneV::FPLL, x, y, -1, CycloneV::LOCK0));

    // Feedback clock input (right/left variants depending on tile).
    if (has_port(CycloneV::FPLL, x, y, -1, CycloneV::FBCLK_IN_R0))
        add_bel_pin(bel, id_FBCLK, PORT_IN, get_port(CycloneV::FPLL, x, y, -1, CycloneV::FBCLK_IN_R0));
    else if (has_port(CycloneV::FPLL, x, y, -1, CycloneV::FBCLK_IN_L0))
        add_bel_pin(bel, id_FBCLK, PORT_IN, get_port(CycloneV::FPLL, x, y, -1, CycloneV::FBCLK_IN_L0));
}

bool Arch::is_pll_cell(IdString cell_type) const { return cell_type == id_altera_pll; }

// ---------------------------------------------------------------------------
// G4 — PLL outclk -> global clock network (PLL_OUTCLK_DESIGN.md).
//
// The PLL C-counter outputs reach the clock tree through dedicated wiring into the clock control
// blocks, selected by the cmux INPUT_SEL bitstream mux — configuration, not routing. Both halves of
// that mapping are already in libmistral:
//   p2p tables:   FPLL(pos).PLLCOUT[c] -> CMUX*(pos).PLLIN[k]   (the dedicated wires)
//   link tables:  cmux*_link_table[inst][e] == {CMUX_PLLIN, k}  (which INPUT_SEL entry selects k)
// build_pllclk_map() composes them once at init. v1 models the GLOBAL cmuxes (CMUXHG/CMUXVG);
// regional (CR/HR/VR) and cascade/LVDS outputs are staged out.
// ---------------------------------------------------------------------------

void Arch::build_pllclk_map()
{
    auto scan = [&](const std::pair<uint8_t, uint8_t> (*table)[64], int ninst, CycloneV::pos_t cmux_pos, int k,
                    CycloneV::pos_t fpll_pos, int counter) {
        for (int inst = 0; inst < ninst; inst++)
            for (int e = 0; e < 64; e++)
                if (table[inst][e].first == CycloneV::CMUX_PLLIN && table[inst][e].second == k) {
                    uint64_t key = pllclk_key(fpll_pos, counter, cmux_pos, inst);
                    // first (lowest) entry wins; the tables repeat some sources
                    if (!pllclk_sel_map.count(key))
                        pllclk_sel_map[key] = uint8_t(e);
                }
    };
    int links = 0;
    for (auto &pp : cyclonev->get_all_p2p()) {
        if (CycloneV::pn2bt(pp.first) != CycloneV::FPLL || CycloneV::pn2pt(pp.first) != CycloneV::PLLCOUT)
            continue;
        if (CycloneV::pn2pt(pp.second) != CycloneV::PLLIN)
            continue;
        int counter = CycloneV::pn2pi(pp.first);
        int k = CycloneV::pn2pi(pp.second);
        auto fpll_pos = CycloneV::pn2p(pp.first);
        auto cmux_pos = CycloneV::pn2p(pp.second);
        switch (CycloneV::pn2bt(pp.second)) {
        case CycloneV::CMUXHG:
            scan(CycloneV::cmuxhg_link_table, 4, cmux_pos, k, fpll_pos, counter);
            links++;
            break;
        case CycloneV::CMUXVG:
            scan(CycloneV::cmuxvg_link_table, 4, cmux_pos, k, fpll_pos, counter);
            links++;
            break;
        default:
            break; // regional cmuxes: v2
        }
    }
    log_info("PLL clock map: %d dedicated FPLL->global-cmux links, %d (pll,counter,gclk) selections.\n", links,
             int(pllclk_sel_map.size()));
}

int Arch::pllclk_lookup(uint32_t fpll_pos, int counter, uint32_t cmux_pos, int inst) const
{
    auto it = pllclk_sel_map.find(pllclk_key(fpll_pos, counter, cmux_pos, inst));
    return it == pllclk_sel_map.end() ? -1 : int(it->second);
}

bool Arch::pllclk_pos_is_vertical(uint32_t cmux_pos) const
{
    for (auto p : cyclonev->cmuxv_get_pos())
        if (uint32_t(p) == cmux_pos)
            return true;
    return false;
}

bool Arch::pllclk_choose(int nclk, uint32_t &fpll_pos_out, std::vector<PllClkChoice> &out) const
{
    // Prefer FPLL(0,0): the ground-truth network-refclk PLL with a PROVEN fabric MCNT feedback
    // path via CMUXVG(42,0) PLL_FEEDBACK_ENABLE_3. (0,14)/(0,31)/(0,55) are HSSI-adjacent fPLLs
    // whose feedback wiring is transceiver-side (p2p-verified) — silicon round 4 froze on (0,14).
    std::vector<CycloneV::pos_t> order;
    for (auto fp : cyclonev->fpll_get_pos())
        if (uint32_t(fp) == uint32_t(CycloneV::xy2pos(0, 0)))
            order.insert(order.begin(), fp);
        else
            order.push_back(fp);
    for (auto fp : order) {
        uint32_t fpll_pos = uint32_t(fp);
        std::vector<PllClkChoice> picks;
        std::set<std::pair<uint32_t, int>> used_gclk;
        std::set<int> used_counter;
        for (int i = 0; i < nclk; i++) {
            bool found = false;
            // ascending counter order keeps the choice deterministic and reproducible
            for (int c = 0; c < 9 && !found; c++) {
                if (used_counter.count(c))
                    continue;
                for (auto &kv : pllclk_sel_map) {
                    uint32_t k_fpll = uint32_t(kv.first >> 40);
                    int k_counter = int((kv.first >> 32) & 0xff);
                    uint32_t k_cmux = uint32_t((kv.first >> 8) & 0xffffff);
                    int k_inst = int(kv.first & 0xff);
                    if (k_fpll != fpll_pos || k_counter != c)
                        continue;
                    if (used_gclk.count({k_cmux, k_inst}))
                        continue;
                    picks.push_back(PllClkChoice{k_cmux, k_inst, c, int(kv.second)});
                    used_gclk.insert({k_cmux, k_inst});
                    used_counter.insert(c);
                    found = true;
                    break;
                }
            }
            if (!found)
                break;
        }
        if (int(picks.size()) == nclk) {
            fpll_pos_out = fpll_pos;
            out = std::move(picks);
            return true;
        }
    }
    return false;
}

// One MISTRAL_PLLCLK bel per gclk instance of a global cmux. Like MISTRAL_CLKENA/CLKBUF its Q pin
// binds the CLKOUT (GCLK root) wire and the downstream GCLK->SCLK->TCLK routing needs nothing new;
// unlike CLKBUF it has NO routed input — the input is the dedicated PLL wiring, chosen purely by
// the INPUT_SEL configuration emitted in bitstream.cc from pllclk_sel_map.
void Arch::create_pllclk(int x, int y, bool vertical)
{
    auto bt = vertical ? CycloneV::CMUXVG : CycloneV::CMUXHG;
    for (int g = 0; g < 4; g++) {
        if (!has_port(bt, x, y, g, CycloneV::CLKOUT))
            continue;
        BelId bel = add_bel(x, y, idf("PLLCLK[%d]", g), id_MISTRAL_PLLCLK);
        add_bel_pin(bel, id_Q, PORT_OUT, get_port(bt, x, y, g, CycloneV::CLKOUT));
        bel_data(bel).block_index = g;
    }
}

NEXTPNR_NAMESPACE_END
