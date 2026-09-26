/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  gatecat <gatecat@ds0.me>
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

#include "design_utils.h"
#include "lab_control_edits.h"
#include "lab_control_plan.h"
#include "lab_preparation.h"
#include "lab_snapshot.h"
#include "lab_v2.h"
#include "log.h"
#include "nextpnr.h"
#include "register_packing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

// This file contains functions related to our custom LAB structure, including creating the LAB bels; checking the
// legality of LABs; and manipulating LUT inputs and equations

// LAB/ALM structure creation functions
namespace {
static void create_alm(Arch *arch, int x, int y, int z, uint32_t lab_idx)
{
    auto &lab = arch->labs.at(lab_idx);
    auto &alm = lab.alms.at(z);
    auto block_type = lab.is_mlab ? CycloneV::MLAB : CycloneV::LAB;
    // Create the control set and E/F selection - which is per pair of FF
    for (int i = 0; i < 2; i++) {
        // Wires
        alm.sel_clk[i] = arch->add_wire(x, y, arch->idf("CLK%c[%d]", i ? 'B' : 'T', z));
        alm.sel_ena[i] = arch->add_wire(x, y, arch->idf("ENA%c[%d]", i ? 'B' : 'T', z));
        alm.sel_aclr[i] = arch->add_wire(x, y, arch->idf("ACLR%c[%d]", i ? 'B' : 'T', z));
        alm.sel_ef[i] = arch->add_wire(x, y, arch->idf("%cEF[%d]", i ? 'B' : 'T', z));
        // Muxes - three CLK/ENA per LAB, two ACLR
        for (int j = 0; j < 3; j++) {
            arch->add_pip(lab.clk_wires[j], alm.sel_clk[i]);
            arch->add_pip(lab.ena_wires[j], alm.sel_ena[i]);
            if (j < 2)
                arch->add_pip(lab.aclr_wires[j], alm.sel_aclr[i]);
        }
        // E/F pips
        // Note that the F choice is mirrored, F from the other half is picked
        arch->add_pip(arch->get_port(block_type, x, y, z, i ? CycloneV::E1 : CycloneV::E0), alm.sel_ef[i]);
        arch->add_pip(arch->get_port(block_type, x, y, z, i ? CycloneV::F0 : CycloneV::F1), alm.sel_ef[i]);
    }
    // Create the combinational part of ALMs.
    // There are two of these, for the two LUT outputs, and these also contain the carry chain and associated logic
    // Each one has all 8 ALM inputs as input pins. In many cases only a subset of these are used; depending on mode;
    // and the bel-cell pin mappings are used to handle this post-placement without losing flexibility
    for (int i = 0; i < 2; i++) {
        // Carry/share wires are a bit tricky due to all the different permutations
        WireId carry_in, share_in;
        WireId carry_out, share_out;
        if (z == 0 && i == 0) {
            carry_in = arch->add_wire(x, y, id_CI);
            share_in = arch->add_wire(x, y, id_SHAREIN);
            if (y < (arch->getGridDimY() - 1)) {
                // Carry is split at tile boundary (TTO_DIS bit), add a PIP to represent this.
                // TODO: what about BTO_DIS, in the middle of the LAB?
                arch->add_pip(arch->add_wire(x, y + 1, id_CO), carry_in);
                arch->add_pip(arch->add_wire(x, y + 1, id_SHAREOUT), share_in);
            }
        } else {
            // Output from last combinational unit
            carry_in = arch->add_wire(x, y, arch->idf("CARRY[%d]", (z * 2 + i) - 1));
            share_in = arch->add_wire(x, y, arch->idf("SHARE[%d]", (z * 2 + i) - 1));
        }

        if (z == 9 && i == 1) {
            carry_out = arch->add_wire(x, y, id_CO);
            share_out = arch->add_wire(x, y, id_SHAREOUT);
        } else {
            carry_out = arch->add_wire(x, y, arch->idf("CARRY[%d]", z * 2 + i));
            share_out = arch->add_wire(x, y, arch->idf("SHARE[%d]", z * 2 + i));
        }

        BelId bel =
                arch->add_bel(x, y, arch->idf("ALM%d_COMB%d", z, i), lab.is_mlab ? id_MISTRAL_MCOMB : id_MISTRAL_COMB);
        // LUT/MUX inputs
        arch->add_bel_pin(bel, id_A, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::A));
        arch->add_bel_pin(bel, id_B, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::B));
        arch->add_bel_pin(bel, id_C, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::C));
        arch->add_bel_pin(bel, id_D, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::D));
        arch->add_bel_pin(bel, id_E0, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::E0));
        arch->add_bel_pin(bel, id_E1, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::E1));
        arch->add_bel_pin(bel, id_F0, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::F0));
        arch->add_bel_pin(bel, id_F1, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::F1));
        // Carry/share chain
        arch->add_bel_pin(bel, id_CI, PORT_IN, carry_in);
        arch->add_bel_pin(bel, id_SHAREIN, PORT_IN, share_in);
        arch->add_bel_pin(bel, id_CO, PORT_OUT, carry_out);
        arch->add_bel_pin(bel, id_SHAREOUT, PORT_OUT, share_out);
        // Combinational output
        alm.comb_out[i] = arch->add_wire(x, y, arch->idf("COMBOUT[%d]", z * 2 + i));
        arch->add_bel_pin(bel, id_COMBOUT, PORT_OUT, alm.comb_out[i]);
        if (lab.is_mlab) {
            // Write address - shared between all ALMs in a LAB
            arch->add_bel_pin(bel, id_WA0, PORT_IN, arch->get_port(block_type, x, y, 2, CycloneV::F1));
            arch->add_bel_pin(bel, id_WA1, PORT_IN, arch->get_port(block_type, x, y, 3, CycloneV::F1));
            arch->add_bel_pin(bel, id_WA2, PORT_IN, arch->get_port(block_type, x, y, 7, CycloneV::F1));
            arch->add_bel_pin(bel, id_WA3, PORT_IN, arch->get_port(block_type, x, y, 6, CycloneV::F1));
            arch->add_bel_pin(bel, id_WA4, PORT_IN, arch->get_port(block_type, x, y, 1, CycloneV::F1));
            // Write clock and enable appear to be based on bottom FF
            arch->add_bel_pin(bel, id_WCLK, PORT_IN, alm.sel_clk[1]);
            arch->add_bel_pin(bel, id_WE, PORT_IN, alm.sel_ena[1]);
        }
        // Design 20.2: five logical-input pseudo-wires per half, each fed from every physical pin of the half
        // (A and B shared, C or D, E_i, F_i); the router picks the pin, lut_permutation_fixup records it.
        if (arch->args.lut_permutation) {
            const std::array<CycloneV::port_type_t, 5> phys{CycloneV::A, CycloneV::B, i ? CycloneV::D : CycloneV::C,
                                                            i ? CycloneV::E1 : CycloneV::E0,
                                                            i ? CycloneV::F1 : CycloneV::F0};
            for (int k = 0; k < 5; k++) {
                WireId w = arch->add_wire(x, y, arch->idf("LPERM%d_%d[%d]", i, k, z));
                arch->add_bel_pin(bel, arch->lperm_pins[k], PORT_IN, w);
                for (auto p : phys)
                    arch->add_pip(arch->get_port(block_type, x, y, z, p), w);
                alm.perm_wires[i][k] = w;
            }
        }
        // Assign indexing
        alm.lut_bels.at(i) = bel;
        auto &b = arch->bel_data(bel);
        b.lab_data.lab = lab_idx;
        b.lab_data.alm = z;
        b.lab_data.idx = i;
    }

    // Create the flipflops and associated routing
    const CycloneV::port_type_t outputs[4] = {CycloneV::FFT0, CycloneV::FFT1, CycloneV::FFB0, CycloneV::FFB1};
    const CycloneV::port_type_t l_outputs[4] = {CycloneV::FFT1L, CycloneV::FFB1L};

    for (int i = 0; i < 4; i++) {
        // FF input, selected by *PKREG*
        alm.ff_in[i] = arch->add_wire(x, y, arch->idf("FFIN[%d]", (z * 4) + i));
        arch->add_pip(alm.comb_out[i / 2], alm.ff_in[i]);
        arch->add_pip(alm.sel_ef[i / 2], alm.ff_in[i]);
        // FF bel
        BelId bel = arch->add_bel(x, y, arch->idf("ALM%d_FF%d", z, i), id_MISTRAL_FF);
        arch->add_bel_pin(bel, id_CLK, PORT_IN, alm.sel_clk[i / 2]);
        arch->add_bel_pin(bel, id_ENA, PORT_IN, alm.sel_ena[i / 2]);
        arch->add_bel_pin(bel, id_ACLR, PORT_IN, alm.sel_aclr[i / 2]);
        arch->add_bel_pin(bel, id_SCLR, PORT_IN, lab.sclr_wire);
        arch->add_bel_pin(bel, id_SLOAD, PORT_IN, lab.sload_wire);
        arch->add_bel_pin(bel, id_DATAIN, PORT_IN, alm.ff_in[i]);
        arch->add_bel_pin(bel, id_SDATA, PORT_IN, alm.sel_ef[i / 2]);

        // FF output
        alm.ff_out[i] = arch->add_wire(x, y, arch->idf("FFOUT[%d]", (z * 4) + i));
        arch->add_bel_pin(bel, id_Q, PORT_OUT, alm.ff_out[i]);
        // Output mux (*DFF*)
        WireId out = arch->get_port(block_type, x, y, z, outputs[i]);
        arch->add_pip(alm.ff_out[i], out);
        arch->add_pip(alm.comb_out[i / 2], out);
        // 'L' output mux where applicable
        if (i == 1 || i == 3) {
            WireId l_out = arch->get_port(block_type, x, y, z, l_outputs[i / 2]);
            arch->add_pip(alm.ff_out[i], l_out);
            arch->add_pip(alm.comb_out[i / 2], l_out);
        }

        lab.alms.at(z).ff_bels.at(i) = bel;
        auto &b = arch->bel_data(bel);
        b.lab_data.lab = lab_idx;
        b.lab_data.alm = z;
        b.lab_data.idx = i;
    }

    // TODO: MLAB-specific pins
}
} // namespace

