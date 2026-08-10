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

#include "log.h"
#include "nextpnr.h"
#include "util.h"

#include <queue>

NEXTPNR_NAMESPACE_BEGIN

void Arch::create_clkbuf(int x, int y)
{
    for (int z = 0; z < 4; z++) {
        if (z != 2)
            continue; // TODO: why do other Zs not work?
        // For now we only consider the input path from general routing, other inputs like dedicated clock pins are
        // still a TODO
        BelId bel = add_bel(x, y, idf("CLKBUF[%d]", z), id_MISTRAL_CLKENA);
        add_bel_pin(bel, id_A, PORT_IN, get_port(CycloneV::CMUXHG, x, y, -1, CycloneV::CLKIN, z));
        add_bel_pin(bel, id_Q, PORT_OUT, get_port(CycloneV::CMUXHG, x, y, z, CycloneV::CLKOUT));
        // TODO: enable pin
        bel_data(bel).block_index = z;
    }
}

bool Arch::is_clkbuf_cell(IdString cell_type) const { return cell_type.in(id_MISTRAL_CLKENA, id_MISTRAL_CLKBUF); }

void Arch::create_hps_mpu_general_purpose(int x, int y)
{
    BelId gp_bel =
            add_bel(x, y, id_cyclonev_hps_interface_mpu_general_purpose, id_cyclonev_hps_interface_mpu_general_purpose);
    for (int i = 0; i < 32; i++) {
        add_bel_pin(gp_bel, idf("gp_in[%d]", i), PORT_IN,
                    get_port(CycloneV::HPS_MPU_GENERAL_PURPOSE, x, y, -1, CycloneV::GP_IN, i));
        add_bel_pin(gp_bel, idf("gp_out[%d]", i), PORT_OUT,
                    get_port(CycloneV::HPS_MPU_GENERAL_PURPOSE, x, y, -1, CycloneV::GP_OUT, i));
    }
}

// G3: the HPS2FPGA lightweight bridge -- the ARM's 2 MB MMIO window at 0xFF200000, and the SVGA
// command-ring path. The HPS is the AXI-3 MASTER: address/data/valid arrive INTO fabric (PORT_OUT
// bel pins -- the interface block drives fabric wires), ready/response go back (PORT_IN). Pin names
// follow the Quartus WYSIWYG primitive so real cores' instantiations match unchanged.
void Arch::create_hps_lwh2f(int x, int y)
{
    BelId bel = add_bel(x, y, id_cyclonev_hps_interface_hps2fpga_light_weight,
                        id_cyclonev_hps_interface_hps2fpga_light_weight);
    auto B = CycloneV::HPS_HPS2FPGA_LIGHT_WEIGHT;
    // Direction is DERIVED from the rnode type in add_hps_pin, not asserted from AXI semantics --
    // the first cut hardcoded PORT_IN/OUT from master/slave reasoning, which is exactly the trap
    // the GP naming inversion sets (see add_hps_pin).
    auto one = [&](const char *n, CycloneV::port_type_t pt) { add_hps_pin(bel, id(n), B, x, y, pt, -1, -1); };
    auto bus = [&](const char *n, CycloneV::port_type_t pt, int w) {
        for (int i = 0; i < w; i++) add_hps_pin(bel, idf("%s[%d]", n, i), B, x, y, pt, -1, i);
    };
    one("clk", CycloneV::CLK);
    bus("awid", CycloneV::AWID, 12); bus("awaddr", CycloneV::AWADDR, 21); bus("awlen", CycloneV::AWLEN, 4);
    bus("awsize", CycloneV::AWSIZE, 3); bus("awburst", CycloneV::AWBURST, 2); bus("awlock", CycloneV::AWLOCK, 2);
    bus("awcache", CycloneV::AWCACHE, 4); bus("awprot", CycloneV::AWPROT, 3);
    one("awvalid", CycloneV::AWVALID); one("awready", CycloneV::AWREADY);
    bus("wid", CycloneV::WID, 12); bus("wdata", CycloneV::WDATA, 32); bus("wstrb", CycloneV::WSTRB, 4);
    one("wlast", CycloneV::WLAST); one("wvalid", CycloneV::WVALID); one("wready", CycloneV::WREADY);
    bus("bid", CycloneV::BID, 12); bus("bresp", CycloneV::BRESP, 2);
    one("bvalid", CycloneV::BVALID); one("bready", CycloneV::BREADY);
    bus("arid", CycloneV::ARID, 12); bus("araddr", CycloneV::ARADDR, 21); bus("arlen", CycloneV::ARLEN, 4);
    bus("arsize", CycloneV::ARSIZE, 3); bus("arburst", CycloneV::ARBURST, 2); bus("arlock", CycloneV::ARLOCK, 2);
    bus("arcache", CycloneV::ARCACHE, 4); bus("arprot", CycloneV::ARPROT, 3);
    one("arvalid", CycloneV::ARVALID); one("arready", CycloneV::ARREADY);
    bus("rid", CycloneV::RID, 12); bus("rdata", CycloneV::RDATA, 32); bus("rresp", CycloneV::RRESP, 2);
    one("rlast", CycloneV::RLAST); one("rvalid", CycloneV::RVALID); one("rready", CycloneV::RREADY);
}

