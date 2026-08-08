# nextpnr-mistral: the gaps blocking a real Cyclone V core

Scoped from measurements taken 2026-07-24/25 building the fabi386 i386 core (47,789 ALUTs) for the
DE10-Nano (5CSEBA6U23I7). Synthesis is solved; every remaining blocker is in this repo or libmistral.
Each item below states what was measured, why it blocks, and the entry point — not a wishlist.

## Status at a glance (2026-08-08)

The **IO and clocking primitives are now proven on silicon** (G4, G6 slice 1). What blocks a real
core is G3 (HPS hard IP), the timing/router work — and **DSP, which is absent entirely** (G7).

| gap | what it is | status |
|---|---|---|
| **G1** | timing-driven placement ineffective (`criticalityExponent = 7`) | **root-caused**; the fix is a *conditional* trade, automated in `critexp_auto.sh` |
| **G2** | `--tmg-ripup` churns without reducing `tmgfail` | **measured broken**; needs work in `router2.cc` |
| **G3** | HPS hard IP: only `mpu_general_purpose` modelled | **OPEN — largest item.** Blocks deploying the real core (needs FPGA2SDRAM + h2f/f2h bridges) |
| **G4** | PLL: reference delivery **and** outclk → clock network | **SOLVED on silicon, and now for real designs** — arbitrary requested frequencies (N/M solved, not pinned) and buffered reference clocks both work: 108 MHz + 135 MHz from one PLL, `LOCKED=1`. Phase taps verified (90°/270°). Generalisation beyond `PIN_V11 → FPLL(0,14)` still remains |
| **G5** | placer delay estimate is congestion-blind | **characterized**; the cheap fix was tried and REVERTED (measurably worse). Negative result, not a blocker |
| **G6** | bidirectional IO, then the SDRAM controller path | **slices 1–3 SOLVED on silicon** — tristate pads drive/release, and a **full 32 MB MemTest passes with 0 errors** on slot 2 @ 50 MHz. Remaining: frequency probe + a performance controller |
| **G7** | **DSP / `MISTRAL_MUL18X18` entirely unimplemented** | **OPEN — hard blocker.** yosys *emits* the cell; the mistral backend has **zero** references to it. Any design with an 18×18 multiply cannot build |
| **G8** | two PLLs in one design abort the tool | **LARGELY DISSOLVED.** The need was misdiagnosed: Quartus **merges** same-reference PLLs into ONE physical PLL with multiple counters (`Total PLLs: 1/6` for a two-instance design), and our flow already does that shape — **10 MHz + 25 MHz from one PLL, both `LOCKED=1` on silicon**. Two *independent* FPLLs now place and route; only one locks, because the reference spine exists for one position. Rarely needed |

> **Caution on "SOLVED".** G4 carried that label for a day while two defects sat inside it, each
> fatal to the first real core that tried to use it: attestation failed for any **buffered** clock
> (the ordinary shape, since synthesis inserts a `CLKBUF`), so the spine was never emitted and the
> PLL never locked; and N/M were pinned, so every design silently got 480/C — asking for 108 MHz
> produced 120 MHz with no warning. Both were found only by using the feature for a real purpose.
> A gap closed against one probe design is closed against one probe design.

### Timing model vs silicon — first calibration (2026-08-08)

Every Fmax number in G1/G2/G5 comes from nextpnr's own model, which had never been checked against
hardware. `openflow-test/fmaxtest.v` + `fmaxgen` (Rust) build a design whose timing failure is
*observable* — cascaded add+xor-shift stages, N iterations, checksum compared against a golden value
— clocked from the PLL at a requested frequency. Sweep until the answer goes wrong and that is the
true Fmax.

| design | nextpnr (post-route) | silicon | model error |
|---|---|---|---|
| 1 chain, ~0.5k cells | ~37 MHz | MATCH 45, WRONG 52 | **~30% pessimistic** |
| 32 chains, ~16k cells | ~31 MHz | MATCH 40, WRONG 60 | **~60% pessimistic** |

**The model is pessimistic, and its error GROWS with design size.** So part of the reported Fmax
collapse is a reporting artifact: a design reported at 6.22 MHz is running meaningfully faster than
that on silicon. It does not close the gap — 108/135 MHz is still far away — but it means tuning
against the reported number is tuning against a figure that is wrong by a size-dependent amount,
which is exactly the failure mode G5 already documents (a *better-fitting* model made routed Fmax
*worse*).

**Take the LAST `Max frequency` line.** nextpnr prints one after placement and one after routing;
reading the first gave ~58 MHz against a routed ~37 and briefly inverted the conclusion.

### Confirmed capabilities (measured — these are NOT gaps)

| item | evidence |
|---|---|
| M10K block RAM | **works on silicon**, 1024×16. Other geometries and true dual port untested |
| single PLL, multiple outputs | **works on silicon**, incl. phase-shifted taps (G4) |
| tristate / bidirectional pads | **works on silicon**, DQ and ordinary pads (G6 slice 1) |
| plain output pads | **works on silicon** |

### Fidelity gaps (real, but they do not stop a design building or driving)

| item | status | entry point |
|---|---|---|
| IO registers (`FAST_*_REGISTER`) | **not packed.** Deliberately not faked — ground truth puts them in the DQS16 block; warns instead of pretending | `bitstream.cc` |
| `IO_STANDARD` / `CURRENT_STRENGTH_NEW` | **parsed, then ignored** — `DRIVE_STRENGTH` hardcoded to `V3P3_LVTTL_16MA`. **Proven not to affect driving.** Warns rather than failing silently | `bitstream.cc:~170` |
| `USE_OPEN_DRAIN` | Quartus sets it on some pads; we never emit it. **Consequence unmeasured** | `bitstream.cc` |
| DDIO | **untested** — unclassified until someone measures it | — |

Ordering note: sections run G1–G4, then G6, G7, G8, then G5 — G5 is placed last because it is
characterization with a *negative* result rather than an actionable blocker. Do not reorder without
reading G5 first.

---

## G1 — Timing-driven placement is ineffective: `criticalityExponent = 7`

**Status: root-caused. The flag fix is a CONDITIONAL trade, not a uniform win — measured across 3 designs.**

**Measured.** `--freq 50` changes the placement *not at all*: post-placement Fmax is **10.40 MHz with and
without it**, byte-identical. The plumbing is fine — `--freq` → `target_freq` (`common/kernel/command.cc:502`,
default `12e6` at `:562`) → `timing.cc:324`; `timing_driven` defaults true; `timingWeight` defaults 10.

**Cause.** `common/place/placer_heap.cc:947`:

```cpp
weight *= (1.0 + cfg.timingWeight * std::pow(tmg.get_criticality(...), cfg.criticalityExponent));
```

and `mistral/arch.cc:482` sets `cfg.criticalityExponent = 7`. Criticality (`timing.cc:782`) is normalized
to the *achieved* worst slack, so it spans [0,1] across the design. At exponent 7 a net at criticality
0.57 contributes `0.57^7 ≈ 0.02` — indistinguishable from zero. **Only paths within a hair of the very
worst get any weight**, so on a design failing timing by ~8× there is no gradient to optimize along.

nextpnr's global default is **2** (`command.cc:581`); ecp5 and machxo2 use **4**; mistral inherited **7**
from nexus, where it was presumably tuned on designs that nearly meet timing.

**Fix — MEASURED, and no source change is required: `criticalityExponent` is already a CLI flag.**

