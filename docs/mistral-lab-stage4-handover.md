# Mistral LAB Stage 4 handover

## Purpose and current state

This document is the restart guide for the Mistral LAB legality and placement
work. Detailed evidence and architectural decisions remain in
[`mistral-lab-next-stages-tracker.md`](mistral-lab-next-stages-tracker.md), and
the original rationale remains in
[`mistral-lab-next-stages-design.md`](mistral-lab-next-stages-design.md).

Handover point:

- Branch: `cyclonev-compress-default`
- Commit: `3c3a00cb` (Stage 6 through 6h, the Rust legality authority at parity and promoted to the default, the coding rules and their gate, the live monitor, pack-time admission)
- Stages 1, 2, and 3 are closed.
- Stage 4A through 4E are complete for the Stage 4 scope.
- Stage 5: 1c complete, 4b retired, 2a and 2b complete (checkpoints for all
  four phases), 3c complete (route reuse), 3b and 3a complete (reuse plan,
  region expansion, typed build states); the candidate list is exhausted.
  The closing measurement is recorded; from it, 3c-2 fixed router2's bind
  order (byte-identical clean flow, 21 to 27% less router time on edits with
  reuse); 3c-3 reports route survival next to applied routes (100% on the
  unchanged design, 82% on the controlled edits); 3c-4 seeds router2's
  history on preserved wires behind `--reuse-routes-history` (99%
  survival, router2 in a third of the time at H = 8, off by default). The
  closing measurement's recommendations are exhausted. The Quartus
  comparison on the identical netlist (1.8x on Fmax: rewrite 1.12x, model
  1.22x, placement 1.32x) and the model attribution on fmaxtest are in
  the tracker; the cell constants turned out to be the -7 table already.
  The first full-core attempt (55k cells) fails on LAB input capacity from
  both sides while Quartus fits the same cells into 20,576 ALMs at 1.92
  per ALM against our 1.4. Stage 6: 6a (legaliser stall exit) and 6b
  (`--alm-pairing`, 1.37 to 1.74 cells per ALM, the full core places) are
  done; the wall is now routing at that density (59% fabric wires, 23%
  input lines); 6c (`--spread-demand`) trims that plateau by 10%, 6e
  (`--spread-congestion`) by 35%, neither to convergence; 6d, the line
  pre-assignment, was built, measured negative, and removed, and the
  per-class rule was shown unable to bind under the count, so the crate
  stays concluded. 6f measured the router's share instead of assuming
  it: on the identical netlist nextpnr uses 2.8 times Quartus's fabric
  wires because LAB lines are fed by row wires (a vertical hop is a
  stair) and registers seldom pack with their LUTs (16% against 95%); negotiation variants
  move the core's plateau 13%, a unit wire cost halves it
  The Rust legality authority was brought to parity at the user's
  direction (2026-09-19, tracker "Rust legality at parity", design section
  10): resident LAB snapshots patched per bel replace the capture per
  query in every non-legacy mode, the exec probe runs at legacy wall time
  with `--sa-seam on`, the full core at 1.4 times legacy placement, both
  byte-identical with the verify harness at zero mismatches.
  (`--router2-unit-cost`, `--router2-reroute`, both opt-in). 6g packs a
  register into its LUT's ALM half (`--register-packing`, opt-in):
  5,360 of the core's 9,632 LUT-driven registers pack; the core's router plateau is a third of 6f's under the unit wire cost and 15% worse under the delay cost. 6h makes HeAP's spreader and legaliser weigh a vertical
  tile like the solver does (`--row-cost W`, opt-in), and with it the
  full core routes to completion for the first time (2026-09-18: pairing,
  congestion spreading, register packing, row cost 2.5, unit wire cost,
  periodic contested re-routes; 15 minutes wall; signoff 9.8 to 11.9 MHz
  against Quartus's 25.2; recipe in `build/stage6-fullcore/core_probe_flow.sh`,
  outside git). The next units are a hybrid base cost, the LAB-level
  assignment, and timing-driven placement quality (design doc 9.5 to 9.7).
- The Rust LAB legality authority is the default in Rust builds since 2026-09-19
  (`--lab-legality rust`; legacy in Rust-disabled builds and by `--lab-legality
  legacy`), with the annealer on the overlay seam (`--sa-seam on`). The Stage 1
  control-plan authority stays legacy: the complete evaluator owns the control
  check. Tracker entry "Promotion".
- Parallel placement evaluation exists behind `--placer-lookahead N` (with
  `--threads W`), is byte-identical to the serial search, and is off by default.
  It is not promoted: it measured no end-to-end gain on Fabi386.