// G3: add an HPS-interface bel pin with direction DERIVED from the routing-node type, not guessed.
// The GP interface proves the port NAMES are inverted vs the physical direction: gp_in (a PORT_IN
// bel pin in the silicon-verified mpu bel) resolves to a GOUT rnode. So the reliable rule is
// GOUT -> PORT_IN (fabric drives into the block), GIN -> PORT_OUT (block drives into fabric); a
// clock node (DCMUX) is a block input. Guessing from AXI master/slave semantics is how the first
// lwh2f cut risked getting 22 directions wrong.
void Arch::add_hps_pin(BelId bel, IdString pin, CycloneV::block_type_t bt, int x, int y,
                       CycloneV::port_type_t pt, int bi, int pi)
{
    if (!has_port(bt, x, y, bi, pt, pi))
        return;
    WireId w = get_port(bt, x, y, bi, pt, pi);
    auto t = CycloneV::rn2t(w.node);
    PortType dir = (t == CycloneV::GIN) ? PORT_OUT : PORT_IN;
    add_bel_pin(bel, pin, dir, w);
}

void Arch::create_hps_f2sdram(int x, int y)
{
    BelId bel = add_bel(x, y, id_cyclonev_hps_interface_fpga2sdram, id_cyclonev_hps_interface_fpga2sdram);
    auto B = CycloneV::HPS_FPGA2SDRAM;
    // bus port on a given command/data port index
    auto bus = [&](const char *n, CycloneV::port_type_t pt, int port, int w) {
        for (int i = 0; i < w; i++)
            add_hps_pin(bel, idf("%s_%d[%d]", n, port, i), B, x, y, pt, port, i);
    };
    auto scalar = [&](const char *n, CycloneV::port_type_t pt, int port) {
        add_hps_pin(bel, idf("%s_%d", n, port), B, x, y, pt, port, -1);
    };
    // 6 command ports
    for (int p = 0; p < 6; p++) {
        bus("cmd_data", CycloneV::CMD_DATA, p, 60);
        scalar("cmd_valid", CycloneV::CMD_VALID, p);
        scalar("cmd_ready", CycloneV::CMD_READY, p);
        scalar("cmd_clk", CycloneV::CMD_PORT_CLK, p);
        bus("wrack_data", CycloneV::WRACK_DATA, p, 10);
        scalar("wrack_valid", CycloneV::WRACK_VALID, p);
        scalar("wrack_ready", CycloneV::WRACK_READY, p);
    }
    // 4 write data ports
    for (int p = 0; p < 4; p++) {
        bus("wr_data", CycloneV::WR_DATA, p, 90);
        scalar("wr_valid", CycloneV::WR_VALID, p);
        scalar("wr_ready", CycloneV::WR_READY, p);
        scalar("wr_clk", CycloneV::WR_CLK, p);
    }
    // 4 read data ports
    for (int p = 0; p < 4; p++) {
        bus("rd_data", CycloneV::RD_DATA, p, 80);
        scalar("rd_valid", CycloneV::RD_VALID, p);
        scalar("rd_ready", CycloneV::RD_READY, p);
        scalar("rd_clk", CycloneV::RD_CLK, p);
    }
    // configuration (single port each) -- fabric drives these to select port widths and FIFO maps.
    // The bitstream carries the wiring; the HPS-side applycfg sequence (MiSTer Main at core load)
    // actually enables the ports. See MISTRAL_GAPS G3.
    // config ports are single (bi = -1), unlike the numbered data ports
    auto cfg = [&](const char *n, CycloneV::port_type_t pt, int w) {
        for (int i = 0; i < w; i++) add_hps_pin(bel, idf("%s[%d]", n, i), B, x, y, pt, -1, i);
    };
    cfg("cfg_port_width", CycloneV::CFG_PORT_WIDTH, 12);
    cfg("cfg_axi_mm_select", CycloneV::CFG_AXI_MM_SELECT, 6);
    cfg("cfg_cport_type", CycloneV::CFG_CPORT_TYPE, 12);
    cfg("cfg_cport_rfifo_map", CycloneV::CFG_CPORT_RFIFO_MAP, 18);
    cfg("cfg_cport_wfifo_map", CycloneV::CFG_CPORT_WFIFO_MAP, 18);
    cfg("cfg_rfifo_cport_map", CycloneV::CFG_RFIFO_CPORT_MAP, 16);
    cfg("cfg_wfifo_cport_map", CycloneV::CFG_WFIFO_CPORT_MAP, 16);
}

