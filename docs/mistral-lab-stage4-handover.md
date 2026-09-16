# Mistral LAB Stage 4 handover

## Purpose and current state

This document is the restart guide for the Mistral LAB legality and placement
work. Detailed evidence and architectural decisions remain in
[`mistral-lab-next-stages-tracker.md`](mistral-lab-next-stages-tracker.md), and
the original rationale remains in
[`mistral-lab-next-stages-design.md`](mistral-lab-next-stages-design.md).

Handover point:

- Branch: `cyclonev-compress-default`
- Commit: `dc7fde5d` (Stage 5, 1c-A)
- Stages 1, 2, and 3 are closed.
- Stage 4A through 4E are complete for the Stage 4 scope.
- Next: cross-build checkpoints and physical artifact provenance (design
  section 6.8 levels three and four), which are a new design decision.
- Legacy LAB legality remains the default. Rust authority is opt-in.
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
| `legacy` | Apply the C++ result; default mode |
| `shadow` | Apply C++ after detached comparison |
| `verify` | Apply Rust only after exact C++ agreement |
| `rust` | Apply a validated Rust result |

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

At minimum, run:

```sh
cmake --build build/rust-enabled --target nextpnr-mistral-test -j4
./build/rust-enabled/nextpnr-mistral-test
cargo test --manifest-path rust/Cargo.toml --offline --workspace
cargo clippy --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab -p npnr_mistral_lab_ffi --all-targets -- -D warnings
cargo fmt --manifest-path rust/Cargo.toml --all -- --check
git diff --check
```

Run the equivalent Rust-disabled native tests for changes that affect common C++
or fallback behavior. Run Fabi386 in all relevant modes and compare normalized
reports and routed JSON. Exact historical commands, hashes, pass counts, and
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

## Stage 5: applying the seams to the rest of the flow

The candidate list and its priority order are recorded in the tracker's
"Stage 5 opening measurement" entry: 1c (annealer through the seam), 4b
(incremental timing, deferred because timing is 10.7% of the run), 2a/2b
(checkpoints), the rest of 3b, 3c (route reuse), 3a (typed build states, in
C++).

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

## Rust evaluator: concluded

The two crates (`npnr_mistral_lab`, `npnr_mistral_lab_ffi`) are concluded at
their current contract: ABI V1, ABI V2, and the frozen-batch handle. They stay
in the build as the parity harness (`--lab-controls`/`--lab-legality` shadow
and verify modes, and the fatal Rust cross-check on every lookahead candidate)
and are not promoted to default authority or extended into the next design.
The tracker records the safety, memory, cost, and maintainability evaluation
behind this. Reopen only for a revision of `LegacyControlRulesV1`, which must
then be versioned in both implementations or the Rust authority modes retired.

## Default-mode and promotion policy

Legacy remains the default. Completing a stage gate does not automatically
promote Rust authority, parallel evaluation, performance QoS, or incremental
reuse. Each promotion requires its own evidence-backed decision, with a simple
fallback path retained until full P&R parity and operational stability are
established.