| run | post-place | routed | router iters | final Fmax | `.rbf` |
|---|---|---|---|---|---|
| baseline (`critexp 7`) | 10.40 MHz | yes | 1312 | **6.22 MHz** | 2.96 MB |
| `--placer-heap-critexp 2 --placer-heap-timingweight 30` | 11.09 MHz | yes | **736** | **7.00 MHz** | 2.95 MB |

**+12.5% Fmax and 44% fewer routing iterations**, EXIT=0 in 24 min — versus every `--tmg-ripup` variant,
which timed out at 48 min with no bitstream at all. `--freq 50` on its own changes nothing (post-place
10.40 MHz either way), so the gain is attributable to the exponent/weight, not the target.

**CORRECTED 2026-07-25 — measured across three designs, `critexp 2` is a TRADE, not an improvement.**
The single-design result above was over-generalized. Full matrix (`scratchpad/pnrcal`, beta 0.35, `--freq 50`):

| design | ALUT | critexp 7 | critexp 2 | Δ Fmax | router iters |
|---|---|---|---|---|---|
| compact | 1,781 | 118.81 MHz | 123.24 MHz | **+3.7%** | 13 → 11 |
| neutral | 31,428 | 8.30 MHz | 7.70 MHz | **−7.2%** | 81 → 102 |
| core | 34,405 | 6.22 MHz | 7.00 MHz | **+12.5%** | 1312 → 736 |

**The predictor is not density** — `neutral` (31.4k) and `core` (34.4k) are nearly the same size and move in
opposite directions. It tracks whether routing is **congestion-bound**: `core` needed 1312 iterations at
critexp 7 and `critexp 2` nearly halved that while gaining 12.5%; `neutral` already routed in 81 iterations
and `critexp 2` made it *harder* (102) and slower. So the working hypothesis is:

> `--placer-heap-critexp 2` helps when the design is congestion-bound (high router iteration count) and
> hurts when routing is already easy. Apply it conditionally, per design, and measure.

That is a hypothesis from **three** data points, not a rule. Do not apply it by default, and do not quote
the +12.5% without the −7.2% beside it.

```
# only after confirming the design is congestion-bound at the default
MISTRAL_HEAP_BETA=0.35 nextpnr-mistral ... \
    --freq <real target> --placer-heap-critexp 2 --placer-heap-timingweight 30
```

**AUTOMATED (2026-08-06): `scratchpad/pnrcal/critexp_auto.sh`** applies exactly this conditional:
default run → parse router iterations → below threshold (`CRITEXP_ITER_THRESHOLD`, default 200,
between the measured 81/neutral and 1312/core populations) means NOT congestion-bound and the flags
are *refused*; above it, both configs run and the winner is chosen by **routed** Fmax only. Emits a
one-line JSON verdict (`critexp-auto/1`) carrying both measurements. Verified live: blinky → 5 iters,
212 MHz, `keep_default` with flags not applied. The threshold is a 3-datapoint hypothesis — re-derive
it via `pnrcal.sh` as designs accrue.

**Use `scratchpad/pnrcal/pnrcal.sh` for any further placer/router change.** It runs the design×config matrix
and tabulates routed/overuse/iters/Fmax/time. Built precisely because three P&R changes this session were
judged by reasoning plus one run, and **all three needed correcting** — the router cost schedule (predicted
win, diverged), the delay recalibration (better description, placer 2× worse), and this one.
*(The original scratchpad copy was never committed; recreated and committed 2026-08-06 —
`designs.tsv`/`configs.tsv` in, routed/iters/overuse/Fmax/time table out.)*

**Not changing `arch.cc:482` — and the second design settled it.** The default was held back pending more
than one data point; the second design came in at **−7.2%**, so flipping it would have made things worse for
part of the design space. The upstream-able version of this finding is therefore an **adaptive** exponent
(keyed on congestion — e.g. router iteration count or post-place overuse at the default), not a new constant.
That is a real change to nextpnr's placer and should be evaluated through `pnrcal` across more designs before
being attempted.

**Why it blocks.** The routed core hits **6.22 MHz** where the critical path is **94% wire** (19.77 ns
routing vs 0.60 ns logic; one 1×3-tile hop costs 11.52 ns — a congestion detour, not distance). Logic depth
is ~4 LUT levels. The design is not slow; the placement and routing are timing-agnostic.

---

## G2 — `--tmg-ripup` churns without reducing timing failures

**Status: measured broken; needs work in router2.**

**Measured.** With `--tmg-ripup`, congestion resolves far faster (overuse 46,009 → 48 by iter 41, → 3 by
iter 161, versus needing 1,312 iterations without it) — but the `tmgfail` counter is **flat across 163
iterations** (109,750 → 115,053 → 114,276 → 114,723 → 114,260) while overuse oscillates 3–9 and **never
reaches 0**. All three beta variants timed out at 2,900 s with no bitstream. Enabling it traded a
*completed* legal route for an *incomplete* one with no timing gain.

nextpnr's own `--help` calls it "enable **experimental** timing-driven ripup", which matches.

**Entry point.** `common/route/router2.cc` — the criticality term in the cost function and the ripup
selection. The observable to drive against is `tmgfail`: any change must make it monotonically decrease.

**Note.** G1 likely feeds G2 — a timing-agnostic placement leaves the router trying to fix timing with
routing alone, which it cannot do when the wires are long by construction. Fix G1 first and re-measure.

---

## G3 — HPS hard-IP interfaces: only `mpu_general_purpose` exists

**Status: hard blocker on deployment; the largest item.**

**Measured.** The real core (`emu`) **cannot be P&R'd standalone at all**: it needs **436 IO bits** against
~314 user pins on this device. `emu` is not the board top — `sys_top` maps `HPS_BUS` (49 b), `DDRAM_*` and
`SDRAM_*` onto HPS hard IP that consumes no user pins. Device utilisation confirms exactly one HPS bel is
modelled: `cyclonev_hps_interface_mpu_general_purpose: 0/1`.

