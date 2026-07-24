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

NEXTPNR_NAMESPACE_END
