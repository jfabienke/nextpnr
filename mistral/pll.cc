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
// Only a subset of the FPLL's ports are exposed to the general routing graph
// in the Mistral p2r model:
//   coreclk0    (PMUX out)  - core-clock output       -> OUTCLK[0]
//   lock0       (GIN)       - PLL locked status output -> LOCKED
//   fbclk_in_r0 (DCMUX in)  - feedback clock input     -> FBCLK
// The reference clock reaches the PLL over dedicated clock routing that is not
// modelled as a fabric port, so REFCLK is added only if a node resolves (it
// generally will not on e50f); routing refclk -> PLL is the known open piece.
void Arch::create_fpll(int x, int y)
{
    BelId bel = add_bel(x, y, id_MISTRAL_FPLL, id_altera_pll);
    bel_data(bel).block_index = 0;

    // Reference clock input - dedicated clock routing, usually not a fabric
    // node. Guarded so bel creation never aborts if the node is absent.
    if (has_port(CycloneV::FPLL, x, y, -1, CycloneV::REFCLKIN))
        add_bel_pin(bel, id_REFCLK, PORT_IN, get_port(CycloneV::FPLL, x, y, -1, CycloneV::REFCLKIN));

    // Output clocks. Only coreclk0 is exposed to routing in the e50f p2r data,
    // so only outclk[0] can currently be routed through general resources.
    if (has_port(CycloneV::FPLL, x, y, -1, CycloneV::CORECLK0))
        add_bel_pin(bel, idf("OUTCLK[%d]", 0), PORT_OUT, get_port(CycloneV::FPLL, x, y, -1, CycloneV::CORECLK0));

    // Locked status output.
    if (has_port(CycloneV::FPLL, x, y, -1, CycloneV::LOCK0))
        add_bel_pin(bel, id_LOCKED, PORT_OUT, get_port(CycloneV::FPLL, x, y, -1, CycloneV::LOCK0));

    // Feedback clock input.
    if (has_port(CycloneV::FPLL, x, y, -1, CycloneV::FBCLK_IN_R0))
        add_bel_pin(bel, id_FBCLK, PORT_IN, get_port(CycloneV::FPLL, x, y, -1, CycloneV::FBCLK_IN_R0));
}

bool Arch::is_pll_cell(IdString cell_type) const { return cell_type == id_altera_pll; }

NEXTPNR_NAMESPACE_END
