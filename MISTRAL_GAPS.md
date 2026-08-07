# nextpnr-mistral: the gaps blocking a real Cyclone V core

Scoped from measurements taken 2026-07-24/25 building the fabi386 i386 core (47,789 ALUTs) for the
DE10-Nano (5CSEBA6U23I7). Synthesis is solved; every remaining blocker is in this repo or libmistral.
Each item below states what was measured, why it blocks, and the entry point — not a wishlist.

## Status at a glance (2026-08-07)

| gap | what it is | status |
|---|---|---|
| **G1** | timing-driven placement ineffective (`criticalityExponent = 7`) | **root-caused**; fix is a *conditional* trade, automated in `critexp_auto.sh` |
| **G2** | `--tmg-ripup` churns without reducing `tmgfail` | **measured broken**; needs work in `router2.cc` |
| **G3** | HPS hard IP: only `mpu_general_purpose` modelled | **open — largest item.** Blocks deploying the real core (needs FPGA2SDRAM + h2f/f2h bridges) |
| **G4** | PLL: reference delivery **and** outclk → clock network | **SOLVED on silicon** (10.066 MHz, `LOCKED=1`). Generalisation beyond the attested pin remains |
| **G5** | placer delay estimate is congestion-blind | **characterized**; the cheap fix was tried and REVERTED (measurably worse) |
| **G6** | SDRAM controller path unproven in the open flow | **open**, mostly leverage; clocking gate lifted by G4, phase-shifted taps still unproven |

Ordering note: sections run G1–G4, then G6, then G5 — G5 is placed last because it is characterization
with a *negative* result rather than an actionable blocker. Do not reorder without reading G5 first.

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

## G6 — SDRAM (board-module) controller path: unproven in the open flow, but mostly leverage

**Status: added 2026-08-06. Not a nextpnr feature gap like G3/G4 — a flow-capability gap: nothing has
ever driven the MiSTer SDRAM module through the open flow, and it is the cheapest route to real
fabric-accessible memory (no HPS needed).**

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
2. **IO ring completeness:** the module wants bidirectional DQ with output/input registers in the IO
   cells (and DQM/address/control at speed). nextpnr-mistral's GPIO support covers plain IO
   (blinky); IO-register packing / DDIO for SDR data capture must be verified — entry point:
   `mistral/io.cc` + libmistral GPIO bmux config.
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

## Anti-patterns already measured out — do not re-run these

- **router2 congestion cost-schedule tuning.** Aggressive escalation *diverges* (min 2,018 overuse then
  climbs past 5,575); gentle escalation is stable but worse than default (min 434 vs 326); the default is
  already near-optimal and still does not route without the beta change. Dead lever in both directions.
- **beta sweeps expecting a timing win.** The knob buys routability, not timing, and only at coarse steps.