void Arch::create_lab(int x, int y, bool is_mlab)
{
    uint32_t lab_idx = labs.size();
    labs.emplace_back();

    auto &lab = labs.back();

    lab.is_mlab = is_mlab;
    auto block_type = is_mlab ? CycloneV::MLAB : CycloneV::LAB;

    // Create common control set configuration. This is actually a subset of what's possible, but errs on the side of
    // caution due to incomplete documentation

    // Clocks - hardcode to CLKA choices, as both CLKA and CLKB coming from general routing causes unexpected
    // permutations
    for (int i = 0; i < 3; i++) {
        lab.clk_wires[i] = add_wire(x, y, idf("CLK%d", i));
        add_pip(get_port(block_type, x, y, -1, CycloneV::CLKIN, 0), lab.clk_wires[i]);  // dedicated routing
        add_pip(get_port(block_type, x, y, -1, CycloneV::DATAIN, 0), lab.clk_wires[i]); // general routing
    }

    // Enables - while it looks from the config like there are choices for these, it seems like EN0_SEL actually selects
    // SCLR not ENA0 and EN1_SEL actually selects SLOAD?
    lab.ena_wires[0] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 2);
    lab.ena_wires[1] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 3);
    lab.ena_wires[2] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 0);

    // ACLRs - only consider general routing for now
    lab.aclr_wires[0] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 3);
    lab.aclr_wires[1] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 2);

    // SCLR and SLOAD - as above it seems like these might be selectable using the "EN*_SEL" bits but play it safe for
    // now
    lab.sclr_wire = get_port(block_type, x, y, -1, CycloneV::DATAIN, 3);
    lab.sload_wire = get_port(block_type, x, y, -1, CycloneV::DATAIN, 1);

    for (int i = 0; i < 10; i++) {
        create_alm(this, x, y, i, lab_idx);
    }
}

// Cell handling and annotation functions
namespace {
ControlSig get_ctrlsig(const Context *ctx, const CellInfo *cell, IdString port, bool explicit_const = false)
{
    ControlSig result;
    result.net = cell->getPort(port);
    if (result.net == nullptr && explicit_const) {
        // For ENA, 1 (and 0) are explicit control set choices even though they aren't routed, as "no ENA" still
        // consumes a clock+ENA pair
        CellPinState st = PIN_1;
        result.net = ctx->nets.at((st == PIN_1) ? ctx->id("$PACKER_VCC_NET") : ctx->id("$PACKER_GND_NET")).get();
    }
    if (cell->pin_data.count(port))
        result.inverted = cell->pin_data.at(port).state == PIN_INV;
    else
        result.inverted = false;
    return result;
}
} // namespace

bool Arch::is_comb_cell(IdString cell_type) const
{
    // Return true if a cell is a combinational cell type, to be a placed at a MISTRAL_COMB location
    switch (cell_type.index) {
    case ID_MISTRAL_ALUT6:
    case ID_MISTRAL_ALUT5:
    case ID_MISTRAL_ALUT4:
    case ID_MISTRAL_ALUT3:
    case ID_MISTRAL_ALUT2:
    case ID_MISTRAL_NOT:
    case ID_MISTRAL_CONST:
    case ID_MISTRAL_ALUT_ARITH:
        return true;
    default:
        return false;
    }
}

dict<IdString, IdString> Arch::get_mlab_key(const CellInfo *cell, bool include_raddr) const
{
    dict<IdString, IdString> key;
    for (auto &port : cell->ports) {
        if (port.first.in(id_A1DATA, id_B1DATA))
            continue;
        if (!include_raddr && port.first.str(this).find("B1ADDR") == 0)
            continue;
        key[port.first] = port.second.net ? port.second.net->name : IdString();
    }
    if (cell->pin_data.count(id_CLK1) && cell->pin_data.at(id_CLK1).state == PIN_INV)
        key[id_WCLK_INV] = id_Y;
    if (cell->pin_data.count(id_A1EN) && cell->pin_data.at(id_A1EN).state == PIN_INV)
        key[id_WE_INV] = id_Y;
    return key;
}

void Arch::assign_comb_info(CellInfo *cell) const
{
    note_lab_cell_mutation(cell);
    cell->combInfo.is_carry = false;
    cell->combInfo.is_shared = false;
    cell->combInfo.is_extended = false;
    cell->combInfo.carry_start = false;
    cell->combInfo.carry_end = false;
    cell->combInfo.chain_shared_input_count = 0;
    cell->combInfo.mlab_group = -1;
    // Only MLAB cells have write controls; the LAB captures read them for every LUT, and the union's bytes are
    // otherwise whatever the allocator left (zero on macOS, string data on Linux).
    cell->combInfo.wclk = ControlSig{nullptr, false};
    cell->combInfo.we = ControlSig{nullptr, false};

    if (cell->type == id_MISTRAL_MLAB) {
        cell->combInfo.wclk = get_ctrlsig(getCtx(), cell, id_CLK1);
        cell->combInfo.we = get_ctrlsig(getCtx(), cell, id_A1EN, true);
        cell->combInfo.lut_input_count = 5;
        cell->combInfo.lut_bits_count = 32;
        for (int i = 0; i < 5; i++)
            cell->combInfo.lut_in[i] = cell->getPort(idf("B1ADDR[%d]", i));
        auto key = get_mlab_key(cell);
        cell->combInfo.mlab_group = mlab_groups(key);
        cell->combInfo.comb_out = cell->getPort(id_B1DATA);
    } else if (cell->type == id_MISTRAL_ALUT_ARITH) {
        cell->combInfo.is_carry = true;
        cell->combInfo.lut_input_count = 5;
        cell->combInfo.lut_bits_count = 32;

        // This is a special case in terms of naming
        const std::array<IdString, 5> arith_pins{id_A, id_B, id_C, id_D0, id_D1};
        {
            int i = 0;
            for (auto pin : arith_pins) {
                cell->combInfo.lut_in[i++] = cell->getPort(pin);
            }
        }

        const NetInfo *ci = cell->getPort(id_CI);
        const NetInfo *co = cell->getPort(id_CO);

        cell->combInfo.comb_out = cell->getPort(id_SO);
        cell->combInfo.carry_start = (ci == nullptr) || (ci->driver.cell == nullptr);
        cell->combInfo.carry_end = (co == nullptr) || (co->users.empty());

        // Compute cross-ALM routing sharing - only check the z=0 case inside ALMs
        if (cell->constr_z > 0 && ((cell->constr_z % 2) == 0) && ci) {
            const CellInfo *prev = ci->driver.cell;
            if (prev != nullptr) {
                for (int i = 0; i < 5; i++) {
                    const NetInfo *a = cell->getPort(arith_pins[i]);
                    if (a == nullptr)
                        continue;
                    const NetInfo *b = prev->getPort(arith_pins[i]);
                    if (a == b)
                        ++cell->combInfo.chain_shared_input_count;
                }
            }
        }

    } else {
        cell->combInfo.comb_out = cell->getPort(id_Q);
        cell->combInfo.lut_input_count = 0;
        switch (cell->type.index) {
        case ID_MISTRAL_ALUT6:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[5] = cell->getPort(id_F);
            [[fallthrough]];
        case ID_MISTRAL_ALUT5:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[4] = cell->getPort(id_E);
            [[fallthrough]];
        case ID_MISTRAL_ALUT4:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[3] = cell->getPort(id_D);
            [[fallthrough]];
        case ID_MISTRAL_ALUT3:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[2] = cell->getPort(id_C);
            [[fallthrough]];
        case ID_MISTRAL_ALUT2:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[1] = cell->getPort(id_B);
            [[fallthrough]];
        case ID_MISTRAL_BUF: // used to route through to FFs etc
        case ID_MISTRAL_NOT: // used for inverters that map to LUTs
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[0] = cell->getPort(id_A);
            [[fallthrough]];
        case ID_MISTRAL_CONST:
            // MISTRAL_CONST is a nextpnr-inserted cell type for 0-input, constant-generating LUTs
            break;
        default:
            log_error("unexpected combinational cell type %s\n", getCtx()->nameOf(cell->type));
        }
        // Note that this relationship won't hold for extended mode, when that is supported
        cell->combInfo.lut_bits_count = (1 << cell->combInfo.lut_input_count);
    }
    cell->combInfo.used_lut_input_count = 0;
    for (int i = 0; i < cell->combInfo.lut_input_count; i++)
        if (cell->combInfo.lut_in[i])
            ++cell->combInfo.used_lut_input_count;
    placement_revision.note_mutation(PlacementMutation::CellFacts);
}