That one interface is silicon-proven end-to-end (rung-1: 32-bit `mpu_gp` round-trip verified on the DE10 via
`devmem`, 4/4 vectors, gpi == ~gpo). What is missing is everything else — notably **FPGA2SDRAM** (the DDR
bridge the core's L2 needs) and the h2f/f2h AXI bridges.

**Entry point.** Follow the `mpu_general_purpose` pattern: bel creation in `mistral/arch.cc`, `constids.inc`,
port mapping in `mistral/pack.cc`, emit in `mistral/bitstream.cc`. libmistral already models the config
surface; this is nextpnr-side integration.

**Scope honestly.** This is a feature, not a fix. Until it exists, "the open flow builds and runs the real
core" is unreachable — which is why a **fabric-only demonstrator** (no HPS, no DDR, BRAM-backed) is the
right way to prove the flow on silicon in the meantime.

---

## G4 — PLL: reference-clock delivery and outclk → clock network

**Status 2026-08-07: SOLVED on silicon, both halves.** A plain `nextpnr-mistral` invocation now
produces a working PLL on the DE10-Nano: **`PLL = 10.066 MHz, LOCKED = 1`** against a 10.000 MHz
request (build-ID verified, 30 s window). Remaining work is *generalisation*, not mechanism.

**The mechanism, as implemented.** Two independent halves, both of which had to be right:

| stage | what the silicon does | how nextpnr emits it |
|---|---|---|
| reference **in** | `CMUXVG(42,0) INPUT_SEL[0] = 0x00 → {CLKPIN,1}` — a dedicated-clock-pin cmux entry | bmux |
| reference **distribution** | 56 CRAM bits: a spine *network* — column 9 rows 9–29, column 15, row 76 across columns 9/12/15/18 | raw CRAM (`mistral/vup_cram.cc`) |
| analog bias | `PL_AUX_BG_POWERDOWN` on the **unused** `FPLL(0,73)` | bmux |
| PLL config | integer family N=5 M=48, VCO 480 MHz, `CTRL_OVERRIDE_SETTING=0`, cmux `PLL_FEEDBACK_ENABLE_<gclk>`, no `FBCLK_MUX_2` | bmux |
| output **out** | `CMUXVG(42,0) INPUT_SEL[1] → {PLLIN,k}`, on a **different gclk instance** than the reference | bmux |

**Why it took ~25 silicon trials — three errors, all now fixed in code:**

1. **`CTRL_OVERRIDE_SETTING` was never emitted.** An escaped `\n` inside a `//` comment swallowed the
   statement, and it used a setter that no-ops on an `MT_BOOL` field. Its default is 1; every Quartus
   core clears it. Every negative collected before this fix is *invalidated*, not refuted.
2. **The reference path was modelled wrongly, and we actively broke the real one.** nextpnr built
   `pin → CLKBUF → GCLK → SCLK → PMUX → CORECLK0`; the silicon uses a `{CLKPIN,n}` cmux entry plus a
   spine libmistral does not model at all. Worse, our emission **set** spine bits that block it — so
   the fix was partly a *removal*, which is why adding configuration never helped.
3. **Reference and output collided on cmux gclk 0.** With the reference installed there the PLL
   locked but the fabric counter measured the raw 50 MHz reference. Moving the output to gclk 1 gave
   the requested 10 MHz.

**Scope limit, enforced in code.** The spine table is empirical and attested for **`PIN_V11 →
FPLL(0,14)` only**. `pack.cc` verifies the reference is driven by that pin and marks the cell
`PLLCLK_ATTESTED_REF`; the position override, the `{CLKPIN,n}` entry, the bandgap bit and the spine
all key off that marker. A PLL fed from any other pin falls back to the old modelled path with an
explicit warning that it is **not** known to work on silicon — an honest failure rather than a spine
derived for the wrong source. `VUP_PLL_LEGACY=1` restores the previous behaviour throughout;
`VUP_PLL_POS` / `VUP_CLKPIN_GCLK` override position and reference gclk instance.

**Remaining work — the earlier "derive the rule from a few references" plan is RETRACTED.** Building
a Quartus reference per DE10-Nano clock pin shows every element of the clock delivery is a per-design
fitter decision, not a function of the endpoints:

| pin | PLL position | reference entry | output | feedback |
|---|---|---|---|---|
| `V11` | `FPLL(0,14)` | `CMUXVG(42,0)[0] = {CLKPIN,1}` | `CMUXHG(0,35) {PLLIN,14}` | `CMUXVG(42,0) FB_0` |
| `Y13` | `FPLL(89,0)` | `CMUXVG(42,0)[1] = {CLKPIN,0}` | `CMUXHG(89,35) {PLLIN,6}` | `CMUXVG(42,0) FB_3` |
| `E11` | `FPLL(0,55)` | `CMUXVG(42,81)[1] = {CLKPIN,1}` | `CMUXHG(0,35) {PLLIN,2}` | `CMUXHG(0,35) FB_1` |

The spine moves too — Y13's clock-region footprint is a **single tile**, where V11's spans eleven
tiles of column 9 plus column 15 and row 76. A table would need one entry per (pin × PLL position)
combination.

1. **Upstream it — the only viable route to generality.** Model the spine in libmistral's routing
   graph so nextpnr can *derive* the path instead of replaying a recorded one. These bits produce no
   bmux difference and no change in `route_all_active_links()`, so no public API expresses them
   today. File against Ravenslofty/mistral with the bit maps and the table above.
2. **Until then the attested guard is the correct posture**, not a stopgap to widen by adding rows.
   Single-PLL designs on the board clock pin work today; anything else warns and falls back.

**Method note — how it was actually cracked.** Sweeping was exhausted (`CLKIN_0_SRC` across its full
range, six CLKBUF sources, PLL position, both feedback modes, and a byte-identical clone of a working
tile — all negative) because a sweep can only turn knobs that *have names*, and the answer had none.
What worked was **differential RE**: a Quartus build of our own probe design as a positive control
(`PLL = 96.469 MHz, LOCKED = 1`, which made every prior negative trustworthy), with/without pairs to
isolate the feature's footprint, and transplant bisection — on a probe rebuilt so its readout no
longer shared hardware with the thing under test. Full evidence trail in
`mistral/PLL_OUTCLK_DESIGN.md`; tools in `../openflow-test/` (`Makefile`): `pramdiff`, `pramown`,
`pramapply`, `prampatch`, `crampatch`, `cramdiff`, `pllports`, `pmuxinfo`, `bmuxdiff`, `bmuxhist`.

**Two traps worth not re-learning:**
- A *single* differential pair **under-reports**: bits the pair happened to agree on are invisible in
  it, which is why the first spine table was short by half. Always re-diff the **built artifact**.
- A build-ID channel only discriminates across **distinct** IDs. Patched/derived bitstreams inherit
  their base's ID, so for those the reboot is what guarantees the load, not the ID.

---

## G6 — bidirectional IO (SOLVED), then the SDRAM controller path

**Status: added 2026-08-06. Slice 1 — bidirectional/tristate IO — is SOLVED on silicon as of
2026-08-08: pads drive when asked and release when not, on DQ pads and ordinary pads alike (see
"SOLVED" below for the fix and the methodology lesson). Remaining: port a controller and run MemTest.**

Not a nextpnr feature gap like G3/G4 — a flow-capability gap: nothing had ever driven the MiSTer
SDRAM module through the open flow, and it is the cheapest route to real fabric-accessible memory
(no HPS needed).

**Why it matters.** G3 (FPGA2SDRAM/HPS bridges) is the largest blocker for DDR — but the MiSTer
ecosystem's **SDRAM modules sit on plain FPGA GPIO banks** (user pins), so a fabric SDR-SDRAM
controller needs *no new bels at all*: the IO path the open flow already proves with blinky is the
same path the SDRAM module uses. A working open-flow SDRAM controller unblocks a memory-backed
fabric demonstrator (the G3 workaround, with real memory instead of BRAM), fabi386's near-memory
tier, and the SVGA-VRAM direction (Slot-2 SDRAM as a framebuffer).

**Leverage first — tried-and-true implementations exist; do not write a controller from scratch:**
- **MiSTer framework `sdram.sv` controllers** — years of on-this-exact-board burn-in across hundreds
  of cores, for the exact SDRAM modules (SDR, 16-bit, up to 128MB) on the exact pins.
- **jotego's `jtframe_sdram`** — heavily exercised multi-bank controller family (48/96 MHz), MIT.
- Simpler single-purpose controllers in individual cores (ao486's, various consoles) as references.

**The honest blockers to check (in order):**
1. **Clocking (was G4-gated — now UNBLOCKED, 2026-08-07):** every proven controller phase-shifts the
   SDRAM clock vs the fabric clock (PLL output tap or -phase clock). G4 now delivers a locked PLL
   whose output reaches the fabric, so full-rate clocking is available on the attested pin. The
   *phase-shifted* tap specifically is still unexercised: `phase_shift0` is accepted by the frontend
   but no phase-shifted output has been silicon-verified — treat that as the first thing to prove
   here, not as done.
2. **IO ring — MEASURED BLOCKER (2026-08-07), not just "to be verified".** The smallest design that
   asks the question (16-bit bidirectional DQ + registered address/nWE on the real SDRAM2 pins,
   `openflow-test/sdrio.v`) **does not build**:

   ```
   ERROR: Unable to place cell 'oe_$_TBUF__E_9', no BELs remaining to implement cell type '$_TBUF_'
   ```

   The qsf path is fine — all 16 DQ constrain onto `MISTRAL_IO` bels. The failure is the tristate.
   Yosys emits *both* 16 `MISTRAL_IO` pads and 16 orphan `$_TBUF_` cells, and inspecting the netlist
   shows why:

   ```
   MISTRAL_IO : {I: [63], OE: ['1'], PAD: [3]}    <- output-enable tied to CONSTANT 1
   $_TBUF_    : {A: [110], E: [111], Y: [70]}     <- the real tristate, stranded in fabric
   ```

   `iopadmap` matched the inout port and created the pad, but tied `OE` high and left the tristate
   behind. A permanently-driving pad would fight the SDRAM even if it placed. Adding an explicit
   `tribuf` pass does not change the counts (16/16), so this is not a missing recipe step.

   **So the open flow cannot currently express a bidirectional bus with a driven output-enable** —
   a hard blocker for any SDRAM controller, and worth knowing before porting one.

   **FIXED 2026-08-07 — `pack_tristates()` in `pack.cc`.** The bel already had an `OE` pin
   (`io.cc`), so this was purely packing. The pass absorbs each `$_TBUF_` into the adjacent
   `MISTRAL_IO`: `I ← A`, `OE ← E`, and any *other* reader of the buffer's output — the read-back
   path, which `iopadmap` had wrongly tapped off the driver — is moved to the pad's `O` port (created
   if absent, as the mis-mapping left it undeclared).

   ```
   Info: Packed 16 tristate buffer(s) into MISTRAL_IO OE.
   Info: Program finished normally.
   ```

   The 16-bit bidirectional bus now builds (39 `MISTRAL_IO`). Regression-checked: an output-only
   design is byte-identical to before and unaffected.

   **SILICON RESULT (2026-08-07): builds, but the pad never drives — OE is not routed.**
   `openflow-test/oetest.v` drives 0xA5A5 then 0x5A5A then releases with a weak pull-up, on the real
   SDRAM2 bus with a module fitted, holding `nCS` high and `CLK` static so the device is inert by
   construction:

   | phase | expected | measured |
   |---|---|---|
   | drive `0xA5A5` | `0xA5A5` | `0xFF` ❌ |
   | drive `0x5A5A` | `0x5A5A` | `0xFF` ❌ |
   | release + weak pull-up | `0xFFFF` | `0xFF` ✅ |

   The release phase passing proves **the input path works and `WEAK_PULL_UP_RESISTOR` is honoured on
   silicon** (validating the qsf fix below). But the pad never drives.

   **STATUS: still not driving. Routing is NOT the cause — that earlier conclusion is retracted.**

   A `<UNDRIVEN>` verdict from a hand-built pnode probe was wrong: it addressed a different node than
   the bel pin actually uses. Asking nextpnr itself (`VUP_DEBUG_OE`) gives the authoritative answer:

   ```
   [oe] SDRAM2_DQ_MISTRAL_IO_PAD_9 bel=MISTRAL_IO.78.0.0 OEpin_wire=GOUT.78.0.34 net=oe  ROUTE REACHES THE OE PIN
   ```

   and the bitstream contains the arc: `GOUT.078.000.0034 <- TD.078.000.0052`. **OE is routed and
   emitted.** (`pack_tristates()` needed one fix to get there: `getBelPinsForCellPin()` is
   `pin_data.at(pin).bel_pins`, an explicit map with no name fallback, so ports created by the packer
   must be mapped or they route nowhere.)

   **Positive control — the hardware and the test are both fine.** A Quartus build of the *identical*
   design, same pins, same board, passes every phase:

   ```
   drive 0xA5A5 -> 0xa5 OK   drive 0x5A5A -> OK   release+pull-up -> 0xff OK   OE IS DYNAMIC
   ```

   So a pad *can* read back its own driven value; the test is valid; ours is simply wrong somewhere.

   **Differences found and eliminated so far** (each matched to ground truth, none sufficient):

   | difference | ground truth | fixed |
   |---|---|---|
   | `DQS16 INPUT_REG4_SEL` on bidirectional pads | Quartus omits it (keeps `RB_T9_SEL_EREG_CFF_DELAY`) | yes — a first cut wrongly dropped *both* |
   | `OEIN` inverter on the OE pin wire | Quartus 0 (pass through); ours was 1 | yes, now the default |

   Neither alone nor both together make the pad drive. GPIO and DQS16 config now match the working
   build on the DQ pins, the OE inverter matches, OE is routed and emitted — and it still reads
   `0xFF` while driving.

   **New instrument: `openflow-test/invdiff`.** Inverter state is `inv_set()`, *not* bmux, so
   `bmuxdiff` was structurally blind to it — an entire class of configuration difference was
   invisible to every diff run before this. That is how the OE inversion survived so long, and it is
   the more reusable finding here.

   **State after the full diff.** All 16 DQ pads' OE inverters now match ground truth (checked by
   resolving each pad's OE wire via `VUP_DEBUG_OE`, then `invdiff` on exactly those wires). What
   remains is **CRAM in the pad tiles** — 45 differing bits in tile (78,0), 10 in (89,6) — which is
   the same class as the historical dark-IO problem: unmodelled IO pad-activation bits that live in
   CRAM and are attributed by no bmux.

   A blanket transplant of donor CRAM over columns 55..90 (9689 bits) **destroyed the readout** —
   `raw 0xffffffff`, `BUILD_ID=0xff` instead of `0xc8`. All-ones made every verdict bit read 1, so
   the script reported "OE IS DYNAMIC": a false PASS. **Void trial, not a result.** The measurement
   scripts now reject all-ones/all-zeros readouts outright, because a dead readout that prints
   success is worse than one that prints nothing.

   **Corpus attempt (2026-08-07), and why tile diffs alone do not work.** Rebuilt the probe on the
   *slot-1* pins that real cores use, so shipped cores become like-for-like references. Key
   observation: at a DQ pad tile, **two shipped cores differ from each other by MORE bits than we
   differ from either** (NeoGeo vs C64: 46; NeoGeo vs ours: 38) — a tile is mostly design-specific
   routing, so pairwise tile diffs are noise.

   New tool `consensus` filters for signal: bits where **every** reference agrees and ours differs.
   Over four SDRAM-driving cores (NeoGeo, C64, AtariST, Gameboy) at one pad tile: 25721 bits of
   agreement, only 10 differing in ours. Across all twelve DQ pad tiles: 49 bits that every core sets
   and we do not. Applying exactly those (`crambits`) and testing: still `0xFF` while driving —
   build-ID verified, readback alive, so a valid negative.

   ⚠️ **The consensus derivation is WEAK, and the negative result is the only safe thing to take from
   it.** All four references are MiSTer cores built from the same `sys` framework, so "all four
   agree" may mean nothing more than *"the MiSTer framework sets this"* — the filter cannot separate
   "a bidirectional pad needs this" from "every MiSTer build has this". Those 49 bits have **no
   causal provenance**. They were fine as a probe; they would have been indefensible as a fix, and if
   the test had passed, the correct response would have been to bisect down to the one or two bits
   that mattered, not to ship 49 unexplained ones.

   Contrast the PLL spine, which is also empirical: it came from a **differential pair** (same design
   built with and without the feature), so every bit in the delta is causally attributable to the
   feature, it was verified end-to-end on silicon, and it is hard-gated in code to the single
   attested configuration. Consensus across unrelated designs carries none of that force. **Derive
   candidates differentially, not statistically.**

   Also checked the other reading of "learn from the cores": their **RTL** drives the bus with plain
   Verilog inference (`assign SDRAM_DQ = oe ? d : 16'bz`), not an explicit IO primitive. So the gap is
   not coding style — Quartus turns that inference into a working pad and our flow does not.

   **Still unsolved.** What is now known: OE is routed and emitted; GPIO/DQS16 config and the OE
   inverters match ground truth; 49 cross-core consensus bits are not sufficient. The remaining
   difference is somewhere the current instruments have not looked — most likely state that is
   neither bmux, nor inverter, nor pad-tile CRAM.

   **Differential attempt (2026-08-08) — the right method, one causal finding, still not driving.**
   Built the same design twice in Quartus, bidirectional bus vs the same pins as plain outputs, the
   *only* difference between them. Delta:

   ```
   IOCSR_STD = 0x209   set by the PLAIN-OUTPUT build, ABSENT in the BIDIRECTIONAL one
   ```

   So a bidirectional pad must not have `IOCSR_STD` set, and we were setting it on every
   output-capable pad. That is causally grounded — unlike the 49 consensus bits — and it is now
   conditioned on `dyn_oe_cell()`.

   **Silicon: the reading CHANGED for the first time in many trials.** The released phase went from
   `0xFF` (pull-up holding the bus) to `0x00`. So `IOCSR_STD` is genuinely part of the pad's input
   path — removing it altered input behaviour rather than enabling the output. The pad still does not
   drive.

   **Stopped here by a pre-committed stop rule** (small causal candidate applied, pad still dead →
   stop rather than grind). The change is kept because it matches ground truth and is causally
   derived, but note it makes the *input* worse in isolation, which is itself the lead: our pad
   configuration is wrong in a way `IOCSR_STD` was partially masking.

   **Bisected down from the working Quartus build (2026-08-08). Every bitstream-difference
   hypothesis is now eliminated, and that is the useful result.**

   `bin/cramunmod` rebuilds a device from only what mistral can express -- every non-default bmux,
   every active routing link, every inverter -- and diffs against the real CRAM. Residue: donor 18
   bits, ours 38, *exactly* the number of settings the tool's own dispatch failed to apply. **mistral
   models both bitstreams completely.** There is no unmodelled-pad-bit mechanism; the PLL-spine
   analogy does not transfer. Do not go looking for one.

   All four stores compared, every difference tested on silicon:

   | store | difference | verdict |
   |---|---|---|
   | CRAM | fully modelled; pad config matches | no donor-only GPIO/DQS16 settings exist |
   | PRAM | 44x `DRIVE_STRENGTH`, 4x `INPUT_REG4_SEL` | donor + OUR drive strength PASSES => innocent |
   | ORAM | `JTAG_ID` only (`optdiff`) | donor's oram into our build: no change |
   | inverters | 44 GOUT nodes | all on nodes neither build routes to |

   Routing endpoints are identical -- the shared OE net lands on the *same 16* `GOUT` nodes in both
   builds, and the data nodes match. Only the fabric-side `TD` source differs, and seed 3 (which
   picks a donor-like `TD` index) fails **identically**. The failure is seed-invariant: systematic to
   the flow, not a placement accident.

   **Measurement confounds -- both self-inflicted, recorded so they are not repeated.** With no
   effective pull-up, a "released" reading is just residual charge from the previous phase: two builds
   in identical release states read `0x00` and `0xff`. And a probe whose data pin is a *constant* is
   degenerate -- yosys folds the readback, and drive-high vs release are indistinguishable under a
   pull-up. The earlier "OE works" reading rests on float readings and is **not firm**.

   **What is solid:** same harness, same pins -- the Quartus build reproduces the driven data; ours
   reads `0xff` for `0xA5A5`, `0x5A5A` and `0x0000`.

   **FABRIC PROVEN CORRECT ON SILICON (2026-08-08).** A probe latches the *very net* that feeds the
   pad's data pin -- verified in the synthesis JSON to be the same net the `$_TBUF_` A ports use, not
   a duplicated copy -- and reports it over HPS `gp`, a channel that never touches the pad. Result,
   same build and same run: `raw 0xe18a5ff0` -> **fab = `0xa5`** while the pad reads back `0xff`.

   So the LAB produces the intended data on the correct net, and the pad does not reflect it. The
   fault is strictly **between the LAB output and the pad**. This is also the first independent
   observation channel in this investigation: it proves the `0xff` readings are real pad behaviour,
   not an artifact of broken fabric.

   (First attempt at this probe was inconclusive through a bug of mine: `t` free-runs and wraps, so
   `t == 1000` fires in *every* phase and the last write landed in phase 2 where `drv` is `0x0000` --
   a correct fabric yields `0x00` there. Fixed with a one-shot latch. Beware equally that gating the
   latch on `phase` lets yosys constant-fold it into a meaningless pass.)

   HMC is not the answer either: nextpnr references `hmc_get_bypass` only for *name resolution*
   (arch.cc:61, bitstream.cc:60) and never configures HMC, but the donor has **zero** non-default HMC
   settings, so there is nothing to copy.

   **Quartus data-path differential, 2026-08-08 (`/projects/qio_in` on the NAS).** Same design built
   twice, identical except the DQ pads are `inout` and driven in one and `input`-only in the other.
   The causal set for "what it takes to drive a pad" is exactly two things:

   * `GPIO DRIVE_STRENGTH` -- already proven innocent on silicon
   * `DQS16 RB_T9_SEL_EREG_CFF_DELAY` -- and we emit **44 entries, byte-identical to the donor**

   So every mechanism Quartus uses to make a pad drive is one we already emit correctly.

   *Build gotcha:* `cp -r` of a project carries the old `min_pll.rbf`, and `--flow compile`
   regenerates only the `.sof`. The first diff came back all-zeros off a stale file. Convert
   explicitly with `quartus_cpf -c min_pll.sof min_pll.rbf` and check the timestamp before trusting
   a diff.

   **The "mistral rmux encoding is wrong" theory is dead too.** `cramunmod` reconstructs the donor's
   CRAM from bmux settings + routing links + inverters with *zero* unexplained bits, so mistral
   writes exactly the bits Quartus writes for the same logical links.

   **Where that leaves it.** Config, routing endpoints, fabric, encoding and the drive-enable
   mechanism all agree with a build that works -- and ours still does not drive.

   ## SOLVED (2026-08-08) — a driven OE must be INVERTED at the pad

   Native `nextpnr-mistral`, **no env vars**, on silicon:

   ```
   drive 0xA5A5 -> readback matches : 1   (0xa5)
   drive 0x5A5A -> readback matches : 1   (0x5a)
   release, weak pull-up -> 0xFFFF  : 1   (0xff)
   VERDICT: OE IS DYNAMIC - drives when asked, releases when not
   ```

   Verified on **DQ pads (inside DQS16 groups)** and on **ordinary LED pads** — so it is the general
   tristate path, not anything memory-specific. One line: `bool inv = true` for `dyn_oe`.

   Without it, the pad is enabled exactly when the design wants it *released*. That reads as "the pad
   never drives", which is what sent this investigation down a very long wrong path: every reading
   was `0xff` while driving (pad released, pull-up wins) and `0x00` on release (pad enabled, driving
   the phase-2 value of `0x0000`). Both were faithful measurements of an inverted enable.

   **The methodological lesson, and it is the expensive one.** A Quartus build of the same
   bidirectional design has this inverter at **0**, and we matched it — twice, deliberately, citing
   ground truth. That bit-level match was *correct and useless*: Quartus computes `~oe` in the fabric
   and cancels it at the pad, while we route `oe` straight through, so copying its bit produces the
   opposite **net** polarity. The invariant is the fabric-to-pad polarity, not the bit.

   > **Matching a ground-truth bit is only sound when the upstream logic matches too.** Where the
   > vendor is free to absorb an inversion into the fabric, the bit is a *consequence* of its
   > placement, not a spec. Compare net semantics, not bits.

   This also explains why a plain output always worked: its OEIN is undriven, so the inversion is
   what enables the buffer at all and signal polarity never arises. That asymmetry is exactly why
   plain outputs passed while every tristate failed.

   **What actually found it:** narrowing to a minimal tristate on one ordinary pad — no SDRAM, no
   DQS16, no HPS. The A/B pair (tristate vs plain output) showed *zero* pad-config difference, which
   pointed at the inverter, and our-build-vs-Quartus on the same minimal RTL put the OE node's
   inverter difference in a list short enough to read.

   ### The route there (kept — every one of these is a real elimination)

   **THE AXIS WAS WRONG: it is the TRISTATE path, not the DQ pads (2026-08-08).** LED7 is lit on
   silicon, so ordinary output pads (`MISTRAL_OB`) driven by our flow work. Moving the tristate onto
   those same ordinary LED pads -- DQ left `input`-only, no DQS16 involved -- reproduces the failure
   **identically** (`s0 = 0xff`). So:

   * plain output (`MISTRAL_OB`) -> **works**
   * tristate / driven OE (`MISTRAL_IO` + OE) -> **fails**, on ordinary GPIO and on DQ pads alike

   Every DQ- and DQS16-specific line of enquiry above was chasing the wrong axis. The bug is in our
   own tristate emission, which also means it is reproducible with a *tiny* design and no memory bus.

   Also settled: `SDRAM2_nCS` really is held high by our bitstreams (plain outputs work), so the
   "device held inert by construction" argument does hold for our builds.

   **`OEIN.1` inversion -- ground-truth match, not a fix.** `find_rnode(GPIO, pos, OEIN, bi, {0,1})`
   was verified to resolve to exactly the wire the OE route reaches (`MATCH` for every pad), so there
   is no node mismatch. But invdiff against the Quartus build flags exactly **16** inverter nodes
   (donor 1, ours 0) and they are precisely the 16 `OEIN.1` nodes of the 16 bidirectional pads --
   nothing else. These never appear in a routing diff because `OEIN.1` is a config inverter, not a
   routed node, which is why an earlier pass wrongly wrote them off as unused. Now emitted as
   inverted: the inverter delta drops 44 -> 28 and the pads match the donor exactly. **Silicon still
   fails**, so this is a ground-truth match kept for correctness, not a proven fix. A plain output
   hides the issue entirely: its OEIN is undriven, so the `OEIN.0` inversion alone enables the buffer
   and `OEIN.1` never matters.

   ### Slice 2 — a real SDRAM round-trip (2026-08-08)

   `openflow-test/sdram1.v`: a minimal controller (plain Verilog, not a port of MiSTer's
   SystemVerilog `sdram.sv` — that would have tested yosys, not our flow), slot 2, 50 MHz straight
   from `FPGA_CLK1_50` so the slice isolates the SDRAM path. Result: **wrote `0xA53C` to the real
   chip and read it back.**

   `sdram2.v` is the control, because a single round-trip is exactly the shape of result that can be
   a false pass: a floating DQ bus holds the last value driven onto it, so **capacitance alone would
   reproduce the pattern**. It writes `0xA53C` to column 0, *then* `0x5AC3` to column 8, then reads
   column 0. Reading `0xA53C` back — not the decoy — is what makes this a real result.

   Facts worth keeping: slot 2 has **no CKE and no DQM** pins (tied high/low on the dual-SDRAM
   board), so those eight signals are the entire interface; `SDRAM2_CLK` is driven inverted for setup
   margin. This slice is also the first that drives the bus for real, so the "held inert by
   construction" safety argument no longer applies — SDRAM has no NVM, so the worst case is garbage
   data, not damaged hardware.

   ### Slice 3 — full 32 MB MemTest (2026-08-08)

   `openflow-test/sdram3.v` walks all **16.7 M words** twice (write pass, then read/verify) at
   50 MHz. **0 errors.** Data is address-dependent (`addr[15:0] ^ 0xA5A5`) rather than a constant, so
   a location that aliases to another address returns the *wrong* value instead of a plausible one —
   a constant pattern passes happily on a bus with stuck or swapped address lines. Refresh matters at
   this scale and did not in slice 2: a pass takes seconds and rows decay in 64 ms, so an AUTO REFRESH
   goes out every 16 words (~190 cycles) against the 7.8 µs the part requires.

   **The scale test earned its keep immediately.** The first run failed with errors from address 0,
   and the cause was in the probe, not the flow: `{4'b0100, a[8:0]}` puts the auto-precharge flag on
   **A11, not A10**, so it was never asserted — the row stayed open and every subsequent `ACTIVATE`
   violated the protocol. Slice 2 passed with the same intent because it used the literal `13'h400`
   and only ever touched one row. **A single-word round-trip cannot see that class of bug.**

   **Next:** frequency probe against the 166 MHz ceiling measured for this slot with MemTest256, then
   a performance controller (jtframe or MiSTer `sdram.sv`).

   **Superseded:** a minimal Quartus differential on a *tristate ordinary pad* -- a handful of pins, no
   SDRAM, no DQS16. Same method as before but on a design small enough that the delta should be a
   handful of bits rather than a haystack.

   **Superseded:** whether ANY output pad works in our flow. All our telemetry goes
   over HPS `gp`, never through a pad, so no output pad has ever been verified end to end. If this is
   general rather than DQ-specific, it is a far bigger and more tractable clue. The board's user LEDs
   are driven by these designs (`LED = {done, ok0, ok1, ok2, 4'd0}`, so LED7 should be lit whenever
   `done` is set) and cost one glance at the hardware to check.

   *Safety note:* if no output pad drives in our flow, then `SDRAM2_nCS` is NOT actually being held
   high by our bitstreams, and the "device held inert by construction" argument does not hold for
   them. It still holds for the Quartus builds, which do drive.

   **Superseded:** bisect DOWN from `gt_bidir.rbf` (a same-design Quartus build that PASSES),
   replacing its regions with ours until it breaks. Narrowing from a working artifact is strictly
   more informative than patching a broken one, and it is the direction that cracked the PLL.

   The speculative inverter change was reverted: with OE unrouted it is unjustified, and a guess left
   in the emission would be indistinguishable from a derived value later.

   Still unverified beyond this: IO-register packing and DDIO for SDR capture.
3. **Constraint fidelity:** the MiSTer `.qsf` pin set for the SDRAM bank (drive strength, fast
   output register) must survive the qsf path.
4. **Silicon validation:** MemTest-style pattern check as the acceptance gate, deployed over the
   existing HPS deploy path — the measured verify model already exists (`deploy.rs`).

**Path.** Port the MiSTer `sdram.sv` (smallest proven variant) + its qsf pin block into the open
flow at conservative clocking → silicon MemTest → raise the clock, now that G4 provides a working
PLL (phase-shifted taps still to be proven). Success criterion is a silicon-verified memory test through the open flow, not "it routes."

**Module design (added 2026-08-06).** Ship the controller as standard slice modules, two variants:
**16-bit** (one module — the common MiSTer analog-board case) and **32-bit** (dual/ganged modules).
Nice-to-have features, in priority order:
1. **Capacity auto-detect** — standard row/col/bank aliasing probe at init (write-pattern address
   folding), reporting detected geometry (32/64/128MB) instead of a build-time parameter.
2. **Frequency capability probe** — trial the clock ladder (G4 now supplies the PLL) with the
   self-check as the pass gate per step; report the highest stable rate rather than assuming one.
3. **Built-in self-check** — a MemTest-class pattern engine (walking bits, address-in-address,
   refresh-retention spot check) exposed as **`vup-telemetry/1` channels** (ADR-0006):
   `sdram.geometry`, `sdram.freq_mhz`, `sdram.selfcheck` as kept registered state — so the same
   assert corpus gates the controller in sim and on silicon, and the deploy gate can read PASS from
   telemetry rather than "it configured."

- **macOS portability fix** in `mistral/pack.cc` — `std::max/min(int64_t, long-literal)` was ambiguous and
  blocked *all* nextpnr-mistral compiles on macOS; now `std::max<int64_t>` / `std::min<int64_t>`.
- **`MISTRAL_HEAP_BETA` env override** in `mistral/arch.cc` — experimental hook for the cut-spreader's
  `beta`, which carries the upstream `TODO: find a good value of beta for sensible ALM spreading`.
  **Measured: `beta = 0.5` (default) never routes the dense core; `beta ≤ 0.40` routes it to overuse 0 and
  emits a `.rbf`.** The knob is coarse — 0.25/0.35/0.40 produce byte-identical placements; only 0.45 differs.
  A defensible upstream change is lowering the default, or making it utilisation-dependent.

## G7 — DSP is not implemented at all (`MISTRAL_MUL18X18`)

**Status: added 2026-08-08. Hard blocker for most real cores.**

**Measured.** yosys emits the cell and the backend does not know it exists:

```
$ yosys -p 'synth_intel_alm -family cyclonev' (18x18 registered multiply)
   MISTRAL_MUL18X18: 1        <- emitted by synthesis

$ rg 'MUL18X18|DSP' nextpnr/mistral/*.cc *.h
   (no matches)               <- no bel, no packing rule, no emission
```

**Why it blocks.** Any design containing an 18×18 multiply fails. That is not an edge case: arcade
and console cores use DSPs for audio mixing, scaling and arithmetic throughout. A core can sometimes
be forced to LUT-based multipliers, at a large area cost, but nothing in the flow does that today.

**Entry point.** Needs the full chain — bel definition in `arch.cc`, a packing rule for
`MISTRAL_MUL18X18`, and bitstream emission. libmistral models the DSP blocks, so the device data
should already be there; this is nextpnr-side work.

---

## G8 — two PLLs in one design abort the tool

**Status: added 2026-08-08. Pre-existing (reproduces with `VUP_PLL_LEGACY=1`), so it is not a
regression from the G4 work.**

**Measured.** `openflow-test/gates/g_2pll.v` — two `altera_pll` instances, a pixel clock and a memory
clock, the ordinary arrangement for a video core:

```
Info:   PLL clock injector 'p2$pllclk[0]': outclk[0] -> FPLL(0,0) C5 -> cmux(42,0) gclk 1
libc++abi: terminating due to uncaught exception of type assertion_failure:
    Assertion failure: data.bound == nullptr (mistral/arch.h:345)
```

Both PLLs are assigned the same bel, and the second bind trips the assertion. Note it **aborts**
rather than reporting a placement error — the crash is a second, separate defect.

**Why it blocks.** Real cores routinely need two independent clocks. Single-PLL-multi-output covers
some of those cases (proven in G4) but not designs needing genuinely independent VCOs.

**Entry point.** `mistral/pll.cc` — the position assignment and clock-injector logic pick a fixed
`FPLL` position and gclk instance instead of allocating a free one per PLL.

---

## G5 — the placer's delay estimate is congestion-blind (and naive recalibration BACKFIRES)

**Status: characterized. The cheap fix was tried and REVERTED — it made things measurably worse.**

**Measured.** `predictDelay`/`estimateDelay` (`mistral/delay.cc:409,421`) are `75·Δx + 200·Δy`;
`getWireDelay` is **0**; `getPipDelay` is, per its own comment, *"guesswork based on average of
(interconnect delay / number of pips)"* — a per-node-**type** table, so every H14 hop scores identically
whether it is direct or a 20-wire detour. Real delays come from `mistral::AnalogSim` in
`getArcDelayOverride`, but that walks the **actual routed PIP chain** and is gated on `bitstream_configured`
— which is not an oversight: you cannot analog-simulate a path that does not exist yet. So P&R optimises
guesswork and only signoff sees reality.

Scraped 958 real routed arcs (delay + endpoint coords from critical-path reports, fabi386 CPU core, 41% COMB):

| | value |
|---|---|
| actual arc delay | mean 1.27 ns, sd 2.44, max 20.56 |
| current model | under-predicts mean by **2.7×** |
| **zero-distance arcs** | **402 of 958** — model says **free**; actual mean 0.28 ns, **max 8.49 ns** |
| delay concentration | top 5% of arcs carry **40%** of all routing delay |
| **R² of distance** | **0.248** — distance explains only a quarter of the variance |
| R² after least-squares recalibration | 0.442 |

**The tempting fix, and why it is wrong.** Least squares gives `0.331·Δx + 0.285·Δy + 0.344` — better
*description* (R² 0.248→0.442), and it exposes three apparent flaws: no constant term, inverted x/y
asymmetry (old weights make y 2.7× costlier; measured is near-parity), slopes too small.

**Substituting it made the placer dramatically WORSE: post-placement Fmax 11.09 → 5.44 MHz**, with routing
also degrading (overuse 369 at iter 72 where the uncalibrated run was converging). Reverted.

**Why it backfired — the transferable lesson.** These coefficients are not a *description* of delay, they
are one term in a **tuned optimisation objective**, balanced against the wirelength term and against
`hpwl_scale_y = 2` (which encodes the same y-penalty the old 200-vs-75 did — flattening one while leaving
the other creates an inconsistency). A constant term also compresses the criticality range, reproducing
exactly the discrimination loss that made `criticalityExponent = 7` ineffective in G1. **Fitting a
descriptive model and dropping it into an optimiser's cost function is not a valid transformation.**

**What is actually missing.** 56% of the variance is congestion, which no distance model can express — the
same-tile arc costing 8.49 ns is a detour, not a distance. Closing it needs a **congestion-aware placement
estimate** (RUDY-style routing-demand, not just cell-slot utilisation, which is what `beta` already covers).
That is real algorithm work, and per the evidence above it must not be attempted without a **calibration
harness**: a way to change one placer cost term and measure *routed* Fmax across several designs. Changing
the objective by reasoning alone is how both this recalibration and the `--tmg-ripup` attempt failed.

**Corroborating evidence that "tighter predicted" ≠ "better routed":** beta 0.40 had *better*
post-placement Fmax than 0.35 (10.02 vs 9.26) and *worse* routed Fmax (6.84 vs 8.30). Any optimiser that
tightens placement against a congestion-blind model is likely to reproduce that inversion — which is the
main reason to be sceptical of wiring in `timing_opt` (`common/place/timing_opt.cc`, called only by ice40)
before G5 is addressed.

---

## Capability audit (2026-08-07) — implicit assumptions made explicit

The IO-ring blocker below was found by *asking* rather than assuming. The same treatment applied to
the other capabilities a video/SDRAM core needs. Each row is one build (and, where it builds, one
silicon load) — cheap, and it moves items out of "should be fine" before anyone depends on them.

| capability | gate | verdict |
|---|---|---|
| **Block RAM (M10K)** | `openflow-test/m10ktest.v` — 1024×16 write, read back, compare | ✅ **PASS ON SILICON.** `DONE=1, ERROR=0`, build-ID verified. Places 2/553 M10K. The framebuffer/line-buffer path is real |
| **Bidirectional IO (tristate)** | `openflow-test/sdrio.v` | ❌ **BLOCKED** — `$_TBUF_` has no bel; pad emitted with `OE` tied to 1. See G6 item 2 |
| **Hard multiplier (DSP)** | `openflow-test/gates/g_dsp.v` | ❌ **BLOCKED** — `ERROR: no BELs remaining to implement cell type 'MISTRAL_MUL18X18'`. Synthesis infers it, nextpnr has no bel. Scalers/gamma/audio filters want these |
| **Two PLLs** | `openflow-test/gates/g_2pll.v` | ❌ **CRASHES** — `Assertion failure: data.bound == nullptr` (`arch.h:345`) during PLL setup. **Pre-existing** (reproduces with `VUP_PLL_LEGACY=1`), not a regression from the G4 work. Blocks any multi-clock core — e.g. SVGA pixel clock + memory clock |
| **IO output registers** | `openflow-test/gates/g_ioreg.v` | ⚠️ **NOT PACKED** — all 16 FFs stay in fabric; nextpnr's GPIO emission writes only `DRIVE_STRENGTH`, `IOCSR_STD`, `USE_WEAK_PULLUP`. Registers in the IO cell are what SDRAM needs for timing. (Ground truth puts IO registers in the **DQS16** block — `INPUT_REG4_SEL` et al — which is the lead to follow) |

**Two of these are new named gaps** (DSP, two-PLL crash) that no prior document mentioned; both are
hard blockers for a real video core, and both were found in minutes rather than mid-port.

### qsf constraint fidelity — **SILENTLY DROPPED** (measured 2026-08-07)

The dangerous failure mode, because the build *succeeds*: constraints are parsed, stored, and never
used. Two builds of the same design whose qsf differ by `IO_STANDARD "2.5 V"`,
`CURRENT_STRENGTH_NEW "4MA"`, `FAST_OUTPUT_REGISTER ON`, `WEAK_PULL_UP_RESISTOR ON` and a
`set_global_assignment` produce **byte-identical bitstreams**. No warning is issued.

Cause, by inspection: `qsf.cc` stores instance assignments into `ctx->io_attr` and `pack.cc` copies
them onto the cell — but `write_io_cell()` in `bitstream.cc` **hardcodes**
`DRIVE_STRENGTH = V3P3_LVTTL_16MA_LVCMOS_2MA`, `IOCSR_STD = DIS`, `USE_WEAK_PULLUP = false` and never
reads them. `set_global_assignment_cmd()` is an empty `// TODO`.

**So every pin gets 3.3 V LVTTL / 16 mA regardless of what the qsf says.** Against the real MiSTer
constraints this is not academic — its SDRAM pin blocks carry:

| assignment | count | consequence of dropping |
|---|---|---|
| `IO_STANDARD` | 33 | wrong bank voltage/standard on a memory bus |
| `FAST_OUTPUT_REGISTER` | 7 | no IO register → timing (ties to the IO-register gap above) |
| `CURRENT_STRENGTH_NEW` | 6 | wrong drive into the SDRAM — signal integrity |
| `WEAK_PULL_UP_RESISTOR` | 5 | forced off; nextpnr writes `USE_WEAK_PULLUP=false` unconditionally |
| `FAST_INPUT_REGISTER` | 2 | no input register → capture timing |

**Severity, measured rather than assumed.** A shipped MiSTer core uses only **two** distinct
`DRIVE_STRENGTH` values — `0x799` (106 pins) and `0x79b` (23 pins) — and the value we hardcode *is*
`0x799`, the majority one. (The minimal Quartus reference uses a third, `0x78c`.) So today's builds
are not uniformly wrong; they are wrong on the minority of pins needing something else — very likely
the memory bank. That is precisely why this went unnoticed, and why the silence matters more than the
default.

**Fixed 2026-08-07 (steps 1–2 of 3):**
- `WEAK_PULL_UP_RESISTOR` is now honoured (was forced `false` unconditionally, overriding 5 real
  assignments). Verified: the constrained build's bitstream now differs at `GPIO USE_WEAK_PULLUP`.
- Every parsed-but-unapplied assignment now emits a **warning** naming the attribute, its value, and
  why it is ignored. Regression-checked: unconstrained designs are byte-identical to before and emit
  no warnings.

**Still to do (step 3): derive the `DRIVE_STRENGTH` mapping by differential.** The vocabulary is tiny
(3 values seen across a shipped core and the reference), so build Quartus references varying
`IO_STANDARD` × `CURRENT_STRENGTH_NEW`, read the emitted value, and build the table — the same method
that produced the phase-shift encoding 4/4. Do this before trusting a memory bus at speed.

**Deliberately NOT done:** `FAST_OUTPUT_REGISTER` / `FAST_INPUT_REGISTER` / `FAST_OUTPUT_ENABLE_REGISTER`
are warned about but not consumed. They are not a qsf problem — they need IO-register packing (ground
truth puts those registers in the **DQS16** block). Consuming the attribute without the packing would
claim an accuracy the flow does not have. `set_global_assignment` stays a no-op: the assignments in
play (`FAMILY`, `DEVICE_FILTER_*`, `QIP_FILE`) have no bitstream meaning.

Still unasked, in rough priority for the SDRAM/SVGA line: DDIO for SDR capture; M10K at depth/width
beyond 1024×16; and M10K true-dual-port (a framebuffer usually wants dual port).

---

## Anti-patterns already measured out — do not re-run these

- **router2 congestion cost-schedule tuning.** Aggressive escalation *diverges* (min 2,018 overuse then
  climbs past 5,575); gentle escalation is stable but worse than default (min 434 vs 326); the default is
  already near-optimal and still does not route without the beta change. Dead lever in both directions.
- **beta sweeps expecting a timing win.** The knob buys routability, not timing, and only at coarse steps.
