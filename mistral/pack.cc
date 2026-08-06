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

    void setup_m10ks()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_M10K)
                continue;

            auto abits = ci->params.at(id_CFG_ABITS).as_int64();
            auto dbits = ci->params.at(id_CFG_DBITS).as_int64();
            NPNR_ASSERT(abits >= 7 && abits <= 13);
            NPNR_ASSERT(dbits == 1 || dbits == 2 || dbits == 5 || dbits == 10 || dbits == 20 || dbits == 40);
            NPNR_ASSERT((1 << abits) * dbits <= 10240);

            log_info("Setting up %ld-bit address, %ld-bit data M10K for %s.\n", abits, dbits,
                     ci->name.str(ctx).c_str());

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
                if (!ctx->pllclk_choose(int(injectors.size()), fpll_pos, picks))
                    log_error("altera_pll '%s': no FPLL position has dedicated global-cmux wiring for "
                              "%d output clock(s)\n",
                              ctx->nameOf(ci), int(injectors.size()));
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
            bool no_refclk_buf = !getenv("VUP_PLL_LEGACY");
            NetInfo *refnet = ci->getPort(id_refclk);
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

    void run()
    {
        init_constant_nets();
        pack_constants();
        pack_io();
        constrain_carries();
        constrain_lutram();
        setup_m10ks();
        setup_fplls();
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