void Arch::assign_ff_info(CellInfo *cell) const
{
    note_lab_cell_mutation(cell);
    cell->ffInfo.ctrlset.clk = get_ctrlsig(getCtx(), cell, id_CLK);
    cell->ffInfo.ctrlset.ena = get_ctrlsig(getCtx(), cell, id_ENA, true);
    cell->ffInfo.ctrlset.aclr = get_ctrlsig(getCtx(), cell, id_ACLR);
    cell->ffInfo.ctrlset.sclr = get_ctrlsig(getCtx(), cell, id_SCLR);
    cell->ffInfo.ctrlset.sload = get_ctrlsig(getCtx(), cell, id_SLOAD);
    // If SCLR is used, but SLOAD isn't, then it seems like we need to pretend as if SLOAD is connected GND (so set
    // [BT]SLOAD_EN inside the ALMs, and clear SLOAD_INV)
    if (cell->ffInfo.ctrlset.sclr.net != nullptr && cell->ffInfo.ctrlset.sload.net == nullptr) {
        cell->ffInfo.ctrlset.sload.net = nets.at(id("$PACKER_GND_NET")).get();
        cell->ffInfo.ctrlset.sload.inverted = false;
    }

    cell->ffInfo.sdata = cell->getPort(id_SDATA);
    cell->ffInfo.datain = cell->getPort(id_DATAIN);
    placement_revision.note_mutation(PlacementMutation::CellFacts);
}

// Validity checking functions
bool Arch::is_alm_legal(uint32_t lab, uint8_t alm) const
{
    // HOT PATH: called on every bind/unbind of a LAB cell, and the strict legaliser does millions
    // of those while searching cluster placements (profiled: the .at() bounds-check chains under
    // this function were the second-largest cycle sink after the search itself). Unchecked
    // indexing; the indices come from the arch's own tables.
    auto bound_fast = [&](BelId b) -> const CellInfo * { return bels_by_tile[pos2idx(b.pos)][b.z].bound; };
    return alm_legal_with(labs[lab].alms[alm], bound_fast);
}

bool Arch::is_alm_legal_overlay(uint32_t lab, uint8_t alm, const BelOverlay &overlay) const
{
    auto bound = [&](BelId b) -> const CellInfo * {
        return overlay.lookup(b, bels_by_tile[pos2idx(b.pos)][b.z].bound);
    };
    return alm_legal_with(labs[lab].alms[alm], bound);
}

template <typename Bound> bool Arch::alm_legal_with(const ALMInfo &alm_data, Bound bound_fast) const
{
    // Get cells into an array for fast access
    std::array<const CellInfo *, 2> luts{bound_fast(alm_data.lut_bels[0]), bound_fast(alm_data.lut_bels[1])};
    std::array<const CellInfo *, 4> ffs{bound_fast(alm_data.ff_bels[0]), bound_fast(alm_data.ff_bels[1]),
                                        bound_fast(alm_data.ff_bels[2]), bound_fast(alm_data.ff_bels[3])};
    int used_lut_bits = 0;

    int total_lut_inputs = 0;
    // TODO: for more complex modes like extended/arithmetic, it might not always be possible for any LUT input to map
    // to any of the ALM half inputs particularly shared and extended mode will need more thought and probably for this
    // to be revisited
    for (int i = 0; i < 2; i++) {
        if (!luts[i])
            continue;
        total_lut_inputs += luts[i]->combInfo.lut_input_count;
        used_lut_bits += luts[i]->combInfo.lut_bits_count;
    }
    // An ALM only has 64 bits of storage. In theory some of these cases might be legal because of overlap between the
    // two functions, but the current placer is unlikely to stumble upon these cases frequently without anything to
    // guide it, and the cost of checking them here almost certainly outweighs any marginal benefit in supporting them,
    // at least for now.
    if (used_lut_bits > 64)
        return false;

    if (total_lut_inputs > 8) {
        NPNR_ASSERT(luts[0] && luts[1]); // something has gone badly wrong if this fails!
        // Make sure that LUT inputs are not overprovisioned
        int shared_lut_inputs = 0;
        // Even though this N^2 search looks inefficient, it's unlikely a set lookup or similar is going to be much
        // better given the low N.
        for (int i = 0; i < luts[1]->combInfo.lut_input_count; i++) {
            const NetInfo *sig = luts[1]->combInfo.lut_in[i];
            for (int j = 0; j < luts[0]->combInfo.lut_input_count; j++) {
                if (sig == luts[0]->combInfo.lut_in[j]) {
                    ++shared_lut_inputs;
                    break;
                }
            }
        }
        if ((total_lut_inputs - shared_lut_inputs) > 8)
            return false;
    }

    bool carry_mode = (luts[0] && luts[0]->combInfo.is_carry) || (luts[1] && luts[1]->combInfo.is_carry);

    // No mixing of carry and non-carry
    if (luts[0] && luts[1] && luts[0]->combInfo.is_carry != luts[1]->combInfo.is_carry)
        return false;

    const bool l6_lut =
            (luts[0] && luts[0]->combInfo.lut_input_count > 5) || (luts[1] && luts[1]->combInfo.lut_input_count > 5);
    // For each ALM half; check FF control set sharing and input routeability
    for (int i = 0; i < 2; i++) {
        // There are two ways to route from the fabric into FF data - either routing through a LUT or using the E/F
        // signals and SLOAD=1 (*PKREF*)
        bool route_thru_lut_avail = !luts[i] && !carry_mode && (total_lut_inputs < 8) && (used_lut_bits < 64);
        // E/F is available if this LUT is using 3 or fewer inputs - this is conservative and sharing can probably
        // improve this situation. (1 - i) because the F input to EF_SEL is mirrored.
        bool ef_available = (!luts[1 - i] || (luts[1 - i]->combInfo.used_lut_input_count <= 2));
        // Control set checking
        bool found_ff = false;

        FFControlSet ctrlset;
        for (int j = 0; j < 2; j++) {
            const CellInfo *ff = ffs[i * 2 + j];
            if (!ff)
                continue;
            // The second register of a half (MISTRAL_GAPS G9): only a register the packer placed there beside the
            // LUT of its half that drives it (--alm-both-registers); never in a carry half or beside a six-input LUT.
            if (j == 1 && !(is_second_register_child(ff) && luts[i] && !carry_mode && !l6_lut && ff->ffInfo.datain &&
                            ff->ffInfo.datain == luts[i]->combInfo.comb_out))
                return false;
            // A second register is clocked through the other half's clock select (MISTRAL_GAPS G9), which the
            // bitstream writer then sets from it: every register of the ALM must share its control set.
            if (j == 1)
                for (const CellInfo *other : ffs)
                    if (other && other != ff && other->ffInfo.ctrlset != ff->ffInfo.ctrlset)
                        return false;
            if (found_ff) {
                // Two FFs in the same half with an incompatible control set
                if (ctrlset != ff->ffInfo.ctrlset)
                    return false;
            } else {
                ctrlset = ff->ffInfo.ctrlset;
            }
            // SDATA must use the E/F input
            // TODO: rare case of two FFs with the same SDATA in the same ALM half
            if (ff->ffInfo.sdata) {
                if (!ef_available)
                    return false;
                ef_available = false;
            }
            // Find a way of routing the input through fabric, if it's not driven by the LUT
            if (ff->ffInfo.datain && (!luts[i] || (ff->ffInfo.datain != luts[i]->combInfo.comb_out))) {
                if (route_thru_lut_avail)
                    route_thru_lut_avail = false;
                else if (ef_available)
                    ef_available = false;
                else
                    return false;
            }
            found_ff = true;
        }
    }

    return true;
}

