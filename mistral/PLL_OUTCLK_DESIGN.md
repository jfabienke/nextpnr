# G4 design — PLL outclk → global clock network (nextpnr-mistral)

**Status: designed 2026-08-06, evidence-based; implementation staged below. Supersedes the original
"add link-table edges to libmistral" plan — the research changed the mechanism twice.**

Instruments built for this design (in `../openflow-test/`, compile recipe in each header):
`cmuxdump` (CMUX bmux state of an RBF, INPUT_SEL decoded through the link tables) and `p2pdump`
(libmistral port-to-port map filtered to FPLL). All claims below trace to their output or to
`mistral/docs/srcdoc/*`.

---

## 1. The mechanism, as the evidence shows it

Three selector layers stand between an FPLL output counter and a global clock line:

```
 FPLL(pos).PLLCOUT[c]                      the C-counter output (bmux-configured, already works)
        │  dedicated wiring — IN LIBMISTRAL'S P2P TABLES (p2pdump: 250 links for 5CSEBA6U23I7)
        ▼
 CMUX<kind>(pos).PLLIN[k]                  e.g. FPLL(0,0).PLLCOUT[5] → CMUXVG(42,0).PLLIN[0]
        │  INPUT_SEL — "clock mux main input selector" (srcdoc/cmux-doc.txt);
        │  link tables map entry index e → source: cmuxvg row: entries 8..23 = {PLLIN, 0..15}
        ▼
 gclk instance g of that cmux              (CMUXHG/VG: 4 gclk instances each; CR/HR/VR: regional)
        │  CLK_SELECT_A..D / dynamic_clk_select — a further per-instance layer (switchover);
        │  PLL_FEEDBACK_ENABLE_x + PLL_MCNT0 — the feedback-path config (observed SET in fabi386)
        ▼
 GCLK[g] → SCLK → HCLK/BCLK → TCLK         (already modelled + routed by nextpnr today)
```

**What is now certain (offline-derived, no Quartus needed):**
- The **(FPLL pos, counter c) → (cmux pos, PLLIN line k)** map is in `get_all_p2p()` (`p2pdump`
  lists it; `srcdoc/p2p-doc.txt` names it "PLL counter output to clock mux").
- The **PLLIN line k → INPUT_SEL entry index e** map is the compiled link tables
  (`cmux*_link_table[instance][entry] = {CMUX_PLLIN, k}`).
- Composing the two gives, for any (PLL position, counter): the exact `INPUT_SEL` value that selects
  it at each reachable cmux gclk instance. This is the whole "routing" of G4 — it is configuration,
  not graph search.

**What ground truth complicates (cmuxdump over 3 Quartus PLL designs — 2× ao486 + fitted fabi386):**
- Quartus sets **zero** direct `{PLLIN,k}` INPUT_SELs and zero `PLL_SEL_x` — its PLL-correlated
  writes are `CLK_SELECT_C/D = 0x2` (on the GCLK-root CMUXVG) and
  `PLL_FEEDBACK_ENABLE_3 = PLL_MCNT0`. Quartus evidently drives GCLK from the PLL through the
  **CLK_SELECT (switchover) layer**, with INPUT_SEL left at defaults.
- The direct `{PLLIN,k}` INPUT_SEL entries nonetheless exist in the hardware's link tables. Whether
  the direct path works standalone (without the CLK_SELECT layer), or CLK_SELECT must be set to pass
  the chosen input through, is **the one remaining semantic unknown**.

## 2a. IMPLEMENTED 2026-08-06 — and the open question answered offline

The v1 below is implemented and V1/V2-verified. During V1 the self-check exposed a decoder bug in
`cmuxdump` (INPUT_SEL is an **r-type** bmux; the tool had read the scalar `.s` field), and the
corrected decode overturned the ground-truth reading in §1: **Quartus DOES use the direct
`INPUT_SEL={PLLIN,k}` path** — the fitted fabi386 shows 7 direct PLLIN selections; the earlier
"two-level CLKPIN_SEL" mechanism was a misdecode artifact. So the open question is closed without
silicon: our emission mechanism matches Quartus 1:1.

Verified V1 (self-check): `plltest.v` (50 MHz pin -> altera_pll -> 10 MHz C5 -> LED divider,
locked on LED[7]) routes end-to-end (exit 0, 1.95 MB `.rbf`) — the previously-impossible
"No wire found for port outclk" case; decode shows exactly the intended
`CMUXVG(42,0) INPUT_SEL[0]=0x10 -> {PLLIN,8}` (= FPLL(0,14) C5) + M=20/VCO=1000/C5=100 config.
Verified V2 (shape): same INPUT_SEL={PLLIN,k} r-type mechanism as the Quartus ground truth.
V3 (silicon lock + blink) pending board availability. Implementation notes: only C4..C8 reach the
global cmuxes, so pack remaps logical outclk[i] to a physical counter (PLLCLK_PHYS_i) and pre-binds
PLL + injector; a default-BelId sentinel collides with the real (0,0) bel — found-flags required.


