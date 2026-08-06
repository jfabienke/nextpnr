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

#include <cmath>
#include <cstdlib>
#include <string>

#include "log.h"
#include "nextpnr.h"
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
struct MistralBitgen
{
    MistralBitgen(Context *ctx) : ctx(ctx), cv(ctx->cyclonev) {};
    Context *ctx;
    CycloneV *cv;

    using rnode_t = CycloneV::rnode_t;
    using pnode_t = CycloneV::pnode_t;
    using pos_t = CycloneV::pos_t;
    using block_type_t = CycloneV::block_type_t;
    using port_type_t = CycloneV::port_type_t;

    rnode_t find_rnode(block_type_t bt, pos_t pos, port_type_t port, int bi = -1, int pi = -1) const
    {
        auto pn1 = CycloneV::pnode(bt, pos, port, bi, pi);
        auto rn1 = cv->pnode_to_rnode(pn1);
        if (rn1)
            return rn1;

        if (bt == CycloneV::GPIO) {
            auto pn2 = cv->p2p_to(pn1);
            if (!pn2) {
                auto pnv = cv->p2p_from(pn1);
                if (!pnv.empty())
                    pn2 = pnv[0];
            }
            auto pn3 = cv->hmc_get_bypass(pn2);
            auto rn2 = cv->pnode_to_rnode(pn3);
            return rn2;
        }

        return 0;
    }

    void options()
    {
        // Cyclone V bitstreams may be emitted compressed or uncompressed.
        // We default to COMPRESSED. An uncompressed bitstream still
        // configures the device (the FPGA manager reports "operating"),
        // but on the socfpga FPGA-manager load path (verified on a
        // DE10-Nano / MiSTer) it leaves user IO non-functional; the
        // compressed encoding matches Quartus output and works on
        // silicon. OPT_B carries the compression-format flag in the
        // header and must track the chosen encoding.
        if (ctx->setting<bool>("uncompressed_rbf", false)) {
            cv->opt_b_set(CycloneV::COMPRESSION_DIS, true);
            cv->opt_r_set(CycloneV::OPT_B, 0xffffff40adffffffULL);
        } else
            cv->opt_r_set(CycloneV::OPT_B, 0xffffff402dffffffULL);
    }