void Arch::create_control(int x, int y)
{
    BelId oscillator_bel = add_bel(x, y, id_cyclonev_oscillator, id_cyclonev_oscillator);
    add_bel_pin(oscillator_bel, id_oscena, PORT_IN, get_port(CycloneV::CTRL, x, y, -1, CycloneV::OSC_ENA, -1));
    add_bel_pin(oscillator_bel, id_clkout, PORT_OUT, get_port(CycloneV::CTRL, x, y, -1, CycloneV::CLK_OUT, -1));
    add_bel_pin(oscillator_bel, id_clkout1, PORT_OUT, get_port(CycloneV::CTRL, x, y, -1, CycloneV::CLK_OUT1, -1));
}

struct MistralGlobalRouter
{
    Context *ctx;

    MistralGlobalRouter(Context *ctx) : ctx(ctx) {};

    // When routing globals; we allow global->local for some tricky cases but never local->local
    bool global_pip_filter(PipId pip) const
    {
        auto src_type = CycloneV::rn2t(pip.src);
        return src_type != CycloneV::H14 && src_type != CycloneV::H6 && src_type != CycloneV::H3 &&
               src_type != CycloneV::V12 && src_type != CycloneV::V2 && src_type != CycloneV::V4 &&
               src_type != CycloneV::WM;
    }

    // Dedicated backwards BFS routing for global networks
    template <typename Tfilt>
    bool backwards_bfs_route(NetInfo *net, store_index<PortRef> user_idx, int iter_limit, bool strict, Tfilt pip_filter)
    {
        // Queue of wires to visit
        std::queue<WireId> visit;
        // Wire -> upstream pip
        dict<WireId, PipId> backtrace;

        // Lookup source and destination wires
        WireId src = ctx->getNetinfoSourceWire(net);
        WireId dst = ctx->getNetinfoSinkWire(net, net->users.at(user_idx), 0);

        if (src == WireId())
            log_error("Net '%s' has an invalid source port %s.%s\n", ctx->nameOf(net), ctx->nameOf(net->driver.cell),
                      ctx->nameOf(net->driver.port));

        if (dst == WireId())
            log_error("Net '%s' has an invalid sink port %s.%s\n", ctx->nameOf(net),
                      ctx->nameOf(net->users.at(user_idx).cell), ctx->nameOf(net->users.at(user_idx).port));

        if (ctx->getBoundWireNet(src) != net)
            ctx->bindWire(src, net, STRENGTH_LOCKED);

        if (src == dst) {
            // Nothing more to do
            return true;
        }

        visit.push(dst);
        backtrace[dst] = PipId();

        int iter = 0;

        while (!visit.empty() && (iter++ < iter_limit)) {
            WireId cursor = visit.front();
            visit.pop();
            // Search uphill pips
            for (PipId pip : ctx->getPipsUphill(cursor)) {
                // Skip pip if unavailable, and not because it's already used for this net
                if (!ctx->checkPipAvail(pip) && ctx->getBoundPipNet(pip) != net)
                    continue;
                WireId prev = ctx->getPipSrcWire(pip);
                // Ditto for the upstream wire
                if (!ctx->checkWireAvail(prev) && ctx->getBoundWireNet(prev) != net)
                    continue;
                // Skip already visited wires
                if (backtrace.count(prev))
                    continue;
                // Apply our custom pip filter
                if (!pip_filter(pip))
                    continue;
                // Add to the queue
                visit.push(prev);
                backtrace[prev] = pip;
                // Check if we are done yet
                if (prev == src)
                    goto done;
            }
            if (false) {
            done:
                break;
            }
        }

        if (backtrace.count(src)) {
            WireId cursor = src;
            std::vector<PipId> pips;
            // Create a list of pips on the routed path
            while (true) {
                PipId pip = backtrace.at(cursor);
                if (pip == PipId())
                    break;
                pips.push_back(pip);
                cursor = ctx->getPipDstWire(pip);
            }
            // Reverse that list
            std::reverse(pips.begin(), pips.end());
            // Bind pips until we hit already-bound routing
            static const bool dbg_clk = getenv("VUP_DEBUG_IOREG_CLK") != nullptr;
            for (PipId pip : pips) {
                WireId dst = ctx->getPipDstWire(pip);
                if (ctx->getBoundWireNet(dst) == net)
                    break;
                ctx->bindPip(pip, net, STRENGTH_LOCKED);
                if (dbg_clk)
                    log_info("      [ioregclk] net '%s' sink %s.%s: %s -> %s\n", ctx->nameOf(net),
                             ctx->nameOf(net->users.at(user_idx).cell), ctx->nameOf(net->users.at(user_idx).port),
                             ctx->nameOfWire(ctx->getPipSrcWire(pip)), ctx->nameOfWire(dst));
            }
            return true;
        } else {
            if (strict)
                log_error("Failed to route net '%s' from %s to %s (sink %s.%s, cell type %s) using dedicated "
                          "routing.\n",
                          ctx->nameOf(net), ctx->nameOfWire(src), ctx->nameOfWire(dst),
                          ctx->nameOf(net->users.at(user_idx).cell), ctx->nameOf(net->users.at(user_idx).port),
                          net->users.at(user_idx).cell->type.c_str(ctx));
            return false;
        }
    }