## 2b. SILICON STATUS 2026-08-06 — outclk routing WORKS, PLL does not yet LOCK (honest)

Five silicon rounds on the DE10-Nano. **The G4 mechanism itself is validated**; the blocker is the
FPLL analog/feedback configuration, which is upstream of G4's scope.

**What silicon PROVED (new facts, each from a real load):**
1. **The clock-network leg works.** A counter clocked through `MISTRAL_CLKBUF` -> GCLK (no PLL)
   blinks correctly (`clkbuftest.rbf`). First silicon validation of that path — it is the leg the
   PLL refclk depends on, and it is sound.
2. **The compressed-RBF requirement still governs.** An uncompressed `.rbf` configures
   (`fpga_manager: operating`) yet leaves ALL user IO dead — reproduced exactly (this is the
   already-known root cause; the tooling default must stay compressed).
3. **The VCO starts and dies.** Rounds 2/4/5 left the LED counter FROZEN at different values each
   time (photographed) — i.e. the counter received a few hundred thousand to a few million edges
   and then the clock stopped. Not "no clock": an unstable/collapsing loop. Round 3 (integer recipe)
   produced no edges at all.
4. **Recipes are matched sets.** Mixing the fractional PLL's loop constants with different N/M
   silicon-failed; so did the integer recipe. FPLL fields are now at byte-parity with the fitted
   fabi386's PLL(0,0) except the C dividers (verified by `fplldump` diff) and it still does not lock.

**Open blockers (in priority order):**
- **B1 — the MCNT feedback loop.** Ground truth closes it at the CMUX
  (`PLL_FEEDBACK_ENABLE_3 = PLL_MCNT0` on CMUXVG(42,0) for FPLL(0,0)); we emit `FBCLK_MUX_2=1` on
  the FPLL side. The cmux-side enable is implemented but has NEVER REACHED SILICON, because:
- **B2 — the placer migrates the PLL.** Pack chooses FPLL(0,0) (the position whose feedback mapping
  is ground-truth-known) but the design is emitted at FPLL(0,14). `STRENGTH_LOCKED`, `STRENGTH_USER`,
  an `isBelLocationValid` pin, and the `BEL` attribute have each been tried; the heap placer still
  relocates it (BEL-attr path additionally hits a name-format mismatch in `getBelByName`). **Fix
  this first — every feedback experiment is invalid until the PLL provably lands where pack chose.**
- **B3 — `CTRL_OVERRIDE_SETTING`** cannot be written through any of `bmux_r/b/n_set` (silently
  absent from the emitted rbf); ground truth sets it on every active PLL.

### The instrument problem is SOLVED (2026-08-06) — `tlmtest` + `measure_clocks.sh`

Watching LEDs was the wrong instrument: a frozen counter and a running one look identical in a
photo, and five rounds burned on ambiguous readings. Replaced with a **self-validating frequency
measurement over SSH**, using the one HPS interface nextpnr already models and that is
silicon-proven (`cyclonev_hps_interface_mpu_general_purpose`):

- `tlmtest.v` runs TWO counters — a reference clocked straight off the 50 MHz pin (exactly like the
  proven blinky) and one clocked by the PLL outclk — and feeds `{cnt_pll[31:16], cnt_ref[31:16]}`
  into `gp_in`, which the ARM reads at **gpi = 0xFF706014**.
- `measure_clocks.sh` samples gpi twice T seconds apart and prints **both clocks in MHz**.
- The reference channel **validates the instrument in the same load**: ~50 MHz there means a 0 on
  the PLL channel is a genuinely dead clock, not a broken measurement.

Measured (T=2s, two independent loads, cmux MCNT feedback enabled in the second):

| channel | reading | verdict |
|---|---|---|
| REF (50 MHz pin) | **50.33–50.36 MHz** | instrument sound |
| PLL (outclk) | **0.000 MHz** | zero edges — the PLL does not start |

Note the emitted PLL landed at **(89,0)**, not the pack-chosen (0,0) — B2 again, and it means these
two runs still did not test the ground-truth-mapped position. B2 is now the *only* thing standing
between us and a valid feedback experiment.

**B2 attempts that FAILED (all measured, do not re-run):** `STRENGTH_LOCKED` / `STRENGTH_USER`
binds (placer migrates anyway), an `isBelLocationValid` pin (placer cannot satisfy → 10001-attempt
timeout), the `BEL` attribute (`getBelByName` name-format mismatch), and an
`isValidBelForCellType` pin to (0,0) (the corner tile is never offered by the placer's radius
search → "no BELs remaining"). **The remaining idea: assign the PLL/PLLCLK pair AFTER placement**
(a post-place arch hook) so the choice is derived from where the placer actually put the PLL,
instead of trying to force the placer to honour a pre-made choice.

