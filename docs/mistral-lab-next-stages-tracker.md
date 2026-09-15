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

## Stage gates and promotion

| Gate | Status | Promotion state |
| --- | --- | --- |
| Stage 1: Rust preparation plans | Complete | Legacy default; Rust preparation authority available only by explicit mode |
| Stage 2: boundary optimization | Complete (2C performance target rejected) | Single-search capture, reduced decoder temporaries, and direct output promoted |
| Stage 3: complete LAB evaluation | Complete | Explicit shadow, verify, and Rust authority modes; legacy remains default |
| Stage 4: transactions and reuse | Complete for the Stage 4 scope (4A–4E); cross-build checkpoints and artifact provenance are the next design | Serial transaction authority and owned frozen batches enabled; `--placer-lookahead`, `--lab-reuse`, and `--reuse-placement` available, all off by default and not promoted |