void Arch::update_alm_input_count(uint32_t lab, uint8_t alm)
{
    // HOT PATH: called on every bind/unbind of a LAB cell, and the strict legaliser does millions
    // of those while searching cluster placements (profiled: the .at() bounds-check chains under
    // this function were the second-largest cycle sink after the search itself). Unchecked
    // indexing; the indices come from the arch's own tables.
    auto bound_fast = [&](BelId b) -> const CellInfo * { return bels_by_tile[pos2idx(b.pos)][b.z].bound; };
    labs[lab].alms[alm].unique_input_count = alm_input_count_with(labs[lab].alms[alm], bound_fast);
}

int Arch::alm_input_count_overlay(uint32_t lab, uint8_t alm, const BelOverlay &overlay) const
{
    auto bound = [&](BelId b) -> const CellInfo * {
        return overlay.lookup(b, bels_by_tile[pos2idx(b.pos)][b.z].bound);
    };
    return alm_input_count_with(labs[lab].alms[alm], bound);
}

template <typename Bound> int Arch::alm_input_count_with(const ALMInfo &alm_data, Bound bound_fast) const
{
    // Get cells into an array for fast access
    std::array<const CellInfo *, 2> luts{bound_fast(alm_data.lut_bels[0]), bound_fast(alm_data.lut_bels[1])};
    std::array<const CellInfo *, 4> ffs{bound_fast(alm_data.ff_bels[0]), bound_fast(alm_data.ff_bels[1]),
                                        bound_fast(alm_data.ff_bels[2]), bound_fast(alm_data.ff_bels[3])};
    int total_inputs = 0;
    int total_lut_inputs = 0;
    for (int i = 0; i < 2; i++) {
        if (!luts[i])
            continue;
        // MLAB that has been clustered with other MLABs (due to shared read port) costs no extra inputs
        if (luts[i]->combInfo.mlab_group != -1 && luts[i]->constr_z > 2)
            return 0;

        total_lut_inputs += luts[i]->combInfo.used_lut_input_count - luts[i]->combInfo.chain_shared_input_count;
    }
    int shared_lut_inputs = 0;
    if (luts[0] && luts[1]) {
        for (int i = 0; i < luts[1]->combInfo.lut_input_count; i++) {
            const NetInfo *sig = luts[1]->combInfo.lut_in[i];
            if (!sig)
                continue;
            for (int j = 0; j < luts[0]->combInfo.lut_input_count; j++) {
                if (sig == luts[0]->combInfo.lut_in[j]) {
                    ++shared_lut_inputs;
                    break;
                }
            }
            if (shared_lut_inputs >= 2 && luts[0]->combInfo.mlab_group == -1) {
                // only 2 inputs have guaranteed sharing in non-MLAB mode, without routeability based LUT permutation at
                // least
                break;
            }
        }
    }
    total_inputs = std::max(0, total_lut_inputs - shared_lut_inputs);
    for (int i = 0; i < 4; i++) {
        const CellInfo *ff = ffs[i];
        if (!ff)
            continue;
        if (ff->ffInfo.sdata)
            ++total_inputs;
        // FF input doesn't consume routing resources if driven by associated LUT
        if (ff->ffInfo.datain && (!luts[i / 2] || ff->ffInfo.datain != luts[i / 2]->combInfo.comb_out))
            ++total_inputs;
    }
    return total_inputs;
}

// Design 19.10: the distinct-net LAB input demand. A net needs an input line into a LAB when a cell of the LAB
// uses it on a data pin (a LUT input, a register's data or synchronous data) and no LUT of the LAB drives it: a
// LUT's output reaches the LAB's own ALMs on local lines, a register's first output does not. The per-ALM count
// above counts a net once per ALM it enters; the hardware carries it on one line.
namespace {
bool is_lut_bel_type(IdString type) { return type == id_MISTRAL_COMB || type == id_MISTRAL_MCOMB; }

// Calls f(net) for each data pin of a cell on a bel of the given kind (a pin per call, repeats included).
template <typename F> void for_data_nets(const CellInfo *cell, bool lut, F f)
{
    if (lut) {
        for (int i = 0; i < cell->combInfo.lut_input_count; i++)
            if (cell->combInfo.lut_in[i])
                f(cell->combInfo.lut_in[i]);
    } else {
        if (cell->ffInfo.datain)
            f(cell->ffInfo.datain);
        if (cell->ffInfo.sdata)
            f(cell->ffInfo.sdata);
    }
}

// A tiny net -> count list for the affected nets of one change.
struct NetDeltas
{
    std::array<std::pair<const NetInfo *, int>, 8 * BelOverlay::MAX * 2> items{};
    unsigned count = 0;
    void add(const NetInfo *net, int k)
    {
        for (unsigned i = 0; i < count; i++)
            if (items[i].first == net) {
                items[i].second += k;
                return;
            }
        NPNR_ASSERT(count < items.size());
        items[count++] = {net, k};
    }
};

int uses_of(const LABInfo &lab, const NetInfo *net)
{
    for (auto &u : lab.net_uses)
        if (u.first == net)
            return u.second;
    return 0;
}
} // namespace

// True when a LUT of the LAB drives the net (or nothing drives it: it needs no line). `moving` sits on
// `moving_bel` for this question (BelId() = not placed), whatever its binding says.
bool Arch::lab_net_internal(const NetInfo *net, uint32_t lab, const CellInfo *moving, BelId moving_bel) const
{
    const CellInfo *driver = net->driver.cell;
    if (driver == nullptr)
        return true;
    const BelId bel = driver == moving ? moving_bel : driver->bel;
    if (bel == BelId())
        return false;
    const auto &data = bel_data(bel);
    return is_lut_bel_type(data.type) && data.lab_data.lab == lab;
}

void Arch::lab_demand_apply(const CellInfo *cell, BelId bel, bool binding)
{
    const auto &data = bel_data(bel);
    if (!data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF))
        return;
    const uint32_t lab_index = data.lab_data.lab;
    auto &lab = labs[lab_index];
    const bool lut = data.type != id_MISTRAL_FF;
    NetDeltas deltas;
    for_data_nets(cell, lut, [&](const NetInfo *net) { deltas.add(net, 1); });
    if (lut && cell->combInfo.comb_out)
        deltas.add(cell->combInfo.comb_out, 0); // its sinks here change from outside to inside the LAB
    for (unsigned i = 0; i < deltas.count; i++) {
        const NetInfo *net = deltas.items[i].first;
        const int k = deltas.items[i].second;
        auto found = std::find_if(lab.net_uses.begin(), lab.net_uses.end(),
                                  [&](const std::pair<const NetInfo *, int> &u) { return u.first == net; });
        const int before = found == lab.net_uses.end() ? 0 : found->second;
        const int after = binding ? before + k : before - k;
        NPNR_ASSERT(after >= 0);
        const bool internal_before = lab_net_internal(net, lab_index, cell, binding ? BelId() : bel);
        const bool internal_after = lab_net_internal(net, lab_index, cell, binding ? bel : BelId());
        lab.net_demand += int(after > 0 && !internal_after) - int(before > 0 && !internal_before);
        if (found == lab.net_uses.end()) {
            if (after > 0)
                lab.net_uses.emplace_back(net, after);
        } else if (after == 0) {
            *found = lab.net_uses.back();
            lab.net_uses.pop_back();
        } else {
            found->second = after;
        }
    }
    static const bool check = getenv("MISTRAL_CHECK_LAB_DEMAND") != nullptr;
    if (check) {
        // On an unbind this runs while the cell still sits on its bel; count it as gone.
        const int fresh = lab_demand_with(
                lab_index,
                [&](BelId b) -> const CellInfo * { return (!binding && b == bel) ? nullptr : bel_data(b).bound; },
                [&](const CellInfo *c) { return (!binding && c == cell) ? BelId() : c->bel; });
        if (fresh != lab.net_demand)
            log_error("LAB %u input demand %d after %s %s, recomputed %d\n", lab_index, lab.net_demand,
                      binding ? "binding" : "unbinding", nameOf(cell), fresh);
    }
}