### B2 RESOLVED 2026-08-06 — post-placement assignment

`Arch::fixup_pllclk_placement()` (pll.cc), called at the end of `Arch::place()`: read where the
placer actually put the FPLL, then re-bind each PLLCLK injector to a cmux gclk instance the
dedicated wiring can feed *from that position*, recording the physical counter for the FPLL
emission. Inverting the dependency sidesteps all four failed pinning approaches.

**Verified self-consistent for the first time:** PLL emitted at (89,0) with
`CMUXVG(42,0) INPUT_SEL=0xf -> {PLLIN,7}` — exactly the p2p wiring from FPLL(89,0) C5 — plus
`PLL_FEEDBACK_ENABLE_3=PLL_MCNT0`. The bitstream now says one coherent thing.

### Measured negative results (P6 — do not re-run)

| experiment | result |
|---|---|
| consistent assignment + cmux MCNT feedback enable | PLL **0.000 MHz** |
| `CLKIN_0_SRC` sweep over 0x00,0x01,0x02,0x03,0x05 | PLL **0.000 MHz** at every value |

So the reference-select hypothesis is refuted, and the PLL still never starts even with a coherent
bitstream. **Caveat on the sweep:** consecutive `load_core`s were issued without a reboot and the
REF channel read an identical 50.725 MHz for all five, which is consistent with quantisation but
does not *prove* each variant actually reconfigured. Before trusting any future sweep, add a
**build-ID channel** to the telemetry word (a few constant bits that differ per variant) so the
harness proves which bitstream is live — a cheap fix that makes sweeps self-verifying.

### PLL RESEARCH 2026-08-06 — the FPLL tile config is EXONERATED; the fault is refclk DELIVERY