The worktree at handover contains the untracked `AGENTS.md` and
`mistral/tests/__pycache__/`. They are not part of this work and must not be
deleted or committed without confirming ownership.

## What has been delivered

### Stages 1–3: control preparation and complete LAB evaluation

The control-set algorithm now has a native C++ allocation model, move-only
preparation tickets, ordered preflighted edits, failure-atomic application, and
explicit authority modes. The V2 LAB schema captures complete normalized LAB
facts, and detached C++ and Rust evaluators agree on structured scoped and
whole-LAB results.

The live modes are:

| Mode | Behavior |
| --- | --- |
| `legacy` | Apply the C++ result; the default for `--lab-controls`, and for `--lab-legality` in Rust-disabled builds |
| `shadow` | Apply C++ after detached comparison |
| `verify` | Apply Rust only after exact C++ agreement |
| `rust` | Apply a validated Rust result; the default for `--lab-legality` in Rust builds since 2026-09-19 |

Stage 2 retained the single-search capture, reduced Rust decoder temporaries,
and direct final-buffer construction. The complete warm V1 dispatch target of
less than 290 ns was rejected rather than weakening validation; the retained
measurement is 347.79 ns.

### Stage 4A: mutation revisioning

The pre-existing HeAP rollback-strength leak was fixed. A session-scoped,
monotonic global placement revision now covers relevant binding, connectivity,
fact, constraint, generated-object, and routing mutations. Frozen work carries a
session and revision stamp and fails closed when stale.

Key files:

- `mistral/placement_revision.h/.cc`
- `mistral/arch.h`
- `mistral/arch.cc`
- `mistral/lab.cc`
- `mistral/pack.cc`
- `common/place/placer_heap.h/.cc`

### Stage 4B: serial detached transactions

`StrictLegaliser::try_place_cluster` captures the complete displacement closure
before live mutation. Mistral converts the candidate into a move-only prepared
transaction, freezes a normalized V2 occupancy overlay, evaluates it without
live mutation, requires exact detached C++/Rust agreement, checks freshness and
expected owners/strengths, and then commits. Unsupported candidates retain the
original bind/check/revert path.

Fabi386 preserved all 2,892 candidate decisions under shadow comparison and then
ran with transaction authority: 1,632 commits and 1,260 mutation-free
rejections, with byte-identical final artifacts.

The current serial callback seam is `PlacerHeapCfg::place_cluster_transaction`.
It is invoked from `StrictLegaliser::try_place_cluster` in
`common/place/placer_heap.cc` and installed by Mistral in `mistral/arch.cc`.

Key files:

- `mistral/placement_transaction.h/.cc`
- `common/place/placer_heap.h/.cc`
- `mistral/arch.cc`
- `mistral/tests/lab_legality.cc`

### Stage 4C: Rust-owned frozen batches

The C++ `RustFrozenLabBatchV2` RAII owner wraps an opaque Rust-owned immutable
batch. Creation copies and validates every V2 fact before publishing a handle;
no C++ pointers are retained. Concurrent readers use disjoint outputs and
private Rust scratch. Cancellation is atomic and panics are contained at the
FFI boundary.

Enforced bounds:

- At most 64 candidates per batch.
- At most two outstanding batches per worker.
- At most 64 MiB aggregate retained storage.

Key files:

- `mistral/lab_frozen_batch.h/.cc`
- `mistral/lab_v2_abi.h`
- `rust/npnr_mistral_lab_ffi/src/lib.rs`
- `rust/npnr_mistral_lab_ffi/src/tests.rs`
- `mistral/tests/lab_ffi.cc`

### Stage 4D: deterministic parallel evaluation

HeAP's random cluster search gained a lookahead form. `legalise_cluster_lookahead`
generates up to N candidates in exactly the serial order without touching live
bindings, records the RNG and radius state at each location, and after the
architecture commits the first legal candidate restores that state so the
trajectory matches the serial search. `try_place_cluster` was split into
`build_cluster_candidate` and `finish_cluster_move`, and the location step into
`next_random_location`, so both paths share one implementation.