// The demand of a LAB from scratch, with its occupants from bound(bel) and every driver's bel from where(cell).
template <typename Bound, typename Where> int Arch::lab_demand_with(uint32_t lab, Bound bound, Where where) const
{
    std::vector<const NetInfo *> nets;
    for (const auto &alm : labs[lab].alms) {
        for (BelId b : alm.lut_bels)
            if (const CellInfo *c = bound(b))
                for_data_nets(c, true, [&](const NetInfo *n) { nets.push_back(n); });
        for (BelId b : alm.ff_bels)
            if (const CellInfo *c = bound(b))
                for_data_nets(c, false, [&](const NetInfo *n) { nets.push_back(n); });
    }
    std::sort(nets.begin(), nets.end());
    nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
    int demand = 0;
    for (const NetInfo *n : nets) {
        const CellInfo *driver = n->driver.cell;
        if (driver == nullptr)
            continue;
        const BelId b = where(driver);
        if (b != BelId() && is_lut_bel_type(bel_data(b).type) && bel_data(b).lab_data.lab == lab)
            continue;
        ++demand;
    }
    return demand;
}

// The demand under an overlay, from the live demand and the overlay's changes only.
int Arch::lab_demand_overlay(uint32_t lab_index, const BelOverlay &overlay) const
{
    const auto &lab = labs[lab_index];
    // Where a cell sits under the overlay.
    auto where = [&](const CellInfo *c) -> BelId {
        for (unsigned i = 0; i < overlay.count; i++)
            if (overlay.cells[i] == c)
                return overlay.bels[i];
        if (c->bel != BelId() && overlay.lookup(c->bel, c) != c)
            return BelId(); // displaced and not re-placed by the overlay
        return c->bel;
    };
    NetDeltas deltas;
    auto outputs = [&](const CellInfo *c, bool lut) {
        if (c && lut && c->combInfo.comb_out)
            deltas.add(c->combInfo.comb_out, 0);
    };
    for (unsigned i = 0; i < overlay.count; i++) {
        const BelId b = overlay.bels[i];
        const auto &data = bel_data(b);
        if (!data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF))
            continue;
        const bool lut = data.type != id_MISTRAL_FF;
        const CellInfo *old_cell = data.bound;
        const CellInfo *new_cell = overlay.cells[i];
        // A LUT moving anywhere can turn its net's sinks here inside or outside the LAB.
        outputs(old_cell, lut);
        outputs(new_cell, lut);
        if (data.lab_data.lab != lab_index || old_cell == new_cell)
            continue;
        if (old_cell)
            for_data_nets(old_cell, lut, [&](const NetInfo *n) { deltas.add(n, -1); });
        if (new_cell)
            for_data_nets(new_cell, lut, [&](const NetInfo *n) { deltas.add(n, 1); });
    }
    int demand = lab.net_demand;
    for (unsigned i = 0; i < deltas.count; i++) {
        const NetInfo *net = deltas.items[i].first;
        const int live = uses_of(lab, net);
        const int after = live + deltas.items[i].second;
        const bool internal_live = lab_net_internal(net, lab_index, nullptr, BelId());
        bool internal_after = true;
        if (const CellInfo *driver = net->driver.cell) {
            const BelId b = where(driver);
            internal_after = b != BelId() && is_lut_bel_type(bel_data(b).type) && bel_data(b).lab_data.lab == lab_index;
        }
        demand += int(after > 0 && !internal_after) - int(live > 0 && !internal_live);
    }
    static const bool check = getenv("MISTRAL_CHECK_LAB_DEMAND") != nullptr;
    if (check) {
        const int fresh =
                lab_demand_with(lab_index, [&](BelId b) { return overlay.lookup(b, bel_data(b).bound); }, where);
        if (fresh != demand)
            log_error("LAB %u overlay input demand %d, recomputed %d\n", lab_index, demand, fresh);
    }
    return demand;
}

bool Arch::check_lab_input_count_overlay(uint32_t lab, const BelOverlay &overlay) const
{
    if (args.lab_input_nets && !labs[lab].is_mlab)
        return lab_demand_overlay(lab, overlay) <= resolved_lab_input_limit();
    // Stored counts for untouched ALMs, recomputed counts for ALMs the overlay changes.
    int count = 0;
    const auto &lab_data = labs[lab];
    for (uint8_t alm = 0; alm < 10; alm++) {
        if (overlay.touches_alm(lab_data.alms[alm]))
            count += alm_input_count_overlay(lab, alm, overlay);
        else
            count += lab_data.alms[alm].unique_input_count;
    }
    return count <= resolved_lab_input_limit();
}

bool Arch::check_lab_input_count(uint32_t lab) const
{
    if (args.lab_input_nets && !labs[lab].is_mlab)
        return labs[lab].net_demand <= resolved_lab_input_limit(); // design 19.10
    // There are only 46 TD signals available to route signals from general routing to the ALM input. Currently, we
    // check the total sum of ALM inputs is less than 42; 46 minus 4 FF control inputs. This is a conservative check for
    // several reasons, because LD signals are also available for feedback routing from ALM output to input, and because
    // TD signals may be shared if the same net routes to multiple ALMs. But these cases will need careful handling and
    // LUT permutation during routing to be useful; and in any event conservative LAB packing will help nextpnr's
    // currently perfunctory place and route algorithms to achieve satisfactory runtimes.
    int count = 0;
    auto &lab_data = labs.at(lab);
    for (int i = 0; i < 10; i++) {
        count += lab_data.alms.at(i).unique_input_count;
    }
    // The 42 is conservative on COUNT but says nothing about WHICH TD wire each net needs, and the
    // router has no LUT input permutation to resolve a clash -- so dense arithmetic can be placed
    // into a LAB that cannot legally route (measured: a 32k-cell design stalls forever at ~40
    // overused TD wires, all on carry chains). MISTRAL_LAB_INPUT_LIMIT makes the threshold sweepable
    // so the trade between density and routability can be measured rather than guessed.
    return (count <= resolved_lab_input_limit());
}

bool Arch::check_mlab_groups(uint32_t lab) const
{
    return mlab_groups_with(lab, [&](BelId b) { return getBoundBelCell(b); });
}

bool Arch::check_mlab_groups_overlay(uint32_t lab, const BelOverlay &overlay) const
{
    return mlab_groups_with(lab, [&](BelId b) { return overlay.lookup(b, getBoundBelCell(b)); });
}

template <typename Bound> bool Arch::mlab_groups_with(uint32_t lab, Bound bound) const
{
    auto &lab_data = labs.at(lab);
    if (!lab_data.is_mlab)
        return true;
    int found_group = -2;
    for (const auto &alm_data : lab_data.alms) {
        std::array<const CellInfo *, 2> luts{bound(alm_data.lut_bels[0]), bound(alm_data.lut_bels[1])};
        for (const CellInfo *lut : luts) {
            if (!lut)
                continue;
            if (found_group == -2)
                found_group = lut->combInfo.mlab_group;
            else if (found_group != lut->combInfo.mlab_group)
                return false;
        }
    }
    if (found_group >= 0) {
        for (const auto &alm_data : lab_data.alms) {
            std::array<const CellInfo *, 4> ffs{bound(alm_data.ff_bels[0]), bound(alm_data.ff_bels[1]),
                                                bound(alm_data.ff_bels[2]), bound(alm_data.ff_bels[3])};
            for (const CellInfo *ff : ffs) {
                if (ff)
                    return false; // be conservative and don't allow LUTRAMs and FFs together
            }
        }
    }
    return true;
}

