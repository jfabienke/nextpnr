# nextpnr-mistral: the gaps blocking a real Cyclone V core

Scoped from measurements taken 2026-07-24/25 building the fabi386 i386 core (47,789 ALUTs) for the
DE10-Nano (5CSEBA6U23I7). Synthesis is solved; every remaining blocker is in this repo or libmistral.
Each item below states what was measured, why it blocks, and the entry point — not a wishlist.

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

## G4 — PLL output → clock network is not in the routing graph

**Status: genuine libmistral chipdb RE residual, not a nextpnr extension.**

**Measured.** The FPLL bel is implemented and works on the input side: `mistral/pll.cc` +
`pack.cc`/`bitstream.cc` (`2ffa7bc`, direction fix `248c655`). The refclk half routes end-to-end
(input pin → `MISTRAL_CLKBUF` → GCLK → SCLK → PMUX → PLL) and emits a valid 1.95 MB `.rbf` whose 22 FPLL
config bmuxes are `fplldump`-verified (M=20, N bypass, C={100,40,10}, analog constants).

The **output** side has no routing port at any FPLL position — enumerated every port at every pos. libmistral
stores the PLL-output→clock-tree path (`CMUX_PLLIN` → CMUX → GCLK) as a **bitstream mux, not as routing-graph
edges**; its `cmux_*_link_table`s are never consumed. So a full `altera_pll` test fails with "No wire found
for port outclk".

**Fix.** Add the `CMUX_PLLIN`→CMUX→GCLK edges to the libmistral routing graph (upstream: Ravenslofty/mistral).

**Workaround.** The fabric can be clocked directly from an input pin (CLKBUF→GCLK, proven by the refclk
route), so a **single-clock** core builds today; only multi-clock freq-synthesis needs G4.

**REVISED 2026-08-06 — ground-truth decode changes the mechanism.** New instrument
`openflow-test/cmuxdump` (fplldump pattern: `bmux_get()` filtered to CMUX blocks, each `INPUT_SEL`
decoded through the link tables). Swept three independent Quartus-fitted designs with PLL-driven
clocks — ao486_20170803, ao486_20220914/20240616, and the fitted fabi386 (`f386_mister.rbf`):

- **Zero direct `{PLLIN,k}` INPUT_SEL selections in any design.** The link tables' PLLIN entries are
  not how Quartus puts a PLL output onto GCLK on this die.
- The consistent mechanism is **two-level**: `INPUT_SEL = 0x6 → {NCLKPIN_SEL_2, n}` (a per-gclk
  *selector line*), plus `CLKPIN_SEL_0/_2` bmuxes (observed values `0x1`, `0x5`) choosing what the
  selector lines carry — i.e. the `CMUX_PLL_SEL_0/1`-style indirection the earlier RE noted in the
  rmux vocabulary. CMUXVG adds a `CLK_SELECT_C/_D` layer (observed `0x2`).
- The residual is therefore **narrow and named: the CLKPIN_SEL_x / CLK_SELECT_x value encoding**
  (which value selects which PLL counter vs. which clock pin). Two values observed so far; the
  clean derivation is 2–3 targeted Quartus diff builds (same design, only the PLL-counter→GCLK
  assignment changed) through the existing quartus_jobs pipeline, diffed with `cmuxdump`.

Implementation stays gated on that value table — emitting guessed selector values would violate the
"every number from a real command" rule. Entry point once derived: CLKBUF-precedent bel whose
bitstream emission programs `INPUT_SEL=0x6` + the derived `CLKPIN_SEL_x` value (nextpnr-side only;
no libmistral graph surgery needed for v1).

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
1. **Clocking (ties to G4):** every proven controller phase-shifts the SDRAM clock vs the fabric
   clock (PLL output tap or -phase clock). Without G4 the open flow has a single pin-driven clock —
   a low-MHz controller variant may run degraded; full-rate needs the PLL outclk path. G4 first.
2. **IO ring completeness:** the module wants bidirectional DQ with output/input registers in the IO
   cells (and DQM/address/control at speed). nextpnr-mistral's GPIO support covers plain IO
   (blinky); IO-register packing / DDIO for SDR data capture must be verified — entry point:
   `mistral/io.cc` + libmistral GPIO bmux config.
3. **Constraint fidelity:** the MiSTer `.qsf` pin set for the SDRAM bank (drive strength, fast
   output register) must survive the qsf path.
4. **Silicon validation:** MemTest-style pattern check as the acceptance gate, deployed over the
   existing HPS deploy path — the measured verify model already exists (`deploy.rs`).

**Path.** Port the MiSTer `sdram.sv` (smallest proven variant) + its qsf pin block into the open
flow at conservative clocking → silicon MemTest → raise the clock once G4 lands phase-shifted
outputs. Success criterion is a silicon-verified memory test through the open flow, not "it routes."

**Module design (added 2026-08-06).** Ship the controller as standard slice modules, two variants:
**16-bit** (one module — the common MiSTer analog-board case) and **32-bit** (dual/ganged modules).
Nice-to-have features, in priority order:
1. **Capacity auto-detect** — standard row/col/bank aliasing probe at init (write-pattern address
   folding), reporting detected geometry (32/64/128MB) instead of a build-time parameter.
2. **Frequency capability probe** — trial the clock ladder (G4-gated) with the self-check as the
   pass gate per step; report the highest stable rate rather than assuming one.
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