    void write_routing()
    {
        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            for (auto &wire : ni->wires) {
                PipId pip = wire.second.pip;
                if (pip == PipId())
                    continue;
                WireId src = ctx->getPipSrcWire(pip), dst = ctx->getPipDstWire(pip);
                // Only write out routes that are entirely in the Mistral domain. Everything else is dealt with
                // specially
                if (src.is_nextpnr_created() || dst.is_nextpnr_created())
                    continue;
                cv->rnode_link(src.node, dst.node);
            }
        }
    }

    void write_io_cell(CellInfo *ci, int x, int y, int bi)
    {
        bool is_output = (ci->type == id_MISTRAL_OB || (ci->type == id_MISTRAL_IO && ci->getPort(id_OE) != nullptr));
        auto pos = CycloneV::xy2pos(x, y);
        // TODO: configurable pull, IO standard, etc
        cv->bmux_b_set(CycloneV::GPIO, pos, CycloneV::USE_WEAK_PULLUP, bi, false);
        if (is_output) {
            cv->bmux_m_set(CycloneV::GPIO, pos, CycloneV::DRIVE_STRENGTH, bi, CycloneV::V3P3_LVTTL_16MA_LVCMOS_2MA);
            cv->bmux_m_set(CycloneV::GPIO, pos, CycloneV::IOCSR_STD, bi, CycloneV::DIS);

            // Output gpios must also bypass things in the associated dqs
            auto dqs = cv->p2p_to(CycloneV::pnode(CycloneV::GPIO, pos, CycloneV::PNONE, bi, -1));
            if (dqs) {
                cv->bmux_m_set(CycloneV::DQS16, CycloneV::pn2p(dqs), CycloneV::INPUT_REG4_SEL, CycloneV::pn2bi(dqs),
                               CycloneV::SEL_LOCKED_DPA);
                cv->bmux_r_set(CycloneV::DQS16, CycloneV::pn2p(dqs), CycloneV::RB_T9_SEL_EREG_CFF_DELAY,
                               CycloneV::pn2bi(dqs), 0x1f);
            }
        }
        // There seem to be two mirrored OEIN inversion bits for constant OE for inputs/outputs. This might be to
        // prevent a single bitflip from turning inputs to outputs and messing up other devices on the boards, notably
        // ECP5 does similar. OEIN.0 inverted for outputs; OEIN.1 for inputs
        cv->inv_set(find_rnode(CycloneV::GPIO, pos, CycloneV::OEIN, bi, 0), is_output);
        cv->inv_set(find_rnode(CycloneV::GPIO, pos, CycloneV::OEIN, bi, 1), !is_output);
    }

    void write_clkbuf_cell(CellInfo *ci, int x, int y, int bi)
    {
        (void)ci; // currently unused
        auto pos = CycloneV::xy2pos(x, y);
        // Default: general routing (entry 0x1b = {CLKIN,2}). Ground truth instead sources the clock
        // network from DEDICATED CLOCK PINS - INPUT_SEL -> {CLKPIN_SEL_x, inst} plus the matching
        // CLKPIN_SEL_x bmux picking the pin. General routing demonstrably drives FABRIC loads
        // (silicon-proven), but is the prime suspect for failing to feed a PLL reference.
        // VUP_CLKBUF_SEL / VUP_CLKPIN_SEL_IDX / VUP_CLKPIN_SEL_VAL drive the silicon sweep.
        uint32_t sel = 0x1b;
        if (const char *e = getenv("VUP_CLKBUF_SEL"))
            sel = uint32_t(strtoul(e, nullptr, 0));
        cv->bmux_r_set(CycloneV::CMUXHG, pos, CycloneV::INPUT_SEL, bi, sel);
        if (const char *ei = getenv("VUP_CLKPIN_SEL_IDX")) {
            static const CycloneV::bmux_type_t sel_mux[4] = {CycloneV::CLKPIN_SEL_0, CycloneV::CLKPIN_SEL_1,
                                                             CycloneV::CLKPIN_SEL_2, CycloneV::CLKPIN_SEL_3};
            int idx = int(strtoul(ei, nullptr, 0)) & 3;
            uint32_t val = 0x1;
            if (const char *ev = getenv("VUP_CLKPIN_SEL_VAL"))
                val = uint32_t(strtoul(ev, nullptr, 0));
            cv->bmux_r_set(CycloneV::CMUXHG, pos, sel_mux[idx], bi, val);
            log_info("CLKBUF (%d,%d)[%d]: INPUT_SEL=0x%x, CLKPIN_SEL_%d=0x%x\n", x, y, bi, sel, idx, val);
        }
        cv->bmux_m_set(CycloneV::CMUXHG, pos, CycloneV::TESTSYN_ENOUT_SELECT, bi, CycloneV::PRE_SYNENB);
    }

    // G4 (PLL_OUTCLK_DESIGN.md): a MISTRAL_PLLCLK bound at gclk instance `bi` of a global cmux
    // injects its source PLL counter via the dedicated wiring — pure configuration: program this
    // instance's INPUT_SEL to the map-derived entry that selects {PLLIN, k} for (pll pos, counter).
    // v1 emits the DIRECT path; whether the CLK_SELECT switchover layer is additionally required is
    // the design's open question, arbitrated on silicon (V3). PLL feedback stays in the FPLL config
    // (write_fpll_cell); the cmux PLL_FEEDBACK_* path (external feedback) is deliberately untouched.
    void write_pllclk_cell(CellInfo *ci, int x, int y, int bi)
    {
        auto pos = CycloneV::xy2pos(x, y);
        auto pll_it = ctx->cells.find(ctx->id(ci->attrs.at(id_PLLCLK_PLL).as_string()));
        if (pll_it == ctx->cells.end() || pll_it->second->bel == BelId())
            log_error("PLLCLK '%s': source PLL missing or unplaced at bitstream time\n", ctx->nameOf(ci));
        Loc pl = ctx->getBelLocation(pll_it->second->bel);
        int counter = int(ci->attrs.at(id_PLLCLK_COUNTER).as_int64());
        int sel = ctx->pllclk_lookup(uint32_t(CycloneV::xy2pos(pl.x, pl.y)), counter, uint32_t(pos), bi);
        if (sel < 0)
            log_error("PLLCLK '%s': no dedicated wiring from FPLL(%d,%d) C%d to cmux (%d,%d) gclk %d — "
                      "placement validity should have prevented this\n",
                      ctx->nameOf(ci), pl.x, pl.y, counter, x, y, bi);
        auto bt = ctx->pllclk_pos_is_vertical(uint32_t(pos)) ? CycloneV::CMUXVG : CycloneV::CMUXHG;
        cv->bmux_r_set(bt, pos, CycloneV::INPUT_SEL, bi, uint32_t(sel));
        cv->bmux_m_set(bt, pos, CycloneV::TESTSYN_ENOUT_SELECT, bi, CycloneV::PRE_SYNENB);
        log_info("PLLCLK '%s': cmux(%d,%d)%s gclk %d INPUT_SEL=0x%x selects FPLL(%d,%d) C%d\n", ctx->nameOf(ci), x,
                 y, bt == CycloneV::CMUXVG ? "VG" : "HG", bi, sel, pl.x, pl.y, counter);
    }

    void write_m10k_cell(CellInfo *ci, int x, int y, int bi)
    {
        auto pos = CycloneV::xy2pos(x, y);

        // Notes:
        // DATA_FLOW_THRU is probably transparent reads.

        auto dbits = ci->params.at(id_CFG_DBITS).as_int64();

        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::A_DATA_FLOW_THRU, bi, 1);
        cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::A_DATA_WIDTH, bi, dbits);
        cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::A_FAST_WRITE, bi, dbits == 40 ? CycloneV::SLOW : CycloneV::FAST);
        cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::A_OUTPUT_SEL, bi, CycloneV::ASYNC);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::A_SA_WREN_DELAY, bi, 1);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::A_SAEN_DELAY, bi, 2);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::A_WL_DELAY, bi, 2);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::A_WR_TIMER_PULSE, bi, 0x0b);

        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::B_DATA_FLOW_THRU, bi, 1);
        cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::B_DATA_WIDTH, bi, dbits);
        cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::B_FAST_WRITE, bi, dbits == 40 ? CycloneV::SLOW : CycloneV::FAST);
        cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::B_OUTPUT_SEL, bi, CycloneV::ASYNC);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::B_SA_WREN_DELAY, bi, 1);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::B_SAEN_DELAY, bi, 2);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::B_WL_DELAY, bi, 2);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::B_WR_TIMER_PULSE, bi, 0x0b);

        cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_CLK_SEL, bi, 1);
        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::TOP_W_INV, bi, dbits != 40);
        cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_W_SEL, bi, dbits != 40);
        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::BOT_CLK_INV, bi, dbits != 40);
        cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_W_SEL, bi, dbits != 40);

        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::TRUE_DUAL_PORT, bi, 0);

        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::DISABLE_UNUSED, bi, 0);

        auto permute_init = [](int64_t init) -> int64_t {
            const int permutation[40] = {0, 20, 10, 30, 1, 21, 11, 31, 2, 22, 12, 32, 3, 23, 13, 33, 4, 24, 14, 34,
                                         5, 25, 15, 35, 6, 26, 16, 36, 7, 27, 17, 37, 8, 28, 18, 38, 9, 29, 19, 39};

            int64_t output = 0;
            for (int bit = 0; bit < 40; bit++)
                output |= ((init >> permutation[bit]) & 1) << bit;
            return ~output; // RAM init is inverted.
        };

        Property init;
        if (ci->params.count(id_INIT) == 0) {
            init = Property{0, 10240};
        } else {
            init = ci->params.at(id_INIT);
        }
        for (int bi = 0; bi < 256; bi++)
            cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::RAM, bi, permute_init(init.extract(bi * 40, 40).as_int64()));
    }

    // Parse a frequency string like "50.0 MHz" / "100.0 MHz" into MHz.
    // A period string ("0 ps") or an unrecognised unit yields 0 (== unused).
    static double parse_freq_mhz(const std::string &s)
    {
        try {
            size_t idx = 0;
            double val = std::stod(s, &idx);
            std::string unit = s.substr(idx);
            while (!unit.empty() && (unit.front() == ' ' || unit.front() == '\t'))
                unit.erase(unit.begin());
            if (unit.rfind("GHz", 0) == 0)
                return val * 1000.0;
            if (unit.rfind("MHz", 0) == 0)
                return val;
            if (unit.rfind("kHz", 0) == 0 || unit.rfind("KHz", 0) == 0)
                return val / 1000.0;
            if (unit.rfind("Hz", 0) == 0)
                return val / 1.0e6;
            // ps/ns are periods, not frequencies -> treat as "unset"
            return 0.0;
        } catch (...) {
            return 0.0;
        }
    }

    void write_fpll_cell(CellInfo *ci, int x, int y, int bi)
    {
        (void)bi; // FPLL has a single instance per tile
        auto pos = CycloneV::xy2pos(x, y);

        // ---- parse the altera_pll parameters (fall back to fabi386 defaults) ----
        double f_ref = 50.0;
        if (ci->params.count(id_reference_clock_frequency)) {
            double f = parse_freq_mhz(ci->params.at(id_reference_clock_frequency).as_string());
            if (f > 0)
                f_ref = f;
        }
        int nclk = ci->params.count(id_number_of_clocks) ? int(ci->params.at(id_number_of_clocks).as_int64()) : 1;
        if (nclk < 1)
            nclk = 1;
        if (nclk > 9)
            nclk = 9; // FPLL has 9 C-counters (C0..C8)

        std::vector<double> fout;
        for (int i = 0; i < nclk; i++) {
            double f = 0;
            IdString p = ctx->idf("output_clock_frequency%d", i);
            if (ci->params.count(p))
                f = parse_freq_mhz(ci->params.at(p).as_string());
            if (f <= 0)
                f = f_ref; // reasonable fall-back for an unspecified output
            fout.push_back(f);
        }

        // ---- freq -> N/M/C solver ----
        // VCO = f_ref * M / N, kept in [600, 1600] MHz. N is bypassed (N=1).
        // For each output C = VCO / f_out must be an (ideally even) integer.
        // Among valid VCOs we prefer even C's (50% duty from HI==LO) and the
        // one closest to a 1000 MHz mid-band target. For the fabi386 case
        // (50 MHz -> 10/25/100 MHz) this yields M=20, VCO=1000, C={100,40,10}.
        // ---------------------------------------------------------------------------------
        // FPLL recipe. RESEARCH (2026-08-06, fplldump over the fitted fabi386's three active PLLs):
        // the ground truth uses TWO COHERENT FAMILIES, and fields must not be mixed across them:
        //
        //   FRACTIONAL (PLL(0,0), PLL(0,55)):  N bypassed + N div 0, M=8, DSM_OUT_SEL=1,
        //     FRACTIONAL_DIVISION_SETTING=<design>, FBCLK_MUX_2=1, VCO_DIV=0, BWCTRL=0x07,
        //     SLF_RST=0x03, no CP_CURRENT.
        //   INTEGER (PLL(89,0)):  N=6 (3+3, NOT bypassed), M=148, no DSM/FBCLK_MUX_2/VCO_DIV/SLF_RST,
        //     frac=1, BWCTRL=0x03, CP_CURRENT=0x01, NREVERT_INVERT=1.
        //   Invariant in BOTH: CTRL_OVERRIDE_SETTING=0, CNT_IN_SRC=0, TCLK_SEL=0,
        //     LOCK_FILTER=0x19, UNLOCK_FILTER=0x02, CLKIN_0_SRC=0x04.
        //
        // Silicon rounds 1-6 each emitted a HYBRID and none locked; notably CP_CURRENT (the charge
        // pump drive, without which the loop cannot be pulled to lock) was lost when the fractional
        // recipe replaced the integer one. v1 therefore emits the INTEGER family verbatim - it is
        // the family of the tile the placer actually lands in - and derives only the C dividers.
        // VUP_PLL_GT_CLONE=1 additionally pins the C dividers to the ground truth's own values, so
        // the emitted tile is byte-identical to a KNOWN-WORKING PLL and the only remaining variable
        // is our cmux/PLLCLK emission (the decisive experiment).
        // ATTESTED ENVELOPE (bmuxhist over 20 shipped MiSTer cores, all three PLL positions): every
        // ground-truth PLL runs its VCO at ~400-500 MHz, never near the 1233 MHz the invented recipe
        // asked for. Apogee's FPLL(89,0) -- our own position, integer family -- is N=5 (3+2), M=48
        // (0x18+0x18), PFD 10 MHz, VCO 480 MHz. Track that: pick N for a 10 MHz PFD, then M for a VCO
        // inside the attested band.
        const int N = 5, M = 48;                    // PFD = f_ref/5 = 10 MHz
        double vco = f_ref * double(M) / double(N); // 480 MHz -- inside the attested band
        int m_hi = M / 2, m_lo = M - m_hi;
        int n_hi = (N + 1) / 2, n_lo = N - n_hi;    // 3 + 2, as ground truth encodes an odd N
        bool gt_clone = getenv("VUP_PLL_GT_CLONE") != nullptr;

        log_info("FPLL '%s': f_ref=%.3f MHz, N=%d, M=%d, VCO=%.3f MHz (INTEGER family%s)\n",
                 ctx->nameOf(ci), f_ref, N, M, vco, gt_clone ? ", GT-clone C dividers" : "");

        // ---- program the block ----
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::N_CNT_HI_DIV_SETTING, 0, n_hi);
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::N_CNT_LO_DIV_SETTING, 0, n_lo);
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::M_CNT_HI_DIV_SETTING, 0, m_hi);
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::M_CNT_LO_DIV_SETTING, 0, m_lo);

        static const CycloneV::bmux_type_t cout_en[9] = {
                CycloneV::C0_COUT_EN, CycloneV::C1_COUT_EN, CycloneV::C2_COUT_EN,
                CycloneV::C3_COUT_EN, CycloneV::C4_COUT_EN, CycloneV::C5_COUT_EN,
                CycloneV::C6_COUT_EN, CycloneV::C7_COUT_EN, CycloneV::C8_COUT_EN};

        for (int c = 0; c < int(fout.size()) && c < 9; c++) {
            int C = int(std::round(vco / fout[c]));
            if (C < 1)
                C = 1;
            int c_hi = C / 2;
            int c_lo = C - c_hi;
            if (gt_clone) {
                // Apogee's own C5 divide (3+2 = 5 -> 96 MHz off the 480 MHz VCO); makes the tile
                // byte-identical to an integer-family PLL known to run at THIS position on THIS
                // silicon. The design's clock is then ground truth's, not the requested one -
                // reported honestly below.
                c_hi = 0x03;
                c_lo = 0x02;
                log_info("  GT-clone: C%d divider pinned to %d+%d -> %.3f MHz (NOT the requested "
                         "%.3f MHz)\n",
                         c, c_hi, c_lo, vco / double(c_hi + c_lo), fout[c]);
            }
            // Physical counter for logical clock c: pack may remap (G4) because only C4..C8 have
            // dedicated wiring to the GLOBAL cmuxes (p2p-verified); C0..C3 reach only regionals.
            int phys = c;
            IdString pa = ctx->idf("PLLCLK_PHYS_%d", c);
            if (ci->attrs.count(pa))
                phys = int(ci->attrs.at(pa).as_int64());
            // Per-counter output divider (midx = counter index 0..8).
            cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::DPRIO0_CNT_HI_DIV, phys, c_hi);
            cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::DPRIO0_CNT_LO_DIV, phys, c_lo);
            // Odd divides need the odd/even-duty enable bit. Keyed off the EMITTED halves, not C,
            // so the gt_clone override above still gets the bit right.
            if ((c_hi + c_lo) % 2)
                cv->bmux_b_set(CycloneV::FPLL, pos, CycloneV::DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN, phys, true);
            // Counter input source: VCO phase 0 (ground truth sets CNT_IN_SRC=0 for active counters).
            cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::CNT_IN_SRC, phys, 0);
            // Enable this counter's clock output.
            cv->bmux_b_set(CycloneV::FPLL, pos, cout_en[phys], 0, true);
        }

        // Integer family, as ATTESTED by Apogee's FPLL(89,0) rather than derived: DSM off (the frac
        // field carries the idle value 1), and BWCTRL / CP_CURRENT left at their DEFAULTS -- ground
        // truth overrides neither, so the earlier "CP_CURRENT is REQUIRED, the charge pump drives
        // the loop" note was a guess that the corpus contradicts.
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::FRACTIONAL_DIVISION_SETTING, 0, 1);
        cv->bmux_b_set(CycloneV::FPLL, pos, CycloneV::NREVERT_INVERT, 0, true);
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::VCO_DIV, 0, 0x00);
        // M-counter preset/phase-mux preset. Only integer-family ground truth sets these, and the
        // corpus is too small to fit a formula (LO_PRESET in {4,5,6}, PH_MUX_PRESET in {3,5,6}), so
        // take Apogee's pair verbatim - it is the sample that shares our position and our family.
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::M_CNT_LO_PRESET_SETTING, 0, 0x05);
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::M_CNT_PH_MUX_PRESET_SETTING, 0, 0x06);
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::LOCK_FILTER_CFG_SETTING, 0, 0x19);
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::UNLOCK_FILTER_CFG_SETTING, 0, 0x02);
        uint32_t clkin_src = 0x04;
        if (const char *e = getenv("VUP_PLL_CLKIN_SRC"))
            clkin_src = uint32_t(strtoul(e, nullptr, 0));
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::CLKIN_0_SRC, 0, clkin_src);
        // Universal ground-truth invariants previously missing entirely.
        // CTRL_OVERRIDE_SETTING is MT_BOOL (type 2 == MT_BOOL in {MUX,NUM,BOOL,RAM}), and its
        // DEFAULT IS 1 -- verified by dumping our own rbf with fplldump --all. Every Quartus core
        // sampled (20/20, all three PLL positions) explicitly clears it to 0. We shipped the
        // default: an escaped "\n" inside the preceding comment had swallowed the emit line
        // entirely, and it used the wrong setter (bmux_r_set/bmux_n_set no-op on a BOOL) besides.
        cv->bmux_b_set(CycloneV::FPLL, pos, CycloneV::CTRL_OVERRIDE_SETTING, 0, false);

        // FEEDBACK. There are two coherent modes and the corpus mixes them, which misled round 8:
        // the shipped cores set FBCLK_MUX_2=1 with no cmux feedback, but they are fitted in a
        // different PLL operation mode. The MINIMAL QUARTUS REFERENCE -- same device, same pin, same
        // 'direct' instantiation as ours, fit report "PLL Operation Mode: Normal" -- does the
        // opposite: PLL_FEEDBACK_ENABLE_0 = PLL_MCNT0 at the GCLK-root CMUXVG(42,0), and NO
        // FBCLK_MUX_2. Match the reference that matches our use case.
        //
        // The index is the cmux GCLK INSTANCE carrying the feedback, not a constant: the reference
        // uses _0 alongside its gclk-0 selection, while this code used to hardcode _3.
        static const CycloneV::bmux_type_t pll_fb_en[4] = {
                CycloneV::PLL_FEEDBACK_ENABLE_0, CycloneV::PLL_FEEDBACK_ENABLE_1,
                CycloneV::PLL_FEEDBACK_ENABLE_2, CycloneV::PLL_FEEDBACK_ENABLE_3};
        int fb_gclk = 0; // the reference's value; VUP_PLL_FB_GCLK makes it sweepable
        if (const char *e = getenv("VUP_PLL_FB_GCLK"))
            fb_gclk = int(strtoul(e, nullptr, 0)) & 3;
        cv->bmux_m_set(CycloneV::CMUXVG, CycloneV::xy2pos(42, 0), pll_fb_en[fb_gclk], 0,
                       CycloneV::PLL_MCNT0);
        cv->bmux_r_set(CycloneV::FPLL, pos, CycloneV::TCLK_SEL, 0, 0);

        // REFERENCE-CLOCK ENABLE. The positive control (a Quartus build of THIS telemetry design,
        // measured at PLL=96.469 MHz / LOCKED=1) brings the clock pin onto the network with
        //     CMUXVG(42,0) INPUT_SEL[0] = 0x00 -> {CLKPIN, 1}
        // whereas nextpnr's write_clkbuf_cell hardcodes {CLKIN,2} = 0x1b (general routing) on
        // CMUXHG. The earlier CLKPIN sweep tested this idea on the wrong mux -- CMUXHG(0,35)/(89,35),
        // never CMUXVG(42,0). Working theory: the dedicated clock-pin buffer is enabled by BEING
        // SELECTED through a {CLKPIN,n} entry, so with only a general-routing selection the pin's
        // clock buffer never turns on and the PLL's dedicated reference is dead -- which is exactly
        // the measured symptom, and why the pin's pad CRAM is identical in working and dead builds.
        if (const char *e = getenv("VUP_CLKPIN_GCLK")) {
            int g = int(strtoul(e, nullptr, 0)) & 3;
            uint32_t sel = 0x00;
            if (const char *v = getenv("VUP_CLKPIN_ENTRY"))
                sel = uint32_t(strtoul(v, nullptr, 0));
            cv->bmux_r_set(CycloneV::CMUXVG, CycloneV::xy2pos(42, 0), CycloneV::INPUT_SEL, g, sel);
            log_info("  CLKPIN enable: CMUXVG(42,0) gclk %d INPUT_SEL=0x%02x\n", g, sel);
        }

        // Enables.
        cv->bmux_b_set(CycloneV::FPLL, pos, CycloneV::FPLL_ENABLE, 0, true);
        cv->bmux_b_set(CycloneV::FPLL, pos, CycloneV::VCO0PH_EN, 0, true);
        static const CycloneV::bmux_type_t vco_ph_en[8] = {
                CycloneV::VCO_PH0_EN, CycloneV::VCO_PH1_EN, CycloneV::VCO_PH2_EN, CycloneV::VCO_PH3_EN,
                CycloneV::VCO_PH4_EN, CycloneV::VCO_PH5_EN, CycloneV::VCO_PH6_EN, CycloneV::VCO_PH7_EN};
        for (int i = 0; i < 8; i++)
            cv->bmux_b_set(CycloneV::FPLL, pos, vco_ph_en[i], 0, true);
    }

    void write_cells()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            Loc loc = ctx->getBelLocation(ci->bel);
            int bi = ctx->bel_data(ci->bel).block_index;
            if (ctx->is_io_cell(ci->type))
                write_io_cell(ci, loc.x, loc.y, bi);
            else if (ctx->is_clkbuf_cell(ci->type))
                write_clkbuf_cell(ci, loc.x, loc.y, bi);
            else if (ci->type == id_MISTRAL_M10K)
                write_m10k_cell(ci, loc.x, loc.y, bi);
            else if (ctx->is_pll_cell(ci->type))
                write_fpll_cell(ci, loc.x, loc.y, bi);
            else if (ci->type == id_MISTRAL_PLLCLK)
                write_pllclk_cell(ci, loc.x, loc.y, bi);
        }
    }

    bool write_alm(uint32_t lab, uint8_t alm)
    {
        auto &alm_data = ctx->labs.at(lab).alms.at(alm);
        auto block_type = ctx->labs.at(lab).is_mlab ? CycloneV::MLAB : CycloneV::LAB;

        std::array<CellInfo *, 2> luts{ctx->getBoundBelCell(alm_data.lut_bels[0]),
                                       ctx->getBoundBelCell(alm_data.lut_bels[1])};
        std::array<CellInfo *, 4> ffs{
                ctx->getBoundBelCell(alm_data.ff_bels[0]), ctx->getBoundBelCell(alm_data.ff_bels[1]),
                ctx->getBoundBelCell(alm_data.ff_bels[2]), ctx->getBoundBelCell(alm_data.ff_bels[3])};
        // Skip empty ALMs
        if (std::all_of(luts.begin(), luts.end(), [](CellInfo *c) { return !c; }) &&
            std::all_of(ffs.begin(), ffs.end(), [](CellInfo *c) { return !c; }))
            return false;

        bool is_lutram =
                (luts[0] && luts[0]->combInfo.mlab_group != -1) || (luts[1] && luts[1]->combInfo.mlab_group != -1);

        auto pos = alm_data.lut_bels[0].pos;
        if (is_lutram) {
            for (int i = 0; i < 10; i++) {
                // Many MLAB settings apply to the whole LAB, not just the ALM
                cv->bmux_m_set(block_type, pos, CycloneV::TMODE, i, CycloneV::RAM);
                cv->bmux_m_set(block_type, pos, CycloneV::BMODE, i, CycloneV::RAM);
                cv->bmux_n_set(block_type, pos, CycloneV::T_FEEDBACK_SEL, i, 1);
            }
            cv->bmux_r_set(block_type, pos, CycloneV::LUT_MASK, alm, 0xFFFFFFFFFFFFFFFFULL); // TODO: LUTRAM init
            cv->bmux_b_set(block_type, pos, CycloneV::BPKREG1, alm, true);
            cv->bmux_b_set(block_type, pos, CycloneV::TPKREG0, alm, true);
            cv->bmux_m_set(block_type, pos, CycloneV::MCRG_VOLTAGE, 0, CycloneV::VCCL);
            cv->bmux_b_set(block_type, pos, CycloneV::RAM_DIS, 0, false);
            cv->bmux_b_set(block_type, pos, CycloneV::WRITE_EN, 0, true);
            cv->bmux_n_set(block_type, pos, CycloneV::WRITE_PULSE_LENGTH, 0, 650); // picoseconds, presumably
            // TODO: understand how these enables really work
            cv->bmux_b_set(block_type, pos, CycloneV::EN2_EN, 0, false);
            cv->bmux_b_set(block_type, pos, CycloneV::SCLR_DIS, 0, true);
        } else {
            // Combinational mode - TODO: flop feedback and more modes...
            cv->bmux_m_set(block_type, pos, CycloneV::TMODE, alm, alm_data.l6_mode ? CycloneV::C_E : CycloneV::E_0);
            cv->bmux_m_set(block_type, pos, CycloneV::BMODE, alm, alm_data.l6_mode ? CycloneV::D_E : CycloneV::E_1);
            // LUT function
            cv->bmux_r_set(block_type, pos, CycloneV::LUT_MASK, alm, ctx->compute_lut_mask(lab, alm));
        }
        // DFF/LUT output selection
        const std::array<CycloneV::bmux_type_t, 6> mux_settings{CycloneV::TDFF0, CycloneV::TDFF1, CycloneV::TDFF1L,
                                                                CycloneV::BDFF0, CycloneV::BDFF1, CycloneV::BDFF1L};
        const std::array<CycloneV::port_type_t, 6> mux_port{CycloneV::FFT0, CycloneV::FFT1, CycloneV::FFT1L,
                                                            CycloneV::FFB0, CycloneV::FFB1, CycloneV::FFB1L};
        for (int i = 0; i < 6; i++) {
            if (ctx->wires_connected(alm_data.comb_out[i / 3], ctx->get_port(block_type, CycloneV::pos2x(pos),
                                                                             CycloneV::pos2y(pos), alm, mux_port[i])))
                cv->bmux_m_set(block_type, pos, mux_settings[i], alm, CycloneV::NLUT);
        }

        bool is_carry = (luts[0] && luts[0]->combInfo.is_carry) || (luts[1] && luts[1]->combInfo.is_carry);
        if (is_carry)
            cv->bmux_m_set(block_type, pos, CycloneV::ARITH_SEL, alm, CycloneV::ADDER);
        // The carry in/out enable bits
        if (is_carry && alm == 0 && !luts[0]->combInfo.carry_start)
            cv->bmux_b_set(block_type, pos, CycloneV::TTO_DIS, 0, true);
        if (is_carry && alm == 5)
            cv->bmux_b_set(block_type, pos, CycloneV::BTO_DIS, 0, true);
        // Flipflop configuration
        const std::array<CycloneV::bmux_type_t, 2> ef_sel{CycloneV::TEF_SEL, CycloneV::BEF_SEL};
        // This isn't a typo; the *PKREG* bits really are mirrored.
        const std::array<CycloneV::bmux_type_t, 4> pkreg{CycloneV::TPKREG1, CycloneV::TPKREG0, CycloneV::BPKREG1,
                                                         CycloneV::BPKREG0};

        const std::array<CycloneV::bmux_type_t, 2> clk_sel{CycloneV::TCLK_SEL, CycloneV::BCLK_SEL},
                clr_sel{CycloneV::TCLR_SEL, CycloneV::BCLR_SEL}, sclr_dis{CycloneV::TSCLR_DIS, CycloneV::BSCLR_DIS},
                sload_en{CycloneV::TSLOAD_EN, CycloneV::BSLOAD_EN};

        const std::array<CycloneV::bmux_type_t, 3> clk_choice{CycloneV::CLK0, CycloneV::CLK1, CycloneV::CLK2};

        const std::array<CycloneV::bmux_type_t, 3> clk_inv{CycloneV::CLK0_INV, CycloneV::CLK1_INV, CycloneV::CLK2_INV},
                en_en{CycloneV::EN0_EN, CycloneV::EN1_EN, CycloneV::EN2_EN},
                en_ninv{CycloneV::EN0_NINV, CycloneV::EN1_NINV, CycloneV::EN2_NINV};
        const std::array<CycloneV::bmux_type_t, 2> aclr_inv{CycloneV::ACLR0_INV, CycloneV::ACLR1_INV};

        for (int i = 0; i < 2; i++) {
            // EF selection mux
            if (ctx->wires_connected(ctx->getBelPinWire(alm_data.lut_bels[i], i ? id_F0 : id_F1), alm_data.sel_ef[i]))
                cv->bmux_m_set(block_type, pos, ef_sel[i], alm, CycloneV::bmux_type_t::F);
        }

        for (int i = 0; i < 4; i++) {
            CellInfo *ff = ffs[i];
            if (!ff)
                continue;
            // PKREG (input selection)
            if (ctx->wires_connected(alm_data.sel_ef[i / 2], alm_data.ff_in[i]))
                cv->bmux_b_set(block_type, pos, pkreg[i], alm, true);
            // Control set
            // CLK+ENA
            int ce_idx = alm_data.clk_ena_idx[i / 2];
            cv->bmux_m_set(block_type, pos, clk_sel[i / 2], alm, clk_choice[ce_idx]);
            if (ff->ffInfo.ctrlset.clk.inverted)
                cv->bmux_b_set(block_type, pos, clk_inv[ce_idx], 0, true);
            if (ff->getPort(id_ENA) != nullptr) { // not using ffInfo.ctrlset, this has a fake net always to
                                                  // ensure different constants don't collide
                cv->bmux_b_set(block_type, pos, en_en[ce_idx], 0, true);
                cv->bmux_b_set(block_type, pos, en_ninv[ce_idx], 0, ff->ffInfo.ctrlset.ena.inverted);
            } else {
                cv->bmux_b_set(block_type, pos, en_en[ce_idx], 0, false);
            }
            // ACLR
            int aclr_idx = alm_data.aclr_idx[i / 2];
            cv->bmux_b_set(block_type, pos, clr_sel[i / 2], alm, aclr_idx == 1);
            if (ff->ffInfo.ctrlset.aclr.inverted)
                cv->bmux_b_set(block_type, pos, aclr_inv[aclr_idx], 0, true);
            // SCLR
            if (ff->ffInfo.ctrlset.sclr.net != nullptr) {
                cv->bmux_b_set(block_type, pos, CycloneV::SCLR_INV, 0, ff->ffInfo.ctrlset.sclr.inverted);
                cv->bmux_b_set(block_type, pos, CycloneV::SCLR_DIS, 0, false);
            } else {
                cv->bmux_b_set(block_type, pos, sclr_dis[i / 2], alm, true);
            }
            // SLOAD
            if (ff->ffInfo.ctrlset.sload.net != nullptr) {
                cv->bmux_b_set(block_type, pos, sload_en[i / 2], alm, true);
                if (ff->ffInfo.ctrlset.sload.net->name == ctx->id("$PACKER_GND_NET")) {
                    // force-disabled LOAD (see workaround in assign_ff_info)
                    cv->bmux_b_set(block_type, pos, CycloneV::SLOAD_EN, 0, false);
                }
                cv->bmux_b_set(block_type, pos, CycloneV::SLOAD_INV, 0, ff->ffInfo.ctrlset.sload.inverted);
            }
        }
        if (is_lutram) {
            for (int i = 0; i < 2; i++) {
                CellInfo *lut = luts[i];
                if (!lut || lut->combInfo.mlab_group == -1)
                    continue;
                int ce_idx = alm_data.clk_ena_idx[1];
                cv->bmux_m_set(block_type, pos, clk_sel[1], alm, clk_choice[ce_idx]);
                if (lut->combInfo.wclk.inverted)
                    cv->bmux_b_set(block_type, pos, clk_inv[ce_idx], 0, true);
                if (lut->getPort(id_A1EN) != nullptr) {
                    cv->bmux_b_set(block_type, pos, en_en[ce_idx], 0, true);
                    cv->bmux_b_set(block_type, pos, en_ninv[ce_idx], 0, lut->combInfo.we.inverted);
                } else {
                    cv->bmux_b_set(block_type, pos, en_en[ce_idx], 0, false);
                }
                // TODO: understand what these are doing
                cv->bmux_b_set(block_type, pos, sclr_dis[0], alm, true);
                cv->bmux_b_set(block_type, pos, sclr_dis[1], alm, true);
            }
        }
        return true;
    }

    void write_ff_routing(uint32_t lab)
    {
        auto &lab_data = ctx->labs.at(lab);
        auto pos = lab_data.alms.at(0).lut_bels[0].pos;
        auto block_type = ctx->labs.at(lab).is_mlab ? CycloneV::MLAB : CycloneV::LAB;

        const std::array<CycloneV::bmux_type_t, 2> aclr_inp{CycloneV::ACLR0_SEL, CycloneV::ACLR1_SEL};
        for (int i = 0; i < 2; i++) {
            // Quartus seems to set unused ACLRs to ACLR0
            if (lab_data.aclr_used[i])
                cv->bmux_m_set(block_type, pos, aclr_inp[i], 0, (i == 1) ? CycloneV::DIN2 : CycloneV::DIN3);
            else if (i == 0)
                cv->bmux_m_set(block_type, pos, aclr_inp[i], 0, CycloneV::ACLR0);
        }
        for (int i = 0; i < 3; i++) {
            // Check for fabric->clock routing
            if (ctx->wires_connected(
                        ctx->get_port(block_type, CycloneV::pos2x(pos), CycloneV::pos2y(pos), -1, CycloneV::DATAIN, 0),
                        lab_data.clk_wires[i]))
                cv->bmux_m_set(block_type, pos, CycloneV::CLKA_SEL, 0, CycloneV::DIN0);
        }
    }

    void write_labs()
    {
        for (size_t lab = 0; lab < ctx->labs.size(); lab++) {
            bool used = false;
            for (uint8_t alm = 0; alm < 10; alm++)
                used |= write_alm(lab, alm);
            if (used)
                write_ff_routing(lab);
        }
    }

    void run()
    {
        cv->clear();
        options();
        write_routing();
        write_cells();
        write_labs();
        ctx->bitstream_configured = true;
    }
};
} // namespace

void Arch::build_bitstream()
{
    MistralBitgen gen(getCtx());
    gen.run();

    // This is a hack to run timing analysis yet again after the bitstream is
    // configured in Mistral, because the analogue simulator won't work until
    // it has a bitstream in the library.
    //
    // A better solution would be to move a lot of this bitstream code to
    // {un,}bind{Bel, Pip} and friends, but we're not there yet.
    log_info("Running signoff timing analysis...\n");

    timing_analysis(getCtx(), true, true, true, true, true);
}

NEXTPNR_NAMESPACE_END