`PlacementCandidateCoordinator` (Mistral) prepares every candidate on the
owner thread; workers then freeze each candidate's overlay facts, create their
own Rust-owned handle, evaluate the detached C++ reference, and require Rust
agreement (parallel freezing, added after the scaling analysis). The owner
consumes results strictly in proposal order and commits after rechecking the
stamp and every expected owner. Workers may read the live design because the
owner is blocked during the parallel section and the capture path writes no
shared state; a test compares worker-captured facts byte for byte with
owner-captured ones. The stale policy (two
asynchronous retries, then synchronous) is implemented and counted; it cannot
trigger because only the owner mutates. Unsupported candidates truncate the
batch and HeAP replays that location through the serial path.

Every Fabi386 configuration from budget 2 to 64 and 1 to 16 workers is
byte-identical to the Stage 4C artifacts. With parallel freezing the lookahead
phase scales (budget 64: 4.69 s on one worker to 1.60 s on eight; budget 8:
1.25 s to 0.83 s) and comes within about 10% of the serial search's 0.75 s
without beating it. It is under 3% of wall time on Fabi386, whose serial
search rejects only 0.77 candidates per commit, so the default stays serial;
the option is meant for workloads where legalisation is minutes. The tracker records the tables.

The option travels in `ArchArgs::placer_lookahead`, not `ctx->settings`.
Interning a new settings key shifted `IdString` indices and changed both the
routing checksum and routed JSON net numbering while the report stayed
identical; do not reintroduce a settings key for it.

Key files:

- `common/place/placer_heap.h/.cc`
- `mistral/placement_coordinator.h/.cc`
- `mistral/arch.h` (`ArchArgs::placer_lookahead`), `mistral/arch.cc`, `mistral/main.cc`
- `mistral/tests/lab_legality.cc` (`BatchCoordinator*`)

### Stage 4E: incremental reuse

Level one (`mistral/lab_reuse.h/.cc`, `--lab-reuse off|shadow|on`): per-LAB
binding versions plus a global facts epoch stamp a cached copy of the live
legacy query's LAB-level sub-results. Active only inside `Arch::place()` and
only in plain legacy LAB modes. Fabi386 is byte-identical in both modes with
zero shadow mismatches; the hit rate is 14.5% and the saving is not measurable
end to end. Its value is the invalidation contract.

Level two (`mistral/placement_reuse.h/.cc`, `--reuse-placement prev.json`):
cells whose name and semantic signature match the previous output get a hard
`BEL` attribute; HeAP's constraint placer binds and validity-checks them and
everything else is placed normally. Route-through buffers in the previous
output are folded out of consumers' signatures, and already-bound cells (QSF
pins) are never annotated. An unchanged rebuild reproduces the previous
placement exactly with placement time under 1 s instead of ~21 s; controlled
edits keep 99.3–99.6% of cells. Routing is re-run in full and is not
byte-identical because the RNG state at route start differs.

The handover's 4E list is closed in full: precise cell and net incidence via
object-carrying kernel hooks, derived per-LAB states (`lab_reuse_state()`),
and a bounded content tier (`--lab-reuse content`). Not done, by design: entity
matching across re-synthesis name churn, reuse of routes or preparation
artifacts, and checkpoint persistence.

Key files:

- `mistral/lab_reuse.h/.cc`, `mistral/arch.h` (stamps, hooks, precise incidence), `mistral/lab.cc` (fact rewrites and the prepared stamp)
- `common/kernel/basectx.h`, `nextpnr_types.cc`, `basectx.cc` (object-carrying mutation hooks)
- `mistral/placement_reuse.h/.cc`, `mistral/arch.cc`, `mistral/main.cc`
- `mistral/tests/lab_legality.cc` (`LabReuse*`, `PlacementReuse*`)
- `build/stage4e-validation/make_edits.py`, `compare_reuse.py`

## Benchmark evidence

The dedicated release benchmark is
`nextpnr-mistral-lab-frozen-bench`, implemented in
`mistral/tests/lab_frozen_bench.cc`. It uses a 64-record representative nonempty
whole-LAB batch. Thread creation, handle creation, and output allocation are
outside the parallel evaluation timing. Parallel slice results are compared
against direct V2 results after timing.

Two independent seven-round runs produced these midpoint medians:

| Workers | Frozen ns/record | Throughput | Speedup |
| ---: | ---: | ---: | ---: |
| 1 | 389.73 | 2.57 M/s | 1.00x |
| 2 | 198.09 | 5.05 M/s | 1.97x |
| 4 | 102.07 | 9.80 M/s | 3.82x |
| 8 | 54.05 | 18.50 M/s | 7.21x |
| 12 | 39.89 | 25.07 M/s | 9.77x |
| 16 | 30.33 | 32.97 M/s | 12.85x |