bool Arch::is_lab_ctrlset_legal_overlay(uint32_t lab, const BelOverlay &overlay) const
{
    return evaluate_lab_controls_native_overlay(*this, lab, overlay).legal;
}

// Equivalent to binding every overlay entry and asking isBelLocationValid for each
// overlay BEL, without binding. Only the legacy live rules are modelled.
bool Arch::overlay_bels_legal(const BelOverlay &overlay) const
{
    return overlay_bels_legal(overlay, overlay.bels, overlay.count);
}

// Equivalent to binding every overlay entry and asking isBelLocationValid for each listed BEL.
bool Arch::overlay_bels_legal(const BelOverlay &overlay, const std::array<BelId, BelOverlay::MAX> &check,
                              unsigned check_count) const
{
    // Distinct LABs touched, with whether any FF BEL among them needs the control-set check.
    std::array<uint32_t, BelOverlay::MAX> labs_seen{};
    std::array<bool, BelOverlay::MAX> need_ctrlset{};
    unsigned lab_count = 0;
    for (unsigned i = 0; i < check_count; ++i) {
        const auto &data = bel_data(check[i]);
        const bool is_ff = data.type == id_MISTRAL_FF;
        if (!is_ff && !data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB))
            return false; // callers gate on placement_candidate_supported; be safe
        if (!is_alm_legal_overlay(data.lab_data.lab, data.lab_data.alm, overlay))
            return false;
        unsigned k = 0;
        while (k < lab_count && labs_seen[k] != data.lab_data.lab)
            ++k;
        if (k == lab_count)
            labs_seen[lab_count++] = data.lab_data.lab;
        need_ctrlset[k] = need_ctrlset[k] || is_ff;
    }
    for (unsigned k = 0; k < lab_count; ++k) {
        const uint32_t lab = labs_seen[k];
        if (!check_lab_input_count_overlay(lab, overlay))
            return false;
        if (need_ctrlset[k] && !is_lab_ctrlset_legal_overlay(lab, overlay))
            return false;
        if (!check_mlab_groups_overlay(lab, overlay))
            return false;
    }
    return true;
}

bool Arch::is_lab_ctrlset_legal(uint32_t lab) const
{
    if (!args.lab_control_profile_path.empty())
        sample_lab_controls(*this, lab, false);
    if (args.lab_controls != LabControlMode::Legacy)
        return dispatch_lab_controls(*this, lab);
    if (args.verify_lab_controls)
        return verify_lab_controls_cpp(*this, lab);
    return evaluate_lab_controls_native(*this, lab).legal;
}

size_t lab_control_legacy_worker_size() { return sizeof(LabControlAllocation); }

NpnrLabControlResultV1 evaluate_lab_controls_legacy(const Arch &arch, uint32_t lab, const LabControlCapture &capture)
{
    const auto evaluation = evaluate_lab_controls_native(arch, lab);
    auto result = lab_control_result(capture.input, evaluation.legal ? NPNR_CONTROL_LEGAL : NPNR_CONTROL_ILLEGAL);
    if (evaluation.allocation) {
        const auto signals = evaluation.allocation->as_array();
        for (unsigned i = 0; i < signals.size(); ++i)
            result.allocation[i] = capture.encode_existing(signals[i]);
    }
    return result;
}

void Arch::lab_pre_route()
{
    log_info("Preparing LABs for routing...\n");
    for (uint32_t lab = 0; lab < labs.size(); lab++) {
        assign_control_sets(lab);
        for (uint8_t alm = 0; alm < 10; alm++) {
            reassign_alm_inputs(lab, alm);
        }
        // Preparation reads the LAB's complete current facts (the route-through
        // rewiring above bumps the LAB's stamps itself, so stamp after it).
        note_lab_prepared(lab);
    }
}

void Arch::assign_control_sets(uint32_t lab)
{
    if (!args.lab_control_profile_path.empty())
        sample_lab_controls(*this, lab, true);
    // Set up reservations for checkPipAvail for control set signals
    // This will be needed because clock and CE are routed together and must be kept together, there isn't free choice
    // e.g. CLK0 & ENA0 must be use for one control set, and CLK1 & ENA1 for another, they can't be mixed and matched
    // Similarly for how inverted & noninverted variants must be kept separate
    auto dispatched = dispatch_lab_controls_for_preparation(*this, lab);
    NPNR_ASSERT(dispatched.selected_status == PreparationStatus::Legal && dispatched.ticket);
    auto translated = translate_preparation_ticket(std::move(*dispatched.ticket));
    NPNR_ASSERT(translated.status == PreparationStatus::Legal && translated.plan);
    auto prepared = prepare_control_edits(*this, *translated.plan);
    NPNR_ASSERT(prepared.status == ControlEditStatus::Ready && prepared.prepared);
    if (translated.comparison_plan) {
        auto comparison = prepare_control_edits(*this, *translated.comparison_plan);
        NPNR_ASSERT(comparison.status == ControlEditStatus::Ready && comparison.prepared);
        const bool same = control_edit_lists_equal(*prepared.prepared, *comparison.prepared);
        if (!same && args.lab_controls != LabControlMode::Shadow)
            log_error("LAB control %s preparation edit mismatch at LAB %u.\n", lab_control_mode_name(args.lab_controls),
                      lab);
        if (!same)
            log_warning("LAB control shadow preparation edit mismatch at LAB %u; applying C++ edits.\n", lab);
    }
    NPNR_ASSERT(apply_prepared_control_edits(*this, std::move(*prepared.prepared)) == ControlEditStatus::Applied);
}

namespace {
// Gets the name of logical LUT pin i for a given cell
static IdString get_lut_pin(CellInfo *cell, int i)
{
    const std::array<IdString, 6> log_pins{id_A, id_B, id_C, id_D, id_E, id_F};
    const std::array<IdString, 5> log_pins_arith{id_A, id_B, id_C, id_D0, id_D1};
    return (cell->type == id_MISTRAL_ALUT_ARITH) ? log_pins_arith.at(i) : log_pins.at(i);
}

static void assign_lut6_inputs(CellInfo *cell, int lut)
{
    std::array<IdString, 6> phys_pins{id_A, id_B, id_C, id_D, (lut == 1) ? id_E1 : id_E0, (lut == 1) ? id_F1 : id_F0};
    int phys_idx = 0;
    for (int i = 0; i < 6; i++) {
        IdString log = get_lut_pin(cell, i);
        if (!cell->ports.count(log) || cell->ports.at(log).net == nullptr)
            continue;
        cell->pin_data[log].bel_pins.clear();
        cell->pin_data[log].bel_pins.push_back(phys_pins.at(phys_idx++));
    }
}

static void assign_mlab_inputs(Context *ctx, CellInfo *cell, int lut)
{
    cell->pin_data[id_CLK1].bel_pins = {id_WCLK};
    cell->pin_data[id_A1EN].bel_pins = {id_WE};
    cell->pin_data[id_A1DATA].bel_pins = {(lut == 1) ? id_E1 : id_E0};
    cell->pin_data[id_B1DATA].bel_pins = {id_COMBOUT};
    cell->pin_data[id_A1EN].bel_pins = {id_WE};

    std::array<IdString, 6> raddr_pins{id_A, id_B, id_C, id_D, id_F0};
    for (int i = 0; i < 5; i++) {
        cell->pin_data[ctx->idf("A1ADDR[%d]", i)].bel_pins = {ctx->idf("WA%d", i)};
        cell->pin_data[ctx->idf("B1ADDR[%d]", i)].bel_pins = {raddr_pins.at(i)};
    }
}

} // namespace

