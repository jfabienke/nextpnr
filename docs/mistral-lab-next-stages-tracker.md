# Mistral LAB next-stages tracker

Execution record started 2026-09-15. Architectural rationale belongs in
[`mistral-lab-next-stages-design.md`](mistral-lab-next-stages-design.md); this
file records implementation state and evidence. The current restart guide is
[`mistral-lab-stage4-handover.md`](mistral-lab-stage4-handover.md).

## Baseline

- Repository: `nextpnr`, branch `cyclonev-compress-default`.
- Baseline commit: `070b598ea8db7848238b367e801f52521f95c652`.
- Worktree: dirty at Unit 1A start with the existing V1 ABI, detached C++/Rust
  evaluators, profiling work, and unrelated in-progress changes. Unit 1A
  preserves those changes.
- Host/profile: macOS 26.6.2 arm64, Apple Clang 17, Rust 1.98, release builds.
- Fabi386 baseline inputs: JSON
  `3cdb742d5b5b85b2905e7957d622c2ec64ab3adc8053ddad80ec7d4c23f54bd3`;
  QSF `67c27631ac1b6eac92e056914d758fd842b09db9a765b8fe46f904338098bcca`.
  **2026-09-16:** the originals lived under `/private/tmp/fabi386-pnr.1FWP2M/`
  and were lost to a reboot (Darwin 27 update). They were reconstructed: the QSF
  from the two `Constraining IO` lines in retained logs (it hashes to the
  pinned value exactly), and the JSON from the Stage 4E controlled-edit copy by
  re-deriving the same 40 seeded bit flips and reversing them. The rebuilt JSON
  is content-identical but re-serialised (sha256
  `29411c1d4c7f68f09731cd9163e02716c87171a423378148f0efcf63bf85c3f5`); a serial
  run on the rebuilt pair reproduces the Stage 4C report, routed JSON, and
  checksums `0xbb18ede9` / `0xbc1365c6` byte for byte, so every comparison in
  this record remains valid. The inputs now live in `build/fabi386-inputs/`
  (uncommitted); keep a copy outside any temporary directory.
- Warm baseline: live C++ verdict 146.09 ns, live C++ plan export 149.59 ns,
  capture 160.28 ns, detached C++ 132.74 ns, Rust FFI 174.66 ns, and complete
  Rust dispatch 363.27 ns per query. Median Fabi386 wall time was 34.981 s in
  legacy mode and 35.453 s in Rust mode; median peak RSS was 1,966.17 MiB and
  1,968.12 MiB respectively. See
  [`mistral-lab-legality-profile.md`](mistral-lab-legality-profile.md).

## Status

`Blocked` lacks a dependency; `Ready` may start; `In progress` is the sole
active implementation unit; `Complete` has met its exit criterion with recorded
evidence; `Rejected` is a measured experiment that will not be retained.

