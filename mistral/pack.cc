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
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
struct MistralPacker
{
    MistralPacker(Context *ctx) : ctx(ctx) {};
    Context *ctx;

    NetInfo *gnd_net, *vcc_net;

    void init_constant_nets()
    {
        CellInfo *gnd_drv = ctx->createCell(ctx->id("$PACKER_GND_DRV"), id_MISTRAL_CONST);
        gnd_drv->params[id_LUT] = 0;
        gnd_drv->addOutput(id_Q);
        CellInfo *vcc_drv = ctx->createCell(ctx->id("$PACKER_VCC_DRV"), id_MISTRAL_CONST);
        vcc_drv->params[id_LUT] = 1;
        vcc_drv->addOutput(id_Q);
        gnd_net = ctx->createNet(ctx->id("$PACKER_GND_NET"));
        vcc_net = ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        gnd_drv->connectPort(id_Q, gnd_net);
        vcc_drv->connectPort(id_Q, vcc_net);
    }

    CellPinState get_pin_needed_muxval(CellInfo *cell, IdString port)
    {
        NetInfo *net = cell->getPort(port);
        if (net == nullptr || net->driver.cell == nullptr) {
            // Pin is disconnected
            // If a mux value exists already, honour it
            CellPinState exist_mux = cell->get_pin_state(port);
            if (exist_mux != PIN_SIG)
                return exist_mux;
            // Otherwise, look up the default value and use that
            CellPinStyle pin_style = ctx->get_cell_pin_style(cell, port);
            if ((pin_style & PINDEF_MASK) == PINDEF_0)
                return PIN_0;
            else if ((pin_style & PINDEF_MASK) == PINDEF_1)
                return PIN_1;
            else
                return PIN_SIG;
        }
        // Look to see if the driver is an inverter or constant
        IdString drv_type = net->driver.cell->type;
        if (drv_type == id_MISTRAL_NOT)
            return PIN_INV;
        else if (drv_type == id_GND)
            return PIN_0;
        else if (drv_type == id_VCC)
            return PIN_1;
        else
            return PIN_SIG;
    }

    void uninvert_port(CellInfo *cell, IdString port)
    {
        // Rewire a port so it is driven by the input to an inverter
        NetInfo *net = cell->getPort(port);
        NPNR_ASSERT(net != nullptr && net->driver.cell != nullptr && net->driver.cell->type == id_MISTRAL_NOT);
        CellInfo *inv = net->driver.cell;
        cell->disconnectPort(port);

        NetInfo *inv_a = inv->getPort(id_A);
        if (inv_a != nullptr) {
            cell->connectPort(port, inv_a);
        }
    }

    void process_inv_constants(CellInfo *cell)
    {
        // TODO: we might need to create missing inputs here in some cases so we can tie them to the correct constant?
        // Fold inverters and constants into a cell
        for (auto &port : cell->ports) {
            // Iterate over all inputs
            if (port.second.type != PORT_IN)
                continue;
            IdString port_name = port.first;

            CellPinState req_mux = get_pin_needed_muxval(cell, port_name);
            if (req_mux == PIN_SIG) {
                // No special setting required, ignore
                continue;
            }

            CellPinStyle pin_style = ctx->get_cell_pin_style(cell, port_name);

            if (req_mux == PIN_INV) {
                // Pin is inverted. If there is a hard inverter; then use it
                if (pin_style & PINOPT_INV) {
                    uninvert_port(cell, port_name);
                    cell->pin_data[port_name].state = PIN_INV;
                }
            } else if (req_mux == PIN_0 || req_mux == PIN_1) {
                // Pin is tied to a constant
                // If there is a hard constant option; use it
                if ((pin_style & int(req_mux)) == req_mux) {
                    cell->disconnectPort(port_name);
                    cell->pin_data[port_name].state = req_mux;
                } else {
                    cell->disconnectPort(port_name);
                    // There is no hard constant, we need to connect it to the relevant soft-constant net
                    cell->connectPort(port_name, (req_mux == PIN_1) ? vcc_net : gnd_net);
                }
            }
        }
    }

    void trim_design()
    {
        // Remove unused inverters and high/low drivers
        std::vector<IdString> trim_cells;
        std::vector<IdString> trim_nets;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_NOT && ci->type != id_GND && ci->type != id_VCC)
                continue;
            IdString port = (ci->type == id_MISTRAL_NOT) ? id_Q : id_Y;
            NetInfo *out = ci->getPort(port);
            if (out == nullptr) {
                trim_cells.push_back(ci->name);
                continue;
            }
            if (!out->users.empty())
                continue;

            ci->disconnectPort(id_A);