void Arch::reassign_alm_inputs(uint32_t lab, uint8_t alm)
{
    // Based on the usage of LUTs inside the ALM, set up cell-bel pin map for the combinational cells in the ALM
    // so that each physical bel pin is only used for one net; and the logical functions can be implemented correctly.
    // This function should also insert route-through LUTs to legalise flipflop inputs as needed.
    auto &alm_data = labs.at(lab).alms.at(alm);
    alm_data.l6_mode = false;
    alm_data.carry_mode = false;
    std::array<CellInfo *, 2> luts{getBoundBelCell(alm_data.lut_bels[0]), getBoundBelCell(alm_data.lut_bels[1])};
    std::array<CellInfo *, 4> ffs{getBoundBelCell(alm_data.ff_bels[0]), getBoundBelCell(alm_data.ff_bels[1]),
                                  getBoundBelCell(alm_data.ff_bels[2]), getBoundBelCell(alm_data.ff_bels[3])};

    bool found_mlab = false;
    for (int i = 0; i < 2; i++) {
        // Currently we treat LUT6s and MLABs as a special case, as they never share inputs or have fixed mappings
        if (!luts[i])
            continue;
        if (luts[i]->combInfo.is_carry)
            alm_data.carry_mode = true;
        if (luts[i]->type == id_MISTRAL_ALUT6) {
            alm_data.l6_mode = true;
            NPNR_ASSERT(luts[1 - i] == nullptr); // only allow one LUT6 per ALM and no other LUTs
            assign_lut6_inputs(luts[i], i);
        } else if (luts[i]->type == id_MISTRAL_MLAB) {
            found_mlab = true;
            assign_mlab_inputs(getCtx(), luts[i], i);
        }
    }

    if (!alm_data.l6_mode && !found_mlab) {
        // In L5 mode; which is what we use in this case
        //  - A and B are shared
        //  - C, E0, and F0 are exclusive to the top LUT5 secion
        //  - D, E1, and F1 are exclusive to the bottom LUT5 section
        // First find up to two shared inputs
        dict<IdString, int> shared_nets;
        if (luts[0] && luts[1]) {
            for (int i = 0; i < luts[0]->combInfo.lut_input_count; i++) {
                for (int j = 0; j < luts[1]->combInfo.lut_input_count; j++) {
                    if (luts[0]->combInfo.lut_in[i] == nullptr)
                        continue;
                    if (luts[0]->combInfo.lut_in[i] != luts[1]->combInfo.lut_in[j])
                        continue;
                    IdString net = luts[0]->combInfo.lut_in[i]->name;
                    if (shared_nets.count(net))
                        continue;
                    int idx = int(shared_nets.size());
                    shared_nets[net] = idx;
                    if (shared_nets.size() >= 2)
                        goto shared_search_done;
                }
            }
        shared_search_done:;
        }
        // A and B can be used for half-specific nets if not assigned to shared nets
        bool a_avail = shared_nets.size() == 0, b_avail = shared_nets.size() <= 1;
        // Do the actual port assignment
        for (int i = 0; i < 2; i++) {
            if (!luts[i])
                continue;
            // Design 20.2: a plain L5 half leaves the choice of physical pin to the router, except for the nets both
            // halves share: those stay on A and B, the only pins both halves read, which is what makes two
            // five-input halves fit (5 + 5 - 2 = 8 ALM inputs).
            if (args.lut_permutation && !luts[i]->combInfo.is_carry && luts[i]->type != id_MISTRAL_BUF) {
                for (int j = 0; j < luts[i]->combInfo.lut_input_count; j++) {
                    IdString log = get_lut_pin(luts[i], j);
                    auto &bel_pins = luts[i]->pin_data[log].bel_pins;
                    bel_pins.clear();
                    NetInfo *net = luts[i]->getPort(log);
                    if (net == nullptr)
                        continue;
                    if (shared_nets.count(net->name))
                        bel_pins.push_back(shared_nets.at(net->name) ? id_B : id_A);
                    else
                        bel_pins.push_back(lperm_pins.at(j));
                }
                continue;
            }
            // Work out which physical ports are available
            std::vector<IdString> avail_phys_ports;
            // D/C always available and dedicated to the half, in L5 mode
            avail_phys_ports.push_back((i == 1) ? id_D : id_C);
            // In arithmetic mode, Ei can only be used for D0 and Fi can only be used for D1
            // otherwise, these are general and dedicated to one half
            if (!luts[i]->combInfo.is_carry) {
                avail_phys_ports.push_back((i == 1) ? id_E1 : id_E0);
                avail_phys_ports.push_back((i == 1) ? id_F1 : id_F0);
            }
            // A and B might be used for shared signals, or already used by the other half
            if (b_avail)
                avail_phys_ports.push_back(id_B);
            if (a_avail)
                avail_phys_ports.push_back(id_A);
            int phys_idx = 0;

            for (int j = 0; j < luts[i]->combInfo.lut_input_count; j++) {
                IdString log = get_lut_pin(luts[i], j);
                auto &bel_pins = luts[i]->pin_data[log].bel_pins;
                bel_pins.clear();

                NetInfo *net = luts[i]->getPort(log);
                if (net == nullptr) {
                    // Disconnected inputs don't need to be allocated a pin, because the router won't be routing these
                    continue;
                } else if (shared_nets.count(net->name)) {
                    // This pin is to be allocated one of the shared nets
                    bel_pins.push_back(shared_nets.at(net->name) ? id_B : id_A);
                } else if (log == id_D0) {
                    // Arithmetic
                    bel_pins.push_back((i == 1) ? id_E1 : id_E0); // reserved
                } else if (log == id_D1) {
                    bel_pins.push_back((i == 1) ? id_F1 : id_F0); // reserved
                } else {
                    // Allocate from the general pool of available physical pins
                    IdString phys = avail_phys_ports.at(phys_idx++);
                    bel_pins.push_back(phys);
                    // Mark A/B unavailable for the other LUT, if needed
                    if (phys == id_A)
                        a_avail = false;
                    else if (phys == id_B)
                        b_avail = false;
                }
            }
        }
    }

    // FF route-through insertion
    for (int i = 0; i < 2; i++) {
        // FF route-through will never be inserted if LUT is used
        if (luts[i])
            continue;
        for (int j = 0; j < 2; j++) {
            CellInfo *ff = ffs[i * 2 + j];
            if (!ff || !ff->ffInfo.datain || alm_data.l6_mode || alm_data.carry_mode)
                continue;
            CellInfo *rt_lut = createCell(idf("%s$ROUTETHRU", nameOf(ff)), id_MISTRAL_BUF);
            rt_lut->addInput(id_A);
            rt_lut->addOutput(id_Q);
            // Disconnect the original data input to the FF, and connect it to the route-thru LUT instead
            NetInfo *datain = ff->getPort(id_DATAIN);
            ff->disconnectPort(id_DATAIN);
            rt_lut->connectPort(id_A, datain);
            rt_lut->connectPorts(id_Q, ff, id_DATAIN);
            // Assign route-thru LUT physical ports, input goes to the first half-specific input
            rt_lut->pin_data[id_A].bel_pins.push_back(i ? id_D : id_C);
            rt_lut->pin_data[id_Q].bel_pins.push_back(id_COMBOUT);
            assign_comb_info(rt_lut);
            // Place the route-thru LUT at the relevant combinational bel
            bindBel(alm_data.lut_bels[i], rt_lut, STRENGTH_STRONG);
            break;
        }
    }

    // TODO: in the future, as well as the reassignment here we will also have pseudo PIPs in front of the ALM so that
    // the router can permute LUTs for routeability; too. Here we will need to lock out some of those PIPs depending on
    // the usage of the ALM, as not all inputs are always interchangeable.
    // Get cells into an array for fast access
}

// This default cell-bel pin mapping is used to provide estimates during placement only. It will have errors and
// overlaps and a correct mapping will be resolved twixt placement and routing
const dict<IdString, IdString> Arch::comb_pinmap = {
        {id_A, id_F0}, // fastest input first
        {id_B, id_E0}, {id_C, id_D}, {id_D, id_C},       {id_D0, id_C},       {id_D1, id_B},
        {id_E, id_B},  {id_F, id_A}, {id_Q, id_COMBOUT}, {id_SO, id_COMBOUT},
};