Direct V2 FFI with validation costs 804.63 ns/record. Frozen handle creation and
destruction costs about 503.42 ns per retained record, or 32.22 microseconds per
64-record batch. Reuse amortizes creation after two serial evaluations.

These are evaluator-throughput results, not end-to-end placement speedups. At
handover, no production HeAP work is evaluated concurrently.

The host is an M1 Ultra with 16 performance and 4 efficiency cores. Public macOS
APIs do not expose hard performance-core pinning. The benchmark has an optional
`performance-qos` mode using `QOS_CLASS_USER_INTERACTIVE`. Contemporaneous runs
changed medians by -2.01% to +2.36%, within observed run variability, so this
mode is retained only as instrumentation and is not recommended as a default.

Benchmark commands:

```sh
cmake --build build/rust-enabled --target nextpnr-mistral-lab-frozen-bench -j4
./build/rust-enabled/mistral/nextpnr-mistral-lab-frozen-bench build/stage4-validation/frozen-v2-scaling.csv 7 20000 default
./build/rust-enabled/mistral/nextpnr-mistral-lab-frozen-bench build/stage4-validation/frozen-v2-performance-qos.csv 7 20000 performance-qos
./build/rust-enabled/mistral/nextpnr-mistral-lab-frozen-bench build/stage4e-validation/bench-dynamic-64.csv 7 20000 dynamic:64
```

The `dynamic[:chunk]` mode (workers claim chunk-record units from a shared
counter) reaches 14.83x at 16 workers and 16.09x at 20 against 12.20x static,
because static partitions wait for threads the scheduler placed on efficiency
cores. The CSV also records per-worker completion times and claimed units.

Artifact hashes and the resource run are recorded in the tracker. Benchmark
outputs belong under `build/` and are intentionally not source-controlled.

## Remaining critical path

### Stage 4D exit evidence (recorded)

Stage 4D closed with identical decision traces, zero stale commits, zero
retries, identical final placement/routing artifacts at 1, 2, 4, 8, and 16
workers, Fabi386 plus the feature fixture, and end-to-end wall time, placement
and routing time, RSS, and counters recorded in the tracker. It is not promoted.

Anyone revisiting lookahead for speed should note that the useful work per batch
is bounded by the rejection run before the next commit (0.77 on Fabi386), so a
budget above 2 mostly discards candidates; parallel freezing makes that
discarded work nearly free but not free. A frozen-epoch search policy
(evaluate several alternatives per cell and choose in sequence order) would
change the search trajectory and needs its own reproducibility gate; that is a
new decision, not a continuation of 4D.

### Stage 4E exit evidence (recorded)

Same-session assessment reuse is byte-identical and mismatch-free on Fabi386;
placement reuse reproduces an unchanged rebuild exactly and keeps 99.3–99.6%
of cells on controlled edits, with full preparation, routing, and signoff.
Neither mode is promoted. The tracker records counters, timings, and Fmax.

### What comes after Stage 4

The remaining reuse levels in design section 6.8 (prepared/routed artifact
reuse and separate-process checkpoints) need artifact provenance for
reservations, generated cells, pin rewrites, and routes, plus durable entity
matching. Start with a design document; do not extend `--reuse-placement` to
routes without it.

## Validation before changing status

At minimum, run the gate (58 s; export `SDKROOT` first on this machine):

```sh
mistral/tests/gate.sh
```

It runs the scoped cargo test, clippy, and fmt, both gtest suites in both
trees, the exec probe on the default path against its recorded checksums and
report hash, `git diff --check`, and clang-format on the changed C++ files.
A change on a legality path also records a verify-mode placement of the
probe, or of the core when the resident protocol changed, in the tracker. Run
Fabi386 in all relevant modes and compare normalized reports and routed JSON;
`--telemetry` writes the counters and phase times of a run as JSON. Exact historical commands, hashes, pass counts, and
known creator-line differences are in the tracker.

Update the tracker in the same change that changes a unit status. Do not mark 4D
or 4E complete until its exit criteria and validation evidence are recorded.

## Commit landmarks