| Unit | Dependency | Status | Exit criterion | Evidence |
| --- | --- | --- | --- | --- |
| 1A: native allocation and consumer extraction | Existing V1 parity | Complete | Behavior-preserving extraction with identical routed output and preparation state | [Validation 2026-09-15](#2026-09-15-unit-1a) |
| 1B: preparation ticket and translation | 1A | Complete | Malformed or mismatched plans cannot reach the physical consumer | [Stage 1 validation](#2026-09-15-units-1b1d) |
| 1C: preflighted edit application | 1B | Complete | Edit-list parity and verified failure atomicity | [Stage 1 validation](#2026-09-15-units-1b1d) |
| 1D: Rust plan authority | 1C | Complete | Rust plan physical-state and final-output parity; legacy remains default | [Stage 1 validation](#2026-09-15-units-1b1d) |
| 2A: single-search capture | Stage 1 | Complete | Preserve V1 bytes, ID order, map, and diagnostics while removing the second lookup | [Unit 2A validation](#2026-09-15-unit-2a) |
| 2B: decode/temporary experiments | 2A | Complete | Each experiment measured and accepted or rejected independently | [Unit 2B validation](#2026-09-15-unit-2b-closure) |
| 2C: boundary promotion | 2B | Rejected | Warm dispatch below 290 ns without validation, allocation, order, or P&R regression | [Unit 2B validation](#2026-09-15-unit-2b-closure) |
| 3A: V2 schema and detached C++ reference | Stable Stage 1 boundary | Complete | Detached C++ reproduces scoped live queries and documents whole-LAB differences | [V2 validation](#2026-09-15-units-3a3b) |
| 3B: Rust V2 evaluator | 3A | Complete | Exact agreement for every subcheck and structured failure | [V2 validation](#2026-09-15-units-3a3b) |
| 3C: live rollout | 3B | Complete | Scoped live parity across Fabi386 and feature fixtures | [Live rollout validation](#2026-09-15-unit-3c) |
| 4A: mutation audit and revisioning | Stage 3 | Complete | Every active mutation advances or invalidates its revision | [Mutation audit validation](#2026-09-15-unit-4a) |
| 4B: serial detached transactions | 4A | Complete | Serial traces match corrected bind/check/revert baseline | [Serial transaction validation](#2026-09-15-unit-4b) |
| 4C: owned frozen batches | 4B | Complete | Lifetime, panic, malformed-input, cancellation, and memory tests pass | [Frozen batch validation](#2026-09-15-unit-4c) |
| 4D: deterministic parallel evaluation | 4C | Complete | Reproducible decisions, zero stale commits, bounded retries, measured scaling | [Deterministic lookahead validation](#2026-09-15-unit-4d-deterministic-lookahead) |
| 4E: incremental reuse | 4D | Complete (all eight handover items; artifact provenance and cross-build checkpoints are the next design) | Incremental results match full recomputation and final signoff | [Reuse validation](#2026-09-15-unit-4e-incremental-reuse), [list closure](#2026-09-15-unit-4e-list-closure) |

## Active unit

Stage 1 is complete. `assign_control_sets` now crosses a move-only preparation
ticket, validates and translates all local IDs while its capture remains owned,
preflights an ordered physical edit list, and applies it with an undo journal.
`legacy` applies C++; `shadow` compares edit lists and applies C++; `verify`
applies Rust only after exact C++ result and edit-list agreement; `rust` applies
a host-validated Rust plan and falls back to a freshly evaluated C++ plan only
for the explicit unsupported-rules status. Legacy remains the default.

Stage 2 is closed. Reduced decoder temporaries and direct caller-buffer output
were accepted independently. Cold outlining and verdict-only capture scratch
were measured and reverted. Bounded V1 batching was measured and is not promoted:
the current mutation-sensitive call sites expose no natural frozen batch, and
even synthetic frozen batches increased per-record cost. The 290 ns engineering
target was not reached; complete warm dispatch remains 347.79 ns, so Unit 2C is
explicitly rejected rather than weakening validation.

Units 3A and 3B are complete. The distinct 3,808-byte V2 input and 328-byte
result ABIs capture normalized LUT, FF, DATAIN/SDATA, carry, MLAB, control, net,
cached-count, and resolved-policy facts. Named-field schema-2 replay round-trips
atomically. Detached C++ and safe Rust expose distinct `CombBel(alm)`,
`FfBel(alm)`, and `WholeLab` queries and agree on every structured subcheck.

Stage 3 is complete. `--lab-legality legacy|shadow|verify|rust` now keeps legacy
as the default, compares detached C++ and Rust results in shadow mode, requires
exact agreement before Rust authority in verify mode, and applies validated Rust
results directly in rust mode. Scoped Fabi386 and feature-fixture runs are
byte-identical across all modes.

Unit 4A is complete. Failed clustered HeAP moves restore exact strengths;
session-scoped typed cell/net/BEL/LAB keys cannot be mixed; and an exhaustion-safe
monotonic design revision advances for BEL occupancy/restoration, standard
connectivity and fact changes, constraints, generated objects, and routing. V2
live dispatch also binds its capture to the current revision and fails closed if
that revision changes before authority.

Unit 4B is complete. `StrictLegaliser::try_place_cluster` captures the complete
displacement closure before mutation. Mistral converts it into a move-only,
revision-stamped transaction, freezes normalized V2 queries with an occupancy
overlay, requires exact detached C++/Rust agreement, and commits legal candidates
only after rechecking freshness and every expected owner/strength. Unsupported
candidates retain the original bind/check/revert path. Fabi386 preserved all
2,892 candidate decisions and final bytes, then repeated them under transaction
authority with 1,632 commits and 1,260 mutation-free rejections.

Unit 4C is complete. A move-only C++ RAII owner now holds an opaque Rust batch.
Creation copies and validates every V2 fact before publishing the handle; the
handle stores no C++ pointer, supports concurrent immutable range evaluation with
private outputs, and has atomic cancellation. Rust enforces 64 candidates, two
outstanding batches per worker, and 64 MiB aggregate retained storage.

Unit 4D is complete. HeAP's random cluster search now has a lookahead form
(`legalise_cluster_lookahead` in `common/place/placer_heap.cc`) that generates
up to `--placer-lookahead N` candidates in exactly the serial order, without
touching live bindings, and snapshots the RNG and radius state at each
location. Mistral's `PlacementCandidateCoordinator`
(`mistral/placement_coordinator.h/.cc`) prepares and freezes every candidate on
the owner, evaluates them on a persistent pool of `--threads` workers (C++
authority, Rust cross-check through Rust-owned frozen handles), consumes
assessments strictly in proposal order, and commits the first legal one after
rechecking the revision stamp and every expected owner. HeAP restores search
state to the point just after the committed candidate, so the trajectory is
byte-identical to the serial search at every budget and worker count. The
option is carried in `ArchArgs`, not `ctx->settings`, because interning a new
settings key shifts `IdString` indices and changes both log checksums and routed
JSON net numbering. Lookahead is off by default. Measured end-to-end it buys
nothing on Fabi386: strict legalisation is under 1 s of a 40 s run, and the
owner-side capture of speculated candidates costs more than the workers save.
4E is ready to start.

Unit 4E is complete at the two levels the design assigns to Stage 4. Level one
is same-session LAB assessment reuse (`mistral/lab_reuse.h/.cc`): every LAB
carries a binding version bumped by each bind/unbind of one of its BELs, the
design carries a facts epoch bumped by every kernel mutation notification and
by Mistral's own `assign_comb_info`/`assign_ff_info`, and the live legacy
query's three LAB-level sub-results (input budget, control sets, MLAB groups)
are cached per LAB behind both stamps. `--lab-reuse off|shadow|on` selects it;
it is active only inside `Arch::place()`, only with plain legacy LAB modes, and
off by default. Level two is a conservative placement-reuse adapter for edited
packed designs (`mistral/placement_reuse.h/.cc`, `--reuse-placement prev.json`):
cells with the same name and an identical semantic signature (type, params,
attributes, port-to-net-name connectivity with route-through buffers folded
out) as a cell in the previous output receive a hard `BEL` attribute, HeAP's
constraint placer binds and validity-checks them, and everything else is placed
normally; preparation and routing run in full. Cross-build checkpoints, physical
artifact provenance, and entity matching that survives re-synthesis name churn
are not part of this unit.

The 4E handover list was then closed in full: precise reverse incidence from
cells and nets to LABs through new object-carrying kernel hooks
(`notifyCellMutation`/`notifyNetMutation`, defaulting to the kind-only hook
for every other backend), an explicit derived per-LAB state
(`Dirty`/`Evaluated`/`Prepared`/`Routed`) stamped by control preparation and
routing completion, and a bounded content-keyed tier (`--lab-reuse content`)
consulted when a LAB's stamps are stale.

## Validation log

### 2026-09-15: Unit 1A

Commands and results:

| Command | Result |
| --- | --- |
| `cmake --build build --target nextpnr-mistral-test --parallel 8` | Pass, Rust disabled |
| `./build/nextpnr-mistral-test '--gtest_filter=LabControl*'` | 18/18 pass |
| `cmake -S . -B build/rust-enabled -DARCH=mistral -DBUILD_RUST=ON -DBUILD_TESTS=ON -DMISTRAL_ROOT=/Users/jvindahl/Development/ext/mistral -DCMAKE_BUILD_TYPE=Release` | Pass |
| `cmake --build build/rust-enabled --target nextpnr-mistral nextpnr-mistral-test --parallel 8` | Pass |
| `./build/rust-enabled/nextpnr-mistral-test '--gtest_filter=LabControl*'` | 22/22 pass |
| `cargo test --manifest-path rust/Cargo.toml --offline --workspace` | 22 Rust tests/doctests pass |
| `cargo clippy --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab -p npnr_mistral_lab_ffi --all-targets -- -D warnings` | Pass |
| `clang-format -i mistral/lab_control_plan.h mistral/lab_control_plan.cc mistral/lab.cc mistral/lab_legality.cc mistral/tests/lab_legality.cc` | Applied |
| `git diff --check` | Pass |

The exact Fabi386 commands were:

```sh
./build/rust-enabled/nextpnr-mistral --device 5CSEBA6U23I7 --json /private/tmp/fabi386-pnr.1FWP2M/f386_exec_probe_nodsp.json --qsf /private/tmp/fabi386-pnr.1FWP2M/exec_probe.qsf --seed 1 --threads 1 --placer heap --router router2 --freq 12 --router2-max-iter 100 --lab-controls legacy --write build/unit1a-validation/fabi386-legacy.json --report build/unit1a-validation/fabi386-legacy.report.json
./build/rust-enabled/nextpnr-mistral --device 5CSEBA6U23I7 --json /private/tmp/fabi386-pnr.1FWP2M/f386_exec_probe_nodsp.json --qsf /private/tmp/fabi386-pnr.1FWP2M/exec_probe.qsf --seed 1 --threads 1 --placer heap --router router2 --freq 12 --router2-max-iter 100 --lab-controls rust --write build/unit1a-validation/fabi386-rust.json --report build/unit1a-validation/fabi386-rust.report.json
cmp build/unit1a-validation/fabi386-legacy.json build/unit1a-validation/fabi386-rust.json
cmp build/unit1a-validation/fabi386-legacy.report.json build/unit1a-validation/fabi386-rust.report.json
```

Both runs completed normally. Routed JSON files compare byte-for-byte and hash to
`7ed738af6eedd35d36e826ef8364918777d2b2ded50cebe895903b2bf57c9fb0`.
Report files compare byte-for-byte and hash to
`56e3b75e84be78a30659aa5e3860c3899eb7597e375cfa34a90d13d94a8a034a`.
The Rust run reported 1,513,411 evaluations, 4,191 preparation comparisons, and
zero errors, mismatches, fallbacks, or diagnostics.

Artifact hashes:

| Artifact | SHA-256 |
| --- | --- |
| `build/nextpnr-mistral-test` | `56d2a0c8e34ec299df77bc27e22c3ec19970296544bec5001b83aa41b0d5b84a` |
| `build/rust-enabled/nextpnr-mistral` | `41ad2554db2443b1e345f794195c118918c65cc46fd87edc46ffd0e80077d025` |
| `build/rust-enabled/nextpnr-mistral-test` | `9ab668a568afbbdc92b5e95dacac706b7b1a6cc11b808ba23d7882cc408372de` |

Focused native tests compare all twelve named allocation fields with the V1
result, verify ENA/ACLR indices and exact reserved sources, check MLAB CLK0/ENA0,
exercise retained inversion on a disconnected captured signal, and prove that an
illegal native evaluation exposes no allocation or preparation-state mutation.

Known limitations: `cargo fmt --manifest-path rust/Cargo.toml --all -- --check`
reports a pre-existing formatting difference in tracked, otherwise-clean
`rust/nextpnr/src/lib.rs`; Unit 1A does not alter that unrelated file. Compiler
output also contains existing Mistral deprecation and format warnings. Fabi386
does not cover every control/MLAB feature, so focused native fixtures provide
that coverage.

### 2026-09-15: Units 1B–1D

Commands and results:

| Command | Result |
| --- | --- |
| `cmake --build build --target nextpnr-mistral-test --parallel 8` | Pass, Rust disabled |
| `./build/nextpnr-mistral-test '--gtest_filter=LabControl*'` | 20/20 pass |
| `cmake --build build/rust-enabled --target nextpnr-mistral nextpnr-mistral-test nextpnr-mistral-lab-bench --parallel 8` | Pass |
| `./build/rust-enabled/nextpnr-mistral-test '--gtest_filter=LabControl*'` | 27/27 pass |
| `ctest --test-dir build --output-on-failure` | 1/1 pass |
| `ctest --test-dir build/rust-enabled --output-on-failure` | 1/1 pass |
| `cargo test --manifest-path rust/Cargo.toml --offline --workspace` | 22 Rust tests/doctests pass |
| `cargo clippy --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab -p npnr_mistral_lab_ffi --all-targets -- -D warnings` | Pass |
| `cargo fmt --manifest-path rust/Cargo.toml --all -- --check` | Existing unrelated `rust/nextpnr/src/lib.rs` difference only |
| `git diff --check` | Pass |

Focused tests prove ticket move-only ownership and local-ID translation; classify
legal, illegal, unsupported, transport, malformed, and mismatch outcomes; reject
bad request IDs and shapes before preflight; compare Rust/C++ edit lists; and
inject missing-edge, changed-value, and interrupted-application failures. Every
injected application failure restores the exact state present at application
entry. Mode tests exercise C++ selection in shadow, fail-closed mismatch in
verify, validated Rust selection in rust, and explicit unsupported fallback.

Fabi386 was run with seed 1, one thread, HeAP, router2, 12 MHz, and a 100-iteration
router limit in all four modes. All runs completed normally. Each routed JSON is
byte-identical with SHA-256
`7ed738af6eedd35d36e826ef8364918777d2b2ded50cebe895903b2bf57c9fb0`;
each timing/utilization report is byte-identical with SHA-256
`56e3b75e84be78a30659aa5e3860c3899eb7597e375cfa34a90d13d94a8a034a`.
Every non-legacy mode reported 1,513,411 evaluations, 4,191 preparations, and
zero errors, mismatches, fallbacks, or diagnostics.

| Mode | Wall time | Peak RSS |
| --- | ---: | ---: |
| legacy | 36.57 s | 1,971 MiB |
| shadow | 37.59 s | 1,969 MiB |
| verify | 38.22 s | 1,969 MiB |
| rust | 37.31 s | 1,978 MiB |

A separately synthesized feature fixture contains one `MISTRAL_CLKBUF`, one
`MISTRAL_FF`, and one `MISTRAL_MLAB`, covering global-clock, FF control, MLAB
write-clock/enable reservation, and LUTRAM placement/routing. Its four routed
outputs and reports are byte-identical: routed SHA-256
`1dfd2a79ec50827265d02823f9026287ce6e914ca2444549f495469e8787a999`,
report SHA-256
`3a9d40d123f387cac9c0d215afeaa40e65beea31a3d112f2655d771005a0f281`.
The non-legacy runs reported 339,517 evaluations, 4,191 preparations, and no
errors or mismatches. The fixture, commands, logs, CSVs, and hashes are retained
under `build/stage1-validation/`.

#### Boundary cost profile

Seven interleaved warm rounds over the 4,096-record Fabi386 corpus, with 128
repeats per record and no timed parsing/binding, produced these median costs:

| Phase | Median ns/call | C++/Rust allocations |
| --- | ---: | ---: |
| Harness | 0.49 | 0 |
| Live C++ verdict | 147.89 | 0 |
| Capture/encoding | 160.80 | 0 |
| Detached C++ rules | 134.37 | 0 |
| Rust decode | 116.81 | 0 |
| Rust rules | 28.68 | 0 |
| Rust encode | 11.77 | 0 |
| Rust safe wire evaluator | 167.93 | 0 |
| Raw Rust FFI from C++ | 177.17 | 0 |
| Host result validation | 24.41 | 0 |
| Complete Rust legality dispatch | 365.42 | 0 |
| Preparation dispatch | 780.48 | 5,388 first-round allocations across 3,670,016 calls; zero after round 1 |
| Ticket ownership + translation, legal cases | 88.00 | 0 |
| Physical edit preflight, legal cases | 265.00 | 1 allocation/call (9,024 requested bytes) |
| Complete production-equivalent preparation, legal cases | 1,549.91 | 3 allocations/call (selected/comparison edit lists plus undo journal) |

The Rust-native ABI entry/exit increment is approximately 6.86 ns over the safe
wire evaluator. Capture + FFI + validation account for 362.38 ns of the 365.42 ns
complete legality dispatch, leaving about 3.04 ns of orchestration in this
measurement. Medians from separately randomized phases are descriptive, so
component subtraction is approximate rather than a confidence interval.
CSV hashes: C++
`76d844e569d30de5a0cbe136af2b5477eac68dd7ff1ac65b23459466c4689622`;
Rust `c87133c1d0bcb2ad5af7459b3979dd99a60e28b5884c66ad6fbecbaa1bfafb15`;
corpus `ee54b557fada0614caa5b5c9daef5a86f5a35bd6b6973235ae92bcfa46bb4e86`.

### 2026-09-15: Unit 2A

`capture_lab_controls` now uses the local ID established by its first pointer
search/insertion and directly encodes the signal. The 4,096 live-corpus records
are reconstructed and compared byte-for-byte with their saved V1 inputs by the
C++ benchmark before timing. Rust-off 20/20 and Rust-on 27/27 focused native
tests pass. The 15,137-record differential corpus passes complete C++/Rust/FFI
result and native-layout parity, including 12 fixed malformed cases and 2,000
mutations; summary SHA-256 is
`d9b4b33074252fda67534ce2fff4e18e1e0a7cfab44b25b5ab9f70b1085f45d0`.

In seven isolated rounds, median capture cost changed from 160.80 ns to
159.38 ns (−1.42 ns, −0.88%) with zero allocations. Before/after CSV hashes are
`76d844e569d30de5a0cbe136af2b5477eac68dd7ff1ac65b23459466c4689622`
and `124637feb13424106bbc9547c0ef9840d45ff99d9d7b315fcdb55cd28fe3ea29`.
A post-change Rust-authority Fabi386 run completed in 38.73 s at 1,985 MiB peak
RSS, reported zero errors/mismatches/fallbacks, and remained byte-identical to
the Stage 1 legacy routed output and report.

### 2026-09-15: Unit 2B accepted and rejected experiments

The reduced-temporary experiment retains validated tri-state net classes in the
typed Rust snapshot, eliminating the final `classes.map(...)` array conversion.
Across seven isolated rounds it changed Rust decode from 116.81 to 113.02 ns and
native FFI from 174.79 to 169.85 ns. Through the C++ caller, FFI changed from
174.63 to 170.19 ns and complete dispatch from 361.19 to 354.28 ns. The
15,137-record valid/malformed differential corpus passed exactly.

The direct-output experiment initializes the result array before the protected
batch loop and evaluates directly into each live caller-owned result. Panic
recovery still reinitializes the entire active batch. Two isolated runs reduced
within-round FFI-minus-safe-evaluator overhead from 6.21 ns to 4.19 and 3.83 ns.
Through C++, raw FFI changed from 170.19 to 164.03 ns, complete dispatch from
354.28 to 347.79 ns, preparation dispatch from 761.95 to 752.93 ns, and complete
production-equivalent preparation from 1,523.32 to 1,507.28 ns. No phase added
an allocation. The 15,137-record C++/Rust/FFI corpus passed; summary SHA-256 is
`2541b8fb7b247631876436dabbbe8fb681fe4b14a1de397808d3f068108af8de`.

Cold/noinline outlining of boundary-error result construction was rejected and
reverted: its within-round FFI-minus-safe overhead was 3.89 ns, inside the
3.83–4.19 ns retained direct-output range. The measurement is preserved at
`build/stage1-validation/rust-boundary-outlined-errors.csv`.

The final retained Rust-authority Fabi386 check completed in 36.37 s at
1,984 MiB peak RSS with zero errors/mismatches/fallbacks and the same routed and
report hashes as legacy. Unit 2B remains in progress because scratch and natural
batching experiments have not yet met their independent decision gates.

Exact profiling/differential command forms:

```sh
./build/rust-enabled/mistral/nextpnr-mistral-lab-bench build/lab-profile-fabi386/corpus.jsonl OUTPUT.csv 7 128
./rust/target/release/examples/lab_control_bench build/lab-profile-fabi386/corpus.jsonl OUTPUT.csv 7 128
uv run python mistral/tests/compare_lab_controls.py --cases 10000 --offline --ffi-driver build/rust-enabled/mistral/nextpnr-mistral-lab-ffi-replay --output-dir OUTPUT_DIR
```

### 2026-09-15: Unit 2B closure

The verdict-only capture experiment returned the 1,952-byte DTO while treating
the 200-pointer translation array as local scratch. It was rejected and reverted:
full capture regressed from 159.38 to 206.67 ns and complete Rust dispatch from
347.79 to 356.67 ns. The isolated scratch capture was 162.86 ns and added no
allocation, so moving the large value outweighed any lifetime benefit. The
seven-round CSV is `build/stage2-validation/cpp-verdict-scratch.csv`.

The existing bounded V1 bridge was measured over adjacent frozen corpus records.
Median Rust FFI cost was 166.51 ns per record singly and 167.81, 168.11, 168.77,
170.66, 173.01, and 180.16 ns at batch sizes 2, 4, 8, 16, 32, and 64. No measured
phase allocated. Placement verdict calls are separated by speculative live-state
changes, while preparation may rewire after each LAB, so neither is a safe natural
batch source. The benchmark support remains to measure future naturally frozen
callers; production batching is rejected. CSV:
`build/stage2-validation/rust-v1-batches.csv`.

Unit 2B is complete with every planned experiment decided independently. Unit 2C
is rejected because retained complete dispatch is 347.79 ns versus the proposed
sub-290 ns target. This target was explicitly non-blocking in the design: Stage 3
proceeds with the measured overhead and all validation intact.

### 2026-09-15: Units 3A–3B

V2 uses fixed C-value transport with compile-time C++ and Rust size/offset checks:
3,808-byte input, 328-byte result. Capture assigns deterministic first-encounter
LAB-local net IDs and resolves `MISTRAL_LAB_INPUT_LIMIT` through the same once-only
policy function used by the live check. The detached reference recomputes all ten
ALM input counts independently of the cached values and returns a separate valid
mask so stale-cache diagnostics do not alter verdicts.

`CombBel(alm)` evaluates the selected ALM, recomputed LAB input limit, and MLAB
compatibility. `FfBel(alm)` additionally projects FF controls through V1.
`WholeLab` deliberately checks all ALMs in physical index order before LAB-wide
input, control, and MLAB checks; unlike a live bel-scoped query, it can therefore
report a different ALM's earlier failure. Whole-LAB remains explicit and
experimental.

Rust validates the complete V2 shape, implements the ordered LUT bit/sharing,
carry/FF accessibility, ALM accounting, LAB limit, and MLAB checks, and projects
controls through the proven V1 safe evaluator. Control result IDs are translated
back into the V2 LAB-local domain. The V2 FFI has an independent symbol and the
same bounded, caller-owned, panic-contained envelope contract as V1.

Validation completed:

| Command | Result |
| --- | --- |
| `./build/nextpnr-mistral-test '--gtest_filter=LabControl*'` | 25/25 pass, Rust disabled |
| `./build/rust-enabled/nextpnr-mistral-test '--gtest_filter=LabControl*'` | 33/33 pass, including C++/Rust V2 field parity |
| `cargo test --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab -p npnr_mistral_lab_ffi --lib` | 15/15 pass |

Focused cases cover null-sharing asymmetry, bit/input limits, carry mixing, odd
FF rejection, SDATA and DATAIN accessibility, signed LAB input policy, V1 control
conflicts, MLAB group/FF conflicts, malformed headers/query/shape, wide provenance,
replay atomicity, and stale cached counts. `FF_CONTROL` remains a documented but
currently unreachable reason because the live algorithm rejects the second/odd
FF before two same-half control sets can be compared.

### 2026-09-15: Unit 3C

The explicit `--lab-legality legacy|shadow|verify|rust` rollout preserves legacy
as the default. Shadow applies the live result after exact detached C++/Rust
comparison; verify applies Rust only after agreement with detached C++ and the
live scoped query; rust applies the validated Rust result. A stale live ALM cache
is diagnostic in shadow and fails closed before authority in verify/rust.

Focused Rust-on native validation passes 34/34 `LabControl*` tests, including
all Stage 3 subchecks, live mode selection, malformed transport, and stale-cache
injection. Rust-off validation passes 25/25 and retains the legacy behavior.

Fabi386 completed normally in shadow, verify, and rust modes. Every mode issued
14,621,350 scoped evaluations (7,403,554 legal and 7,217,796 illegal) with zero
errors, mismatches, stale-cache events, or diagnostics. Each routed artifact is
byte-identical to the Stage 1 legacy baseline and hashes to
`7ed738af6eedd35d36e826ef8364918777d2b2ded50cebe895903b2bf57c9fb0`;
each report hashes to
`56e3b75e84be78a30659aa5e3860c3899eb7597e375cfa34a90d13d94a8a034a`.

The global-clock/FF/MLAB/LUTRAM feature fixture completed in all four modes.
Each non-legacy mode issued 503,153 evaluations with zero errors, mismatches,
stale-cache events, or diagnostics. Routed output hashes to
`1dfd2a79ec50827265d02823f9026287ce6e914ca2444549f495469e8787a999`
and the report to
`3a9d40d123f387cac9c0d215afeaa40e65beea31a3d112f2655d771005a0f281`
in every mode. Commands, logs, and outputs are retained under
`build/stage2-validation/`.

### 2026-09-15: Unit 4A

The HeAP cluster rollback record now stores both each displaced cell and its
original `PlaceStrength`; the common restore helper removes provisional occupants
and restores those exact pairs. Its regression starts with a strong occupant,
performs a provisional displacement, and verifies occupant, BEL, and strength
restoration. Returning to identical occupancy advances the revision for every
provisional and restorative bind/unbind, preventing ABA freshness.

`PlacementRevisionState` supplies one non-reused session domain, separate typed
cell/net/BEL/LAB keys, a monotonic revision stamp, per-domain audit counters, and
fail-closed exhaustion. Standard common mutation entry points notify Mistral for
cell facts, port connectivity, net facts, constraints, and generated objects;
Mistral additionally covers BEL, wire, and pip bindings, derived LUT/FF fact
assignment, route reservations, and backend object removal. The initial retained
placement boundary admits no direct pre-pack field mutation and no post-prepare
teardown, so those phases remain outside transaction lifetime.

Validation:

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test '--gtest_filter=LabControl*:PlacementRevision.*'` | 37/37 pass |
| `./build/nextpnr-mistral-test '--gtest_filter=LabControl*:PlacementRevision.*'` | 28/28 pass, Rust disabled |
| Stage 4A Fabi386 legacy baseline | Completed normally; byte-identical to Stage 3 |
| `git diff --check` | Pass |

The refreshed Fabi386 routed output remains
`7ed738af6eedd35d36e826ef8364918777d2b2ded50cebe895903b2bf57c9fb0`
and its report remains
`56e3b75e84be78a30659aa5e3860c3899eb7597e375cfa34a90d13d94a8a034a`.
Artifacts are retained as `build/stage2-validation/fabi386-stage4a-baseline*`.

### 2026-09-15: Unit 4B

`PreparedPlacementTransaction` owns an ordered, preflighted BEL edit set and the
exact session/revision stamp on which it was prepared. A focused two-BEL swap
preserves asymmetric weak/strong strengths. An intervening bind-and-restore makes
the prepared transaction stale and leaves both BELs untouched; duplicate target
edits are rejected as malformed.

The frozen candidate owns plain V2 facts generated against a complete occupancy
overlay, so evaluation neither binds nor retains live context pointers. Ordered
detached C++ results short-circuit exactly like the live BEL loop and must match
validated Rust results. Shadow integration first compared all 2,892 Fabi386
candidates with the corrected live path: zero unsupported cases and zero
mismatches. Transaction authority then committed 1,632 legal candidates and
rejected 1,260 illegal candidates without provisional live mutation. The
candidate sequence, placement checksum, routing, timing, utilization, routed
JSON, and report were unchanged. The feature fixture committed all 20 candidates
through the transaction path and also remained byte-identical.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test '--gtest_filter=LabControl*:PlacementRevision.*'` | 39/39 pass |
| `./build/nextpnr-mistral-test '--gtest_filter=LabControl*:PlacementRevision.*'` | 30/30 pass, Rust disabled |
| Fabi386 comparison-only transaction run | 2,892/2,892 compared; zero unsupported or mismatched candidates |
| Fabi386 transaction-authority run | 1,632 committed, 1,260 rejected, zero unsupported; completed normally |
| Feature transaction-authority run | 20 committed, zero rejected/unsupported; completed normally |
| `cargo test --manifest-path rust/Cargo.toml --offline --workspace` | 24 Rust tests/doctests pass |
| `cargo clippy --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab -p npnr_mistral_lab_ffi --all-targets -- -D warnings` | Pass |
| `git diff --check` | Pass |

Fabi386 routed JSON hashes to
`7ed738af6eedd35d36e826ef8364918777d2b2ded50cebe895903b2bf57c9fb0`
and its report to
`56e3b75e84be78a30659aa5e3860c3899eb7597e375cfa34a90d13d94a8a034a`,
byte-identical to the corrected Stage 4A and Stage 3 baselines. The feature
fixture routed JSON hashes to
`1dfd2a79ec50827265d02823f9026287ce6e914ca2444549f495469e8787a999`
and its report to
`3a9d40d123f387cac9c0d215afeaa40e65beea31a3d112f2655d771005a0f281`.
Logs and outputs are retained under `build/stage4-validation/`.

### 2026-09-15: Unit 4C

The V2 ABI now exposes an opaque Rust-owned frozen batch. Creation accepts one to
64 value records, validates every record through `ValidatedLabSnapshotV2`, copies
the validated snapshots into boxed Rust storage, and publishes no handle on any
envelope, validation, panic, or quota failure. Evaluation takes an immutable
handle plus a bounded range; independent callers may share the handle only while
owning disjoint output storage. Atomic cancellation is observed before work and
between records, and non-OK range results must be discarded as a unit.

Quota accounting is process-wide and synchronized: at most two live handles per
worker and 64 MiB of aggregate retained snapshot storage. Destroying the move-only
C++ `RustFrozenLabBatchV2` owner releases both quotas. Rust tests exhaust the real
64 MiB bound, inject an evaluator panic, reject malformed snapshots, verify quota
recovery after destruction, exercise cancellation, mutate/drop the source after
creation, and run concurrent readers. Native C++ tests additionally verify move
ownership and RAII quota release.

| Command | Result |
| --- | --- |
| `cargo test --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab_ffi` | 11/11 tests pass, including allocation test |
| `cargo test --manifest-path rust/Cargo.toml --offline --workspace` | 28 Rust tests/doctests pass |
| `cargo clippy --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab_ffi --all-targets -- -D warnings` | Pass |
| `./build/rust-enabled/nextpnr-mistral-test '--gtest_filter=LabControl*:PlacementRevision.*'` | 40/40 pass |
| `./build/nextpnr-mistral-test '--gtest_filter=LabControl*:PlacementRevision.*'` | 30/30 pass, Rust disabled |
| Fabi386 Unit 4C regression | Completed normally; placement/routing checksums and normalized routed JSON unchanged |

The Fabi386 report remains byte-identical with SHA-256
`56e3b75e84be78a30659aa5e3860c3899eb7597e375cfa34a90d13d94a8a034a`.
The routed JSON differs only in its expected build-version `creator` line after
commit `bf30a82`; removing that provenance line gives identical SHA-256
`963a6262c7ae50d2791f92a98e4e35ed92200d7c706f516f2f0666117a2adfa9`.
Placement and routing checksums remain `0xbb18ede9` and `0xbc1365c6`.

### 2026-09-15: Unit 4D benchmark baseline

A dedicated release-mode benchmark measures one 64-record V2 batch without timed
thread creation, handle creation, or output allocation. Each round performs
1,280,000 record evaluations per phase. The benchmark supports uneven contiguous
partitions, so worker counts need not divide the batch size. Two independent
extended seven-round runs produced the following midpoint of their run medians:

| Path | Workers | Median ns/record | Throughput | Speedup vs frozen 1-worker |
| --- | ---: | ---: | ---: | ---: |
| Direct V2 FFI, validates every call | 1 | 804.63 | 1.24 M/s | 0.48x |
| Rust-owned frozen V2 | 1 | 389.73 | 2.57 M/s | 1.00x |
| Rust-owned frozen V2 | 2 | 198.09 | 5.05 M/s | 1.97x |
| Rust-owned frozen V2 | 4 | 102.07 | 9.80 M/s | 3.82x |
| Rust-owned frozen V2 | 8 | 54.05 | 18.50 M/s | 7.21x |
| Rust-owned frozen V2 | 12 | 39.89 | 25.07 M/s | 9.77x |
| Rust-owned frozen V2 | 16 | 30.33 | 32.97 M/s | 12.85x |

Creation plus destruction costs 503.42 ns per retained record, or 32.22 us for
the 64-record handle. Consequently, serial frozen evaluation amortizes creation
after two evaluations of the same batch; one creation plus one serial evaluation
is slower than direct validation. A separate three-round resource run reproduced
the scaling and reported 4,227,072 bytes peak RSS, 2,785,664 bytes peak memory
footprint, zero swaps, and 6.08 s wall time.

This is an evaluator-throughput result over representative nonempty whole-LAB
facts, not an end-to-end placement speedup. Unit 4D remains active until proposals
are generated serially, results are consumed in sequence order, stale retries are
bounded, and 1/2/4/8-worker full-placement traces are proven reproducible.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/mistral/nextpnr-mistral-lab-frozen-bench build/stage4-validation/frozen-v2-scaling-16.csv 7 20000` | Complete; 1/2/4/8/12/16-worker scaling measured |
| Independent extended seven-round repeat | Complete; 12/16-worker medians reproduced within 7.0% |
| `/usr/bin/time -l ... frozen-v2-scaling-rss.csv 3 20000` | Complete; peak RSS 4,227,072 bytes |

Extended CSV SHA-256: primary
`965add4795235ddb9467e39f899d2c1a64ce29d35dc7b73a6aedd824a353e21b`;
repeat `27e90a2b7e2a9587e648772856319a129b260f22614badf7aa057de6f8c5a9b2`.
The original resource-run SHA-256 remains
`41cacb1abac1c0cdfec9f896853838e555d6b0d34d1d3799646a862db723717a`.

### 2026-09-15: Unit 4D deterministic lookahead

`StrictLegaliser::try_place_cluster` was split into `build_cluster_candidate`
(no mutation) and `finish_cluster_move` (post-commit bookkeeping), and the random
location step into `next_random_location`, so the serial and lookahead paths
share one implementation of the RNG draws and radius schedule. The serial path
is unchanged in behavior: a rebuilt serial run reproduces the Stage 4C Fabi386
artifacts byte for byte (routed JSON minus the `creator` line, report, placement
checksum `0xbb18ede9`, routing checksum `0xbc1365c6`).

The lookahead path speculates a batch of at most N candidates, cutting through a
tile's shapes where the budget ends and resuming there if nothing commits. Each
candidate records the search state at its location; a commit restores that state
and applies the serial loop's post-location increment, and an unsupported
candidate restores it and replays the location through the serial path. Only the
owner thread mutates, so a stale stamp at commit is structurally impossible; the
documented policy (two asynchronous retries, then synchronous re-evaluation) is
implemented and counted anyway.

`PlacementCandidateCoordinator` flattens each batch's V2 queries into 64-record
Rust-owned frozen handles created on the owner before dispatch (a candidate that
would straddle a handle starts a new one; one with more than 64 queries uses the
one-shot FFI). Workers evaluate the detached C++ reference per candidate,
short-circuiting like the live BEL loop, then evaluate the same prefix through
the frozen handle and require exact agreement. Each worker owns its scratch and
writes only the result slots it claimed.

Fabi386 (`--seed 1`, `--freq 12`, `--router2-max-iter 100`) with the same inputs
as Stage 4B, every configuration listed below produced a report and routed JSON
byte-identical to the Stage 4C artifact (report SHA-256
`56e3b75e84be78a30659aa5e3860c3899eb7597e375cfa34a90d13d94a8a034a`; routed JSON
minus `creator` and `threads` settings lines `748de61b…`, which equals the
Stage 4C file under the same normalization), identical placement/routing
checksums, 1,632 commits, 1,260 rejections, zero unsupported, zero stale, zero
retries, and zero synchronous fallbacks:

| Budget | Workers | Candidates evaluated | Discarded after commit | V2 queries | Strict legalisation | Peak RSS |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 1 | 2,892 | 0 | 2,892 transactions | 0.81 s | 1,397 MiB |
| 2 | 1 | 4,524 | 1,632 | — | 1.11 s | 1,291 MiB |
| 4 | 1 | 7,788 | 4,896 | — | 1.02 s | 1,683 MiB |
| 8 | 1 | 14,304 | 11,412 | 315,464 | 1.39 s | 1,425 MiB |
| 8 | 2 | 14,304 | 11,412 | 315,464 | 1.25 s | 1,714 MiB |
| 8 | 4 | 14,304 | 11,412 | 315,464 | 1.32 s | 1,468 MiB |
| 8 | 8 | 14,304 | 11,412 | 315,464 | 1.31 s | 1,942 MiB |
| 8 | 16 | 14,304 | 11,412 | 315,464 | 1.44 s | 1,961 MiB |
| 20 | 8 | 33,900 | 31,008 | — | 2.00 s | 1,963 MiB |
| 64 | 8 | 105,600 | 102,708 | 1,957,056 | 4.38 s | 2,034 MiB |
| 64 | 16 | 105,600 | 102,708 | 1,957,056 | 4.29 s | 2,056 MiB |

The decision trace is identical everywhere and the exit criteria for
determinism are met. The scaling result is negative for this workload: the
serial search rejects only 0.77 candidates per commit, so a budget of N discards
about N−1.8 evaluated candidates per commit, and their capture (owner-side V2
overlay construction) dominates. Strict legalisation grows with the budget and
does not fall with workers, and it is under 2.5% of wall time to begin with.
Wall time and HeAP totals varied by several seconds between otherwise identical
runs, more than the legalisation phase itself; the repeated timing runs below
report medians.

Three sequential repeats each, nothing else running, medians (the individual
serial wall times were 42.69, 35.52, and 42.59 s, so the wall-time column is
run-to-run noise, not an effect):

| Configuration | Wall | HeAP total | Strict legalisation | Router2 | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| serial | 42.59 s | 9.06 s | 0.95 s | 9.17 s | 1,714 MiB |
| budget 8, 1 worker | 34.92 s | 9.01 s | 1.44 s | 8.78 s | 1,953 MiB |
| budget 8, 8 workers | 34.35 s | 8.44 s | 1.35 s | 9.12 s | 1,954 MiB |

All nine repeats are byte-identical to the Stage 4C report and routed JSON. The
only phase lookahead changes is strict legalisation, and it is slower with
lookahead at either worker count; eight workers recover roughly 0.1 s of the
0.5 s that speculative capture adds. Peak RSS is about 240 MiB higher with
lookahead, at one worker as much as eight, which points at the retained frozen
facts and result buffers rather than threads.

An earlier iteration batched whole locations (all shapes of a tile, up to 20)
instead of a candidate budget. It was also decision-identical but evaluated
33,900 candidates for one location per batch and 125,200 for four, with strict
legalisation at 2.1–2.3 s regardless of worker count; its outputs are retained
under `build/stage4d-validation/location-granularity/` (routed JSON there
differs from the reference only by the IdString shift described next).

The first serial run after the refactor had a different routing checksum and a
routed JSON that differed in 20 net ids by exactly +1 while the report was
byte-identical. Cause: reading a new `placerHeap/clusterLookahead` settings key
interned one IdString, and both `Context::checksum` and the JSON writer use
IdString indices. The option now travels in `ArchArgs::placer_lookahead` and
nothing is interned in either mode, which is why the table above compares
byte-identical against Stage 4C.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` | 45/45 pass (5 new `BatchCoordinator*` cases: first-legal-in-order at 1/2/4/8 workers, displacing commit, unsupported truncation, two-handle batches, detached parity without mutation) |
| `./build/nextpnr-mistral-test` (Rust disabled) | 35/35 pass; the coordinator falls back to detached C++ only |
| Fabi386 serial after refactor | Byte-identical to Stage 4C; checksums `0xbb18ede9` / `0xbc1365c6` |
| Fabi386 `--placer-lookahead {2,4,8,20,64}` × `--threads {1,2,4,8,16}` (11 runs) | All byte-identical to Stage 4C; decisions, checksums, and counters as tabulated |
| Unmodified HEAD `0d2b6a4` reference run (scratch worktree build) | Byte-identical report and routed JSON, checksums `0xbb18ede9` / `0xbc1365c6`; 2,892/1,632/1,260 |
| Feature fixture (`lab_features`: CLKBUF, FF, MLAB) serial, budget 8 × 1/8 workers, budget 64 × 4 workers | All four byte-identical (report SHA-256 `3a9d40d1…` as recorded for Stage 3C/4B; checksums `0xf3d0465e` / `0xc3f59dd1`); 20 commits, 0 rejections, 0 unsupported/stale; budget 64 evaluated 1,280 candidates and discarded 1,260 |
| `cargo test --manifest-path rust/Cargo.toml --offline --workspace` | 28 Rust tests/doctests pass (no Rust source changed in this unit) |
| `cargo clippy … -p npnr_mistral_lab -p npnr_mistral_lab_ffi --all-targets -- -D warnings` | Pass |
| `cargo fmt --all -- --check` | Fails only in the untouched upstream `rust/nextpnr/src/lib.rs` (edition-2024 import style; last changed in `3232450`); `cargo fmt -p npnr_mistral_lab -p npnr_mistral_lab_ffi -- --check` passes |
| `git diff --check`, `clang-format --dry-run -Werror` on every touched C++ file | Pass |

Logs, JSON, reports, and console output with `/usr/bin/time -l` resource lines
are retained under `build/stage4d-validation/` (`summarise.sh` regenerates the
comparison table).

### 2026-09-15: Unit 4E incremental reuse

**Level one: same-session assessment reuse.** The reverse incidence is
deliberately two-tiered, as the design allows initially: bindings invalidate
precisely (only the LAB whose BEL changed), while every audited fact,
connectivity, constraint, or generated-object mutation invalidates every LAB
through one epoch. Routing changes do not invalidate legality entries because
no LAB legality fact reads routing; routed-dependency states are left for the
artifact-provenance work. Shadow mode evaluates live on every hit and compares.

Fabi386 with `--lab-reuse shadow` and `--lab-reuse on` both produced the Stage
4C report and routed JSON byte for byte and the checksums `0xbb18ede9` /
`0xbc1365c6`:

| Counter | Value |
| --- | ---: |
| LAB-level composite queries during placement | 10,704,787 |
| Sub-results served from cache | 2,841,674 (14.5%) |
| Sub-results evaluated live and stored | 16,741,223 |
| Entries dropped by a LAB version change | 9,456,307 |
| Entries dropped by a facts epoch change | 0 |
| Per-LAB invalidations / facts invalidations | 45,069,307 / 0 |
| Shadow mismatches | 0 |

The hit rate is bounded by the placers' access pattern: nearly every query
follows a bind in the same LAB. Hits come from multi-BEL cluster checks. The
facts epoch never moved during placement, which confirms the audit that no
placer mutates cell or net facts. The saving is a few hundred milliseconds of
evaluation inside a 30–45 s run and is not separable from run-to-run noise;
the unit is kept for its invalidation contract, which level two and later
artifact reuse depend on, not for speed.

**Level two: conservative placement reuse.** Three experiments, each against a
clean full run of the same input, previous placement = the Stage 4D serial
output:

| Design | Reused | Dirty | Placement identical to previous for reused cells | HeAP + SA time | Router2 | Wall | Fmax reuse / clean |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| unchanged rebuild | 11,618 / 11,620 (2 pin-constrained) | 0 | Yes, all 11,620 cells on the same BEL as the clean run | 0.54 s vs 21.6 s | 9.46 s vs 10.07 s | 20.3 s vs 45.5 s | 34.04 / 35.87 MHz |
| 40 LUT INIT edits | 11,578 (99.6%) | 40 changed | 11,585 unchanged BELs; 35 of the 40 dirty cells moved, 5 re-placed onto their old BEL | 0.72 s vs 20.5 s | 8.65 s vs 9.35 s | 13.8 s vs 34.3 s | 33.16 / 35.87 MHz |
| + 40 input swaps | 11,538 (99.3%) | 80 changed | 11,554 unchanged BELs; 66 of the 80 dirty cells moved | 0.65 s vs 18.9 s | 8.41 s vs 10.21 s | 13.5 s vs 33.5 s | 34.36 / 34.97 MHz |

Every reuse run completed placement validity checks, preparation, routing, and
signoff normally; utilization is identical apart from one or two additional
route-through buffers. Placement time collapses because HeAP has nothing to
solve and simulated annealing has nothing movable. The edited designs place
everything differently in a clean run (HeAP is chaotic under any change), so
the clean-run comparison is quality, not bytes, as the gate requires.

Routing is not byte-identical even on the unchanged rebuild: the placer no
longer consumes RNG draws, so router2 starts from a different RNG state and
reaches a different Fmax on the identical placement. That is ordinary router
variance: clean full runs of the unchanged design at seeds 1, 2, and 3 reach
35.87, 34.98, and 32.96 MHz, and every reuse result above (33.16–34.36 MHz)
lies inside that spread.

The first attempt failed on `probe_MISTRAL_OB_PAD`: QSF pin constraints bind
IO cells during packing, and HeAP's constraint placer refuses a second bind.
Already-bound cells now count as user-constrained and are never annotated; the
unit test covers that path, a changed parameter, changed connectivity, a
previous BEL that no longer resolves, a removed cell, and a route-through
buffer folded back out of a consumer's signature.

Why the transplanted set cannot be illegal: changed and new cells are never
constrained, and under the current rules removing an occupant from a LAB never
makes the remaining occupants illegal (input budget, control-set pools, MLAB
grouping, and ALM sharing all only gain headroom). The constraint placer still
validity-checks every transplanted cell and fails the run rather than certify
a wrong placement.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` | 50/50 pass (4 `LabReuse*` cases: stamps follow bindings and facts, precise and global invalidation with ABA safety, shadow agreement over 600 random bind/unbind/query steps with control-set conflicts, mode gating and inactivity outside placement; 1 `PlacementReuse*` case) |
| `./build/nextpnr-mistral-test` (Rust disabled) | 40/40 pass |
| Fabi386 `--lab-reuse shadow` / `on` | Byte-identical to Stage 4C; zero mismatches; counters as tabulated |
| Fabi386 `--reuse-placement` unchanged / INIT edit / INIT+swap edit vs clean runs | As tabulated; all runs completed signoff |
| `git diff --check`, `clang-format --dry-run -Werror` on touched C++ | Pass |

Artifacts, logs, the edit generator (`make_edits.py`), and the comparison
script (`compare_reuse.py`) are under `build/stage4e-validation/`.

### 2026-09-15: Unit 4E list closure

**Items 3 and 4, precise invalidation.** `CellInfo` mutators (ports, params,
attributes, connect/disconnect, rename) now call `notifyCellMutation(this,
kind)` and `renameNet` calls `notifyNetMutation(net, kind)`; both default to
the old kind-only hook, so no other backend changes. Mistral invalidates a
bound cell's LAB only, ignores mutations on unbound cells (no LAB reads them
until a bind, which bumps its LAB), and for a net bumps the LABs of its bound
driver and users only. `assign_comb_info`/`assign_ff_info` use the same cell
path. Constraints and generated objects carry no object and keep the global
epoch. Routing mutations advance a routing epoch that does not touch legality
entries.

**Item 1, explicit states.** `lab_reuse_state()` derives `Dirty`, `Evaluated`,
`Prepared` (control preparation stamped the current LAB stamps), or `Routed`
(prepared, and no routing mutation since routing last completed). States are
derived from stamps, never stored, so they cannot disagree with invalidation.
`Arch::route()` logs the state histogram.

**Item 7, bounded content cache.** `--lab-reuse content` adds a direct-mapped
4,096-slot tier keyed by the complete normalized whole-LAB V2 facts with
provenance zeroed. The hash selects a slot; a hit compares all 3,808 bytes.
Consulted only when a LAB's stamps are stale, it serves an identical LAB after
an ABA move or a structurally identical LAB elsewhere. Eviction is overwrite.

Fabi386 with precise incidence, shadow and content modes, remained
byte-identical to the Stage 4C artifacts with the same checksums and zero
shadow mismatches. The precise counters are all zero during placement (no
cell or net fact mutates while placing), so the hit rate is unchanged at
14.5%. The content tier is a measured loss:

| Mode | Sub-result hit rate | Content lookups / hits | HeAP time | Wall |
| --- | ---: | ---: | ---: | ---: |
| shadow | 14.5% | — | 9.95 s | 43.7 s |
| content | 28.9% | 9,460,498 / 1,838,878 (19.4%) | 20.09 s | 89.7 s |

Every stale query pays one whole-LAB V2 capture (about 1 µs) to form the key,
which costs more than the three live checks it may save, and with 4,096
direct-mapped slots 7.6 M stores evicted 7.6 M entries. The mode is retained
as the bounded content cache the list asked for, with its cost recorded, and
is not promoted.

With object creation no longer invalidating, the state histogram after routing
is `dirty=0, evaluated=0, prepared=0, routed=4191` at routing epoch 150,702:
every LAB's preparation stamp survived route-through insertion and routing,
and all of them are routed against the completed routing epoch. Before that
change 4,020 LABs showed dirty because each generated route-through cell had
bumped the global epoch.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` | 52/52 pass (rewritten stamp test: bound-cell precision, unbound mutations ignored, net incidence through bound users only, constraint fallback to the global epoch; new state-machine and content-tier cases) |
| `./build/nextpnr-mistral-test` (Rust disabled) | 42/42 pass |
| Fabi386 `--lab-reuse shadow` / `content` | Both byte-identical to Stage 4C; counters as tabulated |
| `git diff --check`, `clang-format --dry-run -Werror` on touched C++ | Pass |

### 2026-09-15: Apple Silicon performance-core scheduling experiment

The benchmark host is a Mac Studio with an Apple M1 Ultra, 16 performance cores,
and 4 efficiency cores. Public macOS APIs do not provide hard CPU or cluster
pinning. `THREAD_AFFINITY_POLICY` is documented only as an experimental L2-cache
placement hint and its request was rejected on this host, so it is not used. The
benchmark instead exposes `performance-qos`, which successfully assigns
`QOS_CLASS_USER_INTERACTIVE` to every evaluation worker. This is a supported
performance-oriented scheduler request, not proof that a thread remained on a
specific core.

Two seven-round performance-QoS runs and two contemporaneous default-scheduler
control runs produced these midpoints of run medians. Negative deltas favor
performance QoS:

| Workers | Default ns/record | Performance-QoS ns/record | QoS delta |
| ---: | ---: | ---: | ---: |
| 1 | 392.86 | 393.52 | +0.17% |
| 2 | 199.58 | 200.24 | +0.33% |
| 4 | 102.65 | 102.72 | +0.08% |
| 8 | 53.47 | 52.43 | -1.95% |
| 12 | 41.26 | 40.43 | -2.01% |
| 16 | 33.07 | 33.85 | +2.36% |

The effect is within observed run-to-run variability and provides no
reproducible throughput improvement. The default macOS scheduler already places
this sustained CPU-bound workload effectively. Keep the benchmark mode for
future scheduler experiments, but reject performance QoS for default promotion.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/mistral/nextpnr-mistral-lab-frozen-bench ... 7 20000 performance-qos` (twice) | Complete; QoS request succeeded for every worker |
| Matching `... 7 20000 default` control (twice) | Complete; no reproducible QoS benefit |

Performance-QoS CSV SHA-256: primary
`40e0a061d954c594b65be6d0bbfd7ddc052df0c462b265ff18524002f65b1611`;
repeat `f6361a9ff8c9aa9ec8e30fa210500e13a7e73881b56bdb8f2179440f55ac6bb0`.
Default control CSV SHA-256: primary
`b6d1cbada8ac860bbbbf5e841b75af3714dfc10fd8d25d8a8e2144acf6ae8c0c`;
repeat `7d156f3654fea92f8ed86ac39719a22c9775ef9539168b2b4f342b1121c4c095`.

### 2026-09-15: Scaling linearity analysis

Question: can the parallel evaluator's sub-linear scaling (12.85x at 16
workers in the Stage 4D baseline) be improved, and does it matter end to end?

**Evaluator microbenchmark.** `nextpnr-mistral-lab-frozen-bench` gained
per-worker completion timestamps, a 20-worker point, and a `dynamic[:chunk]`
scheduling mode in which workers claim `chunk`-record units from a shared
counter instead of owning a fixed contiguous range. Seven-round medians of
two runs per mode (one for `dynamic:4`), 20,000 repeats over the 64-record
batch:

| Workers | static | dynamic:4 | dynamic:16 | dynamic:64 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 400.4 ns (1.00x) | 386.5 ns | 385.3 ns | 385.5 ns |
| 2 | 2.00x, 99.9% | 1.87x, 93.6% | 1.93x, 96.3% | 1.95x, 97.4% |
| 4 | 3.91x, 97.8% | 3.24x, 80.9% | 3.67x, 91.8% | 3.80x, 95.0% |
| 8 | 7.53x, 94.2% | 6.17x, 77.1% | 7.18x, 89.7% | 7.53x, 94.2% |
| 12 | 9.98x, 83.2% | 9.16x, 76.4% | 10.76x, 89.7% | 11.22x, 93.5% |
| 16 | 12.20x, 76.2% | 11.51x, 71.9% | 14.16x, 88.5% | **14.83x, 92.7%** |
| 20 | 10.87x, 54.3% | 10.24x, 51.2% | 15.44x, 77.2% | **16.09x, 80.5%** |

The cause of the static collapse is heterogeneous cores, not the evaluator:
with identical work per worker the slowest worker finishes 16.7% (12
workers), 18.8% (16) and 42.8% (20) after the fastest, so beyond eight
workers the scheduler places some threads on the four efficiency cores and
the batch waits for them. Dynamic claiming makes the spread 0.0–0.3% and the
unit counts show the slow workers doing 55–60% of a fast worker's share.
The claim granularity matters: at 4 records the shared counter is contended
about every 120 ns of work and the mode loses to static; at 16 records it
recovers; at 64 (one whole pass per claim) it is never worse than static and
scales to 16.09x on all 20 cores. The Stage 4D coordinator already claims
per candidate (about 22 queries each), which is the right granularity.

**End to end.** Evaluator linearity is not what limits `--placer-lookahead`.
Fabi386 strict legalisation, one run each (serial repeats: 0.94, 0.95, 2.06 s):

| Budget | 1 worker | 8 workers | Candidates | Queries | Batches |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 1.26 s | 1.31 s | 14,304 | 315,464 | 1,788 |
| 64 | 4.95 s | 4.28 s | 105,600 | 1,957,056 | 1,650 |

At budget 64 the eight-worker saving is 0.67 s of 4.95 s. Amdahl gives the
parallel fraction: 4.28 = 4.95 × (1 − P + P/8), so P ≈ 0.155. About 85% of
the lookahead phase is owner-side work: per speculated candidate roughly
39 µs, of which detached evaluation is about 6 µs (18.5 queries at the
benchmark's 0.33–0.39 µs per record) and the remaining
33 µs is overlay capture (about 1 µs per query), transaction preparation,
and Rust handle creation. At budget 8 a batch holds about 50 µs of
evaluation, comparable to the pool's wake-and-join latency, so eight workers
gain nothing at all.

Two conclusions follow. First, the phase can be made to scale: overlay
capture writes no shared state (`capture_lab_v2_impl` reads bound cells,
cell facts, and net flags, and assigns local net ids without interning),
and the owner is blocked during the parallel section, so freezing and
per-worker Rust handle creation could move into the workers, leaving only
preparation on the owner. That would raise P to roughly 0.85 and make a
budget-64 phase scale close to 5x at eight workers. Second, doing so cannot
make the run faster on this design: speculation adds work by construction,
so the best case is returning to the serial 0.95 s, and strict legalisation
is under 2.5% of a 30–45 s run. Lookahead pays off only on a workload whose
serial search rejects many candidates per commit; Fabi386 rejects 0.77.
The evaluator-level fix (dynamic claiming, already the coordinator's policy)
is kept; parallel freezing is recorded as the next step if such a workload
appears, not implemented now.

Artifacts: `build/stage4e-validation/bench-*.csv`, `bench2-*.csv`, and
`amdahl-*` logs.



### 2026-09-15: Parallel freezing

Implemented the step the scaling analysis identified. `PlacementCandidateCoordinator::place()`
now does only preparation on the owner: binding edits, the revision stamp, and
the supported prefix decided from BEL types alone (`placement_candidate_supported`,
no capture). Workers claim candidates, freeze the whole-LAB overlay facts
themselves (`freeze_placement_candidate` on the shared `const Arch`), create a
Rust-owned handle for that candidate under their own worker id, evaluate the
detached C++ reference, and require Rust agreement. The owner consumes in
proposal order and commits exactly as before. Workers read the live design
only while the owner is blocked in the parallel section; the capture path
assigns local net ids without interning and touches no mutable cache, which
the new `BatchCoordinatorParallelFreezeMatchesOwnerFreeze` test checks by
comparing every worker-captured fact record byte for byte with an
owner-captured reference at 1, 4, and 8 workers.

Two pool changes came with it. Workers and the owner spin for at most 50 µs
before blocking on the condition variable, because a budget-8 batch holds
about 50 µs of work. Worker exceptions are caught per job, the run drains,
and the owner reports the first failure through `log_error` instead of the
process terminating inside a worker.

Fabi386, every run byte-identical to the Stage 4C artifacts with checksums
`0xbb18ede9` / `0xbc1365c6`, 1,632 commits, 1,260 rejections, zero stale:

| Budget | Workers | Strict legalisation | Before (owner freeze) | Frozen by workers | Spin / blocking wakes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 1 | 1.28 s | 1.26 s | 0 | — |
| 8 | 8 | 0.83 s | 1.31 s | 11,614 of 14,304 | 7,058 / 5,458 |
| 64 | 1 | 4.69 s | 4.95 s | 0 | — |
| 64 | 8 | 1.60 s (repeat 1.61 s) | 4.28 s | 90,279 of 105,600 | 0 / 11,550 |
| 64 | 16 | 1.49 s | — | 97,376 of 105,600 | 0 / 24,750 |

Amdahl on budget 64 now gives a parallel fraction of about 0.75 (8 workers)
to 0.73 (16 workers), up from 0.155. The remaining serial share is
preparation, ordered consumption, the commit, and HeAP's own candidate
generation between batches, which is why 16 workers add little over 8. At
budget 8 the spin phase serves 56% of wake-ups; at budget 64 the owner's work
between batches exceeds the spin limit and every wake-up blocks, which costs
nothing there because the batches are large.

Contemporaneous repeats (same binary, same session, nothing else running):

| Configuration | Strict legalisation, three runs |
| --- | ---: |
| serial | 0.75, 0.75 s (plus the earlier session's 0.94, 0.95, 2.06 s) |
| budget 8, 1 worker | 1.28, 1.24, 1.24 s |
| budget 8, 8 workers | 0.83, 0.84, 0.82 s |

So parallel freezing takes the budget-8 phase from 1.25 s to 0.83 s, within
about 10% of the serial search measured in the same session, but not below
it: the serial search does 2,892 evaluations and the lookahead does 14,304,
and eight workers do not quite absorb the difference. The end-to-end
conclusion therefore stands: on Fabi386 lookahead is break-even at best and
the phase is under 3% of wall time, so the default stays serial. What
changed is that the phase now scales, which is what a workload with many
rejections per commit needs; there the phase is minutes and the same
parallel fraction applies.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` | 53/53 pass (new: worker-captured facts byte-identical to owner capture at 1/4/8 workers; existing batch tests updated for one handle per candidate) |
| `./build/nextpnr-mistral-test` (Rust disabled) | 43/43 pass |
| Fabi386 budgets 8 and 64, 1/8/16 workers, plus a budget-64 repeat | All byte-identical; as tabulated |
| `git diff --check`, `clang-format --dry-run -Werror` on touched C++ | Pass |

### 2026-09-15: Rust evaluator concluded

The parallel/incremental design's stage 4 asked for an evaluation of the
isolated Rust-owned computation subsystem on safety, memory use,
maintainability, and execution cost. Recorded here; the component is
concluded on this basis.

| Criterion | Finding |
| --- | --- |
| Safety | Delivered. The evaluator is pure by construction (`#![forbid(unsafe_code)]`, validated value snapshots, no host pointers across the FFI), and the frozen-handle contract ran concurrently on per-worker handles through every lookahead run without incident. |
| Memory | Neutral. Peak RSS is within 2 MiB of legacy in every mode; the content tier built on the same facts was a measured loss. |
| Execution cost | Small and real. Rust authority costs about 1.3% of P&R time; the parallel scaling of the lookahead phase came from the value-snapshot contract, which the detached C++ evaluator honours equally, with Rust as the cross-check. |
| Maintainability | Negative. Every rule change must land in two implementations with the C++ one authoritative by default. |
| Defects found by the oracle | None. Roughly 340,000 live queries, 2,892 serial transactions, and every lookahead run since agreed exactly. The stage's one real defect (HeAP rollback strength, 4A) was found by the mutation audit. |
| Stability | No Rust source changed during 4D, 4E, the list closure, or parallel freezing; the contract has been stable since 4C. |

Decisions:

1. The contract is frozen at ABI V1, ABI V2, and the frozen-batch handle. No new Rust surface.
2. Rust authority is not promoted to the default. The section 8 gate asks for broader device and design evidence that does not exist, and the upside is nil.
3. The crates, the shadow and verify modes, and the fatal cross-check in the transaction path are kept as the parity harness. They are cheap, tested, and the only independent implementation of the rules.
4. Rust ownership is not extended into the next design (cross-build checkpoints and artifact provenance) on the strength of this stage. The C++ side carries ownership discipline with stamps, transactions, and tests; if that design needs compiler-enforced guarantees, that is a fresh decision with its own case.
5. Reopening condition: a revision of `LegacyControlRulesV1`. A rules change is the one moment a second implementation earns its keep; it must then be versioned in both, or the Rust authority modes retired.

### 2026-09-15: Stage 5 opening measurement: where placement and routing time goes

Method: one serial Fabi386 run sampled at 1 ms for its full duration
(`sample`, 22,221 main-thread samples), attributed by inclusive call-graph
counts. Artifacts under `build/stage5-profile/`.

| Phase | Share of run | Inside the phase |
| --- | ---: | --- |
| HeAP proper (solve, spread, strict legalise) | 24.7% | equations and solve direction 46%, conjugate gradient 7%, strict legalise 10%, timing 8% |
| SA refinement (`placer1_refine`) | 38.1% | cost-delta computation 32.8%, move-set construction 18.3%, bind/unbind with ALM input recount 16.3%, timing analysis 15.0%, legality checks 3.4% |
| router2 | 30.6% | priority-queue search 44%, pip availability 17%, timing 8.6% |
| LAB preparation, globals, signoff | under 7% | — |

Timing analysis is 10.7% of the whole run. The design's threshold for
choosing the timing kernel next was about a third, so it is not chosen:
candidate 1c (SA refinement through the transaction seam) comes first,
4b (incremental timing) after it.

One finding changes 1c's shape. In the annealer, the work the seam removes
outright (speculative bind/unbind, the `update_alm_input_count` recount they
trigger, and the legality query) is about 20% of the phase. The work that
can become a detached, parallel assessment is the swap's cost delta and move
set, about 51%. A swap assessment must therefore carry the wirelength and
timing cost delta computed from a frozen position snapshot, not only LAB
legality; otherwise the seam captures a fifth of the phase instead of two
thirds. Router2's cost is algorithmic search and is not addressed by any
candidate except route reuse, which reduces the number of nets searched.

### 2026-09-15: Stage 5 unit 1c-A: annealer swap seam, serial

`placer1`'s refinement gained the transaction seam. With `--sa-seam on`, a
two-cell swap is assessed without touching bindings: legality comes from the
architecture (`Placer1Cfg::assess_swap`), the cost delta is computed from a
position overlay inside the annealer (`bel_overlay`, consulted by
`add_move_cell`, `get_net_bounds`, `ignore_net`, and a bel-based
`predict_arc_delay` that mirrors `Context::predictArcDelay`), the acceptance
RNG is drawn in the original order, and only an accepted swap changes
bindings, through `commit_swap` with a revision-stamp check. Cluster swaps,
net-share scoring, and swaps touching non-LAB BELs stay on the live path.
`--sa-seam shadow` runs both paths for every swap and fails the run if
legality or either cost delta differs.

The first implementation assessed legality by freezing a V2 record per LAB
per swap. It was correct (shadow: 5,665,634 swaps, zero mismatches) and 3.8x
slower than live (SA 37.5 s versus 9.9 s): a V2 capture costs about 5 µs
against roughly 300 ns for the live bind, recount, and checks. It was
replaced by overlay forms of the live rules themselves: `is_alm_legal`,
`update_alm_input_count`, `check_mlab_groups`, and the native control-set
evaluator are now templated on the occupancy lookup, and a `BelOverlay` of up
to four BELs substitutes occupants without binding (`Arch::overlay_bels_legal`
composes them exactly as `isBelLocationValid` does for each overlay BEL). A
64-pattern unit test binds every FF-slot combination across two ALMs and
requires the overlay answer to equal bind-and-ask.

Fabi386, same session, all byte-identical to the Stage 4C artifacts:

| Mode | SA refinement | Wall | Swaps assessed | Illegal / rejected / committed | Unsupported |
| --- | ---: | ---: | ---: | ---: | ---: |
| off (live) | 10.38 s, 10.00 s | 28.5 s, 28.1 s | — | — | — |
| shadow | 13.50 s | 34.2 s | 5,665,634 | zero mismatches | 293 |
| on | 9.29 s, 9.31 s | 27.1 s, 27.1 s | 5,665,634 | 2,802,298 / 2,591,524 / 271,812 | 293 |

So the serial seam is about 8% faster than the live path on the phase while
removing every speculative bind/unbind (the live path performed about
22.7 million; the seam performs 1.09 million, four per accepted swap). The
accepted-swap rate of 4.8% is the number that matters for the next unit: a
batch of speculated swaps evaluated on one snapshot stays valid unless an
accepted swap shares a net or a LAB with it, so most of a batch survives each
commit. The Rust oracle does not cover this path (no V2 record is built);
shadow mode against the live rules is its oracle instead.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` | 54/54 pass (new: seam assess/commit, stamp refusal, unsupported BEL, 64-pattern overlay parity) |
| `./build/nextpnr-mistral-test` (Rust disabled) | 44/44 pass |
| Fabi386 `--sa-seam shadow` | Byte-identical; 5,665,634 checked, zero mismatches |
| Fabi386 `--sa-seam on` (twice) and off (twice) | Byte-identical; timings as tabulated |
| `git diff --check`, `clang-format --dry-run -Werror` on touched C++ | Pass |

Artifacts under `build/stage5-validation/` (`seam-*` are the rejected V2
implementation, `seam2-*` the overlay one) and `build/stage5-profile/`.

### 2026-09-16: Stage 5 unit 1c-B: batched refinement

**Why serial identity is impossible here.** In refinement the annealer draws
its acceptance random only for legal, non-improving swaps, and that draw sits
in the same RNG stream as the next candidate's location draws. About half of
all legal swaps take one, so a speculated batch diverges from the serial
stream after its first such swap. This is the case the design's frozen-epoch
policy was written for, and 1c-B implements it: reproducibility across worker
counts, not byte identity with the serial search.

**Policy.** `--sa-batch N` (with `--threads W`) generates N candidates from
the state at batch start, drawing the location RNG in order; acceptance uses
a second `DeterministicRNG` seeded once from the main stream and advanced once
per candidate. Workers evaluate candidates detached (legality through
`Arch::overlay_bels_legal`, cost delta from the position overlay with one
`MoveChangeData` scratch per worker). The owner consumes in order: a candidate
whose read set (both cells, both BELs, their tiles, every net of both cells)
intersects the footprint of swaps accepted earlier in the batch is re-derived
and re-evaluated synchronously; unsupported candidates run the live path at
their turn; an accepted swap is assessed again with a fresh stamp and its
delta recomputed on the owner before commit. A recomputed delta that differs
from the speculative one is fatal: it means an untracked dependency. Results
depend on the seed and N, never on W. Chain swaps and net-share scoring stay
serial. The worker pool moved from the Mistral coordinator into
`common/place/placement_pool.h/.cc` (dynamic claiming, bounded spin, per-job
exception capture on both the threaded and the inline path) so both consumers
share it.

The verification caught one real defect during bring-up: worker scratch copies
of the net bounds were refreshed only after batched commits, while chain swaps
and the live path for unsupported candidates also commit; a stale copy produced
a wirelength delta of 2 where the truth was 1. Every commit now propagates from
`commit_cost_changes` itself.

**Determinism gate: met.** Fabi386, `--sa-batch 32`, seed 1:

| Workers | SA refinement | Wall | Artifacts |
| ---: | ---: | ---: | --- |
| 1 | 4.38 s | 22.2 s | reference |
| 2 | 3.31 s | 21.5 s | identical |
| 4 | 3.40 s | 21.2 s | identical |
| 8 | 5.41 s, repeat 5.23 s | 23.5 s | identical |
| 16 | 7.60 s | 25.5 s | identical |

Identical means routed JSON and report byte for byte, with 2,175,283
candidates, 393,137 stale re-evaluations (18%), 132,478 commits, 110
unsupported, and zero delta mismatches in every run.

**Quality: inside the serial seed spread, trajectory depends on N.** Serial
runs at seeds 1, 2, 3 end at iterations 38, 34, 31 with wirelength 50,280,
50,996, 52,476, timing cost 408, 476, 319, and Fmax 35.87, 34.98, 32.96 MHz.
Batched runs at seed 1:

| N | Candidates | Ends at iteration | Wirelength | Timing cost | Fmax |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 4,962,611 | 37 | 50,701 | — | 35.07 |
| 4 | 5,582,068 | 38 | 50,639 | — | 35.05 |
| 32 | 2,175,283 | 14 | 51,899 | 456 | 35.11 |
| 128 | 4,498,191 | 29 | 50,395 | — | 35.30 |

N = 1 shows the separate acceptance stream alone reproduces the serial run's
length and quality. Larger N generate candidates from a staler state and
change the trajectory; refinement stops at the first non-improving iteration,
so run length is stochastic, and N = 32 happened to stop early. All results
are within the seed spread; none is a regression beyond what a seed change
produces.

**Scaling: negative beyond two workers.** The best phase time on the N = 32
trajectory is 3.31 s at two workers against 4.38 s at one (1.32x), and it
degrades from four workers on. Per candidate the batched path costs 2.0 µs on
one worker against 1.64 µs for the serial seam, because generation,
pre-filtering, the staleness check, in-order consumption, and the
recompute-at-commit all stay on the owner, while the detached share is under
1 µs. A batch of 32 therefore holds about 30 µs of parallel work against a
pool round trip of comparable size (68,070 round trips; at 8 workers 372,312
spin wake-ups and 104,178 blocking ones). Smaller batches make it worse
(N = 8: 562,475 round trips, 29 s); larger ones raise staleness (N = 128: 32%
re-evaluated) and change the trajectory further. The phase is owner-bound.
The remaining lever is to pipeline generation of batch N+1 during evaluation
of batch N, which hides evaluation entirely and leaves the owner's own cost as
the floor; it is not implemented.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` | 55/55 pass (new: `PlacementWorkerPool` index coverage, dynamic claiming, failure capture on both paths, inline no-op) |
| `./build/nextpnr-mistral-test` (Rust disabled) | 45/45 pass |
| Fabi386 `--sa-batch 32` at 1/2/4/8/16 workers plus a repeat | Byte-identical across worker counts; zero delta mismatches |
| Fabi386 `--sa-batch 1/4/8/128` | Quality inside the serial seed spread |
| `git diff --check`, `clang-format --dry-run -Werror` on touched C++ | Pass |

Artifacts under `build/stage5-validation/` (`batch*`, `compare_batch.sh`,
`run-batch.sh`).

### 2026-09-16: Candidate 4b measured and retired; criticality lookups replaced

The opening measurement attributed 10.7% of the run to `TimingAnalyser::*` by
symbol name. Re-attributing the same 1 ms samples by sampled frame under each
phase gives the real split:

| Timing-related cost | Share of run | Where |
| --- | ---: | --- |
| Arrival/required propagation (`walk_forward`, `walk_backward`) | 2.9% | once per SA/HeAP/router iteration |
| Slack and criticality computation | 0.7% | same |
| Route-delay acquisition | 0.4% | same |
| Topological order and port domains | about 0.6% | once per phase (`setup()`), not per iteration |
| Criticality lookups (`ports.at(CellPortKey)`) | 3.5% | SA 2.45%, HeAP 1.09%: the placers' cost functions, per changed arc per swap |

The propagation an incremental kernel would replace is under 3% of the run,
and the structural rebuild that looked like a reuse target happens three
times per run. Candidate 4b (incremental timing) is therefore retired on the
design's own rule: it must not be chosen from an unexplained remainder, and
the remainder is now explained. What the placers actually pay for is asking
the analyser the same question millions of times through a hash map.

Fix: `placer1` keeps a per-arc criticality table (`net_arc_crit`, indexed
like `net_arc_tcost` by net and user index) refreshed in `setup_costs()`
after each timing run, and the swap cost path reads it instead of hashing a
`CellPortKey`. The stored float is the analyser's own value, so costs are
bit-identical. HeAP's solver got the same table (`net_arc_crit`, keyed by a
net index HeAP now assigns and restores like `placer1`), refreshed after each
of its timing runs and read in `build_solve_direction`.

Fabi386, all runs byte-identical to the Stage 4C artifacts:

| Change | HeAP solve time | SA refinement |
| --- | ---: | ---: |
| before (serial, same session) | 4.83 s, 4.73 s | 10.38 s, 10.00 s |
| placer1 table only | 5.13 s, 5.68 s | 9.94 s, 9.97 s |
| both tables | 3.91 s, 4.06 s, 4.85 s | 9.37 s, 10.63 s, 9.26 s |

The HeAP table saves roughly 15% of the solve phase (about 0.8 s) with one
noisy repeat; the annealer's saving is inside run-to-run noise, so the 2.45%
of `at` samples under SA was not all criticality lookups. Both are kept:
they are strictly fewer hash lookups for identical output.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` | 55/55 pass |
| `./build/nextpnr-mistral-test` (Rust disabled) | 45/45 pass |
| Fabi386 serial, seam-on, and HeAP-table runs | Byte-identical; timings as tabulated |
| `git diff --check`, `clang-format --dry-run -Werror` on touched C++ | Pass |

### 2026-09-16: Stage 5 units 2a-1 and 2a-2: packed and placed checkpoints

`--checkpoint <file>` writes the ordinary output JSON plus a
`nextpnr_checkpoint` object for the last completed phase; `--resume <file>`
replaces `--json`, restores that phase, and runs the remaining phases.
Generic hooks (`BaseCtx::writeCheckpoint`, `checkpointPreload`,
`checkpointRestore`, `checkpointPhase`; a phase argument on
`write_json_file`; a resume flag on `parse_json` that defers
`attributesToArchInfo()`) with the Mistral implementation in
`mistral/checkpoint.cc`. Other backends refuse both options.

The design's field audit was right about the arch-owned fields and wrong
about the netlist: a reload of nextpnr's own JSON does not rebuild the
context the writer had. The design document's new section 2.6 records the
seven findings; in short, `dict` iteration order is history-dependent and
json11 reloads in name order, IdString indices depend on interning order,
the frontend skips the top-level port table for a `synth` file, disconnected
cell ports vanish, the JSON writer interned `"module"` on every write, the
packer already binds QSF-located IO cells before placement, and an undriven
net keeps its removed driver's port name. Each one changed the resumed
trajectory, the output, or the checksum until it was persisted or fixed;
the byte gate found two itself (an output pad placed freely and routed to a
pin with no wire; every route-through net numbered one higher) and the log
checksum found the last.

What the checkpoint therefore carries beyond the design's sections: the
IdString table in index order (replayed before the netlist import, prefix
verified against the process's own table), every iteration order (cells,
nets, settings, module attrs, per-cell ports with direction, attrs, params,
per-net users and attrs; index-encoded), the top-level port table, and
bindings at every phase. Default pin maps are omitted and regenerated by
`assignArchInfo()`. Restored placements pass the live legality check over
every bound BEL before the flow continues.

Fabi386 (`build/stage5-validation/checkpoint/run.sh`, same binary for every
run, `--seed 1 --threads 1`):

| Run | Checkpoint written | Placement / routing checksum | Against the clean run |
| --- | --- | --- | --- |
| clean (pack, place, route) | none | `0xbb18ede9` / `0xbc1365c6` (the Stage 4C values) | reference |
| `--pack-only --checkpoint` | packed: 55,319 idstrings, 11,620 cells, 12,847 nets, 2 top ports, 1,226 cluster cells, 7,813 non-default pin entries of 63,103, 2 io_attr ports, 2 bindings, 144 pllclk selections; 73 MB against a 25 MB output JSON | none | |
| `--resume` packed, then place and route | none | `0xbb18ede9` / `0xbc1365c6` | `--write` JSON identical by `cmp`; `--report` identical |
| `--no-route --checkpoint` | placed: 55,330 idstrings, 11,620 bindings; 82 MB | `0xbb18ede9` | |
| `--resume` placed, then route | none | `0xbc1365c6` | `--write` JSON identical by `cmp`; `--report` identical |

Every count the design's gate named is reproduced on restore (1,226 cluster
cells, 7,813 pin entries), and the byte gate holds without stripping the
`creator` line, since the binary is the same. Before the last finding, the
log checksums of the resumed runs (`0xa00774c6` / `0x882d52a9`) differed
from the clean run's while every output was identical. A structural diff of
routed checkpoints written by both runs showed every field equal by name
and the IdString tables identical, so a per-object dump of
`Context::checksum()` (temporary instrumentation, not committed) was run
for both flows: 11,620 cells and 12,846 nets hashed identically, and one
net differed, the top-level `clk`, in its driver port: the packer's
`disconnectPort` on the removed nextpnr input buffer clears the driver cell
but leaves the port name `O`, which the checksum hashes; a reload has no
stale name. The checkpoint now carries `netlist.stale_driver_ports`, and
with it the checksums agree across process kinds (table above).

Costs: the packed checkpoint is about three times the output JSON, almost
all IdString table (28 MB) and iteration orders (about 30 MB after
index-encoding; 67 MB before). The design's estimate of a few hundred
kilobytes assumed an order-preserving reload. Restore on Fabi386 takes a
few seconds on top of the JSON parse.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` (adds `CheckpointRoundTripRestoresPackingStateAndIterationOrders`) | 56/56 pass |
| `./build/nextpnr-mistral-test` (Rust disabled) | 46/46 pass |
| Fabi386 resume-from-packed and resume-from-placed | `cmp` identical `--write` JSON and `--report` against the clean run; log checksums equal |
| `git diff --check`, `clang-format` on the new and edited lines | Pass (pre-existing drift in `command.cc` untouched) |

Not done in 2a: `input_sha256`/`parent_sha256` lineage and rejecting
command-line options that conflict with the checkpoint's settings (the
frontend's settings import makes the file win, including `threads`);
both are 2b-2 with the manifest lineage. Route-prepared and routed resumes
(2b-1, 2b-2) need the `Arch::route()` split. The four fixtures beyond
Fabi386 named in the design (feature fixture, M10K, PLL, IO register) are
still to run; the PLL one matters because `pllclk_sel` on Fabi386 is 144
default entries.

### 2026-09-16: Stage 5 units 2b-1 and 2b-2: route-prepared and routed checkpoints

`Arch::route()` is split into `prepare_route()` (`lab_pre_route`, then
`route_globals`) and the router call; `--route-prepare-only` stops after the
first half so a `route-prepared` checkpoint can be written, and a resume
from one runs the router only. A `routed` resume runs nothing and goes to
`--write`, `--report`, and `--rbf`. The checkpoint gains `physical.labs`
(every ALM's control allocation and LUT6/carry modes, every LAB's
`aclr_used`), `physical.reserved_wires` (the flags word of every wire with
`RESERVED_ROUTE`), and `physical.routes` (every bound route in `wires` map
order, bound back in reverse), plus manifest lineage: `input` (path and
SHA-256 of the Yosys JSON, propagated through every checkpoint) and
`parent` (path, phase, SHA-256 of the checkpoint resumed from), with a
self-contained SHA-256 checked against the FIPS known answers by the unit
test. Settings the command line adds that the checkpoint never had (such as
`--uncompressed-rbf`) are kept; a differing `--seed` is an error and any
other differing setting a warning, because the file's value is what runs.

The gate for these two units is the bitstream, and it found three things
the JSON, report, and log checksum cannot see (design document section
2.6, findings eight and nine, and the restore order in section 5):

1. Routing preparation leaves constant and unused LUT inputs with an empty
   bel-pin list and `compute_lut_mask` keys on that emptiness; the
   restore's default pin pass refilled 4,569 of them. Recorded pin entries
   now win over the defaults.
2. Applying those entries only after the default pass broke the packed and
   placed bitstreams instead: `assign_ff_info` reads the `PIN_INV` states.
   The states are applied before `assignArchInfo()`, the bel-pin lists
   again after it.
3. With every recorded field equal, the bitstreams still differed by
   24,905 bytes of an uncompressed 7 MB image. A per-write log of the
   bitstream generator (temporary instrumentation, not committed) in both
   flows showed whole LABs taking the LUTRAM path in the resumed run. The
   route-through buffers had never been through `assign_comb_info` in the
   restored context, because `assignArchInfo()` skipped `MISTRAL_BUF`,
   and a zero-filled union reads as MLAB group 0. `assignArchInfo()` now
   covers buffers, which also repairs a plain `--json` reload of a routed
   output.

Two placement-time certifications are not applied after routing
preparation: the packer's route-through buffers sit at LUT BELs outside
both `isValidBelForCellType` and the ALM sharing rules, so for
route-prepared and routed restores the writer's own preparation is the
certification and only the structural checks (BEL exists, claimed once)
run. Bitstream generation also re-runs a signoff timing analysis before
the report is written, so a `--rbf` run's report differs from a run
without it; every comparison below has `--rbf` on both sides.

Fabi386 (`build/stage5-validation/checkpoint/run.sh`, same binary for every
run, `--seed 1 --threads 1`, `--rbf` on every compared run):

| Run | Checkpoint written | Checksums | Against the clean run (`--write` JSON, `--report`, `--rbf`) |
| --- | --- | --- | --- |
| clean (pack, place, route, bitstream) | none | `0xbb18ede9` / `0xbc1365c6` | reference |
| `--pack-only --checkpoint` then `--resume` | packed, 73 MB | `0xbb18ede9` / `0xbc1365c6` | all three `cmp` identical |
| `--no-route --checkpoint` then `--resume` | placed, 82 MB | `0xbc1365c6` | all three `cmp` identical |
| `--route-prepare-only --checkpoint` then `--resume` | route-prepared, 90 MB: 56,428 idstrings, 12,169 cells, 13,396 nets, 42,252 non-default pin entries, 12,169 bindings, 4,191 LABs, 2,392 reserved wires, 1 routed net (the global clock) | `0xbc1365c6` (router only) | all three `cmp` identical |
| `--checkpoint` after a full run, then `--resume` | routed, 110 MB: 56,448 idstrings, 12,866 routed nets | none (nothing runs) | all three `cmp` identical |

The route-through cells (549 `MISTRAL_BUF`), the rewired FF inputs, and the
reservations are all named in the route-prepared checkpoint, which is the
provenance candidate 3c (route reuse) needs.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` (checkpoint test extended: routed round trip, empty bel-pin list, route-through buffer, SHA-256 known answers) | 56/56 pass |
| `./build/nextpnr-mistral-test` (Rust disabled) | 46/46 pass (one run under five concurrent Fabi386 jobs reported a failure that three quiet reruns did not reproduce; the name was not captured) |
| `git diff --check`, `clang-format` on the new and edited lines | Pass |

Costs and open items: the route-prepared checkpoint is about 90 MB and the
routed one about 110 MB (routes are named wires and pips). The fixtures
beyond Fabi386 are the next entry; they found two more things to carry.
Rejecting conflicting options outright, as the design proposed, is not
implementable without knowing which settings the user typed; the
warning-plus-seed-error policy is recorded in the design.

### 2026-09-16: Checkpoint fixtures beyond Fabi386

The design named four fixtures the Fabi386 probe cannot exercise. The
silicon-work designs in `/Users/jvindahl/Development/ext/openflow-test/`
cover them and more: `clkbuftest` (CLKBUF), `plltest` and `gates/g_2pll`
(one and two PLLs), `gates/g_m10k`, `m10ktest`, `m10kshapes` (M10K shapes
with MLAB), `m10kdc` (dual-clock M10K), `gates/g_dsp` (MUL18X18),
`gates/g_ioreg` and `sdrio` (IO registers, the latter also under
`constraints/slot2_ioreg.qsf`), and `fmaxtest`. Each was synthesised with
`yosys -p "synth_intel_alm -family cyclonev"` and run through
`build/stage5-validation/fixtures/validate_design.sh`: a clean flow with
`--rbf`, then the packed, placed, route-prepared, and routed chains, each
resumed with `--rbf` and compared with the clean run by `cmp` on the
`--write` JSON, the `--report`, and the bitstream. The synthesised JSON and
the driver stay under `build/`, outside git.

The first pass found two more things the checkpoint had to carry (design
document section 2.6, findings ten and eleven):

1. Nets with neither driver nor users never come back from a reload: the
   frontend materialises a net only when a cell or port refers to it.
   Packing leaves such nets behind (`c0`, a PLL output it disconnected;
   the 32 `$iobuf_i` halves of the bidirectional buffers it removed on the
   SDRAM design), and the order lists then named nets the design did not
   have. They are recorded with their attributes and recreated first.
2. Placement ends by blocking the PLL reference-clock spine
   (`WireInfo::BLOCKED`, 35 wires, in every design whether or not it has a
   PLL), and the checkpoint carried wire flags only for `RESERVED_ROUTE`
   wires and only from route-prepared on. Placed and route-prepared
   resumes then let router2 take the PLL reference clock through the
   spine, which the raw CRAM writes at bitstream time clobber: the G2/G4
   silicon failure the flag exists to prevent, seen here as a different
   route and bitstream under an identical report. Every non-zero flags
   word is now recorded at every phase.

A detour worth recording: after those fixes, `fmaxtest` and `m10kdc` still
diverged on placed and route-prepared resumes, at router2's first
iteration, while the Rust-disabled tree reproduced the clean run. Dumping
router2's RNG state, shuffled order, criticalities, and per-net routes
showed everything equal until the wire flags, and the cause was mundane:
those designs' placed checkpoints had been written by the binary before
the wire-flags change (the validation job was still running while it was
rebuilt), so their restores lacked the 35 spine flags. The final pass below
was written and resumed by one binary.

Final pass, one binary for every writer and resume, `--seed 1 --threads 1`,
`--rbf` on every compared run; each design resumed from packed, placed,
route-prepared, and routed and compared with its clean run by `cmp` on
`--write` JSON, `--report`, and `--rbf`:

| Design | Cells / nets | Flagged wires | Orphan nets recreated | All four resumes |
| --- | ---: | ---: | ---: | --- |
| `clkbuftest` (CLKBUF) | 72 / 100 | 81 | 0 | identical |
| `plltest` (PLL) | 68 / 94 | 107 | 1 | identical |
| `gates/g_2pll` (two PLLs) | 38 / 49 | 59 | 2 | identical |
| `gates/g_dsp` (MUL18X18) | 125 / 197 | 132 | 0 | identical |
| `gates/g_ioreg` (IO register) | 46 / 55 | 48 | 0 | identical |
| `gates/g_m10k` (M10K) | 48 / 68 | 33 | 0 | identical |
| `m10ktest` (M10K) | 114 / 175 | 87 | 0 | identical |
| `m10kshapes` (M10K shapes, MLAB) | 278 / 479 | 135 | 0 | identical |
| `m10kdc` (dual-clock M10K) | 134 / 206 | 138 | 1 | identical |
| `sdrio` (SDRAM IO, `slot2.qsf`) | 139 / 193 | 117 | 32 | identical |
| `sdrio` (SDRAM IO, `slot2_ioreg.qsf`) | 130 / 196 | 90 | 44 | identical |
| `fmaxtest` | 578 / 885 | 188 | 1 | identical |
| Fabi386 (same binary) | 12,169 / 13,396 | 2,392 | 0 | identical, checksums `0xbb18ede9` / `0xbc1365c6` |

Cells and nets are the routed counts; flagged wires are the non-zero flags
words at the routed phase. Twelve designs, forty-eight resumes, no
difference. Both unit suites pass (56/56 Rust-enabled, 46/46 Rust-disabled);
`git diff --check` and `clang-format` on the edited lines pass.

### 2026-09-16: Stage 5 unit 3c: route reuse

`--reuse-routes <file>` takes a routed checkpoint (`physical.routes`) or any
routed nextpnr output (`ROUTING` attributes) and, after routing preparation
and before the router, preserves every previous route the current design
still allows: same source wire, every current sink on the route and
reaching the source, no stale leaf, every wire free, every pip available
under the current reservations and blocked wires. Preserved routes are
bound `STRENGTH_STRONG`; router2 records them as pre-routed arcs it never
revisits and keeps other nets off their wires, and its final pass rebinds
them weak like its own work. Rationale, including why weak binding was
tried first and rejected, is design document section 9
(`mistral-checkpoint-design.md`). Implementation: `mistral/route_reuse.cc`,
the option in `main.cc`, and the fallback in `Arch::route()`.

Fallback. Router2 does not fail at its iteration cap: it gives up and this
fork's router1 pass legalises what is left. That produced a valid but
different routing with preserved routes in the way, and my first fallback
then re-routed only the eleven dropped nets on top of it in under a second.
Router2 now sets `router_gave_up` on the context, and the reuse path treats
it like a router exception: drop the preserved routes, unbind everything
below `STRENGTH_LOCKED` (12,832 nets on the run below), restore the
pre-router RNG state, run the router again. The result is the uninterrupted
run's routing.

Fabi386, same binary throughout, `--seed 1 --threads 1 --rbf`; the clean
reference is the checkpoint validation's clean run:

| Gate | Run | Preserved | Router2 | Against the reference |
| --- | --- | ---: | ---: | --- |
| Identity | `--resume` placed + `--reuse-routes` routed checkpoint | 12,865 of 12,866 (the global clock is the global router's) | 1.26 s (plain placed resume: 11.07 s) | report and bitstream `cmp` identical; every net's wire, pip, and strength set identical; 151 nets differ only in wires-map order (two alternative sink wires of one user bound in the other order; cause not attributed) |
| Forced fallback | same, `MISTRAL_ROUTE_REUSE_FORCE_FALLBACK=1` | 12,865 dropped | 12.60 s | `--write` JSON, report, and bitstream `cmp` identical (creator line aside) |
| Provenance mismatch | `--seed 2` from the Yosys JSON with the seed-1 routes | 11 preserved, 12,624 rejected as endpoint mismatches, 208 without a previous route | gave up at 100 iterations with 1 overused wire (13.05 s); fallback unrouted 12,832 nets and routed again (13.99 s) | JSON, report, and bitstream `cmp` identical to a clean `--seed 2` run (11.70 s) |
| Edited, 40 LUT INIT edits | `--reuse-placement` + `--reuse-routes` vs clean edited run | 12,671 of 13,397 (94.6%); 193 endpoint mismatches | 19.62 s over 29 iterations (clean: 10.25 s, 20) | placement 0.24 s vs 6.66 s; Fmax 34.72 vs 35.71 MHz |
| Edited, + 40 input swaps | same | 12,503 (93.3%); 361 endpoint mismatches | 20.17 s over 40 iterations (clean: 10.23 s, 49) | placement 0.46 s vs 7.21 s; Fmax 35.18 vs 34.24 MHz |

Reading. The mechanism is correct: preserved routes are never invalid
(unit test covers a foreign source, a stale leaf, a blocked pip, a wire
another net holds, an already-routed net, and the drop), an unchanged
design reproduces the clean run, and both fallbacks reproduce the clean
run rather than a third trajectory. The edited-design router times in the
table above were measured with five validation jobs sharing the machine
and are wrong by about two times; the closing measurement below reran
them alone (router2 8.31 s against 9.12 s clean on the INIT edit, 10.22 s
against 9.24 s on the swaps) and attributed the extra iterations and the
`archfail` count to a bind-order collision in router2 that unit 3c-2
removes. Quality is inside the seed spread either way. On the unchanged
design the router is ten times faster. Route reuse stays opt-in and
unpromoted.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` (adds `RouteReuseKeepsOnlyRoutesTheCurrentDesignStillAllows`) | 57/57 pass |
| `./build/nextpnr-mistral-test` (Rust disabled) | 47/47 pass; one run under five concurrent Fabi386 jobs reported a failure that 25 quiet reruns did not reproduce (name not captured) |
| `git diff --check`, `clang-format` on the new and edited lines | Pass |

### 2026-09-16: Stage 5 units 3a and 3b: reuse plan, placement region expansion, typed build states

Rationale is design document section 10 (`mistral-checkpoint-design.md`).
Three deliverables, all in C++:

1. **The reuse plan** (`mistral/reuse_plan.*`, 3a). Placement reuse and
   route reuse now compute a `ReusePlan` before applying anything: one
   decision per cell and per net with a one-line reason.
   `--reuse-plan-out file.json` writes it; `--reuse-dry-run` writes it and
   applies nothing. Applying a plan validates each decision again against
   the live design. The Stage 4E adapter and the 3c module keep their entry
   points and are implemented on the plan.
2. **Placement region expansion** (3b, `Arch::run_placement`). On a placer
   failure the transplants within a growing radius (2, 5, 12 tiles) of the
   dirty cells are released (BEL attribute cleared, decision `released`),
   everything the placer bound is unbound, the pre-placement RNG state is
   restored, and the placer runs again; the last rung releases every
   transplant. `MISTRAL_PLACEMENT_REUSE_FORCE_FALLBACK=n` fails the first n
   attempts; a real failure has not been provoked on Fabi386.
3. **Typed build states** (`mistral/build_state.*`, 3a). `Build<Phase>` is a
   move-only handle; `place_build`, `prepare_build`, `route_build`, and
   `validate_build` consume their input, so routing a packed build or
   placing twice does not compile (`static_assert`s in the unit test).
   `Arch::place()`, `Arch::route()`, and the bitstream writer adopt the
   context into the phase they need, which is a runtime check at the
   legacy boundary, and go through the transitions; `validate_build`
   checks the context and that every arc of every driven net reaches a
   sink over the net's own pips. `--rbf` on a packed or placed design is
   now refused where it wrote a meaningless bitstream before.

Fabi386, same binary throughout:

| Gate | Run | Result |
| --- | --- | --- |
| Plan against a controlled edit | `--reuse-dry-run --reuse-plan-out` on the 40 LUT INIT edits | 40 cells `changed`, every one of the generator's picks and no other, all with reason `parameter LUT differs`; 11,578 `reuse`, 2 `user-constrained` |
| Plan against a controlled edit | same on the 40 INIT edits plus 40 input swaps | 80 `changed`: the 40 INIT cells with `parameter LUT differs` and the 40 swapped cells with `connectivity of port A differs`, exact match with the generator's picks (its RNG replayed, including the per-cell bit draws) |
| Ladder, two forced failures | LUT INIT edit with placement reuse | attempt 1 released 3,606 transplants within 2 tiles, attempt 2 a further 5,844 within 5; attempt 3 placed (HeAP 3.5 s) with 2,128 transplants kept, routed, validated: 42,188 arcs |
| Ladder, full fallback | four forced failures | 2,126 more within 12 tiles, then the last 2 released; placed from scratch (HeAP 5.5 s). Its placement and routing checksums equal the clean edited run's (`0xc9e140ca / 0xa66e5260`): the last rung is the clean placement. |
| Phase check | `--pack-only --rbf`; placed resume `--no-route --rbf` | both refused: `Build phase is 'packed'/'placed', but this step needs 'routed'`; no bitstream written |
| Identity through validation | placed resume + `--reuse-routes` + `--rbf` | 12,865 routes preserved, router2 1.13 s, `Validated: 42196 arcs routed`, report and bitstream `cmp` identical to the clean run |

The dry run's route plan is computed on the clean placement the dry run
produces, so it reports endpoint mismatches for almost every net; the
route decisions that matter are the ones a real reuse run writes.

| Command | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` (adds `BuildStatesAreTypedAndPhaseCheckedAtTheBoundary`; plan and release checks in the placement reuse test) | 58/58 pass |
| `./build/nextpnr-mistral-test` (Rust disabled) | 48/48 pass |
| `git diff --check`, `clang-format` on the new and edited files | Pass |

With this the Stage 5 candidate list from the opening measurement is
exhausted: 1c, 4b (retired), 2a, 2b, 3c, 3b, 3a. What remains open is
recorded per unit: the annealer is owner-bound, route reuse costs the
same router time as a clean run on edits (the "doubles" first recorded
here was a contended measurement; see the closing measurement), placement
region expansion has not met a real failure.

### 2026-09-16: Stage 5 closing measurement: where the time goes now, and one attributed defect

Method as in the opening entry: `sample <pid> 600 1 -mayDie -f`, attribution by
inclusive call-graph counts with `build/stage5-profile/attribute.py` (rewritten
this session: phases are charged on the main thread only, because router2 runs
its worker on a second thread while the main thread waits in `join`, and worker
frames would otherwise double count; the annealer takes its count away from
HeAP, and the router1 legality pass from router2). Calibrated on the opening
sample first: HeAP 25.1%, annealer 38.1%, router2 28.8% plus router1 check 1.8%,
against the opening entry's 24.7 / 38.1 / 30.6.

Fabi386 clean (`--seed 1 --threads 1`, legacy, checksums 0xbb18ede9 / 0xbc1365c6,
25,083 main-thread samples) and the 40-edit design with `--reuse-placement` and
`--reuse-routes` from the checkpoint validation's clean run (13,038 samples):

| Phase | Clean, opening | Clean, closing | Edit with both reuse paths |
| --- | ---: | ---: | ---: |
| chipdb load | 3.0% | 1.2% | 5.9% |
| load, pack | 1.0% | 0.9% | 1.9% |
| HeAP | 25.1% | 19.1% | 2.5% |
| annealer refinement | 38.1% | 40.5% | 3.9% |
| placement reuse | | | 2.9% |
| route reuse checks | | | 8.9% |
| router2 | 28.8% | 35.1% | 67.9% |
| router1 legality check (0 arcs) | 1.8% | 2.1% | 4.0% |
| write, report, other | 2.2% | 1.1% | 2.0% |

Inside router2, across the driver and its worker (the shares are of router
work, wait excluded): `route_net` 83% clean / 76% edit, of which cost
functions (`score_wire_for_arc`, `get_togo_cost`) 28% / 26%, pip availability
(`checkPipAvailForNet`, `is_pip_blocked`, bound-pip lookup) 18% / 24%, priority
queue 21% / 17%, `dict` lookups 10% / 9%; timing analysis 4.5% / 6.7%;
`is_wire_undriveable` scans 3.5% / 3.9%.

Reading. The clean flow's shape is unchanged, as intended: no default changed
in Stage 5. The reuse flow on an edit halves the run (13,038 against 25,083
samples) by removing placement (60% to 6%), and what remains is router2 at
68%, with the reuse checks (9%) and the chipdb load (6%) behind it.

**Correction to the 3c entry.** The 3c edited-design timings (19.6 and 20.2 s
against 10.3 s clean) were taken with five validation jobs running at once.
Rerun sequentially with nothing else on the machine, same binary and inputs:

| Edit | Clean router2 | Reuse router2 | Iterations clean / reuse |
| --- | ---: | ---: | --- |
| 40 LUT INIT edits | 9.12 s | 8.31 s | 20 / 29 |
| 40 input swaps | 9.24 s | 10.22 s | 49 / 40 |

Route reuse does not double router time on an edit; it costs the same, with
93 to 95% of the routes applied. The 3c decision row is corrected below.

**Attributed: the extra iterations are a bind-order collision in router2.**
The reuse run's iteration 25 reached zero overuse and then logged
`archfail=4052`; the clean run's first zero-overuse bind failed nothing. A
`--verbose` rerun classifies all 4,066 failures as wire failures
(`checkWireAvail` false, wire bound to another net), none as pip failures,
across 1,255 failing and 825 blocking nets, every one of them a net the plan
had marked `reuse`. Mechanism, from `router2.cc`: `setup_wires` registers
every pre-bound wire, and `check_arc_routing` marks the reused arcs routed,
but a routed arc is still ripped up in the router's model when it is overused
(`ripup_arc` in `route_net`), and its context binding stays until
`bind_and_check_all`. That pass rips up and rebinds one net at a time, so an
earlier net whose new route crosses the stale binding of a later, re-routed
net fails the bind for nothing, is queued again, and costs a whole
iteration. The clean flow never sees it because nothing is bound in the
context before the first bind pass. Note for the 3c reading: "router2 treats
preserved routes as fixed" is not what happens; it may re-route them, and the
94.6% is what was applied, not what survived.

Prototype (10 lines: unbind every net's weak and strong wires first, then
bind), same inputs, sequential and alone:

| Run | Before | After |
| --- | --- | --- |
| Edit (INIT), reuse | 8.31 s, 29 iterations, archfail 4,052 then 4,066 | 6.53 s, 25 iterations, archfail 0 |
| Edit (swaps), reuse | 10.22 s, 40 iterations, archfail 4,073 | 7.47 s, 36 iterations, archfail 0 |
| Edit (INIT), clean | 9.12 s, 20 iterations | 8.59 s, 20 iterations; both log checksums identical (0xc9e140ca / 0xa66e5260) |
| Fabi386 clean gate | 0xbb18ede9 / 0xbc1365c6 | 0xbb18ede9 / 0xbc1365c6, identical |
| Reused routes identical at the end (INIT edit) | 10,488 of the 12,671 applied (82.8%) | 10,386 (82.0%); swaps 10,169 of 12,503 (81.3%) |

Run-to-run noise on identical clean runs is about 6% (9.12 against
8.59 s), so the after-column gains of 21% and 27% on the two edits are
real, and the clean flow is unchanged byte for byte. The prototype is
kept as unit 3c-2 (validation table below). Route survival is the number
to watch from here: about 18% of the applied routes are re-routed by
router2 on both edits, so "preserved" in the 3c table means applied, not
final; the Stage 5 reading that router2 treats preserved routes as fixed
was wrong (`ripup_arc` takes pre-routed arcs like any other once they are
overused), and CLAUDE.md is corrected in this commit.

Unit 3c-2 validation, one binary for every run:

| Check | Result |
| --- | --- |
| `./build/rust-enabled/nextpnr-mistral-test` alone | 58/58 pass (one failure of `BatchCoordinatorParallelFreezeMatchesOwnerFreeze` while ten fixture flows shared the machine; 5/5 repeats pass alone, same unattributed load sensitivity as before) |
| `./build/nextpnr-mistral-test` (Rust disabled) | 48/48 pass |
| Fabi386 clean, `--seed 1 --threads 1` | checksums 0xbb18ede9 / 0xbc1365c6, archfail 0 at iteration 20, as before |
| Edited design (INIT), clean | checksums 0xc9e140ca / 0xa66e5260, identical to the pre-change run |
| Checkpoint fixtures, all eleven designs, `validate_design.sh` (`slot2.qsf`, `slot2_ioreg.qsf` for sdrio) | Every packed, placed, route-prepared, and routed resume `cmp`-identical to its clean run on JSON, report, and bitstream; every clean run's two log checksums equal the pre-change fixture pass; archfail 0 everywhere |
| `git diff --check`, `clang-format` | Pass |

**Concurrency ceiling (asked after the measurement).** The same Fabi386
run with every parallel option the branch has, one run each, sequential
and alone (`/usr/bin/time`, wall and CPU):

| Configuration | Wall | HeAP | Annealer | Router2 | Fmax | CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `--threads 1` | 25.8 s | 5.5 s | 9.0 s | 8.0 s | 35.87 MHz | 31 s |
| `--threads 4` / `8` / `16`, nothing else | 25.7 / 27.8 / 28.6 s | 5.4 to 5.6 s | 9.0 to 10.4 s | 8.1 to 9.1 s | 35.87 MHz | 31 to 34 s |
| `--threads 8` + `--sa-seam on --sa-batch 32 --placer-lookahead 4` | 22.3 s | 5.6 s | 5.1 s | 8.2 s | 35.11 MHz | 52 s |
| `--threads 16`, same options | 24.7 s | 5.7 s | 7.8 s | 7.8 s | 35.11 MHz | 100 s |
| `--threads 2` + `--sa-seam on --sa-batch 32` | 19.2 s | 5.2 s | 3.0 s | 7.8 s | 35.11 MHz | 27 s |

`--threads` never reaches router2: it partitions nets into a fixed four
quadrants plus boundary passes and spawns exactly those threads
(`partition_nets`, `do_route`), which is why its checksums are identical
at every thread count and the sample showed worker threads at
`--threads 1`. HeAP is serial and the lookahead only parallelises strict
legalisation (under 2.5% of the run). The annealer is the only phase
that gains, and it peaks at two workers, as 1c-B recorded. The serial
baseline is 25.8 s today, not the 35 s median the Stage 4C entry
recorded. The concurrent ceiling on this branch is therefore about 1.35x,
and moving it needs a router partitioned by thread count and a placer
that is not owner-bound, neither of which exists here.

**Recommendation for the next unit, from the measured remainder.**
After 3c-2 the edited-design flow with both reuse paths spends its time
in router2 (about 68% before the fix, still the majority after it), and
inside router2 in `route_net` for the roughly 18% of applied routes that
are re-routed plus the 5 to 7% that were never applied. The next unit
that the measurement supports is therefore router-side, not placement-side:
make a preserved route that router2 rips up count as "changed" in the
reuse report (survival is currently invisible outside this script), and
then reduce the re-routing itself, most plausibly by seeding router2's
history cost from the previous run so the preserved routes are not the
first thing the negotiated-congestion loop sacrifices. The chipdb load
(6% of the edit run) and the reuse checks (9%) are the only other
measurable items, and both are bounded by I/O and one pass over the
nets. Nothing in the clean flow moved, by design; its shape (annealer
40%, router2 35%, HeAP 19%) is the same as at the opening.

### 2026-09-16: Stage 5 unit 3c-3: route survival is measured and reported

The closing measurement found that router2 re-routes about 18% of the
routes the reuse path applies, and that the only way to see it was an
external script. The tool now measures it: after the router,
`measure_route_survival` (`mistral/route_reuse.cc`) compares every applied
route, wire for wire and pip for pip, with the net's final binding. The
`Route reuse:` line reports survived and re-routed counts next to applied;
when `--reuse-plan-out` is given, the plan is rewritten after routing so
each reused net carries `survived` and the summary a `route_survival`
object. The comparison uses the wire and pip ids resolved when the route
was applied, so it interns nothing. Nothing on the clean path changed.

| Run (`--seed 1 --threads 1`) | Applied | Survived | Re-routed |
| --- | ---: | ---: | ---: |
| Unchanged design, placed resume + routed checkpoint | 12,865 | 12,865 (100%) | 0 |
| 40 LUT INIT edits, `--reuse-placement` + `--reuse-routes` | 12,671 | 10,386 (82.0%) | 2,285 |
| + 40 input swaps, same | 12,503 | 10,169 (81.3%) | 2,334 |

The counts equal the closing measurement's external comparison. Tests:
`./build/rust-enabled/nextpnr-mistral-test` 58/58 (the route reuse test
now covers a surviving route, one changed by unbinding a pip, and the plan
stamp), `./build/nextpnr-mistral-test` 48/48, `git diff --check` and
`clang-format` clean.

### 2026-09-16: Stage 5 unit 3c-4: history seeding for preserved routes

The closing measurement's last recommendation. Router2 negotiates
congestion with a per-wire history cost that grows only after a wire has
been overused, so a dirty net pays nothing to take a preserved wire in its
first iteration, both nets are then overused, and the preserved arc is
ripped up as readily as the dirty one; 18% of applied routes ended
re-routed. `--reuse-routes-history H` (opt-in, default 1.0 = off) seeds
router2's history cost with H on every wire bound at `STRENGTH_STRONG`
before the router runs, so the dirty nets treat the preserved wires as
contested from the start and route around them. One branch in
`setup_wires` (`Router2Cfg::prerouted_hist_cost`, set by the arch from
`ArchArgs`, no settings key so nothing is interned) and one option.

Sweep, `--seed 1 --threads 1`, `--reuse-placement` + `--reuse-routes`
from the checkpoint validation's clean run, sequential and alone:

| Edit | H | Router2 | Iterations | Survived of applied | Fmax |
| --- | ---: | ---: | ---: | ---: | ---: |
| 40 LUT INIT edits | 1 (off) | 7.34 s | 25 | 10,386 of 12,671 (82.0%) | 33.54 MHz |
| | 2 | 4.58 s | 9 | 11,887 (93.8%) | 31.94 MHz |
| | 4 | 3.70 s | 10 | 12,389 (97.8%) | 35.38 MHz |
| | 8 | 3.02 s | 8 | 12,592 (99.4%) | 36.41 MHz |
| | 16 | 2.81 s | 6 | 12,645 (99.8%) | 35.47 MHz |
| + 40 input swaps | 1 (off) | 7.55 s | 36 | 10,169 of 12,503 (81.3%) | 35.35 MHz |
| | 2 | 5.76 s | 27 | 11,633 (93.0%) | 35.67 MHz |
| | 4 | 4.34 s | 9 | 12,179 (97.4%) | 34.84 MHz |
| | 8 | 4.05 s | 7 | 12,398 (99.2%) | 33.54 MHz |
| | 16 | 3.80 s | 7 | 12,453 (99.6%) | 35.20 MHz |

Reading. Seeding turns the preserved routes into what the 3c reading
assumed they already were. At H = 8 the router keeps 99% of what was
applied, finishes in a quarter to a third of the iterations, and takes
3.0 to 4.1 s against 7.3 to 7.6 s unseeded and 8.6 to 9.1 s for a clean
route of the same designs; the whole edit flow is then about 10 s against
26 s clean. Fmax moves inside the seed spread in both directions (31.9 to
36.4 MHz across the sweep against 35.87 MHz clean), as every reuse run
does. H = 16 buys little more and pushes the dirty nets harder, so 8 is
the value recorded here; the option stays off by default and unpromoted,
like every reuse path. The clean flow is untouched: the branch is dead
when the option is absent, and the Fabi386 clean gate reproduces
0xbb18ede9 / 0xbc1365c6.

| Check | Result |
| --- | --- |
| Fabi386 clean, `--seed 1 --threads 1`, option absent | checksums 0xbb18ede9 / 0xbc1365c6 |
| `./build/rust-enabled/nextpnr-mistral-test` | 58/58 pass |
| `./build/nextpnr-mistral-test` (Rust disabled) | 48/48 pass |
| `git diff --check`, `clang-format` | Pass |

### 2026-09-16: Fabi386 exec probe through Quartus 17 on the identical netlist

Asked for after the closing measurement: how the current Fabi386 run
compares with the Quartus runs recorded on the NAS. Those runs are not
comparable with ours. They build the full `f386_mister` revision (core
inside the MiSTer framework: 33.5k to 36k ALMs at 80 to 86% of the
device, 28.6k to 31.9k registers, 41 or 42 DSPs, 1.6 Mbit of block RAM,
three PLLs, HPS bridges), with production settings (physical synthesis
for speed, high-performance effort, `parallel=2`) on the NAS's Ryzen 7
2700; the fitter alone took 59 min to 2 h 29 min across the ten full
runs of April to July, and the CPU clock (`emu|pll_inst|u_pll|general[0]`,
33.3 MHz target) reached 15.8 to 18.3 MHz under the slow 1100 mV 100 C
model. Our regression input is the execution-unit probe only: 10,607
LUT cells and 989 registers at 12% of the device, no memory, DSP, or
PLL, which we place and route in 26 s wall on an M1 Ultra (19 s with
the batched annealer) to 35.87 MHz under nextpnr's single corner. Same
device, different design, different scope of work, different machine.

The like-for-like run: the exact netlist nextpnr routes
(`build/fabi386-inputs/f386_exec_probe_nodsp.json`) handed to Quartus
Prime Lite 17.0.2 in the NAS container, with the production settings of
the recorded runs (`f386_mister.qsf` globals), the two pins of
`exec_probe.qsf`, and a 12 MHz `create_clock` to match `--freq 12`, plus
a second job at 50 MHz to make the fitter push. Two hand-overs were tried:

1. Yosys `write_verilog` of the JSON with the cells left as `MISTRAL_*`
   and Yosys's behavioural models as source. Quartus's name builder
   crashed on the hundreds-of-characters escaped instance names
   (`SGN_NAME_MAKER::process_group_name`, internal error); with every
   cell and wire renamed to a short private name (`rename -hide;
   rename -enumerate`) it synthesised, but the `MISTRAL_ALUT_ARITH`
   model (`{CO, SO} = q0 + q1 + CI`) did not land on carry chains:
   18,442 ALUTs and 10,113 ALMs for 10,607 cells, 1,523 registers for
   989, and 11.1 MHz Fmax at both constraints. That is an artefact of
   the hand-over, not a Quartus result, and it is recorded so nobody
   repeats it.
2. Yosys `techmap` with upstream Yosys's own Quartus map
   (`techlibs/intel_alm/common/quartus_rename.v` at yosys-0.33, which
   0.69 no longer ships; output buffer adapted for a cell without OE):
   every cell becomes a `cyclonev_lcell_comb` (the arithmetic cells with
   `lut_mask({16'h0, LUT1, 16'h0, LUT0})`, `datad`/`dataf`, `cin`,
   `sumout`, `cout`), `dffeas`, `NOT`, `cyclonev_io_*`, or
   `cyclonev_clkena`; 10,607 + 989 + 89 + 2 + 1 instances, WYSIWYG
   remapping off so Quartus fits the netlist as given (physical
   synthesis left on, as in production).

| Flow, same 10,607-cell netlist | Place and route | Fmax (`clk`) | ALMs / registers |
| --- | ---: | ---: | ---: |
| nextpnr, `--seed 1 --threads 1 --freq 12`, M1 Ultra | 26 s wall (HeAP 5.5 s, annealer 9.0 s, router2 8.0 s) | 35.87 MHz, single corner (1.1 V, 100 C) | 8,179 ALMs (19.5%), 11,177 comb cells incl. 549 route-throughs, 989 registers |
| nextpnr, `--freq 50` | 30 s wall, same routing (20 iterations) | 35.87 MHz (the run fails its target) | same |
| Quartus 17.0.2, WYSIWYG, 12 MHz constraint, NAS `parallel=2` | map 31 s, fitter 3 min 15 s, timing 20 s, assembler 9 s | 64.7 MHz slow 1100 mV 100 C (66.6 MHz at -40 C) | 5,067 ALMs (12%), 9,269 ALUTs, 1,858 registers |
| Quartus 17.0.2, WYSIWYG, 50 MHz constraint | map 31 s, fitter 3 min 19 s, timing 20 s, assembler 9 s | 69.5 MHz slow 1100 mV 100 C (71.8 MHz at -40 C) | 5,070 ALMs (12%), 9,262 ALUTs, 2,125 registers |

Reading. On identical logic, at the same 1.1 V 100 C corner (nextpnr's
hard-coded corner is the one Quartus calls its slow 1100 mV 100 C
model), Quartus reaches 1.8 to 1.9 times our Fmax and fits the design
into 62% of our ALM count, pairing 1.8 LUTs per ALM against our 1.4.
Its fitter takes 3 min 15 s on the NAS at `parallel=2` against our 26 s
wall on a faster machine: about eight times, perhaps four once the
machines are normalised. The constraint moved Quartus by 7% (64.7 to
69.5 MHz, with 267 more duplicated registers) and moved us not at all:
router2 is not timing-ripup driven and HeAP normalises criticality, so
`--freq` changes the report's verdict, not the routing. Quartus's
physical synthesis rewrote its copy of the netlist (989 registers became
1,858 and 2,125; 10,607 LUTs became 9,269 ALUTs); ours is placed and
routed as given. Neither Fmax is silicon-verified for this design; they
are the two tools' timing engines at one corner. What the Fabi386
pipeline notes recorded as "Fmax-vs-Quartus unproven" is now a number
for the probe: the quality gap is 1.8x on timing and 1.6x on packing
density, the speed gap is in our favour by an order of magnitude, and
the full core remains unattempted. Artifacts: NAS
`fabi386_jobs/exec_probe_20260916{b,w}` (behavioural and WYSIWYG jobs
with reports and bitstreams), local staging under
`build/stage5-profile/quartus-probe/` (the JSON-to-Verilog script, the
adapted techmap, both netlists, and the qsf/sdc per job).

**Why Quartus's timing is better (asked next; two more jobs).** The
1.8x splits into three measured factors:

| Factor | Measurement | Share |
| --- | --- | --- |
| Netlist rewrite | Same WYSIWYG netlist with every physical-synthesis and netlist-optimisation option off (`exec_probe_20260916n`): 58.0 MHz against 64.7 MHz with them on; the fitter log credits register retiming with 2.0 ns of slack and the critical path ends at a `NEW_REG` it created | 1.12x |
| Timing model | The silicon-calibrated `fmaxtest` chain (502 LUT cells, one carry chain, `build/stage5-validation/fixtures/fmaxtest.json`) through both tools the same way (`fmaxtest_20260916`, no physical synthesis): Quartus 41.6 MHz, nextpnr 34.1 MHz, silicon between 45 and 52 MHz per the gaps file's calibration. Placement has little room on one chain, so this is the two models at the same 1.1 V 100 C corner: nextpnr's is 18% below Quartus's and 25 to 35% below silicon | 1.22x |
| Placement and routing | The remainder, 58.0 against 35.9 x 1.22 | 1.32x |

The paths themselves say where the 1.32x lives. Quartus's critical path
(`report_timing -detail full_path` on the 12 MHz job) is 15 LUT levels
and 16 interconnect hops averaging 0.74 ns, no carry cell on it, and
every critical LUT input is on the fast F input (0.09 ns per cell);
nextpnr's is 30 arithmetic cells and 2 LUTs with 22 general routing hops
averaging 0.99 ns (the multiplier's adder rows, entered through their
data inputs), 78% interconnect either way. Quartus packs 1.8 LUTs per
ALM against our 1.4, so its hops are shorter, and it permutes LUT
inputs for timing; our placer and router optimise against
`getPipDelay`'s per-wire-type constants (the gaps file: "P&R optimises
guesswork and only signoff sees reality"; signoff and the constants
agree here, 35.71 against 35.87 MHz). Nothing in this branch's Stage 5
work touches any of the three; they are the next quality work if
timing is the goal, in the order model, placement, then rewrite.

### 2026-09-17: Timing model attribution against Quartus on the fmaxtest chain

The first recommendation after the closing measurement: attribute the
1.22x model gap before any placement work. Vehicle: `fmaxtest` (502
LUT cells, 51 registers, one carry chain, silicon between 45 and 52 MHz
per the gaps file's calibration), the same WYSIWYG netlist through
Quartus 17.0.2 without physical synthesis (`fmaxtest_20260916` on the
NAS, 41.64 MHz, `report_timing -detail full_path`) and through
nextpnr's signoff (`--rbf`, 34.47 MHz; the router's constant table says
34.07). Both critical paths have the same eight-stage structure:
register, carry entry through a data input, three or four carry hops,
sum out, one LUT on its fast input, general routing, next stage.

Element by element, same grade and corner (1.1 V, 100 C, maximum
delays):

| Element | Quartus | nextpnr signoff | nextpnr / Quartus |
| --- | ---: | ---: | ---: |
| Carry entry, data input to carry out (8 per path) | 0.830 ns | 1.157 ns | 1.39 |
| Carry in to sum out | 0.314 ns | 0.370 ns | 1.18 |
| Carry in to carry out | 0.013 ns | 0.078 ns | small either way |
| LUT on its fast input | 0.084 ns | 0.100 ns | 1.19 |
| Logic total on the path | 10.2 ns | 11.8 ns | 1.16 |
| General routing hop (16 and 17 per path) | 0.85 ns | 1.01 ns | 1.19 |
| Routing total on the path | 13.6 ns | 17.2 ns | 1.26 |
| Path | 24.0 ns | 29.0 ns | 1.21 |

What each half is. The cell numbers are the hard-coded constants in
`mistral/delay.cc` ("1.1V 100C corner of sx120f"). Aggregating the cell
rows of 3,000 Quartus paths from each job (`report_timing -npaths 3000
-detail full_path`, 357k and 517k rows, `quartus_cell_arcs.json` in the
staging directory) shows those constants are the -7 table already,
arc for arc: LUT inputs A to F 0.605 / 0.583 / 0.510 / 0.512 / 0.400 /
0.097 ns in nextpnr against Quartus maxima 0.603 / 0.582 / 0.512 /
0.514 / 0.341 / 0.101; carry entry through C 0.813 against 0.880; A to
sum out 1.342 against 1.304; carry in to sum out 0.368 against a
Quartus median of 0.355 (maximum 0.527); carry in to carry out 0.036
against a median under 0.046. The 16% on the path is not the table:
Quartus enters every carry cell through its C input (0.83 ns) where
our netlist enters through A or B (1.06 to 1.16 ns, the same constants
for those inputs), and it rides the LUTs on their F input. That is
input assignment, a placement decision Quartus makes and we do not.
The routing numbers are libmistral's analog simulation of the routed
pip chain. Sweeps on the routing half, placement and routing identical
throughout (checksums 0x5c9de842 / 0xfda10b63):

| Switch | Routing on the path | Signoff Fmax |
| --- | ---: | ---: |
| Device name `5CSEBA6U23I7`, `C7`, `A7` (grade 7) | 17.19 ns | 34.47 MHz |
| `5CSEBA6U23C6` | 15.56 ns | 36.52 MHz |
| `5CSEBA6U23C8` | 19.32 ns | 32.12 MHz |
| `MISTRAL_SIGNOFF_TEMP=85` / `0` (grade 7) | 16.92 / 15.87 ns | 34.79 / 36.11 MHz |
| `MISTRAL_SIGNOFF_EST=fast` | 17.19 ns (no change) | 34.47 MHz |
| `MISTRAL_SIGNOFF_BOUND=min` (sum of per-stage lower bounds) | 15.20 ns | 37.02 MHz |

So libmistral applies the speed grade and the -7 grade is what the I7
device gets; the input edge-speed setting changes nothing; temperature
is worth 8% at 0 C, which is not a legitimate setting; and the interval
the simulator returns per stage (`delay x timing_scale x cor_factor`,
upper and lower) is worth 12% end to end, with even the lower bound
12% above Quartus. The routing model's pessimism is therefore in
libmistral's reverse-engineered correction factors or in chaining one
simulation per pip, and that is an upstream question, not a branch
change. The cell side is ours, but as placement, not as data: the
multiplier rows of the Fabi386 probe enter their carry cells through
A and B at 1.06 to 1.32 ns per stage where the C and D inputs cost
0.81 to 0.87 ns with the same table.

Against silicon (45 to 52 MHz, room temperature), Quartus's 100 C
model is 8 to 20% pessimistic and nextpnr's 30 to 45%. The three new
`getenv` switches (`MISTRAL_SIGNOFF_TEMP`, `MISTRAL_SIGNOFF_EST`,
`MISTRAL_SIGNOFF_BOUND`) change the report only, never placement or
routing, and are off unless set; the Fabi386 clean gate with `--rbf`
reproduces its checksums.

Recommended unit from this: input permutation for timing, on the
placement side. The per-input constants are right and unused: assign
each cell's critical signal to its fastest permutable input (C or D on
a carry cell, F then E on a LUT) with the LUT mask permuted to match,
during or after placement, driven by the criticality the placer
already computes. Measurable targets: fmaxtest logic from 11.8 to
about 10.2 ns, the probe's nine carry entries from 1.06 to 1.32 down
to 0.81 to 0.87 ns. It changes sink wires and therefore routing, so it
is validated on Fmax against Quartus and silicon, not by byte identity,
and it stays opt-in like every Stage 5 capability. The cell constants
stay as they are; the routing bound stays as it is until silicon says
otherwise.

### 2026-09-17: First full-core Fabi386 attempt through nextpnr

The second item of the post-closing plan. Design: the CPU core top
(`f386_ooo_core_top`, the Fabi386 repo's `yosys-full` file list and
defines, sv2v then `synth_intel_alm -family cyclonev`) wrapped the way the
exec probe was: every input driven from a 414-bit register bank stepped
as an LFSR, every output XOR-reduced onto one registered pin, two pins
total (`build/stage6-fullcore/f386_core_probe.v`, generated from the
core's port list). `-nodsp`, because Yosys mapped 27x27 and 9x9
multipliers the branch does not support; the plain core has 1,175 ports
against 472 IO bels, and the packer refuses a top port without an IO
cell, which is why the wrapper is needed at all. Synthesis: 9 min for
the core, 37 min for the probe (ABC on the XOR tree). Result: 39,425 LUT
cells (3,276 arithmetic), 15,647 registers, one M10K, 83 MB of JSON;
47% of the comb bels, about 28,000 ALMs at our 1.4 cells per ALM (67%).

Run 1, reference configuration (`--seed 1 --threads 1 --freq 33
--router2-max-iter 100 --timing-allow-fail --ignore-loops`; the core's
RTL has combinational loops between the store-index stall and the
renamer's busy bits, which Quartus only warns about). HeAP's strict
legaliser never finished its first main iteration: the queue went from
5,194 cells at rip-up radius 16 to 1,583 at radius 64 and then flattened
at 1,339, 1,334, 1,333, 1,332, 1,323 over 30,000 attempts at radius 89,
the whole device. Killed after 25 minutes. This reproduces the gaps
file's 2026-08-10 record on the same design at 39,090 ALUTs, where the
limit at 34 never completed the pass and the control without the limit
legalised in one second: mechanism 2 of G2, carry chains plus the LAB
input limit as a genuine resource conflict, and a legaliser with no
infeasibility exit. Five stages of LAB legality work since then did not
touch it, because none of them was meant to: they made the check faster
and reusable, not the spreading aware of it.

Run 2, control, `MISTRAL_LAB_INPUT_LIMIT=999`, everything else equal:

| Phase | Time |
| --- | ---: |
| HeAP (of which strict legalisation) | 253 s (203 s) |
| Annealer | 58 s |
| Router2 (100 iterations, gave up) | 2,751 s |
| Router1 legalisation of 197,622 arcs | stalled: 176196 arcs still unrouted after 91000 iterations and 525 s, rip-ups outpacing routes (176,166 remaining at 250 s, 176,196 at 525 s); killed |

Router2 never converged: 61,742 overused wires after iteration 1,
18,295 after 3, then a plateau between 9,000 and 10,800 from iteration
11 to the cap at 100 (8,467 at the end). A rerun with
`NEXTPNR_ROUTER2_DUMP_OVERUSE=20` names the wires at iteration 20
(10,238 overused): 46% of the overuse is on the LABs' own input lines
(`TD`), 39% of the contending nets are on arithmetic cells, and the
rest is general fabric (`V2` 12%, `H3` 11%, `WM` 9%, `H6` 9%, `V12`
5%, `V4` 5%, `H14` 4%), spread over the whole array rather than one
tile. So the limit is not a placement artefact: with it, placement
cannot finish; without it, the LABs are oversubscribed on inputs and
the router cannot finish either. G2 mechanism 2, at full scale, from
both sides.

Quartus 17.0.2 on the identical netlist (WYSIWYG hand-over as before,
the block RAM as a memory-array model since Quartus rejects Yosys's
variable part-selects; `core_probe_20260917` on the NAS, production
settings, 33 MHz constraint): synthesis 10 min 3 s, fitter 26 min 37 s
(placement 6 min 10 s), timing 2 min 30 s, assembler 19 s. It fits the
same 39,425 LUT cells as 35,153 ALUTs into 20,576 ALMs, 49% of the
device, at 1.92 cells per ALM against our 1.4, keeps 14,590 of the
15,647 registers, absorbs the block RAM into logic, and reaches
25.2 MHz against the 33 MHz constraint under the slow 1100 mV 100 C
model. For scale: the recorded full MiSTer builds of the same core
with DSPs, memories, and the framework need 33.5k to 36k ALMs and one
to two and a half hours of fitter.

Reading. The full core does not build through this branch, and the
reason is now quantified from both sides. At our packing density the
design needs about 28,000 ALMs of 41,910 and the strict legaliser
cannot find legal homes for the last 1,323 cells under the LAB input
limit; with the limit lifted, placement takes five minutes and the
router stalls on the input lines the limit exists to protect, plus
general congestion at that density. Quartus needs 20,576 ALMs for the
same cells because it packs 1.92 per ALM, which leaves it 51% of the
device free and a fitter that finishes in 27 minutes. The lever is
therefore density, not the legaliser's search: a LAB-aware clustering
that pairs LUTs by shared inputs before or during spreading (Quartus's
packing step, which nextpnr's light packing philosophy has no
counterpart for on this family), with the input limit as its
constraint rather than a post-hoc check, and an infeasibility exit in
the legaliser so a design that cannot fit fails in seconds instead of
never. That is unit 3's second bullet, ALM pairing density, promoted
to first; input permutation for timing comes after it, because it
only matters once the design places. Speed at this scale, for the
record: placement 5.2 min and 100 router iterations in 46 min on the
M1 Ultra against Quartus's 6 min placement and 20 min routing on the
NAS, on a placement that does not route.

Artifacts: `build/stage6-fullcore/` (sv2v output, both JSONs, the
generated wrapper, the logs of all three runs, `core_probe_dump.log`
with the 296,493 overuse lines) and `core_probe_20260917` on the NAS.

### 2026-09-17: Stage 6, units 6a and 6b: legaliser stall exit and ALM pairing

Closing the density gap the full-core attempt measured, in the order the
analysis set: an infeasibility exit first, then a pairing packer.

**6a, stall exit (`common/place/placer_heap.cc`).** The strict legaliser
had an exit only at eight times the cell count of outer iterations, which
on 55k cells is hours at the maximum rip-up radius, and a per-cell
attempt cap the stall never reached because every cell was retried in
turn. Once the rip-up radius covers the device, the queue is now
measured every `stall_rounds` (4) passes over it; if it shrank by less
than `stall_progress` (1%) the placer names the stuck cells by type,
calls an arch report (`PlacerHeapCfg::report_infeasible`), and stops.
Mistral's report prints the LAB input occupancy and the ALM pairing
density behind the failure. No settings key, so nothing is interned.

Full core, reference configuration, before and after:

| | Before | After |
| --- | --- | --- |
| Outcome | killed by hand after 25 min at 110,000 attempts, 1,323 queued | exits at 944 s after 104,000 attempts |
| Report | none | 1,333 stuck cells, all `MISTRAL_FF`, 3.2 input nets each; 3,543 of 4,191 LABs at the input limit of 42, 38.9 inputs per LAB on average; 14,257 ALMs holding two LUTs and 10,934 one (1.57 per used ALM) |

The report changes the reading of the first attempt: at this density the
legaliser had already paired LUTs to 1.57 per ALM, 85% of the LABs sat
exactly at the input limit, and the cells with nowhere to go were
registers whose data input comes from a LUT in another ALM, each of
which costs a LAB input line the device no longer had. Fabi386 clean
gate: 0xbb18ede9 / 0xbc1365c6, unchanged; the exit never triggers on a
design that fits.

**6b, ALM pairing (`mistral/alm_pairing.{h,cc}`, `--alm-pairing N`, off
by default).** The packer pairs plain LUTs before placement under the
ALM rule the checker enforces (64 LUT bits; eight unique inputs with
only the A and B lines shareable, so two 5-input LUTs need two shared
nets, a 5- and a 4-input LUT one, anything of three inputs or fewer
pairs freely; 6-input LUTs never pair; carry cells are already two per
ALM through their chains). Level 1 pairs LUTs that share an input net,
greedily, most shared first; level 2 also pairs a LUT with one it drives
or is driven by; level 3 pairs whatever is left and compatible. A pair
is a two-cell cluster whose placement the arch overrides
(`Arch::getClusterPlacement`) onto the two LUT halves of the ALM its
root bel is in, so HeAP and the annealer move it as a unit and the
legality checks see both cells. Pairs survive a checkpoint because the
cluster fields are what the checkpoint already carries. The unit test
checks the rule against the checker on a real ALM (a compatible pair is
legal in one ALM, an incompatible one is rejected by both), the cluster
placement, and the levels.

Exec probe (`--seed 1 --threads 1 --freq 12`), sequential and alone:

| Level | Pairs of 8,574 pairable | Cells per ALM (ALMs) | HeAP | Annealer | Router2 | Fmax | Wall |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 0 (off) | | 1.37 (8,177) | 5.2 s | 9.2 s | 8.1 s, 20 iterations | 35.87 MHz | 28.1 s |
| 1 | 4,108 sharing inputs, 358 single | 1.74 (6,492) | 5.6 s | 5.0 s | gave up at 100 on one wire, router1 70 s | 37.73 MHz | 92.4 s |
| 2 | +27 linked | 1.75 (6,460) | 6.4 s | 6.0 s | 8.6 s, 74 iterations | 32.13 MHz | 24.6 s |
| 3 | +152 unrelated, 0 single | 1.77 (6,369) | 6.0 s | 4.1 s | 7.4 s, 17 iterations | 32.45 MHz | 21.1 s |

Level 0 is byte-identical to the reference. The single wire level 1
could not clear is one high-fanout net (`u_exec.fpu_done...`) around a
dense cluster at columns 50 to 53, on `V2`, `H6`, and `TD` wires: the
count-only input limit's "which line" conflict (G2 mechanism 2),
resolved by router1. Fmax moves both ways within the seed spread.

Full core, level 1, default input limit of 42: 15,181 pairs from 31,988
pairable LUTs, 32,395 input nets shared inside pairs, 1,626 left single.
**Placement completes**: HeAP 573 s (511 s legalisation), no stall.
Router2 then plateaus twice as high as the unpaired control: 73,005
overused wires at iteration 1, 39,288, 26,584, 22,500, 21,130, 20,454,
20,508 at 7, two minutes per iteration; stopped there. Pairing moved
the wall from placement into routing: the LABs are legal by count and
full, and the router cannot find the specific input lines.

A lower placement-time limit does not rescue this. With pairing at
limits 36 and 30 (the gaps file's routable point for the 32k design was
34) the legaliser did not reach its first 2,000-cell heartbeat in 55
minutes: pairs find almost no LAB with input room, every failed
candidate costs a full evaluation, and the maximum-radius exit is never
reached. Both killed. The overuse dump on the paired placement at
router iteration 10 (21,455 overused wires, 308,340 lines) splits 23%
`TD` input lines (46% unpaired), then `V2` 19%, `H3` 17%, `H6` 14%, `V4`
9%, `WM` 8%, `V12` 6%, `H14` 4%: pairing halved the input-line share,
and what remains is mostly the short and medium fabric wires around
LABs packed at 1.74 cells per ALM. Spreading them thinner helps and
saturates: `MISTRAL_HEAP_BETA` at 0.35 and at 0.25 give one identical
placement (below the design's own occupancy every region counts as
overused and the cut degenerates to one global spread), HeAP 425 s,
annealer 115 s, and router2 plateaus at 14,600 to 15,900 overused wires
over 15 iterations against 20,500 at the default 0.5: 28% better, not
convergent.

What the lines actually are (measured from the routing graph on LAB 0,
every ALM identical; the temporary test that printed it was removed
before the commit): a LAB has 46 `TD` input lines; each LUT pin can be
fed by 21 to 25 of them; the A and C pins draw on one group of 25 lines
and the B and D pins on a disjoint group of 21, while E pins draw on a
group of 22 and F pins on a disjoint group of 24, each line sitting in
one A/C-or-B/D group and one E-or-F group (12, 13, 10, and 11 lines in
the four quadrants). A net feeding an A pin in one ALM and a B pin in
another therefore needs two lines unless one LUT's inputs are permuted,
and the count of 42 sees none of it: it is exactly the "which TD wire"
conflict G2 mechanism 2 describes, now with its geometry.

Reading. 6a is complete and default-path safe. 6b is complete as an
opt-in and delivers the density it was meant to: 1.37 to 1.74 cells per
ALM on the probe, and the full core placed in 9.6 minutes where it never
placed before; Fmax on the probe moves inside the seed spread; level 1
is the level to use and it stays off by default. The wall moved into
routing and the dump splits it: 59% short and medium fabric wires
around the packed LABs, 23% input lines. Two units follow, in that
order. 6c, routing-demand-aware spreading: a uniform spread factor
helps 28% and then saturates, so the spreader needs a per-region
capacity that reflects the routing demand of what it packs (the code
comment at the knob already names "a routing-demand-aware inflator"),
placement-side, no rules change. 6d, per-class input-line feasibility
in the LAB checker: replace the total of 42 with nets on A/C pins at
most 25, on B/D at most 21, on E at most 22, on F at most 24, a net
that must reach two classes counted in each, using the pin assignment
`reassign_alm_inputs` already makes and permuting plain LUT inputs so a
net keeps one class across the LAB. That is a change to the rules the
V1 and V2 evaluators and the concluded Rust crate all implement, so it
reopens the crate under its own terms (a rules revision), validated on
the probe at level 1 first (the one wire router1 had to clear) and then
on the full core. Neither is started here; they are the decisions the
record now supports.

Validation of 6a and 6b as landed: `./build/rust-enabled/nextpnr-mistral-test`
59/59 (adds `AlmPairingFormsOnlyPairsTheAlmRuleAllows`),
`./build/nextpnr-mistral-test` 49/49, Fabi386 clean gate 0xbb18ede9 /
0xbc1365c6 with both options absent, exec probe level 0 byte-identical,
`git diff --check` and `clang-format` clean.

### 2026-09-17: Stage 6, unit 6c: routing-demand-aware spreading

HeAP's cut spreader counted one unit per cell against one per bel, with
one global factor (`beta`) to thin every region alike; the previous entry
showed that factor helping 28% and then saturating. `--spread-demand`
(off by default) gives the spreader an arch hook
(`PlacerHeapCfg::get_cell_spread_units`, `spread_units_per_bel`): a comb
cell occupies as many units as it has unique input nets (one to eight, the
carry input excluded) and a bel offers four, everything else keeps a
bel's worth, and cluster roots carry the sum of their members. Both
sides of every comparison scale together, so with the hook unset the
arithmetic is the reference's; occupancy, capacity, the region test, and
the cut balancing all go through the same unit lookup now.

Exec probe (`--seed 1 --threads 1 --freq 12`), sequential and alone:

| Configuration | Cells per ALM | HeAP | Annealer | Router2 | Fmax | Wall |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Off | 1.37 | 5.2 s | 9.2 s | 8.1 s, 20 iterations | 35.87 MHz | 28.1 s |
| `--spread-demand` | 1.39 | 6.3 s | 6.8 s | 8.9 s, 20 iterations | 33.58 MHz | 28.0 s |
| `--alm-pairing 1` | 1.74 | 5.6 s | 5.0 s | cap at 100 on one wire, router1 70 s | 37.73 MHz | 92.4 s |
| `--alm-pairing 1 --spread-demand` | 1.74 | 7.1 s | 4.0 s | 8.1 s, 21 iterations | 33.95 MHz | 22.8 s |

Off is byte-identical to the reference (0xbb18ede9 / 0xbc1365c6). With
pairing, demand spreading removes the wire router1 had to clear: the
paired probe routes in 21 iterations at the same density. Fmax stays
inside the seed spread.

Full core, `--alm-pairing 1 --spread-demand`, default input limit: places
(HeAP 722 s, 667 s legalisation, annealer 107 s, against 573 s
with pairing alone; the weighting adds HeAP iterations). Router2's
overuse by iteration: 73586, 40143, 25423, 20858, 19298, 18532, 18333, 18496, 19182, 19478, 19847; stopped at 10 once it turned
back up. Against the record: pairing alone 20,454 to 20,508 at the same
point, the uniform spread factor at 0.35 or 0.25 14,612 to 15,877.

Reading. Demand weighting does what its design says and not more: it
redistributes, so the probe's one stubborn wire disappears and the
core's plateau drops 10%, but at four units per bel the average cell
still weighs about one bel, so the total spread is the reference's and
the LABs stay as packed as before. The uniform factor thins more and
saturates. Neither converges, and the split from the previous entry
stands: what the router cannot find at this density is mostly the short
fabric around packed LABs and the specific input lines. 6c stays as an
opt-in knob with the measured effect. The next unit is 6d, per-class
input-line feasibility with pin permutation, and the placement-routing
gap after it is a congestion feedback loop (placement re-spread from a
routing estimate), which is beyond a single unit and is noted rather
than planned. Validation as landed: `./build/rust-enabled/nextpnr-mistral-test`
59/59, `./build/nextpnr-mistral-test` 49/49, exec probe off
byte-identical, `git diff --check` and `clang-format` clean.

### 2026-09-17: Stage 6, unit 6d re-scoped before implementation

The rules revision was authorised and surveyed (V1 count in `lab.cc`,
the V2 record and its `input_net` ids, the Rust `recompute_inputs`, the
parity harness in `placement_transaction.cc`), and the per-class rule was
designed: replay `reassign_alm_inputs`' deterministic pin choice at
placement time, give every net the classes it lands on across the LAB,
split a net that needs two disjoint groups into two lines, and check
Hall's condition over the four quadrants (12, 13, 10, 11 lines). Before
writing it, the question of whether it can bind under the count it
would join was answered on paper, and it cannot: the count caps pin uses
at 42 per LAB, a line is never more than one pin use, and the tightest
quadrant (12 lines for nets that touch both an A/C pin and an E pin)
needs at least two pin uses per such net. With top LUTs drawing on C
first and A only fifth, a LAB under the count carries about ten to
twelve such nets, so the quadrant limits are met by construction in all
but contrived cases. The rule would be near-vacuous and the crate would
be reopened for nothing; it is not implemented.

What the 23% of overuse on input lines therefore means: a line
assignment exists for every LAB the count admits, and router2 does not
find it. Negotiated congestion resolves structured bipartite conflicts
slowly or not at all, which is what the paired probe showed on one wire
for 80 iterations. That is a router matching problem, and the fix that
follows is a LAB input-line assignment at routing preparation: after
`reassign_alm_inputs` fixes the pins, compute the matching per LAB
(nets to lines over the quadrants, Hopcroft-Karp on at most 46 lines)
and bind the chosen line-to-pin pip for every LAB input as a pre-routed
arc, so router2 only has to reach the line from the fabric. No rule
changes, no crate, arch-side only, and the Hall check from the design
above becomes the feasibility guard of that step rather than a
placement rule. Not started; recorded for the decision.

The experiment that would have separated the two readings on the probe
(pairing at input limits 38 and 34) did not run: with pairs, the
legaliser makes no progress under a stricter count even at 12%
utilisation, the same pathology as the full core at 36 and 30, which
is itself a finding about pair candidates under a tight count.

### 2026-09-17: Stage 6, unit 6d as built: LAB input-line pre-assignment, measured negative

Built as re-scoped: `--lab-input-lines` runs at routing preparation, after
the pins are fixed and after route reuse has applied what it can. For
every LAB it gathers each externally driven net's LAB-input pins (nets
driven inside the LAB stay on the local feedback lines), takes each net's
allowed lines from the routing graph (the `TD` wires that can drive all
its pins, splitting a net whose pins share none), matches nets to lines
with augmenting paths over at most 46 lines, and binds every pin through
its line's pip at `STRENGTH_PLACER`; the line itself stays unbound because
router2 cannot drive a wire that arrives bound without a pip, and
`Arch::checkPipAvailForNet` admits only the reserved pip into a reserved
pin. Unit test on a real LAB: three nets, three lines, every other pip
refused, a second pass binds nothing. The matching found a perfect
assignment in every LAB of the probe (1,068 LABs, 20,782 nets on 22,549
lines, 1,767 split). The router then lost badly:

| Exec probe | Wires at iteration 1 | Overused at 1 | End |
| --- | ---: | ---: | --- |
| Default | 133,430 | 9,323 | converged at 20 |
| `--alm-pairing 1` | 133,430 | 9,323 | one wire to the cap, router1 70 s |
| `--lab-input-lines` | 212,653 at the cap | | 493 overused at 100; router1 asserts on the partial bindings |
| `--alm-pairing 1 --lab-input-lines` | 217,455 at the cap | | 468 overused at 100 |
| same, lines spread over the 46 by a per-group offset | 187,786 | 22,032 | 626 overused at 40 |

Reading. A net's line decides which fabric muxes feed it, and the
router's freedom to pick the line together with the fabric route is
worth more than any LAB-internal matching: fixed lines cost 40 to 60%
more wires in detours and moved the conflict from the LAB's lines to the
wires that feed them. The 23% of the core's overuse on input lines is a
router negotiation problem to be solved inside the router, with the
fabric in view, not ahead of it. The code is landed in this commit for
the record and removed in the next; the design and numbers stay here.
Router1's assertion on placer-strength partial bindings is noted as a
second reason the mechanism cannot ship as built.

### 2026-09-17: Stage 6, unit 6e: congestion-driven spreading

The placement-routing loop, in the cheapest form that closes it inside
HeAP. `--spread-congestion` (off by default) adds a pass-begin hook to
the cut spreader (`PlacerHeapCfg::on_spread_begin`): at the start of
every spreading pass the arch receives the pass's own cell positions and
rebuilds a wire-density estimate over the tiles (RUDY: every net with two
or more placed endpoints spreads width plus height over area across its
bounding box), and the per-cell unit hook from 6c multiplies a LAB
cell's units by its tile's density over a threshold of `k` times the
mean of the used tiles (`MISTRAL_SPREAD_CONGESTION_K`, default 2), capped
at three. Only comb and register cells inflate: the first version
inflated the design's one clock-enable cell past its two-bel bucket and
the spreader could not expand; and only tiles above the threshold count:
inflating everything above the mean pulled the 12%-utilised probe apart
for a quarter of its Fmax. Both are recorded as the two defects the
probe caught before the core saw the unit.

Exec probe (`--seed 1 --threads 1 --freq 12`):

| Configuration | Cells per ALM | Router2 | Fmax | Wall |
| --- | ---: | ---: | ---: | ---: |
| Off | 1.37 | 20 iterations | 35.87 MHz | 30.6 s |
| `--spread-congestion` | 1.33 | cap at 100 on six wires, router1 116 s | 35.03 MHz | 149 s |
| `--alm-pairing 1 --spread-congestion` | 1.73 | 62 iterations, no router1 | 33.55 MHz | 27.5 s |
| `--alm-pairing 1 --spread-demand --spread-congestion` | 1.73 | 16 iterations | 29.22 MHz | 22.0 s |

Off is byte-identical (0xbb18ede9 / 0xbc1365c6). The probe is not
congested, so the estimator only perturbs it: mixed, as expected.

Full core, `--alm-pairing 1`, default input limit, router2 capped at 15
(overused wires by iteration; the record's other plateaus for scale):

| Spreading | Placement | Router2 overused by iteration |
| --- | ---: | --- |
| none (pairing alone) | 573 s | 73,005, 39,288, 26,584, 22,500, 21,130, 20,454, 20,508 |
| `--spread-demand` | 722 s | 73,586, 40,143, 25,423, 20,858, 19,298, 18,532, 18,333, 18,496, 19,182, 19,478, 19,847 |
| uniform factor 0.35 | 425 s | 69,745, 34,524, 20,884, 16,871, 15,176, 14,612, 14,861, 15,092, 15,065, 14,839, 14,990, ... 15,877 at 15 |
| `--spread-congestion` (k = 2) | 602 s | 70,023, 34,315, 20,337, 15,726, 13,950, 12,910, 12,590, 12,182, 12,312, 12,497, 12,936, 13,153, 13,035, 13,175, 13,419 |
| `--spread-demand --spread-congestion` | stall exit at 2,031 s | 1,032 registers with no legal location; 1,401 LABs at the limit, 1.75 LUTs per used ALM: the two inflations together exceed what the device absorbs |
| `--spread-congestion` with k = 1.5 | stall exit at 1,670 s | 731 cells with no legal location: the sharper threshold inflates more than the device absorbs |
| `--spread-congestion` (k = 2) with the uniform factor at 0.35 | stall exit at 1,402 s | 483 cells with no legal location: the same, from the other side |

Overuse split of the k = 2 placement at iteration 10 (282,560 lines):
`TD` 22%, `V2` 19%, `H3` 16%, `H6` 13%, `WM` 10%, `V4` 8%: the same
shape as every paired placement, at the lowest level so far.

Reading. Congestion-driven spreading is the best configuration measured
on the full core: with pairing, at the default threshold and factor, the
router's plateau drops to 12,200 to 13,400 overused wires, 35% below
pairing alone and below the uniform thinner factor's 14,600, and the
placement stays legal. It does not converge, and every attempt to
spread further (a sharper threshold, a thinner factor, demand weighting
on top) leaves the legaliser without room: at 54% ALM occupancy under
the input limit of 42 the paired core has that little slack. So the
loop is closed as far as an estimate can close it; the estimate is a
proxy for the fabric, and the fabric at this density is short of the
real router. What the record now says about the remaining gap: the
placement side has delivered density (6b), a stall exit (6a), and two
spreading modes (6c, 6e) that trim the plateau by a third; the routing
side has been measured from both ends (a matching exists for every LAB
the count admits, pre-assigning it costs 40 to 60% more wires); and the
next lever is the router itself, a negotiation that handles the
structured line conflicts and the local fabric together, which is a
router2 change with the Mistral graph in view and beyond a single unit.
Validation as landed: `./build/rust-enabled/nextpnr-mistral-test` 59/59,
`./build/nextpnr-mistral-test` 49/49, exec probe off byte-identical,
`git diff --check` and `clang-format` clean.

### 2026-09-17: Stage 6, unit 6f: the router's share of the gap, measured

The unit set out to give router2 a negotiation that handles the LAB
input lines together with the local fabric. It started with the best 6e
plateau's overuse dump and ended by measuring where the wires go, on the
full core and on the identical exec-probe netlist that Quartus routed.

The plateau is diffuse and churns. At the record's best point (pairing
with congestion-driven spreading, iteration 8) the 12,182 overused wires
sit in 2,990 tiles, the top 50 tiles hold 8% of them, 96% are contested
by exactly two nets, only 10% are still overused seven iterations later,
and consecutive iterations share 12%; total wire use grows every
iteration (755k to 897k). Only the length-12 column wires persist (46%
of them). That is not a matching problem on the lines and not a hot
spot; it is a region whose demand exceeds the fabric.

Six router2 variants from one placed checkpoint of that configuration
(`--no-route --checkpoint`, 819 s; the control's trajectory equals the
record's run to the wire; `--router2-max-iter 15`; the router1 fallback
after the cap was discarded). Overused wires, best iteration and last:

| Variant | Best | At 15 | Router2 |
| --- | ---: | ---: | ---: |
| control | 12,182 (8) | 13,419 | 1,830 s (six in parallel; 1,367 s alone) |
| present-congestion floor 1.0 (legality pressure independent of criticality) | 13,263 (8) | 14,533 | 2,069 s |
| no timing-driven routing | 13,574 (8) | 15,251 | 2,019 s |
| `--router2-reroute 4` (every arc re-routed every fourth iteration) | 11,863 (8) | 12,269 | 1,823 s |
| `--router2-reroute 4 --router2-reroute-contested` | 12,072 (11) | 12,531 | 1,875 s |
| A* estimate weight 1.0 instead of 1.25 | 10,629 (8) | 12,212 | 1,934 s |
| `--router2-unit-cost` (one unit per wire instead of its delay) | 5,694 (5) | 6,626 | 1,781 s |
| `--router2-unit-cost` with the estimate at 1.0 | 5,256 (5) | 6,122 | 1,811 s |

None converges. The criticality formula is not the cause: with the
worst slack far negative, a path's criticality is its violation as a
fraction of the worst, so most arcs keep most of their congestion
pressure, and removing the scaling makes the plateau worse. The one
router-side lever that moves the core is the base cost: costing every
wire one unit instead of its delay halves the plateau (5,694 against
12,182, with 7% fewer wires in use, 703k against 755k at the first
iteration) and with the admissible estimate 5,256; the overuse that
remains is more concentrated (1,290 tiles at the best iteration, the
top 50 tiles hold 15%; V2 27%, input lines 20%, V4 17%, H6 15%, H3
14%) and churns the same way (5% persists from iteration 5 to 15). It
buys that with timing: on the probe 4% of Fmax, on the core unmeasured
because the core does not route.

Supply against demand, from a per-tile utilisation dump added to
router2's heatmap set (`_utilisation_by_tile_<iter>.csv`: wires in the
graph, in use, overused, per tile and wire type). The core at iteration
8: fabric wires 66.7% used device-wide (V2 76.6%, V4 74.6%, H3 73.7%,
H6 57.7%, WM 66.9%, input lines 45.5%), the congested region (x 22 to
52, y 28 to 66) 73.6%, the median tile 77%, the top tenth of tiles 91%,
the peak tile 98%. Quartus's fit of the same core (`core_probe_20260917`)
reports 23.8% average and 66.1% peak interconnect usage (C2 20%, C4 26%,
C12 25%, R3 24%, R6 20%, R14 28%, block 26%, local 22%).

The exec probe, identical netlist, both flows routed (Quartus
`exec_probe_20260916n`, physical synthesis off; nextpnr the reference
run). The graph's wire counts against Quartus's resource table: V2
117,357 against C2 119,108, V4 55,431 against C4 56,300, H3 126,673
against R3 130,992, H6 248,612 against R6 266,960, input lines 285,892
against block interconnects 289,320, local lines 84,580 against 84,580.
Wires used:

| Resource | nextpnr | Quartus | Ratio |
| --- | ---: | ---: | ---: |
| fabric (V2, V4, V12, H3, H6, H14 against C2, C4, C12, R3, R6, R14) | 52,882 | 18,676 | 2.8 |
| V2 / C2 | 13,401 | 3,662 | 3.7 |
| V4 / C4 | 5,224 | 1,857 | 2.8 |
| V12 / C12 | 809 | 63 | 12.8 |
| H3 / R3 | 15,356 | 5,308 | 2.9 |
| H6 / R6 | 17,240 | 7,554 | 2.3 |
| H14 / R14 | 852 | 232 | 3.7 |
| LAB input lines / block interconnects | 27,389 | 14,778 | 1.9 |
| local lines / local interconnects | 2,714 | 4,312 | 0.6 |

The two placements (Quartus's cell locations from a TimeQuest script on
the NAS, joined to the hand-over Verilog by name; 78% of its nets
matched):

| Measure | nextpnr | Quartus |
| --- | ---: | ---: |
| half-perimeter wirelength per sink | 0.79 tiles | 0.92 tiles |
| LABs entered per net | 2.24 | 1.78 |
| sinks in the driver's LAB | 22.7% | 28.8% |
| sinks in the driver's row, another LAB | 14.4% | 18.8% |
| sinks in the driver's column | 15.3% | 7.0% |
| rows touched per net | 1.80 | 1.52 |
| register in the same ALM as its LUT | 16% (55 of 341) | 95% (536 of 562) |
| LABs used | 1,069 | 655 |
| fabric wires per LAB entered | 1.83 | 0.82 to 1.06 |

Why a LAB entry costs what it costs, from the graph (LAB 45,47 and its
four neighbours, `MISTRAL_DUMP_LAB_LINES=x,y`): the 46 lines have 568
source pips, H6 379, H3 123, the LAB's own outputs 40, V2 8, V4 6, clock
12; 12 lines have any column source; the 40 outputs drive 422 pips (H6
150, H3 92, V2 88, V4 40, lines 52); the LAB above drives 64 column
wires of which one feeds a line of this LAB, the LAB below none. In the
routed probe, entries into lines come from H6 13,734, H3 10,306, the
LAB's own outputs 2,161, V2 798, V4 404. Single-sink nets by Manhattan
distance cost 0.41 fabric wires in the same LAB, 3.12 one tile away,
4.33 two, 4.68 three, 5.71 four, 6.51 at five to nine, 14.19 at ten and
more; a one-row hop is a stair, H3 then V2 then H3 then the line.

The base cost's share, exec probe: `--router2-unit-cost` routes it in 16
iterations with 137,736 wires against 149,576, fabric 41,781 in the
four short classes against 51,221 (18% fewer), 34.56 MHz against 35.87;
the A* estimate at 1.0 routes it in 21 iterations, 7.27 s against
9.51 s, 36.23 MHz.

Reading. The gap is demand, and the demand is structural: the fabric
enters a LAB through row wires, a vertical hop costs two or three wires,
and nextpnr's placement neither avoids vertical hops, nor packs a
register with its LUT, nor draws a net's sinks into fewer rows and LABs,
so it enters a LAB with 1.83 fabric wires where Quartus spends about
one. On the core that is 67% of the fabric against Quartus's 24%, and
no negotiation closes a fabric at 67%. The router-side unit is closed
with two opt-in knobs (the periodic re-route, 3% on the core, and the
unit cost, which halves the core's plateau at the price of delay-blind
routes) and the diagnostic dumps; the next unit is placement-side:
a row-aware cost (a column hop at its wire cost) through spreading and
legalisation, register-with-LUT packing as a pack-time cluster the way
6b pairs LUTs, and net clustering into fewer rows. Design section 9.5.

Two constraints found on the way. A checkpoint resumes only under the
same common command-line options: `--router2-max-iter`,
`--router2-heatmap`, and the `--router2-*` weights intern their settings
key before the checkpoint replays its table and the restore rejects the
mismatch, so router experiments from a checkpoint set router2's terms
through the Mistral environment block (`MISTRAL_R2_*`, read after the
netlist). And after `--router2-max-iter` gives up, the flow runs router1
on the whole design, which on the core never finishes; the runs above
were stopped there.

Correction (2026-09-18). The register-with-LUT figures were first
computed with the wrong bel numbering (an ALM's four register bels sit
at z modulo 6 of 2 to 5, its two LUT halves at 0 and 1, so the ALM is z
divided by 6 for both), and the probe's denominator missed the registers
whose data had been rewired through a route-through LUT after placement
(the routed JSON shows those driven by the route-through). Corrected: on
the probe 7% of the 827 LUT-driven registers share the LUT's ALM (55),
on the core 7% (671 of 9,790); Quartus's 95% stands. The reading does
not change.

Validation as landed: `./build/rust-enabled/nextpnr-mistral-test` 59/59,
`./build/nextpnr-mistral-test` 49/49, exec probe off byte-identical
(0xbb18ede9 / 0xbc1365c6), `git diff --check` and `clang-format` clean.

### 2026-09-18: Stage 6, unit 6g: register packing

`--register-packing` (off by default) packs a register into the ALM half
of the LUT that drives it, as a cluster child placed by the same arch
override that places 6b's pairs (`mistral/register_packing.*`, design
section 9.6). One register per LUT, since the arch admits one per half
(`lab.cc` marks the second slot unusable); plain LUTs only, so carry
chains and MLAB groups are untouched.

Two defects found on the way and fixed before the measurements. The cut
spreader weighed a LUT-plus-register cluster as two LUTs in the LUT pass
(`PlacerHeapCfg::cluster_units_by_bucket`, on with the option, keeps the
reference accounting off). And a pair whose two registers could not
share a LAB's control lines was rejected by the detached transaction at
every ALM until HeAP's cell placement timeout (a non-global clock, two
enables and a synchronous clear exceed the DATAIN lines; reason 21,
`NPNR_CONTROL_DATAIN_CONFLICT`, found with the new
`MISTRAL_DEBUG_CLUSTER_REJECT` switch); the packer now asks the control
model whether a cluster's own registers fit an empty LAB
(`registers_share_a_lab`). The packing therefore runs after
`assignArchInfo`, which fills the control sets.

Exec probe (`--seed 1 --threads 1 --freq 12`; four runs in parallel, so
the times are contended):

| Configuration | Registers packed | Wires | Fabric wires | Fmax | Router2 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Off | 55 of 827 by chance | 149,576 | 52,952 | 35.87 MHz | 20 iterations |
| `--register-packing` | 820 of 827 (7 LUTs drive two) | 145,104 | 51,494 | 33.83 MHz | 18 iterations |
| `--alm-pairing 1` | 45 of 827 by chance | | 56,099 | 37.73 MHz | 62 iterations, 58.7 s |
| `--alm-pairing 1 --register-packing` | 817 of 827 (3 control conflicts kept apart) | 143,796 | 57,218 | 34.66 MHz | 16 iterations, 17.7 s |

Off is byte-identical (0xbb18ede9 / 0xbc1365c6). On the probe the
packing costs a few percent of Fmax and does not reduce the fabric
wires; with pairing it makes the paired placement route in a third of
the time. The probe is 12% utilised and was never the target.

Full core, pairing with congestion-driven spreading (the 6e
configuration, plateau 12,182 at iteration 8, 13,419 at 15):

| Configuration | Placement | Router2 overused by iteration |
| --- | ---: | --- |
| `--register-packing` (5,360 of 9,632 LUT-driven registers packed; 3,980 share a LUT with another, 292 kept apart by the control model) | 1,214 s | 68,894, 34,649, 20,934, 16,777, 15,098, 14,198, 14,114, 13,992, 14,387, 14,899, 15,408, 15,547, 16,339, 16,745, 17,069 |
| `--register-packing --router2-unit-cost` | 1,212 s | 40,993, 9,869, 3,962, 2,678, 2,326, 2,158, 2,001, 1,967, 1,964, 2,045, 2,028, 2,121, 2,155, 2,278, 2,377 |

For scale: without packing the same placement configuration plateaus
at 12,182 under the delay cost and 5,694 under the unit cost (6f).

Reading. Under the delay-based cost the packing does not help the core:
the plateau is 15% higher (13,992 against 12,182), the placement takes
twice as long (the clusters give the legaliser less room, as pairing
did), and the delay cost keeps spending long wires and stairs on the
connections that remain. Under the unit wire cost the packing takes the
plateau from 5,694 to 1,964, a third, and the remaining overuse is what
the record has seen before: diffuse (1,183 tiles, the top 50 hold 12%),
two nets per wire, and churning without a stubborn set (6 wires of the
2,377 at iteration 15 were overused at iteration 9). The two placement-side facts behind
it: 5,360 register data inputs left the fabric, and 3,980 more could
not because their LUT drives several registers and the arch admits one
per half. The unit is closed with the option opt-in and the reading
that the placement side has now delivered the register, and that the
router's base cost decides whether the placement's savings reach the
fabric; the next units are the row-aware cost and a LAB-level
assignment (design 9.5), and a look at why the delay cost defeats the
packing.

Validation as landed: `./build/rust-enabled/nextpnr-mistral-test` 60/60,
`./build/nextpnr-mistral-test` 50/50, exec probe off byte-identical,
`git diff --check` and `clang-format` clean.

## Decision log

| Date | Unit | Decision | Evidence |
| --- | --- | --- | --- |
| 2026-09-15 | 1A | Use an optional native allocation so illegal results cannot publish preparation state | Focused illegal-evaluation test |
| 2026-09-15 | 1A | Preserve the original greedy DATAIN orders, disconnected first-match behavior, and replacement reservation flags | ENA/ACLR and MLAB reservation tests; identical Fabi386 output |
| 2026-09-15 | 1A | Keep Rust comparison-only during preparation in every non-legacy mode | Rust Fabi386 preparation count with zero mismatches; physical consumer accepts only the native C++ allocation |
| 2026-09-15 | 1B | Keep Boolean placement dispatch separate from plan-returning preparation dispatch | Ticket failure-classification tests; no preparation flag remains on the Boolean API |
| 2026-09-15 | 1C | Represent expected old values in ordered edits and reserve the undo journal before the first write | Missing-edge, changed-value, and interruption injection tests |
| 2026-09-15 | 1D | Promote authority by mode while retaining legacy as the CLI default | Mode-selection tests and byte-identical four-mode P&R outputs |
| 2026-09-15 | 2A | Treat capture's second local-net lookup as the first measured optimization target | Capture is 160.80 ns of a 365.42 ns complete Rust dispatch |
| 2026-09-15 | 2A | Accept single-search capture | Exact capture replay and 15,137-record differential parity; −1.42 ns median capture cost |
| 2026-09-15 | 2B | Accept retained tri-state decoder classes | Rust decode 116.81→113.02 ns; native FFI 174.79→169.85 ns; 15,137 differential records pass |
| 2026-09-15 | 2B | Accept direct final-buffer construction in the FFI batch loop | C++ FFI 170.19→164.03 ns and complete dispatch 354.28→347.79 ns; no allocations; 15,137 C++/Rust/FFI records pass |
| 2026-09-15 | 2B | Reject cold/noinline boundary-error outlining | Within-round FFI-minus-safe overhead was 3.89 ns versus 3.83–4.19 ns without outlining; no reproducible benefit, change reverted |
| 2026-09-15 | 2B | Reject verdict-only translation scratch | Full capture 159.38→206.67 ns and complete dispatch 347.79→356.67 ns; change reverted |
| 2026-09-15 | 2B | Reject production V1 batching; retain benchmark coverage | Synthetic frozen batches cost 167.81–180.16 ns/record versus 166.51 ns singly, with no safe naturally frozen live call stream |
| 2026-09-15 | 2C | Close Stage 2 without target promotion | Retained complete dispatch is 347.79 ns; validation, candidate order, and mutation boundaries remain unchanged |
| 2026-09-15 | 3A | Use fixed V2 value transport and schema-2 named-field replay | Cross-language ABI assertions, replay round trip, and Rust-off scoped live parity |
| 2026-09-15 | 3A | Keep WholeLab explicit with physical-ALM-first composition | It intentionally differs from a bel-scoped live query by checking all ten ALMs |
| 2026-09-15 | 3B | Project V2 controls through the proven V1 evaluator | Exact C++/Rust structured result parity, with IDs translated back to the V2 domain |
| 2026-09-15 | 3C | Promote scoped live Rust authority only through an explicit mode | Fabi386 and feature fixture byte parity across all four modes; legacy remains default |
| 2026-09-15 | 4A | Correct HeAP rollback before establishing the transaction baseline | Strong displaced bindings restore exactly; refreshed Fabi386 artifacts remain byte-identical |
| 2026-09-15 | 4A | Use a conservative global revision before fine-grained dependencies | Typed-key, ABA, exhaustion, connectivity, fact, and binding tests |
| 2026-09-15 | 4B | Promote supported HeAP candidates from shadow comparison to serial frozen transaction authority | 2,892 exact shadow comparisons, then byte-identical Fabi386 and feature-fixture authority runs |
| 2026-09-15 | 4B | Preserve the original live path for facts outside the frozen Mistral LAB model | Explicit `Unsupported` transaction outcome; candidate and RNG order are unchanged |
| 2026-09-15 | 4C | Publish only fully copied and validated Rust-owned batches | Malformed and panic tests leave the output handle null; source lifetime test mutates and drops host inputs |
| 2026-09-15 | 4C | Bound retained work before enabling scheduler concurrency | 64 records per batch, two handles per worker, real 64 MiB aggregate exhaustion and quota-recovery tests |
| 2026-09-15 | 4D | Retain validated V2 facts when a frozen batch will be reused | 804.63 ns/record direct versus 389.73 ns frozen; creation amortizes after two serial evaluations |
| 2026-09-15 | 4D | Continue with bounded parallel scheduling | Frozen evaluation scales 1.97x, 3.82x, 7.21x, 9.77x, and 12.85x at 2/4/8/12/16 workers; live determinism remains gated |
| 2026-09-15 | 4D | Reject performance QoS for default promotion; retain it as benchmark instrumentation | Public macOS APIs cannot hard-pin P-cores, and `QOS_CLASS_USER_INTERACTIVE` changed contemporaneous medians by -2.01% to +2.36% |
| 2026-09-15 | 4D | Implement serial-compatible lookahead rather than a new frozen-epoch search policy | Byte-identical Fabi386 artifacts at every budget and worker count; the existing search order and RNG stream are preserved by state restore |
| 2026-09-15 | 4D | Carry `--placer-lookahead` in `ArchArgs`, never in `ctx->settings` | A settings key interned one IdString and shifted the routing checksum and 20 JSON net ids while the report stayed identical |
| 2026-09-15 | 4D | Budget candidates, not locations, and let a batch cut through a tile's shapes | Location batches evaluated 12x the serial candidate count; candidate budgets bound discarded work to N−1 per commit |
| 2026-09-15 | 4D | Keep lookahead off by default and do not promote parallel evaluation | Decision-identical, but strict legalisation is <2.5% of wall time and owner-side capture of discarded candidates outweighs worker savings on Fabi386 |
| 2026-09-15 | 4E | Invalidate bindings per LAB and every fact mutation globally, rather than building net/cell reverse incidence now | Facts epoch never moves during placement (0 of 10.7 M queries), so precise fact incidence would buy nothing yet |
| 2026-09-15 | 4E | Reuse placement as hard `BEL` constraints validated by the constraint placer, not as soft preferences | Unchanged rebuild reproduces the previous placement exactly; edited designs keep 99.3–99.6% of cells and cut placement from ~20 s to under 1 s |
| 2026-09-15 | 4E | Match cells by name plus full semantic signature, folding route-through buffers out of the previous output | 100% reuse on an unchanged rebuild with 549 generated buffers present in the previous output |
| 2026-09-15 | 4E | Keep both reuse modes off by default | Assessment reuse is not measurable end to end; placement reuse changes routing RNG state and needs a provenance decision before promotion |
| 2026-09-15 | 4E | Extend the kernel hooks with object-carrying forms rather than reverse-mapping from kinds | Defaults preserve every other backend; Mistral gets exact cell and net incidence with no lookup tables |
| 2026-09-15 | 4E | Derive LAB states from stamps instead of storing a state field | A stored state could disagree with the invalidation rules; derivation cannot |
| 2026-09-15 | 4E | Key the content tier on complete V2 whole-LAB facts and compare in full on a hit | The hash only selects a slot; correctness never depends on it |
| 2026-09-15 | 4E | Reject the content tier for promotion; keep it as an explicit mode | Sub-result hits rose from 14.5% to 28.9% but HeAP time doubled (9.95 s to 20.09 s) because each stale query pays a whole-LAB capture |
| 2026-09-15 | 4E | Object creation does not invalidate LAB assessments | A created cell or net is nobody's dependency until bound or connected, both tracked precisely; the global bump had dirtied 4,020 prepared LABs during route-through insertion |
| 2026-09-15 | scaling | Prefer dynamic claiming with whole-pass units for parallel evaluation | 14.83x at 16 workers and 16.09x at 20 versus 12.20x static; static partitions wait for threads on efficiency cores, and 4-record claims contend on the counter |
| 2026-09-15 | scaling | Do not implement parallel freezing now (superseded the same day at the user's request) | Only 15% of the lookahead phase was evaluation; implementing it confirmed the phase scales without beating the serial search on this design |
| 2026-09-15 | scaling | Implement parallel freezing with per-worker Rust handles and a bounded spin before blocking | Parallel fraction 0.15 to 0.75; budget-64 phase 4.69 s to 1.60 s on 8 workers; budget 8 on 8 workers within 10% of the serial phase; all artifacts byte-identical |
| 2026-09-15 | scaling | Keep the serial search as the default | The phase is under 3% of wall time on Fabi386; enable lookahead only where rejections per commit are high |
| 2026-09-15 | Rust | Conclude the Rust evaluator: freeze the ABI, keep it as the parity harness, do not promote or extend it | Safety delivered, memory neutral, +1.3% cost in authority mode, zero oracle findings, dual-implementation maintenance; contract unchanged since 4C |
| 2026-09-15 | Stage 5 | Take SA refinement (1c) before incremental timing (4b), and make the swap assessment carry cost deltas | Timing is 10.7% of the run; in the annealer the cost delta and move set are 51% of the phase versus 20% for binding and legality |
| 2026-09-15 | 1c-A | Assess swaps with overlay forms of the live rules, not V2 captures | V2 capture per swap made SA 3.8x slower (37.5 s); overlay rules make it 8% faster (9.3 s) with identical output and zero shadow mismatches |
| 2026-09-15 | 1c-A | Keep the seam off by default | Serial gain is 1 s of a 28 s run; the unit's purpose is the detached evaluation the parallel batch needs |
| 2026-09-16 | 1c-B | Batched refinement is a frozen-epoch policy with a per-candidate acceptance stream, gated on determinism across worker counts | Serial identity is impossible because acceptance draws interleave with location draws; identical artifacts at 1/2/4/8/16 workers |
| 2026-09-16 | 1c-B | Verify every accepted swap by re-assessing and recomputing on the owner | Caught a stale worker-scratch defect during bring-up; zero mismatches over 2.2 M candidates afterwards |
| 2026-09-16 | 1c-B | Keep `--sa-batch` off by default and record the phase as owner-bound | Best 1.32x at two workers, worse beyond four; detached work is under 1 µs per candidate against a comparable pool round trip |
| 2026-09-16 | 1c-B | Share one worker pool between the lookahead coordinator and the annealer | `common/place/placement_pool.*`; the inline path now reports job failures like the threaded path |
| 2026-09-16 | 4b | Retire incremental timing; replace the placers' hashed criticality lookups with per-arc tables | Propagation is 2.9% of the run and structure is built once per phase; the lookups are 3.5% |
| 2026-09-16 | 2a | Persist iteration orders and the IdString table, not only the arch-owned fields | `dict` order is history-dependent and json11 reloads sorted; the byte gate passed only once both were restored |
| 2026-09-16 | 2a | Byte identity of `--write` JSON and `--report` is the resume gate; the name comparison is the diagnostic | Replaying the table makes net numbering identical; both resumes are `cmp`-identical to the clean run |
| 2026-09-16 | 2a | Bindings and PLL clock selections travel at every phase | The packer binds QSF-located IO cells (`STRENGTH_LOCKED`) before placement; resume-from-packed routed to a pin with no wire until they did |
| 2026-09-16 | 2a | Resolve BEL names through the BEL list and make the writer's `"module"` lookup non-interning | `IdStringList::parse` and `write_module` each interned one string, shifting every route-through net's number |
| 2026-09-16 | 2a | Omit default pin maps from the checkpoint | 63,103 entries to 7,813; `assignArchInfo()` regenerates the rest |
| 2026-09-16 | 2a | Keep `--checkpoint`/`--resume` opt-in; a checkpoint from another device, an unknown phase, or a table that is not a prefix of the process's own is refused | Unit test and preload checks; identity is only promised for the same build and options |
| 2026-09-16 | 2b | Split `Arch::route()` into `prepare_route()` and the router; `--route-prepare-only` writes the route-prepared phase | Resume-from-prepared runs the router only and reproduces the clean run's routing, report, and bitstream |
| 2026-09-16 | 2b | The bitstream is the gate for 2b; the JSON, report, and checksum see neither pin maps nor comb facts | Three restore defects (empty bel-pin lists, inversion states, route-through comb facts) were invisible to every other comparison |
| 2026-09-16 | 2b | Apply recorded pin entries before `assignArchInfo()` and their bel-pin lists again after it | `assign_ff_info` reads the states; the default pass refills empty lists |
| 2026-09-16 | 2b | `assignArchInfo()` covers `MISTRAL_BUF` | Route-through buffers need comb facts in any restored or reloaded context; the clean flow only ever set them at creation |
| 2026-09-16 | 2b | Placement-time certifications run only for packed and placed restores | Route-through buffers are placed outside those rules by design; the writer's preparation is the certification after it |
| 2026-09-16 | 2b | Warn on settings the checkpoint overrides, error only on `seed`, keep extra command-line settings | The flow cannot tell a typed option from a filled-in default; refusing every difference would refuse any checkpoint written with a non-default option |
| 2026-09-16 | 2b fixtures | Record orphan nets with attributes and recreate them before the orders are restored | The frontend never materialises a net nothing refers to; PLL and SDRAM IO designs lose 1 to 44 such nets on reload |
| 2026-09-16 | 2b fixtures | Record every non-zero wire flags word at every phase | Placement blocks the PLL reference-clock spine in every design; without the flags a resumed router routes the reference clock through it |
| 2026-09-16 | 2b fixtures | Validate every checkpoint chain with one binary, writer and reader alike | Two designs failed only because their checkpoints predated a rebuild; the diagnosis cost more than the fix |
| 2026-09-16 | 3c | Preserve a previous route only under the full endpoint, leaf, resolution, and availability checks | router2 trusts pre-routed pips past its own availability test; the checks are the certification |
| 2026-09-16 | 3c | Bind preserved routes strong, not weak | Weak wires stayed open to other nets while the pre-routed owner never moved; the provenance run crawled to the cap with one overused wire |
| 2026-09-16 | 3c | Treat router2 giving up as failure when routes were reused, and route from scratch below locked strength | Router1's legalisation pass otherwise yields a different routing; the fallback now reproduces the clean run byte for byte |
| 2026-09-16 | 3c | Keep `--reuse-routes` opt-in; the edited-design router time is equal to clean, not double | 94 to 95% of nets applied; the 19.6 and 20.2 s first recorded were contended, sequential reruns give 8.31 against 9.12 s and 10.22 against 9.24 s; ten times faster on an unchanged design |
| 2026-09-16 | 3c-2 | Rip up every net before binding any in router2's bind pass | All 4,066 bind failures on the INIT edit were wires still bound to a later reused net's stale route; two passes give archfail 0, four fewer iterations, 21 to 27% less router time on the edits, byte-identical clean flow |
| 2026-09-16 | closing | Next unit is router-side: report route survival, then seed router2's history from the previous run | 18% of applied routes are re-routed on both edits; router2 is 68% of the edit run after placement reuse removed 54 points |
| 2026-09-16 | 3c-3 | Report survival next to applied, and rewrite the plan after routing rather than keep a second record | Same numbers as the external script: 100% on the unchanged design, 82.0% and 81.3% on the edits |
| 2026-09-16 | 3c-4 | Seed router2's history on preserved wires behind `--reuse-routes-history`; record 8 as the measured value and keep it off by default | 99% survival, 6 to 8 iterations, router2 3.0 to 4.1 s against 7.3 to 7.6 s unseeded; Fmax inside the seed spread; clean gate byte-identical |
| 2026-09-16 | measurement | Compare with Quartus only on the identical netlist handed over as WYSIWYG primitives; the recorded full-core runs are a different design | Behavioural hand-over doubled ALMs and tripled the critical path (11 MHz); WYSIWYG: Quartus 64.7 to 69.5 MHz and 5,067 ALMs in 3 min 15 s of fitter against our 35.9 MHz and 8,179 ALMs in 26 s |
| 2026-09-17 | model | Attribute the timing model before placement work; the cell constants are the -7 table already, the path gap on the cell side is input assignment, the routing bound is upstream's | fmaxtest element by element: constants match Quartus's I7 maxima arc for arc; Quartus enters carry cells through C (0.83 ns) where we enter through A or B (1.06 to 1.16); routing 1.26x from libmistral's correction factors; grade and edge speed ruled out, temperature and the interval bound sized |
| 2026-09-17 | full core | The first full-core attempt fails on LAB input capacity from both sides; ALM pairing density is the next unit, ahead of input permutation | 55k cells: legaliser stalls at 1,323 cells under the limit; without it placement takes 5 min and router2 plateaus at 8,500 to 10,800 overused wires, 46% on LAB input lines; Quartus fits the same cells into 20,576 ALMs (1.92 per ALM against our 1.4) in 27 min at 25.2 MHz |
| 2026-09-17 | 6a | Stop the strict legaliser when the queue stops shrinking at the maximum rip-up radius, with an arch report | 944 s exit with 1,333 stuck registers named, 85% of LABs at the input limit, against a 25-minute hand kill and an hours-long budget; clean gate byte-identical |
| 2026-09-17 | 6b | Pair plain LUTs into ALM clusters at pack time behind `--alm-pairing`, placement overridden onto one ALM | Probe 1.37 to 1.74 cells per ALM, level 0 byte-identical; full core places in 9.6 min where it never placed; router2 then plateaus at 20,500 overused wires |
| 2026-09-17 | 6c | Routing-demand-aware spreading behind `--spread-demand`: comb cells weigh their unique inputs, four units per bel | Removes the paired probe's stubborn wire (21 iterations, no router1); the core's plateau drops 10% (18,300 to 19,500) and does not converge; a uniform thinner factor reaches 14,600 and saturates; off by default |
| 2026-09-17 | 6d | After 6c, per-class input-line feasibility with pin permutation in the LAB checker, a rules revision that reopens the Rust crate under its own terms | Structure measured from the routing graph (A/C 25, B/D 21, E 22, F 24 of 46 lines); 23% of the overuse after pairing is input lines; the count of 42 cannot see a net needing two classes; lower limits with pairing never legalise |
| 2026-09-17 | 6d | Re-scoped before implementation: not a placement rule, since the count already implies line feasibility; the fix is a LAB input-line assignment bound as pre-routed arcs at routing preparation, arch-side, no crate | On paper: 42 pin uses cannot overflow a 12-line quadrant that needs two uses per net; the router's failure to find an existing matching is the problem |
| 2026-09-17 | 6d | Pre-assigning LAB input lines at routing preparation is a negative result; landed for the record, then removed | 40 to 60% more wires and the router at its cap on the probe, in every variant; the line must be chosen with the fabric route |
| 2026-09-17 | 6e | Close the placement-routing loop inside HeAP with a per-pass wire-density estimate behind `--spread-congestion`; k = 2, LAB cells only, not stacked on demand weighting | Best plateau on the core, 12,200 to 13,400 overused (35% below pairing alone), placement legal; k = 1.5, a thinner factor, or demand on top all leave the legaliser without room |
| 2026-09-17 | 6f | Measure the router's share before changing its negotiation; land the periodic re-route and the unit wire cost as opt-in options with the per-tile utilisation dump; move the next unit to the placement cost model | On the identical netlist nextpnr uses 2.8 times Quartus's fabric wires with a placement of lower wirelength: LAB lines are fed by row wires (88% of inputs), a vertical hop is a stair, registers seldom pack with their LUTs (16% against 95%); the core sits at 67% fabric use against Quartus's 24%; six negotiation variants move the plateau 13% either way, the unit wire cost halves it, none converges |
| 2026-09-18 | 6g | Pack a register into its LUT's ALM half as a cluster child behind `--register-packing`; one per LUT; the cluster's registers must pass the LAB control model | Probe: 99% of LUT-driven registers packed, Fmax down a few percent, pairing routes in a third of the time; core: 5,360 registers packed, plateau 13,992 under the delay cost (worse than 12,182) and 1,964 under the unit cost (a third of 5,694) |
| 2026-09-16 | 3a | Compute a reuse plan with reasons before applying anything, and validate each decision again when applying | Plans for both controlled edits name exactly the edited cells with the right reason |
| 2026-09-16 | 3b | Region expansion releases transplants by growing radius around the dirty cells, then everything, each retry from the pre-placement RNG state | Forced ladder: 3,606 then 5,844 then 2,126 then the rest; the last rung is the clean placement |
| 2026-09-16 | 3a | Typed build states in C++ with runtime adoption at the legacy boundary; a bitstream needs a validated build | `--rbf` on an unrouted design is refused instead of writing a meaningless file |

## Stage gates and promotion

| Gate | Status | Promotion state |
| --- | --- | --- |
| Stage 1: Rust preparation plans | Complete | Legacy default; Rust preparation authority available only by explicit mode |
| Stage 2: boundary optimization | Complete (2C performance target rejected) | Single-search capture, reduced decoder temporaries, and direct output promoted |
| Stage 3: complete LAB evaluation | Complete | Explicit shadow, verify, and Rust authority modes; legacy remains default |
| Stage 4: transactions and reuse | Complete for the Stage 4 scope (4A–4E); cross-build checkpoints and artifact provenance are the next design | Serial transaction authority and owned frozen batches enabled; `--placer-lookahead`, `--lab-reuse`, and `--reuse-placement` available, all off by default and not promoted |
| Stage 5: seams and checkpoints | Candidate list complete: 1c, 4b (retired), 2a, 2b, 3c, 3b, 3a; closing measurement recorded; 3c-2 (router2 bind order), 3c-3 (route survival), and 3c-4 (history seeding) landed from it | `--sa-seam`, `--sa-batch`, `--checkpoint`, `--resume`, `--route-prepare-only`, `--reuse-routes`, `--reuse-routes-history`, `--reuse-plan-out`, `--reuse-dry-run` available, all off by default; nothing promoted; 3c-2 is a default-path fix that is byte-identical for the clean flow |
| Stage 6: density | 6a (legaliser stall exit), 6b (ALM pairing), 6c (demand-weighted spreading), 6e (congestion-driven spreading), and 6f (the router's share measured; re-route and unit cost landed) complete; 6d (line pre-assignment) built, measured negative, removed; the crate stays concluded; the full core places and routes to a 12,200-wire plateau at 67% fabric use, and the next unit is the placement cost model | `--alm-pairing`, `--spread-demand`, `--spread-congestion`, `--router2-reroute`, `--router2-unit-cost` available, off by default, unpromoted; the stall exit is on the default path and byte-identical for designs that fit |