namespace {
// gets the value of the ith LUT init property of a given cell
uint64_t get_lut_init(const CellInfo *cell, int i)
{
    if (cell->type == id_MISTRAL_NOT) {
        return 1;
    } else if (cell->type == id_MISTRAL_BUF) {
        return 2;
    } else {
        IdString prop;
        if (cell->type == id_MISTRAL_ALUT_ARITH)
            prop = (i == 1) ? id_LUT1 : id_LUT0;
        else
            prop = id_LUT;
        auto fnd = cell->params.find(prop);
        if (fnd == cell->params.end())
            return 0;
        else
            return fnd->second.as_int64();
    }
}
// gets the state of a physical pin when evaluating the a given bit of LUT init for
bool get_phys_pin_val(bool l6_mode, bool arith_mode, int bit, IdString pin)
{
    switch (pin.index) {
    case ID_A:
        return (bit >> 0) & 0x1;
    case ID_B:
        return (bit >> 1) & 0x1;
    case ID_C:
        return (l6_mode && bit >= 32) ? ((bit >> 3) & 0x1) : ((bit >> 2) & 0x1);
    case ID_D:
        return (l6_mode && bit < 32) ? ((bit >> 3) & 0x1) : ((bit >> 2) & 0x1);
    case ID_E0:
    case ID_E1:
        return l6_mode ? ((bit >> 5) & 0x1) : ((bit >> 3) & 0x1);
    case ID_F0:
    case ID_F1:
        return arith_mode ? ((bit >> 3) & 0x1) : ((bit >> 4) & 0x1);
    default:
        NPNR_ASSERT_FALSE("unknown physical pin!");
    }
}

static const std::array<int, 64> mlab_permute = {0,  1,  4,  5,  8,  9,  12, 13, 29, 28, 25, 24, 21, 20, 17, 16,
                                                 2,  3,  6,  7,  10, 11, 14, 15, 31, 30, 27, 26, 23, 22, 19, 18,
                                                 32, 33, 36, 37, 40, 41, 44, 45, 61, 60, 57, 56, 53, 52, 49, 48,
                                                 34, 35, 38, 39, 42, 43, 46, 47, 63, 62, 59, 58, 55, 54, 51, 50};

// MLABs have permuted init values in hardware, we need to correct for this
uint64_t permute_mlab_init(uint64_t orig)
{
    uint64_t result = 0;
    for (int i = 0; i < 64; i++) {
        if ((orig >> uint64_t(i)) & 0x1) {
            result |= (uint64_t(1) << uint64_t(mlab_permute.at(i)));
        }
    }
    return result;
}

} // namespace

// Design 20.2: after routing, each permuted logical input's LPERM wire is driven by a pseudo-pip from one of the
// half's physical pins. Record that pin as the logical pin's bel pin (compute_lut_mask then builds the mask for it),
// check the assignment, and unbind the pseudo-wire so the net ends on the physical pin.
void Arch::lut_permutation_fixup()
{
    int moved = 0, luts_permuted = 0;
    for (uint32_t lab = 0; lab < labs.size(); lab++) {
        auto &lab_data = labs[lab];
        auto block_type = lab_data.is_mlab ? CycloneV::MLAB : CycloneV::LAB;
        for (uint8_t z = 0; z < 10; z++) {
            auto &alm_data = lab_data.alms[z];
            Loc loc = getBelLocation(alm_data.lut_bels[0]);
            for (int i = 0; i < 2; i++) {
                CellInfo *lut = getBoundBelCell(alm_data.lut_bels[i]);
                if (!lut)
                    continue;
                const std::array<std::pair<IdString, CycloneV::port_type_t>, 5> phys{
                        std::make_pair(id_A, CycloneV::A), std::make_pair(id_B, CycloneV::B),
                        i ? std::make_pair(id_D, CycloneV::D) : std::make_pair(id_C, CycloneV::C),
                        i ? std::make_pair(id_E1, CycloneV::E1) : std::make_pair(id_E0, CycloneV::E0),
                        i ? std::make_pair(id_F1, CycloneV::F1) : std::make_pair(id_F0, CycloneV::F0)};
                std::vector<IdString> used;
                bool permuted = false;
                for (int j = 0; j < lut->combInfo.lut_input_count; j++) {
                    IdString log_pin = get_lut_pin(lut, j);
                    auto found = lut->pin_data.find(log_pin);
                    if (found == lut->pin_data.end() || found->second.bel_pins.size() != 1)
                        continue;
                    int k = -1;
                    for (int q = 0; q < 5; q++)
                        if (found->second.bel_pins[0] == lperm_pins[q])
                            k = q;
                    if (k < 0)
                        continue;
                    NetInfo *net = lut->getPort(log_pin);
                    WireId w = alm_data.perm_wires[i][k];
                    auto bound = net ? net->wires.find(w) : net->wires.end();
                    if (!net || bound == net->wires.end() || bound->second.pip == PipId())
                        log_error("LUT permutation: %s.%s is not routed to its logical input wire\n", nameOf(lut),
                                  nameOf(log_pin));
                    WireId src = getPipSrcWire(bound->second.pip);
                    IdString pin;
                    for (auto &p : phys)
                        if (get_port(block_type, loc.x, loc.y, z, p.second) == src)
                            pin = p.first;
                    if (pin == IdString() || getBoundWireNet(src) != net ||
                        std::find(used.begin(), used.end(), pin) != used.end())
                        log_error("LUT permutation: %s.%s arrives on an invalid or shared physical pin\n", nameOf(lut),
                                  nameOf(log_pin));
                    used.push_back(pin);
                    unbindWire(w);
                    found->second.bel_pins[0] = pin;
                    permuted = true;
                    ++moved;
                }
                luts_permuted += permuted;
            }
        }
    }
    log_info("LUT permutation: %d LUT inputs placed by the router on %d LUTs.\n", moved, luts_permuted);
}

uint64_t Arch::compute_lut_mask(uint32_t lab, uint8_t alm)
{
    uint64_t mask = 0;
    auto &alm_data = labs.at(lab).alms.at(alm);
    std::array<CellInfo *, 2> luts{getBoundBelCell(alm_data.lut_bels[0]), getBoundBelCell(alm_data.lut_bels[1])};

    for (int i = 0; i < 2; i++) {
        CellInfo *lut = luts[i];
        if (!lut)
            continue;
        int offset = ((i == 1) && !alm_data.l6_mode) ? 32 : 0;
        bool arith = lut->combInfo.is_carry;
        for (int j = 0; j < (alm_data.l6_mode ? 64 : 32); j++) {
            // Evaluate LUT function at this point
            uint64_t init = get_lut_init(lut, (arith && j >= 16) ? 1 : 0);

            int index = 0;
            for (int k = 0; k < lut->combInfo.lut_input_count; k++) {
                IdString log_pin = get_lut_pin(lut, k);
                int init_idx = k;
                if (arith) {
                    // D0 only affects lower half; D1 upper half
                    if (k == 3 && j >= 16)
                        continue;
                    if (k == 4) {
                        if (j < 16)
                            continue;
                        else
                            init_idx = 3;
                    }
                }
                CellPinState state = lut->get_pin_state(log_pin);
                if (state == PIN_0) {
                    continue;
                } else if (state == PIN_1) {
                    index |= (1 << init_idx);
                    continue;
                }
                // Ignore if no associated physical pin
                if (lut->getPort(log_pin) == nullptr || lut->pin_data.at(log_pin).bel_pins.empty())
                    continue;
                // ALM inputs appear to be inverted by default (TODO: check!)
                // so only invert if an inverter has _not_ been folded into the pin
                bool inverted = (state != PIN_INV);
                // Depermute physical pin
                IdString phys_pin = lut->pin_data.at(log_pin).bel_pins.at(0);
                if (get_phys_pin_val(alm_data.l6_mode, arith, j, phys_pin) != inverted)
                    index |= (1 << init_idx);
            }
            if ((init >> index) & 0x1) {
                mask |= (1ULL << uint64_t(j + offset));
            }
        }
    }

    // TODO: always inverted, or just certain paths?
    mask = ~mask;

    if (labs.at(lab).is_mlab)
        mask = permute_mlab_init(mask);

#if 1
    if (getCtx()->debug) {
        auto pos = alm_data.lut_bels[0].pos;
        log("ALM %03d.%03d.%d\n", CycloneV::pos2x(pos), CycloneV::pos2y(pos), alm);
        for (int i = 0; i < 2; i++) {
            log("    LUT%d: ", i);
            if (luts[i]) {
                log("%s:%s", nameOf(luts[i]), nameOf(luts[i]->type));
                for (auto &pin : luts[i]->pin_data) {
                    if (!luts[i]->ports.count(pin.first) || luts[i]->ports.at(pin.first).type != PORT_IN)
                        continue;
                    log(" %s:", nameOf(pin.first));
                    if (pin.second.state == PIN_0)
                        log("0");
                    else if (pin.second.state == PIN_1)
                        log("1");
                    else if (pin.second.state == PIN_INV)
                        log("~");
                    for (auto bp : pin.second.bel_pins)
                        log("%s", nameOf(bp));
                }
            } else {
                log("<null>");
            }
            log("\n");
        }
        log("INIT: %016lx\n", mask);
        log("\n");
    }
#endif

    return mask;
}

NEXTPNR_NAMESPACE_END