| Commit | Meaning |
| --- | --- |
| `1261e6d` | Rust LAB planning and legality pipeline through Stage 4A |
| `bf30a82` | Serial frozen placement transactions, Stage 4B |
| `d93799c` | Rust-owned frozen LAB batches, Stage 4C |
| `3139f9b` | Initial frozen evaluator scaling benchmark |
| `b8b48ec` | 12/16-worker and uneven-partition benchmark support |
| `16a233e` | Apple performance-QoS experiment |
| `9c7e56f` | Deterministic parallel cluster lookahead, Stage 4D |
| `662383a` | Same-session LAB assessment reuse and placement reuse, Stage 4E |
| `7fd9482` | Stage 4E list closure: precise incidence, LAB states, content tier |
| `3aaa173` | Parallel freezing for lookahead candidates; scaling analysis |
| `dc7fde5d` | Stage 5 unit 1c-A: annealer swap seam |
| `c57e1163` | Stage 5 unit 1c-B: batched refinement, deterministic across workers |
| `da7aa9c9` | Stage 5 units 2a-1 and 2a-2: packed and placed checkpoints, byte-identical on resume |
| `7462f138` | Stage 5 units 2b-1 and 2b-2: route-prepared and routed checkpoints, bitstream-identical on resume |
| `1b912e54` | Checkpoint fixtures beyond Fabi386: twelve designs identical on every resume; orphan nets and wire flags |
| `9e5222a6` | Stage 5 unit 3c: route reuse, correct with a from-scratch fallback; not a router-time win on edits |
| `386461fb` | Stage 5 units 3a and 3b: reuse plan with reasons, placement region expansion, typed build states |
| `e608bf49` | Stage 5 closing measurement and unit 3c-2: router2 rips up every net before binding; 3c timing corrected (equal to clean, not double) |
| `c8348030` | Stage 5 unit 3c-3: route survival measured after the router and reported next to applied routes |
| `cfa98d50` | Stage 5 unit 3c-4: history seeding for preserved routes behind `--reuse-routes-history`; 99% survival at H = 8 |
| `af6c01b3` | Quartus comparison on the identical netlist, timing-gap decomposition, and model attribution on fmaxtest; signoff report switches |
| `b5dc749d` | First full-core attempt: legaliser stall under the LAB input limit, router plateau without it, Quartus at 1.92 cells per ALM; ALM pairing density is next |
| `8f31b9af` | Stage 6 units 6a and 6b: legaliser stall exit with a LAB occupancy report; ALM pairing packer, full core places; input-line structure measured |
| `9b648dcb` | Stage 6 units 6c, 6d (negative, removed), 6e: demand-weighted and congestion-driven spreading; the core's router plateau at 12,200 |
| `18841bf3` | Stage 6 unit 6f: the router's share measured on the identical netlist (2.8 times Quartus's fabric wires, row-fed LAB lines, unpacked registers); `--router2-reroute`, `--router2-unit-cost`, per-tile utilisation dump |
| `0f9abd87` | Stage 6 unit 6g: register packing (`--register-packing`), the spreader's per-bucket cluster weight, the packer's control-set check |
| `d0689a6f` | Stage 6 unit 6h: the row-aware placement cost (`--row-cost`); the full core routes to completion |
| `e5699589` | The Rust legality authority at parity: resident LAB snapshots (`ResidentLabs`, `BelPatchV2`, `mistral/lab_resident.*`), the capture path kept as the harness |
| `733693a0` | The lab crates deny panics outside tests; the resident oracle generates every patch shape the arch sends |
| `75bb5dc2` | `--telemetry`: the run's counters, checksum, options, and phase times as JSON (`mistral/telemetry.*`) |
| `8db93cbe` | The fixture fails a leaking test; `mistral/tests/gate.sh`; the coding rules in CLAUDE.md, the tracker, and design section 11 |
| `dfae9b8d` | The live monitor's renderer (`rust/npnr_mistral_monitor`) and its C ABI (the FFI crate's `monitor` module) |
| `ac23a858` | `--monitor`: the session that owns the terminal, feeds the log tail, and renders from atomics and the phase clock (`mistral/monitor.*`) |
| `6aeaea47` | The monitor recorded: tracker entry, decision row, design section 12; the gate and CLAUDE.md cover the monitor crate |
| `79e977f5` | The Rust LAB evaluator promoted to the default authority in Rust builds; `--sa-seam on` default; the gate's probe on the real default path |
| `8adb09f8` | The promotion and the concurrency decision recorded |
| `b03b3e37` | Pack-time admission: the placer's LAB legality authority admits every cluster the pairing and register packers commit (`mistral/pack_admission.*`) |
| `3c3a00cb` | Pack-time admission designed (section 13) and recorded |
| `48171fab` | Stage 6 unit 6c: demand-weighted spreading behind `--spread-demand`; clears the paired probe's wire, trims the core's plateau 10% |

## Stage 5: applying the seams to the rest of the flow