**1. Ground truth uses two coherent recipe FAMILIES; fields must never be mixed** (fplldump over the
fitted fabi386's three active PLLs):

| | fractional — PLL(0,0), PLL(0,55) | integer — PLL(89,0) |
|---|---|---|
| N | bypassed, dividers 0 | 6 (3+3), NOT bypassed |
| M | 8 | 148 |
| DSM / frac | `DSM_OUT_SEL=1`, design-specific frac | off, frac = 1 |
| `FBCLK_MUX_2` / `VCO_DIV` / `SLF_RST` | set | absent |
| `BWCTRL` | 0x07 | 0x03 |
| `CP_CURRENT` / `NREVERT_INVERT` | — | 0x01 / 1 |
| invariant in both | `CTRL_OVERRIDE=0`, `CNT_IN_SRC=0`, `TCLK_SEL=0`, `LOCK_FILTER=0x19`, `UNLOCK=0x02`, `CLKIN_0_SRC=0x04` | |

Rounds 1–6 each emitted a **hybrid** of the two — and `CP_CURRENT` (the charge-pump drive, without
which the loop cannot be pulled to lock) was silently lost when the fractional recipe replaced the
integer one. v1 now emits the INTEGER family verbatim, matching the tile the placer lands in.

**2. A byte-identical clone of a WORKING ground-truth PLL still produces 0 Hz.** With
`VUP_PLL_GT_CLONE=1` the emitted FPLL(89,0) tile is field-for-field identical to the fitted
fabi386's own integer PLL — zero extra fields, zero value differences, only GT's unused C0 counter
and the unwritable `CTRL_OVERRIDE` remain — and the measured output is still **0.000 MHz** (REF
channel sound at 50.33 MHz). **This exonerates the entire 141-field FPLL configuration space.** The
fault is therefore in the *delivery* of the reference clock INTO the PLL, or of the output OUT of it.

**3. Why the reference is the prime suspect — and the concrete blocker.** libmistral's p2p tables
show the FPLLs have *dedicated* CLKIN inputs, fed only from GPIO positions (32,0) (40,0) (56,0)
(64,0) (89,23) (89,25) (32,81) (40,81). **None of the DE10-Nano's three 50 MHz clock pins is one of
them** — FPGA_CLK1_50 = V11 = GPIO(10,17), CLK2 = Y13 = GPIO(12,19), CLK3 = E11 = GPIO(10,4)
(`pinfind`). So on this board a PLL reference **must** arrive over the clock network
(pin → CLKBUF → GCLK → SCLK → PMUX → `CORECLK0`), which is exactly what nextpnr models as the
refclk bel pin — but `CLKIN_0_SRC` selects among the *dedicated* sources and has been swept and
refuted (0x00–0x05, all 0 Hz). **The unidentified item is the mux field that selects the
core/PMUX reference instead of a dedicated pin.** Unexercised candidates from the 141-field
vocabulary: `sw_refclk_src`, `src`, `bypass_en`, `m_cnt_in_src`, `dll_src`.

**4. The targeted RE that answers it (one Quartus diff pair, now a sharp question):** build the same
PLL design in Quartus twice — once with the reference on a dedicated PLL clock pin, once with it
arriving from core/general routing — and diff the FPLL bmuxes with `fplldump`. Exactly one field
family should change: that is the field. This is far narrower than the earlier "2–3 diff builds"
plan because the tile config and the output path are both now eliminated.

### 2026-08-06 (later) — the planned Quartus diff pair is UNBUILDABLE, and the real difference is the CLKBUF

Two findings while staging the NAS job; the second makes the job unnecessary for now.

**(a) There are no bonded dedicated PLL clock pins on this package.** `pinat` (new tool) enumerates
the package pin table against the FPLL-dedicated CLKIN GPIO positions and finds **zero** signal pins
there (only `VCCA_FPLL` power pins exist). So the planned A/B — "PLL reference on a dedicated pin vs
from core routing" — **cannot be built on 5CSEBA6U23I7**, and it follows that the fitted fabi386's
own working PLLs already take their reference over the clock network. Do not spend a Quartus cycle
on that pair.

**(b) The real difference is how the clock ENTERS the network (CMUXHG INPUT_SEL).**

| CLKBUF instance | ground truth | ours |
|---|---|---|
| 0 | `INPUT_SEL=0x0 -> {CLKPIN_SEL_0,0}`, with `CLKPIN_SEL_0=0x1` | — |
| 1 | `INPUT_SEL=0x2 -> {CLKPIN_SEL_2,1}`, with `CLKPIN_SEL_2=0x5` | — |
| 2 | — | **`0x1b -> {CLKIN,2}` (general routing)** |
| 3 | `0x1b -> {CLKIN,2}` (general routing) | — |

Ground truth drives the clock network from **dedicated clock PINS** (`CLKPIN_SEL_x`) and uses the
general-routing entry only for one spare instance. nextpnr's `write_clkbuf_cell` hardcodes `0x1b`
("hardcode to general routing", bitstream.cc). A general-routing-sourced GCLK demonstrably works for
**fabric** loads (`clkbuftest` blinks on silicon) — but it is now the *only* remaining structural
difference between our bitstream and a working one, and therefore the prime suspect for why the
PLL's PMUX/`CORECLK0` reference never arrives.

**Next experiment — no Quartus needed:** emit the CLKPIN-sourced form for the refclk CLKBUF
(`INPUT_SEL` = the `{CLKPIN_SEL_x, n}` entry plus the matching `CLKPIN_SEL_x` bmux) instead of
`0x1b`, and measure. The one unknown is which CLKPIN index the board's 50 MHz pin lands on; the
link tables enumerate the candidates, so a short sweep over them (with the telemetry harness, which
makes each trial ~90 s) resolves it. **Add the build-ID channel first** so the sweep proves which
bitstream is live.

**If that sweep fails, THEN the NAS job worth running** is not a diff pair but a single minimal
Quartus PLL design for this board — it would hand us the pin -> CLKPIN_SEL mapping and the complete
cmux+FPLL configuration for a trivial 50 MHz -> N MHz PLL, as a minimal reference to diff against
(the fitted fabi386 is a large, noisy reference by comparison).

**Next experiment (cheap, decisive):** fix B2, confirm via `fplldump` that the emitted PLL tile is
(0,0), then reload. If it still does not lock, bisect the feedback by cloning the ENTIRE ground-truth
PLL(0,0) tile bit-for-bit (including the C dividers) so the only variable left is our cmux/PLLCLK
emission — a design that produces the GT's own clock frequency is an acceptable proof for G4.

### 2026-08-06 (later still) — the CLKBUF sweep is a measured NEGATIVE, and a 20-core corpus reframes the fault

**(a) The CLKBUF-source hypothesis is refuted on silicon.** Six variants, each with the build-ID
channel and a reboot between loads, all verified `LOAD OK`:

| CLKBUF config | REF | PLL |
|---|---|---|
| baseline general routing `0x1b` | 50.332 MHz | **0.000** |
| `INPUT_SEL=0`, `CLKPIN_SEL_0=0x1` (the exact GT form) | 50.332 MHz | **0.000** |
| `INPUT_SEL=0`, `CLKPIN_SEL_0=0x5` | 50.332 MHz | **0.000** |
| `INPUT_SEL=2`, `CLKPIN_SEL_2=0x5` | 0.000 | 0.000 |
| `INPUT_SEL=6`, `CLKPIN_SEL_2=0x5` | 0.000 | 0.000 |
| `INPUT_SEL=2`, `CLKPIN_SEL_2=0x1` | 0.000 | 0.000 |

The REF column proves the override is *live* (entries 2/6 kill the clock network outright), and the
REF counter shares the overridden CMUXHG with the PLL's refclk — so in rows 1/2/5 a counter on that
very node is running at 50 MHz while the PLL emits nothing. **Refclk delivery is exonerated too.**

**(b) A 20-core ground-truth corpus (new tools: `pmuxdump`, `bmuxhist`, `bmuxdiff`).** Every real
MiSTer core on the SD card is a Quartus bitstream with working PLLs. Scanning 20 of them:

- **all 20 drive ZERO `PMUX` nodes**, yet every one has `CLKIN_0_SRC=0x04` — so the reference does
  not arrive over the routed core-clock path that Mistral models as `CORECLK0`→`PMUX`. Ours is the
  only bitstream in the corpus that drives a PMUX.
- **all 20 have an active FPLL at (89,0)** — exactly where nextpnr places ours — so the position is
  not the problem either.
- two block types appear in every core and never in ours: `HPS_CLOCKS` (3 settings) and `CMUXVR`
  (1 setting), identical across cores — board-level constants of the MiSTer template.

**(c) THE BUG: `CTRL_OVERRIDE_SETTING` was never emitted.** An escaped `\n` inside a `//` comment had
swallowed the emit statement, so the line was inert comment text; the call also used the wrong setter
(the field is `MT_BOOL`, and `bmux_r_set`/`bmux_n_set` silently no-op on it). Consequence, confirmed
by `fplldump --all` on our own rbf:

> **its default is 1, and all 20 Quartus cores explicitly clear it to 0 at all three PLL positions.**

We shipped a PLL with its control-override bit asserted. Fixed with `bmux_b_set(..., 0, false)`;
`bmuxdiff` now shows no `CTRL_OVERRIDE_SETTING` delta against ground truth at (89,0). The remaining
FPLL deltas are all fractional-family fields (GT's PLL is fractional, ours integer) plus `SLF_RST`
and `VCO_DIV`, which are behind `VUP_PLL_SLF_RST` / `VUP_PLL_VCO_DIV` pending silicon.

**Lesson worth keeping:** "config space is exonerated" rested on a byte-identical GT clone. That
clone was byte-identical *in the fields we emit* — it could never have caught a field we never emit
at all. Diff against ground truth over the **full** non-default set, not the set you wrote.

### 2026-08-06 (round 8) — recipe rebuilt on the corpus; the FPLL tile is now PROPERLY exonerated

Clearing `CTRL_OVERRIDE_SETTING` alone did not start the PLL (`PLL=0.000`, `LOAD OK`). Diffing our
`FPLL(89,0)` against **Apogee**, the one integer-family PLL in the corpus and at our own position,
found three more substantive errors — all of them invented values that ground truth contradicts:

| | ours (before) | ground truth (20 cores) |
|---|---|---|
| feedback | no `FBCLK_MUX_2`; `PLL_FEEDBACK_ENABLE_3=PLL_MCNT0` at CMUXVG(42,0) | `FBCLK_MUX_2=1`; **`PLL_FEEDBACK_ENABLE_*` in ZERO cores** |
| VCO | 1233 MHz (N=6, M=148) | **~400–500 MHz in every core** (Apogee: N=5, M=48 → 480 MHz) |
| `BWCTRL` / `CP_CURRENT` | overridden to 0x03 / 0x01 | **not overridden — defaults** |

The feedback was exactly inverted, and the "CP_CURRENT is REQUIRED, the charge pump drives the loop"
note was a guess. Recipe now tracks Apogee; `VUP_PLL_GT_CLONE` clones Apogee and `bmuxdiff` confirms
the emitted tile is **byte-identical** to it.

**Silicon (build-ID verified, reboot between loads):**

| variant | expected | REF | PLL |
|---|---|---|---|
| `CTRL_OVERRIDE=0` | — | 50.332 MHz | **0.000** |
| `+ SLF_RST=3, VCO_DIV=0` | — | 50.332 MHz | **0.000** |
| Apogee clone (byte-identical) | 96.0 MHz | 50.332 MHz | **0.000** |
| new recipe, VCO 480 MHz | 10.0 MHz | 50.856 MHz | **0.000** |

A tile that is bit-for-bit a PLL known to run on this silicon, at that PLL's own position, still
produces nothing. **The FPLL tile configuration is now exonerated on a correct reference** — and the
one structural difference the corpus leaves standing is the one it has pointed at all along:

> 20 of 20 shipped cores drive **zero** PMUX nodes. Ours is the only bitstream in the corpus that
> routes its reference over `CORECLK0`→`PMUX`. Whatever delivers the reference in ground truth is
> not a routed core clock, and is not a field of the FPLL block.

### 2026-08-06 (round 9) — the blocker is a MODELLING GAP, not a config value

First, a correction to round 8's own statistic. `route_all_active_links()` returns nothing for a mux
left at its **default**, so "20/20 cores drive zero PMUX" only means *ground truth never overrides
the PMUX default*. New tool `pmuxinfo` closes that hole by reading the mux value directly:

```
PMUX.077.000.0000   ours:   val=0x90 def=0x21  (OVERRIDDEN)  -> SCLK.077.000.0004
PMUX.077.000.0000   Apogee: val=0x21 def=0x21  (AT DEFAULT)  -> <none>
```

**The PMUX default `0x21` = 33 is out of range of its 30 sources — it selects nothing.** So ground
truth does not merely decline to override the PMUX; its PLL reference genuinely does not arrive
through `CORECLK0`/`PMUX` at all. The caveat does not rescue the earlier reading, it sharpens it.

Second, the device model's own answer for what *can* reach `FPLL(89,0)` (all 44 p2p edges into any
FPLL):

| input port | driven by | bonded on 5CSEBA6U23I7? |
|---|---|---|
| `CLKIN[0..3]` | GPIO(56,0), GPIO(64,0), GPIO(89,25), GPIO(89,23) | **no** — `pinat` finds no package pin at any FPLL-CLKIN GPIO position |
| `FBLVDS_IN0` | CBUF(87,0), CBUF(89,2) | CBUF is configured in **zero** of the 20 cores |
| `DB_IN0` | GPIO(89,23) | same unbonded position |
| `CORECLK0` | PMUX ← SCLK ← GCLK | modelled, and **the one we use** |

And the board's clock pin is not among them: `pinfind V11` → `GPIO(10,17)`, feeding **no** FPLL CLKIN.

So every reference input the model exposes has now been tried or excluded, the FPLL tile is
byte-identical to a PLL known to run at this position, `CLKIN_0_SRC` has been swept across its whole
range, and the clock network is proven alive by a counter on the PLL's own refclk node. The
conclusion this evidence supports is not "one more field to find" but:

> **libmistral/nextpnr's model of the Cyclone V PLL reference path is incomplete.** The path every
> shipped core actually uses is not in the routing graph, not an FPLL bmux, and not a bonded
> dedicated pin. It is configured somewhere we have not identified — the blocks ground truth writes
> and we never do are `HPS_CLOCKS(51,80)`, `CMUXVR(42,81)`, `CMUXVG(42,81)`, and the CMUXHG/CMUXVG
> instances at (0,35)/(89,35)/(42,0) that GT drives beyond the ones we drive.

This is a lead's call, not a sweep: see MISTRAL_GAPS.md for the options.

### 2026-08-07 (round 10) — the minimal Quartus reference names the root cause

Built the reference the gap table asked for: one PLL, 50 MHz → 96 MHz, nothing else, on the NAS
(`admin@192.168.50.100`, container `quartus`, Quartus Prime Lite 17.0.2). `altera_pll` elaborates
directly from Verilog — no Qsys needed. Its fit report is unambiguous:

```
Reference Clock Sourced by : Dedicated Pin
CLKIN(0) source            : FPGA_CLK1_50~input
PLL VCO Frequency          : 480.0 MHz
PLL Operation Mode         : Normal
```

and the bitstream agrees. The entire design has **one** non-default clock mux —
`CMUXHG(0,35)[0] = 0x16 → {PLLIN,14}`, which is the PLL's *output* — and its complete 24-link clock
network contains **no path into any PLL**. The 50 MHz pin never enters the clock network at all.

> **nextpnr's whole refclk model is wrong-headed.** We build
> `pin → CLKBUF → GCLK → SCLK → PMUX → CORECLK0`; the silicon uses a hardwired `pin → CLKIN(0)` that
> needs no bitstream configuration. That is why twelve trials with a byte-identical FPLL tile
> produced nothing: the PLL was listening to a pin we never fed.

**Why every offline check missed it:** libmistral's p2p table maps `FPLL(0,14).CLKIN[0]` to
`GPIO(32,0)`, but the pin is at `GPIO(10,17)` (`pinfind V11`). The table does not carry this edge for
5CSEBA6U23I7 — which is also what made `pinat` report "no bonded dedicated PLL clock pins" and cancel
the original NAS job. The negative was an artefact of the model, not of the silicon.

**Two further corrections it forced:**

- **The feedback, again.** Round 8 concluded from the 20 shipped cores that `FBCLK_MUX_2=1` with no
  cmux feedback was correct. The minimal reference — same device, same pin, same instantiation as
  ours — does the *opposite*: `PLL_FEEDBACK_ENABLE_0 = PLL_MCNT0` at `CMUXVG(42,0)`, no
  `FBCLK_MUX_2`. The shipped cores are fitted in a different operation mode; generalizing from them
  was wrong. The index also tracks the cmux GCLK instance rather than being the hardcoded `_3`.
- **The VCO, confirmed.** Quartus's own VCO for 50 → 96 MHz is **480.0 MHz** — exactly what round 8's
  rebuilt recipe derived from the corpus band.

**Instrument gap closed.** The harness could not distinguish "never locked" from "locked but the
output does not reach the counter", so every `0.000 MHz` was ambiguous between the two halves of G4 —
and I had been assuming the first without measuring it. `gp_in` is now
`{BUILD_ID[7:0], locked, cnt_pll[31:21], cnt_ref[31:20]}`; `locked` is a level, so it reads even with
no working clock in the design.

**Silicon after all of the above** (build-ID verified, reboot between loads):

| variant | REF | PLL | LOCKED |
|---|---|---|---|
| PLL relocated to FPLL(0,14) (`VUP_PLL_POS`) | 50.332 MHz | 0.000 | — |
| + `PLL_FEEDBACK_ENABLE_0`, no `FBCLK_MUX_2` | 50.332 MHz | 0.000 | — |
| + refclk left unrouted (`VUP_PLL_NO_REFCLK_BUF`) | 50.332 MHz | 0.000 | **0** |

`LOCKED=0` is now measured: the PLL never starts, so G4a's output path is neither validated nor
implicated. The emitted `FPLL(0,14)` differs from the reference only in which C-counter the design
asks for.

### 2026-08-07 (round 11) — PRAM is where the unnamed bits live, and the FPLL's own chain is NOT the answer

Two corrections to the round-10 plan, both from measurement:

1. **A raw CRAM diff cannot see PLL configuration.** libmistral keeps `pram[32]` separate from
   `cram`; periphery/FPLL config lives in PRAM. A windowed `cramdiff` over the FPLL(0,14) tile
   reports "0 differing bits" — which means the window does not cover the block, **not** that the
   tiles agree (`bmuxdiff` shows the counters differ). Do not read that zero as equality.
2. **Mirroring the reference exactly still does not lock.** With the clock pin driving *only* the PLL
   (REF counter removed, so the pin has no fabric load at all, as in the reference): `LOCKED=0`,
   `LOAD OK`. The full non-logic diff at that point is the C-counter, LED drive strength, and
   `PL_AUX_BG_POWERDOWN` on an unused PLL — nothing on the reference-clock side.

New instruments: `pramdiff` (per-chain PRAM diff) and `pramown` (attributes each chain to its owning
blocks — the index is `pram[(base >> 16) & 31]`, with `base & 0xffff` the bit offset, from
`fpll2pram`/`cmuxh2pram`/... ). Reference vs ours: **219 differing PRAM bits**, of which `bmuxdiff`
names only the counter choice.

| chain | owners | differ | set only in reference |
|---|---|---|---|
| 13 | **FPLL(0,14)** + FPLL(0,31), HSSI(0,23), PMA3 | 14 | 7 |
| 12 | **CMUXH(0,35)** + HSSI(0,35), HIP(1,56) | 65 | 63 |
| 5 | CMUXH(89,35) + TERM(89,27) | 64 | 61 |
| 4 | CBUF(87,0), LVL(89,17), SERPAR, TERM(89,5) | 72 | 32 |
| 1, 10 | CMUXV(42,0)/LVL, FPLL(0,73)/CMUXC/DLL/CBUF | 3, 1 | 3, 1 |

**Transplant results** (`prampatch`, donor = the minimal Quartus reference; note it also moves
`oram[5]`/`oram[7]` unconditionally, so each trial carries *more* reference config than named):

| transplant | REF | LOCKED | verdict |
|---|---|---|---|
| all 6 chains | 0.000 | 0 | **INCONCLUSIVE** — the donor's periphery clobbered our clock network, killing the instrument |
| chain 13 only (the FPLL's own) | 50.332 MHz | **0** | **valid negative** |

So installing every bit of a working PLL's own PRAM chain — modelled and unmodelled alike — does not
start it. **The missing configuration is not in the FPLL block at all**, which retires the last
hypothesis that the fault lies in how we program the PLL.

What remains are the analog/periphery blocks that carry the bulk of the reference-only bits:
`HSSI(0,35)` and `CMUXH(0,35)` (chain 12, 63 reference-only bits), `TERM` and `CBUF` and `LVL`
(chains 4/5). A chain-12 transplant is the next targeted step, but it touches the clock muxes, so it
needs the instrument's clock moved off that path first or the trial repeats the inconclusive result.

**Method note.** Sweeping is exhausted as an instrument here: `CLKIN_0_SRC` (all 8), CLKBUF source
(6), PLL position, both feedback modes, and a byte-identical tile clone are all negative. A sweep can
only turn knobs that have names, and the remaining difference is in bits libmistral does not name —
so differential transplant, not a wider sweep, is the technique that can still make progress.

## 2. Design decision: try the direct path first, silicon is the arbiter

**v1 emits the direct configuration:** `INPUT_SEL = e({PLLIN,k})` for the chosen gclk instance,
plus the feedback enable if the PLL is in normal (MCNT-feedback) mode. Rationale: it is fully
offline-derivable today, and the arbiter between "direct works" and "CLK_SELECT required" is a
**cheap silicon experiment** (§5 V3), not more RE. If V3 fails, v2 mimics the Quartus CLK_SELECT
layer, whose value semantics are then derived by the differential already specified in
MISTRAL_GAPS §G4 (2–3 Quartus diff builds through the quartus_jobs pipeline, cmuxdump-diffed) —
narrowed at that point to a single bmux family.

## 3. Implementation (nextpnr fork only; no libmistral changes)

Follows the proven CLKBUF precedent (`globals.cc:35-37` + `bitstream.cc:129-130`): the cmux is a
bel whose output pin binds the CLKOUT rnode and whose input side is *configuration*, not routing.

1. **Arch data (init):** build `pllclk_map`: for each cmux position × gclk instance × link-table
   entry `{PLLIN,k}` × p2p row `FPLL(p).PLLCOUT[c] → this cmux PLLIN[k]` → record
   `(fpll_pos p, counter c) → (cmux pos, instance g, input_sel value e)`. Pure table composition at
   startup from `get_all_p2p()` + the link tables.
2. **Bel:** `MISTRAL_PLLCLK[g]` at each CMUXHG/CMUXVG position (GCLK first; regional CR/HR/VR later),
   pin `Q` = `get_port(<cmux>, x, y, g, CLKOUT)` — same wire family CLKBUF drives; downstream
   GCLK→…→TCLK routing needs nothing new.
3. **Pack:** for each `altera_pll` output `outclk[c]` net with fabric loads: insert a
   `MISTRAL_PLLCLK` cell driving the net (CLKENA-insertion pattern, `globals.cc:198`), annotate it
   with (source PLL cell, counter c).
4. **Placement validity:** `isBelLocationValid` on `MISTRAL_PLLCLK` = the annotated (PLL placement,
   c) has a `pllclk_map` entry for this (cmux pos, instance). The placer then co-locates legally;
   with one fPLL in use this is deterministic table lookup.
5. **Bitstream:** for each bound `MISTRAL_PLLCLK`: `bmux_r_set(<cmux>, pos, INPUT_SEL, g, e)` from
   the map (instead of CLKBUF's hardcoded 0x1b), `TESTSYN_ENOUT_SELECT = PRE_SYNENB` (as CLKBUF),
   and on the PLL side `PLL_FEEDBACK_ENABLE_<n> = PLL_MCNT0` when feedback is normal-mode (matching
   the fabi386 observation).
6. **Refusal semantics (P-honest):** an `outclk` whose (PLL pos, counter) reaches no cmux in the
   map, or a second clock contending for the same gclk instance, is a **pack error naming the
   conflict** — never a silent drop, never a guessed value.

## 4. What stays out of v1

PLL cascades (`PLL_CAS_*`), LVDS/EXTCLK outputs, `PLLMOUT/PLLDOUT` special taps, dynamic switchover
(`dynamic_clk_select`), fractional-mode spread config beyond what `pll.cc` already emits, and the
regional (CR/HR/VR) cmuxes. Each has p2p/link data and can extend the same map later.

## 5. Verification ladder (each step gates the next)

- **V1 — self-check (no hardware):** build a two-clock design (`altera_pll` with two counters) →
  `.rbf`; `cmuxdump` must show exactly the intended `INPUT_SEL={PLLIN,k}` entries and `fplldump`
  the counter config. Also assert the previously-failing "no wire found for port outclk" is gone.
- **V2 — ground-truth shape check (optional, no hardware):** same clock topology built by Quartus;
  diff the *FPLL* bmux sections (counters/feedback must match; CMUX sections will differ by design
  — direct vs CLK_SELECT — which is the documented open question, not a failure).
- **V3 — silicon arbiter:** LED-divider design: `outclk[0]` at a distinctive frequency (e.g. 10 MHz
  → LED toggling at ~0.6 Hz through a 2^24 divider), deployed via the existing S8 gate
  (`mister_gate`, `vup-agent-hw/1`). LED blinks at the predicted rate ⇒ the direct path works and
  G4-v1 is CLOSED on silicon. LED dark/wrong-rate ⇒ v2 (CLK_SELECT derivation) with the failure
  recorded per P6.
- **V4 — upstream:** PR the map + bel to the fork's upstream, and offer the p2p-composition note to
  Ravenslofty/mistral (their data already contained the answer; the docs' G4 framing can drop the
  "add graph edges" plan).