            trim_cells.push_back(ci->name);
            trim_nets.push_back(out->name);
        }

        for (IdString rem_net : trim_nets)
            ctx->nets.erase(rem_net);
        for (IdString rem_cell : trim_cells)
            ctx->cells.erase(rem_cell);
    }

    void pack_constants()
    {
        // Iterate through cells
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            // Skip certain cells at this point
            if (ci->type != id_MISTRAL_NOT && ci->type != id_GND && ci->type != id_VCC)
                process_inv_constants(ci);
        }
        // Special case - SDATA can only be trimmed if SLOAD is low
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_FF)
                continue;
            if (ci->get_pin_state(id_SLOAD) != PIN_0)
                continue;
            ci->disconnectPort(id_SDATA);
        }
        // Remove superfluous inverters and constant drivers
        trim_design();
    }

    void prepare_io()
    {
        // Find the actual IO buffer corresponding to a port; and copy attributes across to it
        // Note that this relies on Yosys to do IO buffer inference, to avoid tristate issues once we get to synthesised
        // JSON. In all cases the nextpnr-inserted IO buffers are removed as redundant.
        for (auto &port : ctx->ports) {
            if (!ctx->cells.count(port.first))
                log_error("Port '%s' doesn't seem to have a corresponding top level IO\n", ctx->nameOf(port.first));
            CellInfo *ci = ctx->cells.at(port.first).get();

            PortRef top_port;
            top_port.cell = nullptr;
            bool is_npnr_iob = false;

            if (ci->type == ctx->id("$nextpnr_ibuf") || ci->type == ctx->id("$nextpnr_iobuf")) {
                // Might have an input buffer (IB etc) connected to it
                is_npnr_iob = true;
                NetInfo *o = ci->getPort(id_O);
                if (o == nullptr)
                    ;
                else if (o->users.entries() > 1)
                    log_error("Top level pin '%s' has multiple input buffers\n", ctx->nameOf(port.first));
                else if (o->users.entries() == 1)
                    top_port = *o->users.begin();
            }
            if (ci->type == ctx->id("$nextpnr_obuf") || ci->type == ctx->id("$nextpnr_iobuf")) {
                // Might have an output buffer (OB etc) connected to it
                is_npnr_iob = true;
                NetInfo *i = ci->getPort(id_I);
                if (i != nullptr && i->driver.cell != nullptr) {
                    if (top_port.cell != nullptr)
                        log_error("Top level pin '%s' has multiple input/output buffers\n", ctx->nameOf(port.first));
                    top_port = i->driver;
                }
                // Edge case of a bidirectional buffer driving an output pin
                if (i->users.entries() > 2) {
                    log_error("Top level pin '%s' has illegal buffer configuration\n", ctx->nameOf(port.first));
                } else if (i->users.entries() == 2) {
                    if (top_port.cell != nullptr)
                        log_error("Top level pin '%s' has illegal buffer configuration\n", ctx->nameOf(port.first));
                    for (auto &usr : i->users) {
                        if (usr.cell->type == ctx->id("$nextpnr_obuf") || usr.cell->type == ctx->id("$nextpnr_iobuf"))
                            continue;
                        top_port = usr;
                        break;
                    }
                }
            }
            if (!is_npnr_iob)
                log_error("Port '%s' doesn't seem to have a corresponding top level IO (internal cell type mismatch)\n",
                          ctx->nameOf(port.first));

            if (top_port.cell == nullptr) {
                log_info("Trimming port '%s' as it is unused.\n", ctx->nameOf(port.first));
            } else {
                // Copy attributes to real IO buffer
                if (ctx->io_attr.count(port.first)) {
                    for (auto &kv : ctx->io_attr.at(port.first)) {
                        top_port.cell->attrs[kv.first] = kv.second;
                    }
                }
                // Make sure that top level net is set correctly
                port.second.net = top_port.cell->ports.at(top_port.port).net;
            }
            // Now remove the nextpnr-inserted buffer
            ci->disconnectPort(id_I);
            ci->disconnectPort(id_O);
            ctx->cells.erase(port.first);
        }
    }

    void pack_io()
    {
        // Step 0: deal with top level inserted IO buffers
        prepare_io();
        // Stage 1: apply constraints
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            // Iterate through all IO buffer primitives
            if (!ctx->is_io_cell(ci->type))
                continue;
            // We need all IO constrained at the moment, unconstrained IO are rare enough not to care
            if (!ci->attrs.count(id_LOC))
                log_error("Found unconstrained IO '%s', these are currently unsupported\n", ctx->nameOf(ci));
            // Convert package pin constraint to bel constraint
            std::string loc = ci->attrs.at(id_LOC).as_string();
            if (loc.compare(0, 4, "PIN_") != 0)
                log_error("Expecting PIN_-prefixed pin for IO '%s', got '%s'\n", ctx->nameOf(ci), loc.c_str());
            auto pin_info = ctx->cyclonev->pin_find_name(loc.substr(4));
            if (pin_info == nullptr)
                log_error("IO '%s' is constrained to invalid pin '%s'\n", ctx->nameOf(ci), loc.c_str());
            BelId bel = ctx->get_io_pin_bel(pin_info);

            if (bel == BelId()) {
                log_error("IO '%s' is constrained to pin %s which is not a supported IO pin.\n", ctx->nameOf(ci),
                          loc.c_str());
            } else {
                log_info("Constraining IO '%s' to pin %s (bel %s)\n", ctx->nameOf(ci), loc.c_str(),
                         ctx->nameOfBel(bel));
                ctx->bindBel(bel, ci, STRENGTH_LOCKED);
            }
        }
    }

    void constrain_carries()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_ALUT_ARITH)
                continue;
            const NetInfo *cin = ci->getPort(id_CI);
            if (cin != nullptr && cin->driver.cell != nullptr)
                continue; // not the start of a chain
            std::vector<CellInfo *> chain;
            CellInfo *cursor = ci;
            while (true) {
                chain.push_back(cursor);
                const NetInfo *co = cursor->getPort(id_CO);
                if (co == nullptr || co->users.empty())
                    break;
                if (co->users.entries() > 1)
                    log_error("Carry net %s has more than one sink!\n", ctx->nameOf(co));
                auto &usr = *co->users.begin();
                if (usr.port != id_CI)
                    log_error("Carry net %s drives port %s, expected CI\n", ctx->nameOf(co), ctx->nameOf(usr.port));
                cursor = usr.cell;
            }

            chain.at(0)->constr_abs_z = true;
            chain.at(0)->constr_z = 0;
            chain.at(0)->cluster = chain.at(0)->name;

            for (int i = 1; i < int(chain.size()); i++) {
                chain.at(i)->constr_x = 0;
                chain.at(i)->constr_y = -(i / 20);
                // 2 COMB, 4 FF per ALM
                chain.at(i)->constr_z = ((i / 2) % 10) * 6 + (i % 2);
                chain.at(i)->constr_abs_z = true;
                chain.at(i)->cluster = chain.at(0)->name;
                chain.at(0)->constr_children.push_back(chain.at(i));
            }

            if (ctx->debug) {
                log_info("Chain: \n");
                for (int i = 0; i < int(chain.size()); i++) {
                    auto &c = chain.at(i);
                    log_info("    i=%d cell=%s dy=%d z=%d ci=%s co=%s\n", i, ctx->nameOf(c), c->constr_y, c->constr_z,
                             ctx->nameOf(c->getPort(id_CI)), ctx->nameOf(c->getPort(id_CO)));
                }
            }
        }
        // Check we reached all the cells in the above pass
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_ALUT_ARITH)
                continue;
            if (ci->cluster == ClusterId())
                log_error("Failed to include arith cell '%s' in any chain (CI=%s)\n", ctx->nameOf(ci),
                          ctx->nameOf(ci->getPort(id_CI)));
        }
    }

    void constrain_lutram()
    {
        // We form clusters based on both read and write address; as both being the same makes it more likely these
        // cells should be packed together, too.
        // This makes things easier for the placement legaliser to deal with RAM in LAB-compatible blocks without
        // over-constraining things
        idict<dict<IdString, IdString>> mlab_keys;
        std::vector<std::vector<CellInfo *>> mlab_groups;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_MLAB)
                continue;
            auto key = ctx->get_mlab_key(ci, true);
            int key_idx = mlab_keys(key);
            if (key_idx >= int(mlab_groups.size()))
                mlab_groups.resize(key_idx + 1);
            mlab_groups.at(key_idx).push_back(ci);
        }
        // Combine into clusters
        size_t cluster_size = 20;
        for (auto &group : mlab_groups) {
            for (size_t i = 0; i < group.size(); i++) {
                CellInfo *ci = group.at(i);
                CellInfo *base = group.at((i / cluster_size) * cluster_size);
                int cell_index = int(i) % cluster_size;
                int alm = cell_index / 2;
                int alm_cell = cell_index % 2;
                ci->cluster = base->name;
                ci->constr_abs_z = true;
                ci->constr_z = alm * 6 + alm_cell;
                if (cell_index != 0) {
                    // Not the root of a cluster
                    base->constr_children.push_back(ci);
                    ci->constr_x = 0;
                    ci->constr_y = 0;
                }
            }
        }
    }

    // G7: map MISTRAL_MUL18X18's logical ports onto the DSP bel pins created in dsp.cc.
    // getBelPinsForCellPin() is pin_data.at(pin).bel_pins -- an explicit map with no name-based
    // fallback (the tristate work established that the hard way), so every bit of A/B/Y needs an
    // entry even though the names match.
    void setup_dsps()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_MUL18X18)
                continue;
            for (auto &port : ci->ports)
                ci->pin_data[port.first];
            for (int i = 0; i < 18; i++) {
                ci->pin_data[ctx->idf("A[%d]", i)].bel_pins = {ctx->idf("A[%d]", i)};
                ci->pin_data[ctx->idf("B[%d]", i)].bel_pins = {ctx->idf("B[%d]", i)};
            }
            for (int i = 0; i < 36; i++)
                ci->pin_data[ctx->idf("Y[%d]", i)].bel_pins = {ctx->idf("Y[%d]", i)};
            log_info("  DSP: set up MISTRAL_MUL18X18 '%s'\n", ctx->nameOf(ci));
        }
    }

    void setup_m10ks()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            // MISTRAL_M10K_DC: hand-instantiated dual-clock simple-dual-port variant (yosys cannot
            // infer it -- its M10K cell is single-clock). Same interface plus CLK2 = the read-port
            // clock. Ground truth (qm10k_sc/dc differential): dual-clock is the SAME static config
            // in Quartus's shape -- port B hard-selected onto clock rail 1 -- and the second clock
            // is ROUTED to CLKIN[1]; single- vs dual-clock differ only in which net feeds that rail.
            if (ci->type == id_MISTRAL_M10K_DC) {
                // Defensive: any pinmap assigned before this point (e.g. a load-time
                // assign_default_pinmap) would sit AHEAD of the real pins below.
                for (auto &pd : ci->pin_data)
                    pd.second.bel_pins.clear();
                ci->pin_data[id_CLK2].bel_pins = {ctx->id("CLKIN[1]")};
                ci->params[id_M10K_DC] = 1;
                ci->type = id_MISTRAL_M10K;
            }
            if (ci->type != id_MISTRAL_M10K)
                continue;

            auto abits = ci->params.at(id_CFG_ABITS).as_int64();
            auto dbits = ci->params.at(id_CFG_DBITS).as_int64();
            NPNR_ASSERT(abits >= 7 && abits <= 13);
            NPNR_ASSERT(dbits == 1 || dbits == 2 || dbits == 5 || dbits == 10 || dbits == 20 || dbits == 40);
            NPNR_ASSERT((1 << abits) * dbits <= 10240);

            log_info("Setting up %ld-bit address, %ld-bit data M10K for %s.\n", abits, dbits,
                     ci->name.str(ctx).c_str());
            if (getenv("VUP_DEBUG_M10K"))
                for (auto &pd : ci->pin_data)
                    for (auto bp : pd.second.bel_pins)
                        log_info("  [m10k-pre] %s -> %s\n", pd.first.c_str(ctx), bp.c_str(ctx));

            // Quartus doesn't seem to generate ADDRSTALL[AB], BYTEENABLE[AB][01].

            // It *does* generate ACLR[01] but leaves them unconnected if unused.

            // Enables.
            // RDEN[1] is left unconnected.
            if (dbits == 40)
                ci->pin_data[ctx->id("A1EN")].bel_pins = {ctx->id("WREN[0]")};
            else
                ci->pin_data[ctx->id("A1EN")].bel_pins = {ctx->id("WREN[1]")};
            ci->pin_data[ctx->id("B1EN")].bel_pins = {ctx->id("RDEN[0]")};

            // Clocks.
            ci->pin_data[ctx->id("CLK1")].bel_pins = {ctx->id("CLKIN[0]")};

            // Enables left unconnected.

            // Address lines.

            // One could remove the std::max here and the `- bit_offset`s here,
            // because they would cancel out, but I think this way is less confusing.
            int addr_offset = std::max<int64_t>(12 - std::max<int64_t>(abits, dbits == 40 ? 8 : 9), 0);
            int bit_offset = (abits == 13);
            if (abits == 13) {
                ci->pin_data[ctx->id("A1ADDR[0]")].bel_pins = {ctx->id("DATAAIN[4]")};
                ci->pin_data[ctx->id("B1ADDR[0]")].bel_pins = {ctx->id("DATABIN[19]")};
            }
            for (int bit = bit_offset; bit < abits; bit++) {
                ci->pin_data[ctx->idf("A1ADDR[%d]", bit)].bel_pins = {
                        ctx->idf("ADDRA[%d]", bit + addr_offset - bit_offset)};
                ci->pin_data[ctx->idf("B1ADDR[%d]", bit)].bel_pins = {
                        ctx->idf("ADDRB[%d]", bit + addr_offset - bit_offset)};
            }

            // Data lines
            std::vector<int> offsets;
            offsets.push_back(0);
            if (abits >= 10 && dbits <= 10) {
                offsets.push_back(10);
            }
            if (abits >= 11 && dbits <= 5) {
                offsets.push_back(5);
                offsets.push_back(15);
            }
            if (abits >= 12 && dbits <= 2) {
                offsets.push_back(2);
                offsets.push_back(7);
                offsets.push_back(12);
                offsets.push_back(17);
            }
            if (abits == 13 && dbits == 1) {
                offsets.push_back(1);
                offsets.push_back(3);
                offsets.push_back(6);
                offsets.push_back(8);
                offsets.push_back(11);
                offsets.push_back(13);
                offsets.push_back(16);
                offsets.push_back(18);
            }

            // In this corner case the pin name does not have indexing
            // because it's a single bit wide...
            if (abits == 13 && dbits == 1) {
                for (int offset : offsets)
                    ci->pin_data[ctx->idf("A1DATA")].bel_pins.push_back(ctx->idf("DATAAIN[%d]", offset));
                ci->pin_data[ctx->idf("B1DATA")].bel_pins = {ctx->idf("DATABOUT[0]")};
                continue;
            }

            // 40-bit data mode causes some headaches...
            bit_offset = dbits == 40 ? 20 : 0;

            // Write port
            for (int bit = 0; bit < std::min<int64_t>(dbits, 20); bit++)
                for (int offset : offsets)
                    ci->pin_data[ctx->idf("A1DATA[%d]", bit)].bel_pins.push_back(ctx->idf("DATAAIN[%d]", bit + offset));

            if (dbits == 40)
                for (int bit = bit_offset; bit < dbits; bit++)
                    ci->pin_data[ctx->idf("A1DATA[%d]", bit)].bel_pins.push_back(
                            ctx->idf("DATABIN[%d]", bit - bit_offset));

            // Read port
            if (dbits == 40)
                for (int bit = 0; bit < 20; bit++)
                    ci->pin_data[ctx->idf("B1DATA[%d]", bit)].bel_pins = {ctx->idf("DATAAOUT[%d]", bit)};

            for (int bit = bit_offset; bit < dbits; bit++)
                ci->pin_data[ctx->idf("B1DATA[%d]", bit)].bel_pins = {ctx->idf("DATABOUT[%d]", bit - bit_offset)};
        }
    }

    void setup_fplls()
    {
        // Allocation state shared across every PLL in the design. Without this each PLL picks the
        // same FPLL position and the second bindBel trips `data.bound == nullptr` (arch.h) -- an
        // abort, not a placement error. Real cores routinely want two PLLs (pixel + memory clock).
        std::set<uint32_t> taken_fpll;
        std::set<std::pair<uint32_t, int>> taken_gclk;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_altera_pll)
                continue;

            // getBelPinsForCellPin() does pin_data.at(pin); make sure *every*
            // port has an entry so it can never throw. Ports we don't map keep
            // an empty bel_pins list (they simply have no physical sink/driver).
            for (auto &port : ci->ports)
                ci->pin_data[port.first];

            // Map the logical PLL ports onto the physical FPLL bel pins that the
            // Mistral routing model actually exposes (see create_fpll()).
            if (ci->ports.count(id_refclk))
                ci->pin_data[id_refclk].bel_pins = {id_REFCLK};
            if (ci->ports.count(id_locked))
                ci->pin_data[id_locked].bel_pins = {id_LOCKED};
            if (ci->ports.count(id_fbclk))
                ci->pin_data[id_fbclk].bel_pins = {id_FBCLK};

            // PLL output clocks (G4, PLL_OUTCLK_DESIGN.md). The C-counter outputs reach the clock
            // tree through dedicated wiring into the global clock control blocks, selected by the
            // cmux INPUT_SEL bitstream mux — configuration, not routing, so there is no FPLL bel
            // pin to map an outclk onto. Instead each used outclk net gets a MISTRAL_PLLCLK cell
            // spliced in as its driver: its Q binds a global cmux CLKOUT (GCLK root) wire,
            // placement validity restricts it to gclk instances the dedicated wiring can actually
            // feed (pllclk_sel_map), and bitstream emission programs the INPUT_SEL accordingly.
            int nclk = ci->params.count(id_number_of_clocks) ? int(ci->params.at(id_number_of_clocks).as_int64()) : 1;
            std::vector<std::pair<int, CellInfo *>> injectors; // (logical clock index, pllclk cell)
            for (int i = 0; i < nclk; i++) {
                IdString cellport =
                        (nclk == 1 && ci->ports.count(id_outclk)) ? id_outclk : ctx->idf("outclk[%d]", i);
                if (!ci->ports.count(cellport))
                    continue;
                ci->pin_data[cellport].bel_pins.clear();
                NetInfo *on = ci->getPort(cellport);
                if (on == nullptr || on->users.empty())
                    continue;
                CellInfo *pc =
                        ctx->createCell(ctx->idf("%s$pllclk[%d]", ci->name.c_str(ctx), i), id_MISTRAL_PLLCLK);
                pc->addOutput(id_Q);
                // The outclk net's driver becomes the PLLCLK cell; the PLL port is left
                // disconnected (the physical connection is the dedicated wiring the map encodes).
                ci->movePortTo(cellport, pc, id_Q);
                pc->attrs[id_PLLCLK_PLL] = ci->name.str(ctx);
                injectors.emplace_back(i, pc);
            }
            if (!injectors.empty()) {
                // Deterministic co-assignment (G4): choose the FPLL position and, per output clock,
                // the physical counter + global-cmux gclk instance the dedicated wiring supports —
                // only C4..C8 reach the global cmuxes, so logical clock i is remapped to a physical
                // counter and both cells are pre-bound (LOCKED). Refusal is loud, never a guess.
                uint32_t fpll_pos = 0;
                std::vector<Arch::PllClkChoice> picks;
                if (!ctx->pllclk_choose(int(injectors.size()), fpll_pos, picks, taken_fpll, taken_gclk))
                    log_error("altera_pll '%s': no FPLL position has dedicated global-cmux wiring for "
                              "%d output clock(s)%s\n",
                              ctx->nameOf(ci), int(injectors.size()),
                              taken_fpll.empty() ? "" : " (the free ones are already used by another PLL)");
                taken_fpll.insert(fpll_pos);
                // The first PLL is the one that may be relocated to the attested position (0,14) by
                // the post-place pass in pll.cc, so no later PLL may claim that bel either -- nor
                // (0,0), which collides with the "unplaced" sentinel and is only survivable because
                // the relocation moves the first PLL off it.
                if (taken_fpll.size() == 1) {
                    taken_fpll.insert(uint32_t(CycloneV::xy2pos(0, 14)));
                    taken_fpll.insert(uint32_t(CycloneV::xy2pos(0, 0)));
                }
                for (auto &pk : picks)
                    taken_gclk.insert({pk.cmux_pos, pk.inst});
                // NOTE: a default BelId() is (pos 0, z 0), which is a REAL bel id in tile (0,0) —
                // FPLL(0,0) can be exactly that — so "found" needs an explicit flag, not a sentinel.
                BelId pll_bel;
                bool pll_found = false;
                for (BelId b : ctx->getBels())
                    if (ctx->getBelType(b) == id_altera_pll && uint32_t(b.pos) == fpll_pos) {
                        pll_bel = b;
                        pll_found = true;
                        break;
                    }
                if (!pll_found)
                    log_error("altera_pll '%s': no FPLL bel at chosen position (%d,%d)\n", ctx->nameOf(ci),
                              CycloneV::pos2x(CycloneV::pos_t(fpll_pos)), CycloneV::pos2y(CycloneV::pos_t(fpll_pos)));
                // Bind both cells so the pack-time (PLL, counter, cmux, gclk) choice survives into
                // bitstream emission. NOTE (B2, unresolved): the heap placer still MIGRATES the PLL
                // off this bel — measured, pack chose (0,0), emission saw (89,0) — while the PLLCLK
                // stays put. Attempts that failed: STRENGTH_USER, an isBelLocationValid pin, the BEL
                // attribute (getBelByName name-format mismatch), and an isValidBelForCellType pin
                // (makes the corner tile unreachable for the placer's radius search).
                ctx->bindBel(pll_bel, ci, STRENGTH_LOCKED);
                for (size_t j = 0; j < injectors.size(); j++) {
                    auto &pk = picks.at(j);
                    CellInfo *pc = injectors[j].second;
                    pc->attrs[id_PLLCLK_COUNTER] = Property(pk.phys_counter);
                    ci->attrs[ctx->idf("PLLCLK_PHYS_%d", injectors[j].first)] = Property(pk.phys_counter);
                    BelId cb;
                    bool cb_found = false;
                    for (BelId b : ctx->getBels())
                        if (ctx->getBelType(b) == id_MISTRAL_PLLCLK && uint32_t(b.pos) == pk.cmux_pos &&
                            ctx->bel_data(b).block_index == pk.inst) {
                            cb = b;
                            cb_found = true;
                            break;
                        }
                    if (!cb_found)
                        log_error("altera_pll '%s': no PLLCLK bel at chosen cmux position\n", ctx->nameOf(ci));
                    ctx->bindBel(cb, pc, STRENGTH_LOCKED);
                    Loc pl = ctx->getBelLocation(pll_bel), cl = ctx->getBelLocation(cb);
                    log_info("  PLL clock injector '%s': outclk[%d] -> FPLL(%d,%d) C%d -> cmux(%d,%d) gclk %d "
                             "(INPUT_SEL=0x%x)\n",
                             ctx->nameOf(pc), injectors[j].first, pl.x, pl.y, pk.phys_counter, cl.x, cl.y, pk.inst,
                             pk.sel);
                }
            }

            // The only *input* pins we model on the FPLL bel are refclk and
            // fbclk. Every other input the altera_pll cell carries (reset
            // tie-offs, unused reconfig/phase pins, ...) has no routable bel
            // pin; assign_default_pinmap would give it an identity mapping onto
            // a nonexistent bel pin and routing would then fail on it. The PLL
            // reset in particular is configured through SLF_RST in the
            // bitstream rather than routed, so we clear the mapping and
            // disconnect those inputs. refclk is deliberately kept mapped so
            // the still-unmodelled refclk -> PLL dedicated clock route surfaces
            // as a clear, explicit routing failure rather than being silently
            // dropped.
            const pool<IdString> kept_inputs = {id_refclk, id_fbclk};
            std::vector<IdString> to_disconnect;
            for (auto &pd : ci->ports) {
                if (pd.second.type != PORT_IN)
                    continue;
                if (kept_inputs.count(pd.first))
                    continue;
                ci->pin_data[pd.first].bel_pins.clear();
                to_disconnect.push_back(pd.first);
            }
            for (IdString p : to_disconnect)
                ci->disconnectPort(p);

            // Reference-clock delivery. The PLL reference input is the PMUX node
            // (mapped to id_REFCLK); it is fed only by the sector-clock (SCLK)
            // network, i.e. it is reachable only over a global clock spine whose
            // roots (GCLK/RCLK) are injected by a clock buffer (CMUX). A plain
            // fabric net from an input pin cannot reach it. So splice a
            // MISTRAL_CLKBUF onto the refclk: the reference clock is buffered
            // onto a global clock line (input pin -> CLKBUF -> GCLK -> SCLK ->
            // PMUX) and route_globals() then delivers it to the PLL. (Verified:
            // both create_clkbuf CMUXHG outputs reach the e50f PLL PMUX nodes.)
            // ...except the minimal Quartus reference does none of this. Its fit report says
            // "Reference Clock Sourced by: Dedicated Pin / CLKIN(0) source: FPGA_CLK1_50~input",
            // and its whole bitstream contains no clock-network path into any PLL and no CLKBUF on
            // the reference at all: the pin reaches the PLL over hardwiring that needs no bitstream
            // configuration. libmistral's p2p CLKIN table does not carry that edge for this package,
            // which is why the model forces the detour above. VUP_PLL_NO_REFCLK_BUF drops the detour
            // so the dedicated path can be tested on silicon.
            //
            // GUARD: the dedicated-pin path is ATTESTED FOR ONE CONFIGURATION ONLY -- the board
            // clock pin PIN_V11 feeding FPLL(0,14). Applying it to a PLL fed from some other pin
            // would emit a spine derived for the wrong source, i.e. a subtly WRONG bitstream rather
            // than an obviously dead one. So verify the reference really is driven by that pin, and
            // otherwise fall back to the old (never-working, but honest) modelled path with a loud
            // warning. Mark the cell so bitstream.cc applies the spine only in the attested case.
            NetInfo *refnet = ci->getPort(id_refclk);
            bool attested_pin = false;
            if (refnet != nullptr && refnet->driver.cell != nullptr) {
                // Walk back through clock buffers to the pad. Synthesis normally inserts a
                // MISTRAL_CLKBUF (and may insert a CLKENA) between the pin and the PLL -- that is
                // the ordinary shape for a clock -- and the LOC lives on the IO cell, not on the
                // buffer. Checking only the immediate driver meant attestation FAILED for every
                // buffered clock, so the reference spine was never emitted and the PLL could not
                // lock. It only ever passed for a PLL fed directly by a pad.
                CellInfo *drv = refnet->driver.cell;
                for (int hops = 0; hops < 8 && drv != nullptr && !drv->attrs.count(id_LOC); hops++) {
                    if (!ctx->is_clkbuf_cell(drv->type))
                        break;
                    NetInfo *up = drv->getPort(id_A);
                    if (up == nullptr || up->driver.cell == nullptr)
                        break;
                    drv = up->driver.cell;
                }
                if (drv != nullptr && drv->attrs.count(id_LOC) && drv->attrs.at(id_LOC).as_string() == "PIN_V11")
                    attested_pin = true;
            }
            if (!attested_pin && getenv("VUP_DEBUG_ATTEST")) {
                if (refnet == nullptr)
                    log_info("  [attest] '%s' has no refclk net\n", ctx->nameOf(ci));
                else if (refnet->driver.cell == nullptr)
                    log_info("  [attest] refclk net '%s' has no driver cell\n", ctx->nameOf(refnet));
                else {
                    CellInfo *drv = refnet->driver.cell;
                    log_info("  [attest] refclk net '%s' driver '%s' type '%s' port '%s'; LOC=%s\n",
                             ctx->nameOf(refnet), ctx->nameOf(drv), drv->type.c_str(ctx),
                             refnet->driver.port.c_str(ctx),
                             drv->attrs.count(id_LOC) ? drv->attrs.at(id_LOC).as_string().c_str() : "<absent>");
                    for (auto &a : drv->attrs)
                        log_info("  [attest]   attr %s = %s\n", a.first.c_str(ctx), a.second.as_string().c_str());
                }
            }
            if (!attested_pin && !getenv("VUP_PLL_LEGACY"))
                log_warning("altera_pll '%s': reference is not the attested board clock pin (PIN_V11); "
                            "falling back to the modelled refclk path, which is NOT known to work on "
                            "silicon. See mistral/PLL_OUTCLK_DESIGN.md.\n",
                            ctx->nameOf(ci));
            if (attested_pin)
                ci->attrs[id_PLLCLK_ATTESTED_REF] = 1;
            bool no_refclk_buf = attested_pin && !getenv("VUP_PLL_LEGACY");
            if (no_refclk_buf && refnet != nullptr) {
                ci->pin_data[id_refclk].bel_pins.clear();
                ci->disconnectPort(id_refclk);
                log_info("  refclk left UNROUTED for altera_pll '%s' (dedicated-pin path)\n", ctx->nameOf(ci));
            } else if (refnet != nullptr && refnet->driver.cell != nullptr &&
                       !ctx->is_clkbuf_cell(refnet->driver.cell->type)) {
                CellInfo *cbuf =
                        ctx->createCell(ctx->idf("%s$refclk_clkbuf", ci->name.c_str(ctx)), id_MISTRAL_CLKBUF);
                cbuf->addInput(id_A);
                cbuf->addOutput(id_Q);
                // input-pin net -> CLKBUF.A
                ci->movePortTo(id_refclk, cbuf, id_A);
                // CLKBUF.Q -> buffered net -> PLL.refclk
                NetInfo *bufnet = ctx->createNet(ctx->idf("%s$refclk_buf", ci->name.c_str(ctx)));
                cbuf->connectPort(id_Q, bufnet);
                ci->connectPort(id_refclk, bufnet);
                log_info("  inserted refclk clock buffer '%s' for altera_pll '%s'\n", ctx->nameOf(cbuf),
                         ctx->nameOf(ci));
            }

            log_info("Set up altera_pll '%s' (%d output clock%s).\n", ctx->nameOf(ci), nclk, nclk == 1 ? "" : "s");
        }
    }


    // Absorb $_TBUF_ into MISTRAL_IO's OE — G6 IO-ring blocker.
    //
    // yosys' iopadmap creates the inout pad but leaves the tristate behind: measured on a 16-bit
    // bidirectional bus it emits 16 MISTRAL_IO *and* 16 orphan $_TBUF_, with the pad's OE tied to
    // constant 1 (i.e. permanently driving, which would fight an SDRAM), and P&R then dies with
    // "no BELs remaining to implement cell type '$_TBUF_'". The bel already has an OE pin
    // (io.cc add_bel_pin(..., id_OE, ...)), so the missing piece is purely packing.
    //
    // Shape, from the emitted netlist:
    //     $_TBUF_    A=<data out>  E=<output enable>  Y=<net N>
    //     MISTRAL_IO I=<net N>     OE=1               PAD=<port>
    //     <readers>  ... also on <net N>   (the read-back path, which belongs on IO.O)
    // so: I <- A, OE <- E, other readers of N move to IO.O, drop the buffer.
    void pack_tristates()
    {
        std::vector<IdString> to_remove;
        for (auto &cell : ctx->cells) {
            CellInfo *io = cell.second.get();
            if (io->type != id_MISTRAL_IO)
                continue;
            NetInfo *inet = io->getPort(id_I);
            if (inet == nullptr || inet->driver.cell == nullptr)
                continue;
            CellInfo *tb = inet->driver.cell;
            if (tb->type != ctx->id("$_TBUF_"))
                continue;

            NetInfo *data = tb->getPort(ctx->id("A"));
            NetInfo *oe = tb->getPort(ctx->id("E"));
            if (data == nullptr || oe == nullptr)
                continue;

            // Everything else reading the buffer's output is the read-back path; it must come from
            // the pad (IO.O), not from the driver we are about to delete.
            std::vector<PortRef> readers;
            for (auto &u : inet->users)
                if (u.cell != io)
                    readers.push_back(u);

            // The pad cell as emitted may not declare every port (its OE arrived tied to a
            // constant, and O is absent entirely because the read-back was mis-wired to the buffer
            // output) -- create what is missing before connecting, or connectPort throws.
            if (!io->ports.count(id_OE))
                io->addInput(id_OE);
            if (!io->ports.count(id_O))
                io->addOutput(id_O);

            io->disconnectPort(id_I);
            tb->disconnectPort(ctx->id("A"));
            tb->disconnectPort(ctx->id("E"));
            tb->disconnectPort(ctx->id("Y"));
            io->connectPort(id_I, data);
            io->disconnectPort(id_OE);
            io->connectPort(id_OE, oe);

            if (!readers.empty()) {
                NetInfo *obuf = io->getPort(id_O);
                if (obuf == nullptr) {
                    obuf = ctx->createNet(ctx->idf("%s$pad_in", io->name.c_str(ctx)));
                    io->connectPort(id_O, obuf);
                }
                for (auto &r : readers) {
                    r.cell->disconnectPort(r.port);
                    r.cell->connectPort(r.port, obuf);
                }
            }
            // getBelPinsForCellPin() is pin_data.at(pin).bel_pins -- an EXPLICIT map with no
            // name-based fallback. Ports created here therefore route nowhere unless mapped, which
            // is exactly how the first version failed: it built, but silicon showed the pad never
            // driving and the OEIN node <UNDRIVEN> in the bitstream. io.cc binds I->DATAOUT,
            // OE->OEIN, O->DATAIN on the bel, so mirror those names here.
            for (auto &port : io->ports)
                io->pin_data[port.first];
            io->pin_data[id_I].bel_pins = {id_I};
            io->pin_data[id_OE].bel_pins = {id_OE};
            if (io->ports.count(id_O))
                io->pin_data[id_O].bel_pins = {id_O};

            log_info("  tristate %s: OE net '%s' (%d user(s)), data net '%s'\n", ctx->nameOf(io),
                     ctx->nameOf(oe), int(oe->users.entries()), ctx->nameOf(data));
            to_remove.push_back(tb->name);
        }
        for (IdString n : to_remove)
            ctx->cells.erase(n);
        if (!to_remove.empty())
            log_info("Packed %d tristate buffer(s) into MISTRAL_IO OE.\n", int(to_remove.size()));
    }

    // IO-register packing (MISTRAL_GAPS "IO registers"). The three IO registers are inline stages
    // in the DQS16 block between the fabric-facing IOINT ports and the pad-facing PHYDDIO ports --
    // there is no separate register bel to place. Packing therefore means: delete the pad-adjacent
    // fabric FF, hand its D-side net to the pad, route its clock to the per-pad DCMUX sink, and mark
    // the pad so write_io_cell emits the DQS16 config. Legality is deliberately strict: a plain
    // posedge DFF (ENA/ACLR/SCLR/SLOAD all inactive), single fanout, real clock net.
    //
    // Gate: the qsf FAST_INPUT_REGISTER / FAST_OUTPUT_REGISTER / FAST_OUTPUT_ENABLE_REGISTER
    // instance assignments (copied onto the cell by prepare_io), or VUP_IOREG=1 to pack every legal
    // pad. A requested-but-illegal pack warns and falls back to the fabric FF -- never silently.
    bool plain_dff(CellInfo *ff, bool allow_inv_d = false)
    {
        if (ff == nullptr || ff->type != id_MISTRAL_FF)
            return false;
        // Control set must be inactive: ENA high, ACLR high (active-low), SCLR/SLOAD low.
        auto st = [&](IdString p) { return ff->get_pin_state(p); };
        if (ff->getPort(id_ENA) != nullptr && st(id_ENA) != PIN_1)
            return false;
        if (ff->getPort(id_ACLR) != nullptr && st(id_ACLR) != PIN_1)
            return false;
        if (ff->getPort(id_SCLR) != nullptr && st(id_SCLR) != PIN_0)
            return false;
        if (ff->getPort(id_SLOAD) != nullptr && st(id_SLOAD) != PIN_0)
            return false;
        NetInfo *clk = ff->getPort(id_CLK);
        if (clk == nullptr || clk->driver.cell == nullptr || st(id_CLK) != PIN_SIG)
            return false;
        NetInfo *d = ff->getPort(id_DATAIN);
        if (d == nullptr)
            return false;
        // An absorbed inverter on D (PIN_INV, common for mostly-1 signals) is packable for
        // out/OE registers: the packer re-materialises the inversion as a MISTRAL_NOT LUT.
        CellPinState dst = ff->get_pin_state(id_DATAIN);
        if (dst != PIN_SIG && !(allow_inv_d && dst == PIN_INV))
            return false;
        return true;
    }

    // Resolve the FF behind a pad's out/OE driver. Synthesis stores mostly-1 signals INVERTED and,
    // because pads have no hard inverter option, a MISTRAL_NOT survives between FF and pad (the
    // LAB-absorbed variant shows up as PIN_INV on the FF's D instead). Walk one optional NOT and
    // fold both inversions into a single parity: the intended pad value V = reg(D ^ parity).
    struct PadDrv
    {
        CellInfo *ff = nullptr;
        CellInfo *pad_not = nullptr; // surviving inverter between FF and pad, if any
        bool parity = false;         // 1 -> pad value is the INVERSE of what reg(D net) gives
    };
    PadDrv resolve_pad_ff(NetInfo *net)
    {
        PadDrv r;
        CellInfo *drv = (net != nullptr) ? net->driver.cell : nullptr;
        if (drv != nullptr && drv->type == id_MISTRAL_NOT) {
            NetInfo *a = drv->getPort(id_A);
            if (a == nullptr || a->driver.cell == nullptr)
                return r;
            r.pad_not = drv;
            r.parity = !r.parity;
            drv = a->driver.cell;
        }
        if (!plain_dff(drv, /*allow_inv_d=*/true))
            return r;
        if (drv->get_pin_state(id_DATAIN) == PIN_INV)
            r.parity = !r.parity;
        r.ff = drv;
        return r;
    }

    // The D-side net a packed out/OE register must present to the pad: the FF's D net, re-inverted
    // through a real LUT when the folded parity is odd (MISTRAL_NOT places as a LUT).
    NetInfo *ioreg_pad_d(CellInfo *io, const PadDrv &pd, const char *tag)
    {
        NetInfo *d = pd.ff->getPort(id_DATAIN);
        if (!pd.parity)
            return d;
        CellInfo *inv = ctx->createCell(ctx->idf("%s$ioreg_%s_inv", ctx->nameOf(io), tag), id_MISTRAL_NOT);
        inv->addInput(id_A);
        inv->addOutput(id_Q);
        NetInfo *q = ctx->createNet(ctx->idf("%s$ioreg_%s_inv_q", ctx->nameOf(io), tag));
        inv->connectPort(id_A, d);
        inv->connectPort(id_Q, q);
        return q;
    }

    bool attr_on(CellInfo *ci, IdString attr)
    {
        if (!ci->attrs.count(attr))
            return false;
        std::string v = ci->attrs.at(attr).as_string();
        return v == "ON" || v == "on" || v == "1";
    }

    // Attach the register clock net to the pad cell on `port` (ICLK or OCLK), mapped to the
    // matching bel pin. Both out and OE registers clock from the one CLKOUT[0] DCMUX, so a second
    // caller must agree on the net.
    bool attach_ioreg_clock(CellInfo *io, IdString port, NetInfo *clk)
    {
        if (io->ports.count(port)) {
            NetInfo *have = io->getPort(port);
            if (have != clk) {
                log_warning("IO '%s': %s already carries clock '%s', cannot also clock from '%s'\n", ctx->nameOf(io),
                            port.c_str(ctx), ctx->nameOf(have), ctx->nameOf(clk));
                return false;
            }
            return true;
        }
        io->addInput(port);
        io->connectPort(port, clk);
        io->pin_data[port].bel_pins = {port};
        return true;
    }

    void pack_io_registers()
    {
        bool force_all = getenv("VUP_IOREG") != nullptr;
        int packed_in = 0, packed_out = 0, packed_oe = 0;
        std::vector<IdString> dead_ffs;      // input FFs: always deleted (their Q moved to the pad)
        pool<IdString> absorbed_ffs;         // out/OE FFs: deleted only if the pad was the last user
        for (auto &cell : ctx->cells) {
            CellInfo *io = cell.second.get();
            if (!ctx->is_io_cell(io->type))
                continue;
            bool want_in = force_all || attr_on(io, id_FAST_INPUT_REGISTER);
            bool want_out = force_all || attr_on(io, id_FAST_OUTPUT_REGISTER);
            bool want_oe = force_all || attr_on(io, id_FAST_OUTPUT_ENABLE_REGISTER);
            if (!want_in && !want_out && !want_oe)
                continue;
            // The registers live in the pad's associated DQS16; a pad without one (and an unplaced
            // pad) cannot pack. pack_io() has already LOC-bound every top-level pin's bel.
            if (io->bel == BelId()) {
                if (!force_all)
                    log_warning("IO '%s': FAST_*_REGISTER requested but the pad has no bound bel\n", ctx->nameOf(io));
                continue;
            }
            int pad_bi = ctx->bel_data(io->bel).block_index;
            if (!ctx->cyclonev->p2p_to(
                        CycloneV::pnode(CycloneV::GPIO, CycloneV::pos_t(io->bel.pos), CycloneV::PNONE, pad_bi, -1))) {
                if (!force_all)
                    log_warning("IO '%s': FAST_*_REGISTER requested but the pad has no associated DQS16\n",
                                ctx->nameOf(io));
                continue;
            }

            // OUTPUT register: pad I driven by a plain DFF. The pad re-sources its data from the
            // FF's D net and captures it in the DQS16 OUTREG instead -- same D, same clock, same
            // value. If the FF's Q has other fabric users the FF simply STAYS for them (register
            // duplication, exactly what Quartus does for a driver with feedback like drv_r <=
            // drv_r + 1); if the pad was its only user it becomes dead and is swept afterwards.
            if (want_out) {
                PadDrv pd = resolve_pad_ff(io->getPort(id_I));
                if (pd.ff != nullptr) {
                    NetInfo *clk = pd.ff->getPort(id_CLK);
                    if (attach_ioreg_clock(io, id_OCLK, clk)) {
                        NetInfo *d = ioreg_pad_d(io, pd, "out");
                        io->disconnectPort(id_I);
                        io->connectPort(id_I, d);
                        absorbed_ffs.insert(pd.ff->name);
                        if (pd.pad_not != nullptr)
                            absorbed_ffs.insert(pd.pad_not->name);
                        io->params[id_IOREG_OUT] = 1;
                        // Intended pad value at power-up: the FF inits to 0, so V_init = parity.
                        io->params[id_IOREG_OUT_INIT] = pd.parity ? 1 : 0;
                        packed_out++;
                        log_info("  IO-reg out: '%s' registers D net '%s' (FF '%s'%s)\n", ctx->nameOf(io),
                                 ctx->nameOf(d), ctx->nameOf(pd.ff), pd.parity ? ", inverted" : "");
                    }
                } else if (!force_all) {
                    log_warning("IO '%s': FAST_OUTPUT_REGISTER requested but the driver is not a packable plain "
                                "DFF -- keeping the fabric path\n",
                                ctx->nameOf(io));
                }
            }

            // OE register: same shape on the OE port. A single fabric OE FF fanning out to a whole
            // bus packs as one OEREG per pad, all fed from the FF's D net.
            if (want_oe) {
                PadDrv pd = resolve_pad_ff(io->getPort(id_OE));
                if (pd.ff != nullptr) {
                    NetInfo *clk = pd.ff->getPort(id_CLK);
                    if (attach_ioreg_clock(io, id_OCLK, clk)) {
                        NetInfo *d = ioreg_pad_d(io, pd, "oe");
                        io->disconnectPort(id_OE);
                        io->connectPort(id_OE, d);
                        absorbed_ffs.insert(pd.ff->name);
                        if (pd.pad_not != nullptr)
                            absorbed_ffs.insert(pd.pad_not->name);
                        io->params[id_IOREG_OE] = 1;
                        io->params[id_IOREG_OE_INIT] = pd.parity ? 1 : 0;
                        packed_oe++;
                        log_info("  IO-reg OE: '%s' registers D net '%s' (FF '%s'%s)\n", ctx->nameOf(io),
                                 ctx->nameOf(d), ctx->nameOf(pd.ff), pd.parity ? ", inverted" : "");
                    }
                } else if (!force_all) {
                    log_warning("IO '%s': FAST_OUTPUT_ENABLE_REGISTER requested but the OE driver is not a packable "
                                "plain DFF -- keeping the fabric path\n",
                                ctx->nameOf(io));
                }
            }

            // INPUT register: pad O feeding exactly one plain DFF's D. The pad then presents the
            // FF's Q net on the REGISTERED tap (bel pin OREG = DATAIN[3] = the DQS16 read-FIFO
            // output) and the fabric FF disappears.
            if (want_in) {
                NetInfo *onet = io->getPort(id_O);
                CellInfo *ff = nullptr;
                if (onet != nullptr && onet->users.entries() == 1) {
                    auto &usr = *onet->users.begin();
                    if (usr.port == id_DATAIN && plain_dff(usr.cell))
                        ff = usr.cell;
                }
                if (ff != nullptr) {
                    NetInfo *q = ff->getPort(id_Q);
                    NetInfo *clk = ff->getPort(id_CLK);
                    if (q != nullptr && attach_ioreg_clock(io, id_ICLK, clk)) {
                        ff->disconnectPort(id_Q);
                        for (auto &p : ff->ports)
                            if (p.second.net != nullptr)
                                ff->disconnectPort(p.first);
                        io->disconnectPort(id_O);
                        io->connectPort(id_O, q);
                        io->pin_data[id_O].bel_pins = {id_OREG};
                        dead_ffs.push_back(ff->name);
                        io->params[id_IOREG_IN] = 1;
                        packed_in++;
                        log_info("  IO-reg in: '%s' absorbed FF '%s' (Q net '%s')\n", ctx->nameOf(io), ctx->nameOf(ff),
                                 ctx->nameOf(q));
                    }
                } else if (!force_all) {
                    log_warning("IO '%s': FAST_INPUT_REGISTER requested but the pad does not feed exactly one "
                                "packable plain DFF -- keeping the fabric FF\n",
                                ctx->nameOf(io));
                }
            }
        }
        for (IdString n : dead_ffs)
            ctx->cells.erase(n);
        // Sweep out/OE FFs whose Q lost its last user to the packing; keep the duplicated ones.
        for (IdString n : absorbed_ffs) {
            if (!ctx->cells.count(n))
                continue;
            CellInfo *ff = ctx->cells.at(n).get();
            NetInfo *q = ff->getPort(id_Q);
            if (q != nullptr && q->users.entries() > 0)
                continue;
            for (auto &p : ff->ports)
                if (p.second.net != nullptr)
                    ff->disconnectPort(p.first);
            ctx->cells.erase(n);
        }
        if (packed_in + packed_out + packed_oe > 0)
            log_info("Packed IO registers: %d input, %d output, %d OE.\n", packed_in, packed_out, packed_oe);
    }

    // A clock buffer sitting on a PLL output is redundant and actively harmful. The PLLCLK injector
    // spliced in by setup_fplls IS the global driver -- its Q binds the cmux CLKOUT (GCLK root) --
    // so a downstream CLKBUF claims a SECOND global root for the same signal. Worse, CLKBUF and
    // PLLCLK bels model the same physical CLKOUT, so the two can be placed on top of each other and
    // global routing then aborts binding that wire to a second net. Synthesis inserts these buffers
    // routinely, which is why it bit as soon as a design had two PLLs.
    void bypass_pll_clkbufs()
    {
        std::vector<IdString> dead;
        for (auto &cell : ctx->cells) {
            CellInfo *cb = cell.second.get();
            if (cb->type != id_MISTRAL_CLKBUF)
                continue;
            NetInfo *in = cb->getPort(id_A);
            NetInfo *out = cb->getPort(id_Q);
            if (in == nullptr || out == nullptr || in->driver.cell == nullptr)
                continue;
            if (in->driver.cell->type != id_MISTRAL_PLLCLK)
                continue;
            std::vector<PortRef> users;
            for (auto &u : out->users)
                users.push_back(u);
            for (auto &u : users) {
                u.cell->disconnectPort(u.port);
                u.cell->connectPort(u.port, in);
            }
            cb->disconnectPort(id_A);
            cb->disconnectPort(id_Q);
            dead.push_back(cb->name);
            log_info("  bypassed redundant clock buffer '%s' on PLL output net '%s'\n", ctx->nameOf(cb),
                     ctx->nameOf(in));
        }
        for (IdString n : dead)
            ctx->cells.erase(n);
    }

    void run()
    {
        init_constant_nets();
        pack_constants();
        pack_io();
        pack_tristates();
        pack_io_registers();
        constrain_carries();
        constrain_lutram();
        setup_m10ks();
        setup_dsps();
        setup_fplls();
        bypass_pll_clkbufs();
    }
};
}; // namespace

bool Arch::pack()
{
    MistralPacker packer(getCtx());
    packer.run();

    assignArchInfo();

    return true;
}

NEXTPNR_NAMESPACE_END