The candidate list and its priority order are recorded in the tracker's
"Stage 5 opening measurement" entry: 1c (annealer through the seam), 4b
(incremental timing), 2a/2b (checkpoints), the rest of 3b, 3c (route reuse),
3a (typed build states, in C++). 1c is complete (1c-A and 1c-B). 4b is
retired: re-attributing the profile shows propagation at 2.9% of the run and
the timing structure built once per phase; the placers' hashed criticality
lookups were the real cost and are now per-arc tables. 2a and 2b are
complete: `--checkpoint` and `--resume` for all four phases (packed, placed,
route-prepared via `--route-prepare-only`, routed), each byte-identical on
resume in output JSON, report, and bitstream (tracker entries "Stage 5 units
2a-1 and 2a-2" and "Stage 5 units 2b-1 and 2b-2"); the design is
[`mistral-checkpoint-design.md`](mistral-checkpoint-design.md), whose section
2.6 lists what a reload of nextpnr's own JSON loses and why the checkpoint
carries the IdString table and every iteration order. `Arch::route()` is
split into `prepare_route()` and the router. 3c is complete: `--reuse-routes`
preserves a previous run's routes where the current design still allows
them (design section 9; tracker entry "Stage 5 unit 3c"). 3b and 3a are
complete: the reuse plan with reasons (`--reuse-plan-out`, `--reuse-dry-run`),
placement region expansion on placer failure, and the typed build states
that every placement, routing, and bitstream now goes through (design
section 10; tracker entry "Stage 5 units 3a and 3b"). The Stage 5 candidate
list is exhausted; the next decision is which measured gap to open.

Unit 1c-A is complete: `--sa-seam off|shadow|on` gives `placer1` refinement a
detached swap assessment (legality from `Arch::overlay_bels_legal`, cost
delta from a position overlay), byte-identical output, and about 8% less SA
time serially with no provisional binding. Off by default.

Unit 1c-B is complete as a measured experiment: `--sa-batch N` speculates N
swaps per batch, evaluates them on `--threads` workers, and consumes them in
order with a per-candidate acceptance stream and dependency-tracked
re-evaluation. It is deterministic across worker counts (byte-identical at
1/2/4/8/16), quality sits inside the serial seed spread, and every accepted
swap is verified by recomputation. It does not scale: 1.32x at two workers,
worse beyond four, because the annealer's per-candidate work is owner-bound
and the detached share is under 1 µs. Off by default. The lever not taken is
pipelining generation of the next batch during evaluation of the current one.
Note that serial identity is structurally impossible for the annealer (the
acceptance draw shares the RNG stream with location draws), which is why this
unit uses the design's frozen-epoch policy rather than 4D's serial one.

Key files: `common/place/placer1.h/.cc` (seam types, overlay, shadow, batch),
`common/place/placement_pool.h/.cc` (shared worker pool),
`common/place/placer_heap.h/.cc` (pass-through), `mistral/lab.cc` and
`mistral/lab_control_plan.cc` (rules templated on occupancy),
`mistral/arch.h` (`BelOverlay`), `mistral/placement_coordinator.cc`
(`mistral_assess_swap`, `mistral_commit_swap`), `mistral/tests/lab_legality.cc`
(`SwapSeam*`).

## Rust evaluator: concluded in Stage 4, reopened twice, promoted

The crates (`npnr_mistral_lab`, `npnr_mistral_lab_ffi`, and since 2026-09-19
`npnr_mistral_monitor`) were concluded at the end of Stage 4 at their contract:
ABI V1, ABI V2, and the frozen-batch handle. They were reopened twice at the
user's direction, each time for one recorded unit: the resident LAB snapshots
that brought the Rust legality authority to parity (tracker entry "Rust
legality at parity"), and the live monitor (tracker entry "The live monitor").
On 2026-09-19 the Rust legality authority became the default in Rust builds
(tracker entry "Promotion"); the shadow and verify modes and the capture path
remain the parity harness, and the fatal Rust cross-check on every lookahead
candidate stays. A rules revision must be versioned in both implementations,
and a protocol revision must extend the resident module's patch-shape list and
the oracle in the same change.

## Default-mode and promotion policy

The Rust legality authority and the annealer's overlay seam are the defaults
in Rust builds since 2026-09-19, by the user's decision on the recorded
evidence; the legacy rules remain in every build as the fallback and the
harness reference. Completing a stage gate does not automatically promote
anything else: parallel evaluation, performance QoS, and incremental reuse
each require their own evidence-backed decision, and the concurrency paths
were considered and left off on 2026-09-19 (tracker decision log).