    bool is_relaxed_sink(const PortRef &sink) const
    {
        // Cases where global clocks are driving fabric
        if (sink.cell->type == id_MISTRAL_FF && sink.port != id_CLK)
            return true;
        return false;
    }

    // IO-edge clock-spine legs attested on silicon for PAD-REGISTER clock delivery (the
    // ICLK/OCLK DCMUX sinks). The table is the UNION of every leg observed in a working
    // bitstream: Quartus qrall (vs qrbase) and the proven open build sdreg135 (vs qrbase).
    // The routing model exposes parallel legs beyond these; at least one is a FALSE PIP:
    // XCLKB2B.*.000.0008 -> TD routes fine in the model but measured DEAD on silicon
    // (svga_m0 builds 0x10-0x14: the DQS16-stage registers froze at init, every read
    // returned the last-driven DQ word), while sdreg135 -- same pads, same qsf -- carries
    // that delivery exclusively on XCLKB2B.*.000.0004. Legs 0001/0008 ARE attested for
    // the XCLKB2A/TCLK per-pad branch, so this is a per-(type,row,index) property, not a
    // per-index one. Routes to pad-clock sinks are pinned to attested legs; every other
    // clock consumer (LAB/M10K/DSP clocking) is unrestricted. A registered pad on an
    // edge row outside this table fails the strict BFS loudly -- validate the new leg on
    // silicon, then extend the table.
    bool ioreg_leg_ok(PipId pip) const
    {
        WireId w = ctx->getPipDstWire(pip);
        if (w.is_nextpnr_created())
            return true;
        auto t = CycloneV::rn2t(w.node);
        int y = CycloneV::rn2y(w.node);
        int z = CycloneV::rn2z(w.node);
        // Only two constraints are backed by silicon evidence; the rest of the spine
        // stages are 1:1 links (rmux pattern 254 -- no choice to make) or have several
        // attested-working legs, so they stay unrestricted:
        //  1. XCLKB2B row 0: leg 0008 measured DEAD (svga_m0 0x10-0x14: SDRAM2_CLK pad
        //     fed through it received no clock); every working bitstream uses 0004.
        if (t == CycloneV::XCLKB2B && y == 0)
            return z == 4;
        //  2. DCMUX: ground truth (qrall, qireg_on) always enters via the TD->TDMUX leg
        //     (TCLK->DCMUX attested only at (89,6)). Our TCLK leg happens to clock the
        //     OUTPUT registers but is FALSE for CLKIN: the read-FIFO write clock never
        //     arrived and DATAIN[3] degenerated to an OUTREG loopback -- the registered-
        //     input acceptance passed spuriously (its dq_out<=expect default masked it)
        //     until the svga_m0 controller exposed it.
        if (t == CycloneV::DCMUX) {
            auto st = CycloneV::rn2t(pip.src);
            if (st == CycloneV::TCLK)
                return CycloneV::rn2x(w.node) == 89 && y == 6;
            return true;
        }
        return true;
    }

    // The one pip PROVEN false on silicon (three dead builds routed SDRAM2_CLK through it,
    // chip received no clock; no working bitstream -- vendor or open -- has ever used it).
    bool ioreg_leg_poison(PipId pip) const
    {
        WireId w = ctx->getPipDstWire(pip);
        if (w.is_nextpnr_created())
            return false;
        return CycloneV::rn2t(w.node) == CycloneV::XCLKB2B && CycloneV::rn2y(w.node) == 0 &&
               CycloneV::rn2z(w.node) == 8;
    }

