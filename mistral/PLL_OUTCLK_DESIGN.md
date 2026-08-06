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