    enum class RoutePhase { PAD_DATA, REST };

    void route_clk_net(NetInfo *net, RoutePhase rp)
    {
        for (auto usr : net->users.enumerate()) {
            // Phase PAD_DATA routes only clocks driven out a pad (port I on an IO cell):
            // few sinks, and each needs one exact attested leg (e.g. SDRAM2_CLK's
            // XCLKB2B.77.0.4 -> TD.77.0.33 -> GOUT). Register-clock fanout is routed in
            // REST so it cannot steal those wires first.
            bool pad_data = ctx->is_io_cell(usr.value.cell->type) && usr.value.port == id_I;
            if ((rp == RoutePhase::PAD_DATA) != pad_data)
                continue;
            // Any pad-bound sink of a clock net rides the IO-edge spines: the register
            // clocks (ICLK/OCLK -> DCMUX) and equally a clock DRIVEN OUT A PAD (port I,
            // e.g. an SDRAM clock pin). The false-pip trap caught svga_m0 on the latter:
            // SDRAM2_CLK's data feed routed XCLKB2B.77.0.8 -> TD.77.0.11 and the SDRAM
            // chip received no clock at all.
            bool pad_sink = ctx->is_io_cell(usr.value.cell->type);
            if (pad_sink) {
                // Prefer the silicon-attested spine legs (ioreg_leg_ok). The attested set
                // is incomplete by construction (diffs against a baseline hide shared
                // arcs), so when it cannot reach the sink, fall back to permissive
                // routing minus the proven-poison pip(s).
                if (backwards_bfs_route(net, usr.index, 1000000, false, [&](PipId pip) {
                        return ioreg_leg_ok(pip) && global_pip_filter(pip);
                    }))
                    continue;
                // Loud: a fallback route may land on a leg that was never proven on
                // silicon (the TCLK->DCMUX CLKIN leg was exactly such a silent trap).
                log_warning("pad clock sink %s.%s not reachable via attested legs; "
                            "falling back to permissive routing\n",
                            ctx->nameOf(usr.value.cell), ctx->nameOf(usr.value.port));
                backwards_bfs_route(net, usr.index, 1000000, true, [&](PipId pip) {
                    return !ioreg_leg_poison(pip) && global_pip_filter(pip);
                });
                continue;
            }
            backwards_bfs_route(net, usr.index, 1000000, true,
                                [&](PipId pip) { return (is_relaxed_sink(usr.value) || global_pip_filter(pip)); });
        }
        if (rp == RoutePhase::REST)
            log_info("    routed net '%s' using global resources\n", ctx->nameOf(net));
        if (getenv("VUP_DEBUG_HPSCLK")) {
            for (auto &u : net->users)
                if (std::string(u.port.c_str(ctx)) == "clk")
                    log_info("      [hpsclk] net '%s' drives a 'clk' pin on cell '%s' (%s)\n",
                             ctx->nameOf(net), ctx->nameOf(u.cell), u.cell->type.c_str(ctx));
        }
    }

    void operator()()
    {
        log_info("Routing globals...\n");
        // Two passes: pad-data clock sinks first (each needs one exact attested leg),
        // then the wide register-clock/fabric fanout.
        for (auto phase : {RoutePhase::PAD_DATA, RoutePhase::REST})
        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            CellInfo *drv = ni->driver.cell;
            if (drv == nullptr)
                continue;
            if (drv->type.in(id_MISTRAL_CLKENA, id_MISTRAL_CLKBUF, id_MISTRAL_PLLCLK)) {
                if (getenv("VUP_DEBUG_GLOBALS")) {
                    WireId sw = ctx->getNetinfoSourceWire(ni);
                    NetInfo *owner = (sw == WireId()) ? nullptr : ctx->getBoundWireNet(sw);
                    log_info("  [glb] net '%s' drv '%s' (%s) src wire %s%s\n", ctx->nameOf(ni), ctx->nameOf(drv),
                             drv->type.c_str(ctx), sw == WireId() ? "<none>" : ctx->nameOfWire(sw),
                             (owner != nullptr && owner != ni)
                                     ? (std::string("  *** ALREADY OWNED BY '") + ctx->nameOf(owner) + "' ***").c_str()
                                     : "");
                }
                route_clk_net(ni, phase);
                continue;
            }
        }
    }
};

void Arch::route_globals()
{
    MistralGlobalRouter router(getCtx());
    router();
}

NEXTPNR_NAMESPACE_END
