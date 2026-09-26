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
for the explicit unsupported-rules status. Legacy remained the default until the 2026-09-19 promotion of the complete evaluator (entry "Promotion").

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

### 2026-09-18: Stage 6, unit 6h: the row-aware placement cost

`--row-cost W` (off by default) weighs a vertical tile W horizontal
tiles in HeAP's solver, cut spreader, and strict legaliser
(`PlacerHeapCfg::anisotropic`; design section 9.7). The measured ratio
on the routed probe: a one-tile vertical hop costs 2.30 fabric wires, a
horizontal one 0.93, and the 2.5 ratio holds out to four tiles (vertical
4.22, horizontal 1.69).

Exec probe (`--seed 1 --threads 1 --freq 12`, four runs in parallel):

| Configuration | Same column | Same row | Rows per net | Fabric wires | Column wires (V2, V4, V12) | Router2 | Fmax |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Off | 15.3% | 14.4% | 1.80 | 52,952 | 13,420 / 5,236 / 811 | 20 iterations | 35.87 MHz |
| `--row-cost 2.5` | 11.8% | 19.2% | 1.65 | 48,696 | 11,589 / 4,313 / 660 | 47 | 33.48 MHz |
| `--row-cost 4` | 8.9% | 23.3% | 1.55 | 48,100 | 10,551 / 3,797 / 409 | 63 | 33.21 MHz |
| `--alm-pairing 1 --register-packing --row-cost 2.5` | 10.9% | 19.6% | 1.79 | 53,589 (57,218 without the row cost) | 12,725 / 4,836 / 684 | 27 (16 without) | 35.41 MHz (34.66 without) |
| Quartus (6f) | 7.0% | 18.8% | 1.52 | 18,676 | | | |

Off is byte-identical (0xbb18ede9 / 0xbc1365c6). The placement's shape
moves to Quartus's and the column wires fall by a fifth to a half; the
routed total falls 9%, the router takes two to three times the
iterations to settle the busier rows, and the probe's Fmax falls 7%.

Full core, pairing with congestion-driven spreading and register
packing (6g: plateau 13,992 under the delay cost, 1,964 under the unit
cost):

| Configuration | Placement | Router2 overused by iteration |
| --- | ---: | --- |
| `--row-cost 2.5 --router2-unit-cost`, capped at 15 | 739 s | 37,499, 6,368, 1,358, 628, 389, 292, 252, 218, 193, 172, 153, 142, 142, 131, 119: falling at every iteration |
| `--row-cost 2.5` (delay cost), capped at 15 | 737 s | 63,277, 26,668, 12,644, 8,009, 5,870, 4,664, 3,878, 3,318, 2,828, 2,511, 2,235, 2,084, 1,981, 1,822, 1,630: falling at every iteration |
| `--row-cost 2.5 --router2-unit-cost`, cap 80 | 754 s | 37,499 to 119 at 15 as above, 34 at 60, 29 at 80; router2 73 s; the last thirty are LAB input lines in 26 tiles, churning (3 persist from 60 to 80); router1's fallback then thrashes (its remaining-arc count does not fall in twenty minutes) and was stopped |
| `--row-cost 4 --router2-unit-cost`, cap 80 | | 24 at 65, 20 at 80; router2 88 s; router1 the same, stopped |
| `--row-cost 2.5` (delay cost), cap 80 | 737 s | 1,281 at 20, 905 at 30, then back up to 1,256 at 80: a plateau at a tenth of the delay cost's plateau without the row cost |
| `--row-cost 2.5 --router2-unit-cost --router2-reroute 20 --router2-reroute-contested`, cap 80 | 760 s | 37,499, 6,368, 1,358, 628, ... 119 at 15, 98, 88, 75, 74, 65, then the first re-route (18,373 nets queued beyond the 109 that failed): 44, 17, 17, 15, 15, 9, 7, 8, 10, 10, 10, 9, 17, 13, 15, 11, 9, 6, 6, 2, the second re-route (17,064 beyond 4): 18, 1, 1, 1, **0 at iteration 45**; router2 107 s, archfail 0, router1's check passed, `Routing complete`, JSON and report written; signoff 11.39 MHz at the 33 MHz target (Quartus 25.18 MHz for this core) |

**The full core routed to completion for the first time** (2026-09-18,
09:44, `build/stage6-fullcore/6h/core_rc25_unit80_rr.{log,json,report.json}`,
placement checksum 0xe0b15557, routing 0x681553a4): pairing, congestion
spreading, register packing, the row cost, the unit wire cost, and 6f's
periodic contested re-route every 20 iterations. What the last wires
were: LAB input lines in LABs at 40 to 45 distinct external input nets
(the device's median is 29, its 90th percentile 38; 286 LABs sit at 40 or
more), where the entry-wire-to-line matching is tight enough that
negotiation churns; a re-route of every net on a wire with history
resolves it in two rounds where router1's fallback does not finish.

Router-only experiments from a placed checkpoint of the same
configuration (`--no-route --checkpoint`, 320 MB; resumed with the
options that travel in `ArchArgs` and `MISTRAL_R2_MAX_ITER`):

| Router configuration | Result |
| --- | --- |
| unit cost, no re-route, cap 300 | 0 at iteration 244 (172 at 20, 34 at 100, 16 at 200); router2 127 s; 10.67 MHz |
| unit cost, `--router2-reroute 10 --router2-reroute-contested` | 0 at 45, four re-routes; 163 s; 11.28 MHz |
| unit cost, `--router2-reroute 10` (every net) | 0 at 40, three re-routes; 159 s; 11.86 MHz |
| unit cost, present-congestion floor 1.0, cap 120 | 28 at 120: no help |
| unit cost, no timing-driven routing, cap 120 | 825 at 120: worse |
| delay cost, `--router2-reroute 20 --router2-reroute-contested`, cap 200 | 0 at iteration 125 (2,511 at 10, 608 at 30, 86 at 90, 12 at 110), six re-routes of about 31,000 nets; router2 395 s, 842,300 wires in use against the unit cost's 745,000 to 767,000; **9.76 MHz**, below every unit-cost result |
| full run with `MISTRAL_LAB_INPUT_LIMIT=40` (placement slack under the lines), unit cost, no re-route, cap 80 | placement 1,001 s (754 s at the default limit); **0 at iteration 73 with plain negotiation** (38,046 at 1; the last wires no longer churn: two nets fewer per LAB at the top of the distribution is the slack the line matching needed); router2 113 s; 9.49 MHz |

Reading. The row cost is the lever that turns the core from a plateau
into a descent: with it, the same placement recipe that sat at 12,000
overused wires under the delay cost and 1,964 under the unit cost falls
at every iteration, to 119 at 15 under the unit cost, and the last few
dozen wires are the structured LAB-input-line conflicts of 6d in LABs at
the top of the input distribution. Those the periodic contested re-route
of 6f resolves in two rounds; plain negotiation also resolves them given
244 iterations; router1's fallback does not. The full core therefore
routes to completion under four router configurations, all with the row
cost, at 10.7 to 11.9 MHz signoff under the unit wire cost and 9.76 MHz
under the delay cost, which uses 10 to 13% more wires and settles later.
Quartus's fit of the same core is 25.18 MHz, and nextpnr's timing model
reads about 18% below Quartus's on the same path (6f), so the remaining
Fmax gap is placement and routing quality, not the model alone. The
input limit at 40 instead of 42 is the placement-side finisher: with two
nets of slack per LAB the line matching stops churning and plain
negotiation closes in 73 iterations, at a third more placement time. The
unit's recipe, as landed in `build/stage6-fullcore/core_probe_flow.sh`
(outside git): `--alm-pairing 1 --spread-congestion --register-packing
--row-cost 2.5 --router2-unit-cost --router2-reroute 20
--router2-reroute-contested`, about 15 minutes wall. Everything stays
opt-in; the next unit is the delay-aware base cost that keeps the unit
cost's wire count (a hybrid), then the LAB-level assignment of 9.5, and
timing-driven placement quality, which the core now makes measurable.

Validation as landed: `./build/rust-enabled/nextpnr-mistral-test` 60/60,
`./build/nextpnr-mistral-test` 50/50, exec probe off byte-identical,
`git diff --check` and `clang-format` clean.

### 2026-09-18: Benchmark: the concurrent Rust mode on the full core

The milestone recipe (6h) on the core, `--router2-max-iter 100`, each
run alone or alongside one other, M1 Ultra. The serial C++ path is the
reference; the question was whether the Stage 4D lookahead, the Rust
legality evaluator, and the batched annealer buy wall time on a design
where nearly every cell is a cluster.

| Configuration | HeAP | Refine | Router2 | Wall | CPU | Placement checksum |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `--threads 1`, legacy evaluators | 724 s | 70 s | 94 s | 904 s | 960 s | 0xe0b15557 |
| `--threads 8`, legacy | 722 s | 70 s | 96 s | 904 s | 960 s | 0xe0b15557 (router2 did not engage its workers; routing 0x681553a4 in both) |
| `--threads 8 --placer-lookahead 8`, legacy | 798 s | 62 s | 87 s | 959 s | 1,685 s | 0xe0b15557: byte-identical, 10% slower, 1.8 times the CPU |
| `--threads 8 --lab-legality rust` | HeAP iteration 2 of 21 after 61 min | | | stopped | | an order of magnitude slower |
| `--threads 8 --placer-lookahead 8 --lab-legality rust --sa-seam on --sa-batch 32` | HeAP iteration 6 of 21 after 92 min, about one core busy | | | stopped | | five to ten times slower |
| Quartus 17.0.2 on NAS01 (Ryzen 7 2700, `--parallel=2`) | fit 26:35 | | | 39.5 min (map 10:03, fit 26:35, sta 2:30, asm 0:20) | | 20,576 ALMs, 25.18 MHz |

Attribution on the exec probe (single thread, four runs, all
byte-identical to the reference 0xbb18ede9 / 0xbc1365c6):

| Configuration | HeAP | Refine (SA) | Router2 | Wall |
| --- | ---: | ---: | ---: | ---: |
| legacy | 5.38 s | 8.87 s | 8.42 s | 29.3 s |
| `--lab-legality rust` | 12.23 s | 25.66 s | 8.57 s | 53.3 s |
| legacy, `--threads 8 --placer-lookahead 8` | 5.55 s | 8.88 s | 8.43 s | 29.5 s |
| rust, `--threads 8 --placer-lookahead 8` | 12.27 s | 25.51 s | 8.23 s | 52.9 s |
| rust with the unused live check skipped (experiment, not landed) | 11.95 s | 28.54 s | 8.75 s | 55.3 s |

The Rust mode made 14,585,426 evaluations (7,368,890 legal, 7,216,536
illegal, zero mismatches, zero errors), so the extra 24 s is about
1.6 µs per evaluation: the cost of capturing the whole LAB into a value
record for every check, not of Rust (the warm dispatch is 350 ns, unit
2C) and not of the live check the mode still runs alongside (skipping it
changes nothing). The legacy path checks the live structures in place,
which the evaluator's contract forbids by design (no live pointer across
the FFI). Eight workers with the lookahead change nothing in either mode
because the capture runs on the owner and the annealer, where most of
the Rust mode's extra time sits, is serial.

What the record's earlier speed figures were, since they read as
faster. The Stage 4C benchmark (390 ns per record on one worker, 12.85
times that on sixteen) timed the evaluation of records already captured,
and its speedup column is against the one-worker frozen path, not the
live C++ check; it never included the capture. The "about 1.3% of P&R
time" of the concluded entry is the control-set authority
(`--lab-controls rust`), re-measured today at 28.3 s against 29.3 s
legacy on the probe (1,513,411 evaluations, zero mismatches): still
true. The legality authority (`--lab-legality rust`) was validated in
Stage 3 for parity, 14,621,350 evaluations with zero mismatches and
byte-identical artefacts, and its wall time was not recorded then; today
it makes the same fourteen and a half million evaluations and costs 24 s
more than legacy. No file on the capture path (`lab_v2.cc`,
`lab_snapshot.*`, `lab_legality.cc`, the ABI, the crates) has changed
since the crate was concluded on 2026-09-15, and the per-capture cost
(1.6 µs) is below the 5 µs Stage 1c measured before Stage 2's capture
work. Nothing regressed; the legality mode was never faster end to end,
and the evaluator's own speed was and is at parity with C++.

Where the legality mode's microsecond goes (probe, `sample` over eight
seconds of the annealer, 3,737 samples inside `dispatch_lab_legality`):
the whole-LAB capture 62% (`capture_lab_v2_impl`: a linear first-
encounter search for every net id, about a hundred lookups over up to
46 nets per call, plus zeroing and copying the 3.8 KB record), the Rust
call 34% (validation `try_from` 19%, the evaluation proper 9%,
conversion 7%), the live check the mode runs and never reads 3%
(skipping it: HeAP 12.0 s against 12.3 s over two runs each). Two
levers measured or sized from that: `--sa-seam on` moves the annealer
to the overlay rules (Stage 1c) and takes the mode from 53.3 s to
36.0 s wall (evaluations 14.6 million to 5.6 million, byte-identical);
an O(1) net-id map and a caller-owned record would take the capture
from about 0.75 µs to about 0.2 µs, leaving the Rust call's 0.4 µs,
of which the evaluation is 0.1 µs and the rest is the validation the
contract requires. Parity beyond that needs validated LAB snapshots
that accept an ALM patch, a crate extension.

Reading. Threads buy nothing on the C++ path: router2's partitioned
threading does not engage on this design and the placement is serial,
so eight threads reproduce one thread to the checksum. The lookahead
does what it was built to do, a byte-identical placement, and costs 10%
of wall for it, because most speculated candidates are rejected. The
Rust legality path is the large cost by construction: a whole-LAB
capture per check, 1.8 times the probe's wall and an order of magnitude
on the core, where the legaliser makes far more checks on fuller LABs;
the lookahead's workers cannot recover it. The Rust mode is correct
(identical checksums, zero mismatches); it was built and concluded as a
parity harness, and a same-speed Rust authority would need an evaluator
over the live structures, which is the contract the crate was built not
to have. The single-threaded C++ recipe, 15 minutes, is the one to use;
against Quartus it is 2.6 times faster in wall time on newer hardware,
at 0.45 times the Fmax and 1.39 times the ALMs (28,652, of which 8,627
are route-through LUTs for registers that did not pack).

### 2026-09-18: Rust legality at parity: resident LAB snapshots

The user directed a performance revision of the concluded crate to
bring the Rust legality authority to parity with the C++ path. The
benchmark entry above attributed the mode's cost: the whole-LAB capture
per query 62%, the record's validation 19%, the evaluation itself 9%.
The revision (design section 10) keeps the value contract and removes
the per-query capture: Rust owns a resident snapshot per LAB, patched
one ALM at a time with net ids as run-stable keys; the arch marks the
changed ALMs from the LAB-version hooks; a changed ALM travels as a
trial the session undoes after the evaluation and is committed when the
next query of the LAB finds it unchanged. New surface, all under the
existing safety rules: `ResidentLabs` and `AlmPatchV2` in the crate,
`npnr_mistral_resident_v2_{create,reset,evaluate,destroy}` in the FFI
(owner-only handle, envelope checks, a panic poisons it),
`ResidentLabLegality` in `mistral/lab_resident.*`, `Arch::lab_alm_dirty`.
The capture path stays as the parity harness: shadow and verify run the
capture C++, the capture Rust, and the resident Rust on every query and
require the verdicts to agree.

Steps and what each measured on the exec probe (`--seed 1 --threads 1
--freq 12`, four runs in parallel per round):

| Step | HeAP | Refine (SA) | Wall | Note |
| --- | ---: | ---: | ---: | --- |
| legacy (final round) | 5.54 s | 9.55 s | 31.1 s | |
| capture path, `--lab-legality rust` (before) | 12.23 s | 25.66 s | 53.3 s | 14.6 million whole-LAB captures |
| resident, every dirty ALM patched and applied | 6.85 s | 16.46 s | 39.8 s | 26.0 million patches for 14.6 million queries |
| resident, ALMs back to the sent facts skipped | 7.09 s | 15.73 s | 39.0 s | 24.0 million patches: a query rarely sees a state the session already holds |
| resident, trials and commits, pending facts hashed | 9.60 s | 20.78 s | 45.6 s | slower: a byte hash of 376 bytes per differing ALM |
| resident, trials and commits, pending facts compared | 6.86 s | 15.13 s | 37.3 s | 14.9 million trials, 1.9 million commits, 11.1 million restores |
| resident, trials as views, control-set cache, lean verdict, occupant signature | 6.78 s | 16.49 s | 40.0 s | the same counts; the session copies nothing for a trial |
| the same with `--sa-seam on` | 6.41 s | 9.42 s | 32.0 s | within 3% of legacy in the same round |
| resident on per-bel patches, control rules on a resident mirror of the model's snapshot | 6.74 s | 15.34 s | 38.2 s | the placer changes one bel per query: 15.5 million trials, 2.6 million commits, 15.2 million restores by occupant; register queries run the control rules without a projection (legacy in this round: 5.61 s, 9.79 s, 31.6 s) |
| the same with `--sa-seam on` | 6.77 s | 10.98 s | 34.6 s | |
| resident without per-query buffers (trials composed on demand, the mirror's table scanned to its used length, resyncs and chain binds sent as commits, at most eight trials in view) | 6.21 s | 13.25 s | 35.3 s | legacy in this round: 5.54 s, 9.80 s, 31.1 s |
| the same with `--sa-seam on` | 6.14 s | 9.37 s | 31.3 s | at parity with legacy (31.1 s), verify zero mismatches |
| resident with the ALM rules over slot references and the arch's ALM input count carried in each patch (recomputed only in the harness modes) | 6.33 s | 13.68 s | 36.3 s | legacy in this round: 6.00 s, 10.95 s, 33.4 s |
| the same with `--sa-seam on` | 6.41 s | 10.99 s | 34.2 s | at parity with legacy (33.4 s), verify zero mismatches |
| resident with the trial rows written into the mirror in place and undone, the patch batch off the stack, and no count comparison in the authority mode (the final build) | 5.81 s | 12.09 s | 33.4 s | legacy in this round: 5.47 s, 9.49 s, 30.7 s |
| the same with `--sa-seam on` | 5.83 s | 9.16 s | 30.6 s | **at parity with legacy (30.7 s), verify zero mismatches** |
| `--lab-legality verify` (harness: capture C++, capture Rust, resident Rust, live) | 15.70 s | 41.11 s | 73.4 s | 14,585,426 queries, zero mismatches, zero errors |

Every run's checksums equal the legacy reference (0xbb18ede9 /
0xbc1365c6). Per query the resident path now costs about 0.2 µs in the
legaliser against the live check's 0.05 to 0.1 µs; the annealer's swaps
dirty two ALMs each and cost more, which the overlay seam (Stage 1c)
answers.

Full core, milestone recipe, four runs in parallel:

| Build and mode | HeAP | Refine | Router2 | Wall | Checksums and harness |
| --- | ---: | ---: | ---: | ---: | --- |
| legacy (same round as each row below) | 740 to 757 s | 69 to 73 s | 96 to 103 s | 921 to 953 s | 0xe0b15557 / 0x681553a4 |
| capture path, `--lab-legality rust` (before) | iteration 2 of 21 after 61 min | | | stopped | |
| resident, ALM patches, trials and commits | iteration 17 of 21 after 65 min | | | stopped | |
| resident, per-bel patches and the control mirror | iteration 7 at 16 min, the same pace | | | stopped | |
| resident, in-place trials and the arch's counts | 1,103 s | 90 s | 97 s | 1,303 s | identical; 5,356,364,659 queries (26.5 million legal), zero mismatches, zero errors; the same with the seam: 1,104 s, 85 s, 97 s, 1,299 s |
| the final build: trial rows in place, the batch off the stack, no count comparison in the authority mode | 1,065 s | 106 s | 95 s | 1,278 s | identical; 5,356,364,659 queries, zero mismatches, zero errors; the same with the seam: 1,057 s, 95 s, 94 s, 1,260 s (legacy in this round: 750 s, 67 s, 96 s, 926 s) |
| `--lab-legality verify` on the core (the harness: capture C++, capture Rust, resident Rust, and the live check on every query) | 13,875 s | 173 s | 86 s | 14,147 s | identical; 5,356,364,659 queries, **zero mismatches, zero errors, zero stale counts**: the capture C++, the capture Rust, and the resident Rust agreed on every verdict of the core's placement |

Reading. On the exec probe the Rust legality authority now runs at the
legacy path's wall time (30.6 s against 30.7 s with the annealer on the
overlay seam; 33.4 s against 30.7 s without it), byte-identical, with the
verify harness agreeing on every one of 14.6 million queries. On the
full core it runs at 1.4 times the legacy placement (1,065 s against
750 s), byte-identical, from the six times of the first resident build
and the order of magnitude of the capture path. The gap that remains is
59 nanoseconds per query over 5.36 billion queries, and it is the
architecture: the legacy check reads the arch's cached counts and runs
the native control-set evaluator in place, while the Rust authority
marshals the changed bel, crosses the FFI, runs the ALM rules on the
queried ALM and the control rules on the mirror, and writes a verdict,
about 60 nanoseconds of work that no further trimming of the same design
removes (the rules themselves are now cheaper than the native ones: the
mirror's control evaluation costs a quarter of `evaluate_lab_controls_native`).
What would close it is a per-tile query, one call for a candidate cell
against every bel of a tile with the verdicts returned as a mask, which
changes HeAP's legaliser loop (`try_place_cell`) and is a unit of its
own; it would remove the per-query call and marshalling but not the
rules, so it lands below 1.2 rather than at 1.0. The harness stands in
every mode: zero mismatches on the probe and on the core.

Validation as landed: `cargo test --offline --workspace` (the crate's
oracle test compares 8,000 random per-bel patch sequences, trials and
commits, with the arch's counts supplied and alternately recomputed,
against `evaluate_lab_v2` over a shadow model, and the control mirror's
conflicts, id release, and rejected patches have their own tests; the
FFI test covers envelopes, malformed patches, the flag, and an unreset
LAB), `cargo clippy -D warnings` and `cargo fmt --check` on the two lab
crates (the unrelated `nextpnr` binding crate is not fmt-clean upstream
and was left alone), `./build/rust-enabled/nextpnr-mistral-test` 61/61
(new: `ResidentLegalityPatchesChangedAlmsAndMatchesTheCapturePath`; the
stale-authority test now expects the authority mode to take the arch's
count and the verify mode to fail closed), `./build/nextpnr-mistral-test`
50/50, exec probe legacy byte-identical, `git diff --check` and
`clang-format` clean.

### 2026-09-19: Coding rules: no panics, protocol coverage, telemetry, fixture leaks, the gate

The user asked which coding rules the work carries and whether to add
more, and then to design and land the gaps found. Five landed, each
with the check that enforces it (design section 11):

- **No panics in the crates.** `#![deny(clippy::unwrap_used,
  clippy::expect_used, clippy::panic, clippy::unreachable)]` in
  `npnr_mistral_lab` and `npnr_mistral_lab_ffi`, tests exempt. The audit
  found one `expect`, on the FFI's worker quota release; it is a
  `let else` now. Indexing by validated ids is the panic source that
  remains, and the FFI's `catch_unwind` with the poisoned handle stays
  the backstop.
- **Protocol coverage in the oracle.** The resident module's doc lists
  the seven patch shapes `mistral/lab_resident.cc` sends; the oracle
  generator produces all seven, with bursts of sixty commits every
  fortieth step and unchanged resends every tenth, and asserts that it
  did (at least 190 bursts and 300 unchanged resends over 8,000 steps).
  The burst is the shape the core's verify run caught after the crate's
  tests had passed; the oracle catches it now.
- **Telemetry.** `--telemetry file.json` (`ArchArgs`,
  `mistral/telemetry.*`) writes the phase, device, checksum, the Stage 5
  and 6 options, the LAB legality, resident, and control-set counters,
  cell and net counts, and the placement and routing wall times, after
  placement and again after routing. The checksum is taken where the
  placer and router log theirs, so the file's number is the log's.
  `--report` is untouched.
- **Fixture leaks.** `LabControlCaptureTest` records the shared
  context's cell and net counts in `SetUp` and fails a test in
  `TearDown` that left any. Two tests leaked (MLAB grouping two RAM
  cells; ALM pairing five cells and 24 nets); both take theirs down now.
- **The gate.** `mistral/tests/gate.sh` runs the scoped cargo test,
  clippy, and fmt, builds and runs both gtest suites, runs the probe on
  the default path against its recorded checksums and report hash, then
  `git diff --check` and clang-format on the changed C++ files. Its
  logs go under `build/gate/`.

| Check | Result |
| --- | --- |
| `cargo clippy -D warnings` with the deny lints, both crates | Clean once the one `expect` was replaced |
| `cargo test --workspace` | Every crate test passes; the resident oracle's 8,000 steps include the bursts and unchanged resends it now asserts |
| gtest, `build/rust-enabled` | 62 tests pass (the telemetry writer test is new) |
| gtest, `build` (Rust disabled) | 51 tests pass |
| Fixture leak check, first run | 2 of 61 tests failed it (MLAB grouping, ALM pairing); 0 after the fixes |
| Probe with `--telemetry` against the same run without | Checksums `0xbb18ede9` / `0xbc1365c6`, `--report` byte-identical, the log differs in timings only |
| Telemetry files | Placement-only run: phase `placed`, checksum `0xbb18ede9`; full run: phase `routed`, checksum `0xbc1365c6`, placement 16.2 s, routing 8.2 s, 12,169 cells, 13,396 nets |
| `mistral/tests/gate.sh` end to end | Passed in 58 s (cargo 3 s, builds up to date, both suites 20 s, probe 25 s) |

### 2026-09-19: The live monitor (`--monitor`)

The user asked for the telemetry as a live dashboard, in Rust, behind a
`--monitor` parameter. It is a directed addition of Rust surface, the
second after the parity revision, recorded here and in CLAUDE.md
(design section 12 has the rationale). What landed:

- `rust/npnr_mistral_monitor`: a pure renderer. A `Snapshot` (phase,
  terminal size, checksum, run and phase seconds, the legality,
  resident, and control-set counters, cell and net counts) and the log
  tail go in; lines of exactly the terminal's width come out, with the
  panels chosen by priority when the terminal is short. The placer's
  and router's progress is read from their log lines (HeAP iterations,
  the annealer's, router2's `iter= ... overused=` lines, drawn as a
  sparkline). `#![forbid(unsafe_code)]`, the no-panic lints, nine tests
  (width and height at every clamp, panel contents, the small-terminal
  drop order, bars, number and clock formats, the three progress lines,
  the bounded tail, the rate, the phase round trip).
- The `monitor` module of `npnr_mistral_lab_ffi`:
  `npnr_mistral_monitor_{create,log,render,destroy}` over a handle with
  an internal lock (the log hook and the ticker are different threads),
  poisoned by a panic like the LAB handles; envelope checks on every
  pointer; layout asserts on the 264-byte snapshot and the 96-byte
  config on both sides; a dry flag for tests and callers that take the
  frame through the buffer. Two FFI tests (a round trip into the
  caller's buffer, the envelope errors).
- `mistral/monitor.*`: the session. Started by the command handler after
  the netlist is loaded (so the design name is the JSON's), refused with
  a warning when stdout is not a terminal or the build has no Rust. It
  removes the log's terminal streams, keeps the file stream, and feeds
  every message to the tail through `log_write_function`; a ticker
  thread renders every 250 ms from the arch's atomics, the phase clock,
  and cell and net counts snapshotted at each phase entry, and never
  reads the netlist. The resident session's five counters became
  single-writer atomics (a relaxed load and store, a plain add on the
  hot path) and the resident pointer is read with `atomic_load`; the
  telemetry checksum became atomic. `Arch::monitor_phase` marks pack,
  place, route preparation, route, and signoff; the destructor marks
  done or, during unwinding, failed, draws the last frame, and hands
  the cursor back below it.
- Tests: the log chunk splitter (both trees), and a dry session that
  collects the phase clock and counters and renders the frame
  (Rust-enabled tree).

| Check | Result |
| --- | --- |
| `cargo test --workspace` | monitor crate 9, lab crate 12, FFI crate 13 (two new); clippy `-D warnings` and fmt clean with the monitor crate in scope |
| gtest | 64 pass in `build/rust-enabled` (two new), 52 in `build` (one new) |
| Probe with `--monitor` under a 100x42 pseudo-terminal (legacy modes) | 96 frames over the 24 s run; checksums `0xbb18ede9` / `0xbc1365c6`, `--report` byte-identical to the run without the monitor; the final frame shows phases pack 0.0 s, place 14.6 s, route prep 0.1 s, route 9.1 s, signoff 0.2 s, router2 at iteration 20 with 0 overused, cursor restored, "Program finished normally" below it |
| Probe with `--monitor --lab-legality rust --sa-seam on` | Live panels mid-placement: 5,198,769 queries at 38,207/s in the annealer, legal 15.2%, trials 0.90 and restored 0.93 per evaluation; the run's resident totals equal the run without the monitor (5,570,583 / 4,191 / 4,699,227 / 1,053,914 / 4,940,434); placement 14.9 s, routing 8.7 s |
| `--monitor` with stdout redirected | "stdout is not a terminal; running without the monitor", checksums unchanged |
| `--monitor` on the Rust-disabled build | "needs the Rust build; running without the monitor" |
| `mistral/tests/gate.sh` | Passed in 39 s with the monitor crate in the cargo scope |

### 2026-09-19: Promotion: the Rust LAB evaluator is the default authority

The user directed the default to the Rust version and asked which
concurrency paths to enable with it. What changed:

- `--lab-legality` defaults to `rust` in Rust builds and to `legacy` in
  Rust-disabled builds (`kDefaultEvaluator` in `mistral/main.cc`, the
  same in `ArchArgs`); `--lab-legality legacy` selects the C++ rules in
  any build. Shadow and verify remain the harness.
- `--sa-seam` defaults to `on` in every build: byte-identical (1c-A,
  Fabi386 twice each way), 8% faster annealing, and the configuration
  the Rust authority's parity was measured with.
- `--lab-controls` stays `legacy`. The complete evaluator owns the
  control check and `main.cc` refuses a second Rust authority beside it
  ("--lab-legality owns its internal control check"); the Stage 1
  control-plan modes remain available with `--lab-legality legacy`.
  `--verify-lab-controls` selects the legacy control mode unless one is
  named.
- The gate's probe runs the default path as it now is (no explicit
  `--lab-controls`), against the unchanged identity constants.

Concurrency: nothing is enabled. The benchmark entry above is the
evidence: `--threads 8` equals `--threads 1` on the default path
because router2 partitions its nets across its own workers regardless
and placement has no parallel section without the lookahead; the
lookahead is byte-identical, 10% slower, and 1.8 times the CPU on the
core, bounded by the 0.77 useful commits per batch; `--sa-batch` is not
byte-identical and is owner-bound past two workers. Each stays opt-in.

| Check | Result |
| --- | --- |
| Rust-enabled binary, default options, exec probe | Checksums `0xbb18ede9` / `0xbc1365c6`, report SHA-256 `56e3b75e84be78a3…` (the recorded identity), 29.4 s wall; `LAB legality rust: evaluations=5570583 ... mismatches=0`, resident totals 5,570,583 / 4,191 / 4,699,227 / 1,053,914 / 4,940,434; telemetry options `legacy` / `rust` / `on`, placement 15.4 s, routing 9.2 s |
| Rust-disabled binary, default options, exec probe | Same checksums and report hash, 30.0 s wall, `Annealer swap seam: on` |
| Both Rust authorities by default (first attempt) | Refused by the existing guard two seconds in; the controls default was returned to legacy |
| gtest | 64 pass in `build/rust-enabled`, 52 in `build` |
| `mistral/tests/gate.sh` on the new default path | Passed in 44 s, probe identical |
| Full core | Not rerun: the parity entry records the Rust authority byte-identical to legacy on the core at 1,065 s placement against 750 s (1.4 times), verify harness zero mismatches over 5.36 billion queries; that cost is now the default's |

### 2026-09-21: Pack-time admission by the placer's authority

An evaluation of further Rust port targets found two, both pack-time
rules: the pairing rule of 6b, a third statement of the ALM input rule
kept as strict as the checker only by a unit test, and the register
rule of 6g, which asks the C++ twin of the control model while the
placer, since the promotion, asks Rust. The design (section 13) did not
port either. Reading the code showed the pairing rule is algebraically
the checker's input rule on plain LUTs and that the search evaluates it
for every candidate on every shared net, millions of times on the core;
an evaluator call there would cost more than the packing it guards.
What landed is an admission: before either packer commits a cluster,
`mistral/pack_admission.*` lays it on the first clean, non-MLAB LAB
through `alm_cluster_placement` and asks the run's LAB legality
authority, the bel overlay for the C++ rules and the overlay capture
with `npnr_mistral_eval_lab_v2` for Rust, both compared in shadow and
verify (a verify mismatch is an error). Nothing is bound; a refusal
undoes the cluster, is counted in the packer's report (`refused`), and
the cells go on unclustered. The pairing's gate assigns the two LUTs'
comb facts itself, since the pairing runs before `assignArchInfo`. No
new Rust surface.

| Check | Result |
| --- | --- |
| Probe, `--alm-pairing 1 --register-packing`, default (Rust) authority, against the binary before the change | Checksums `0x8d5fc97c` / `0x956e97b5` and report hash `5c670e0139b66090…` identical; `Pack admission alm-pairing: authority=rust checked=4108 refused=0 mismatches=0 errors=0`, `register-packing: checked=817 refused=0` |
| Core, `--pack-only` with both packers, Rust authority | 15,181 pairs and 5,360 packed registers as before; packed JSON identical to the baseline's in all 1,953,267 lines but the `creator` version; `checked=15181 refused=0` and `checked=5360 refused=0` |
| Core, `--pack-only`, `--lab-legality verify` | The C++ rules and the Rust evaluator agree on all 20,541 clusters: `mismatches=0 errors=0`; packed JSON equal to the Rust run's |
| Core, `--pack-only`, `--lab-legality legacy` | Same counts, packed JSON equal |
| Unit tests | Both packers' tests assert, in legacy, shadow, verify, and Rust modes, that the gate admits the clusters the packers form (a pair, a pair with a register on each half, a single LUT with its register) and refuses a pair forced against the input rule and a register forced against the control rules, with zero mismatches and the cluster left as it was |
| gtest, gate | 64 pass in `build/rust-enabled`, 52 in `build`; `mistral/tests/gate.sh` passed, default-path probe identity unchanged |

### 2026-09-21: The tile scan: the legaliser's scan of a tile as one question

The second target of the port evaluation was the per-tile query the
parity entry had named. The design (section 14) began with a
measurement, a temporary run-length counter in the legality dispatch
(not landed): how long are the runs of consecutive queries on one LAB,
and how do they end?

| Placement | Queries | Refusals | Mean run | Queries in runs of 17+ | Calls if batched per tile |
| --- | ---: | ---: | ---: | ---: | ---: |
| Exec probe, default options | 5,570,583 | 79.2% | 4.5 | 71.4% | 25.3% |
| Exec probe, recipe options | 4,287,045 | 8.5% | 1.1 | 7.0% | 92.5% |
| Full core, recipe options | 5,348,124,234 | 99.55% | 29.8 | 96.1% | 6.2% |

The probe under the recipe said not to build it; the core said the
opposite, and said what to build. Its queries are an unclustered cell,
mostly a register, bound, asked about, and unbound at every free bel of
a crowded LAB, tile after tile. The scan always ends at the first bel
the arch accepts, so the batch question is the scan itself, the first
legal bel in scan order, which evaluates the rules no more often than
the per-bel scan; and it is asked only after a first live refusal, so
an uncrowded tile costs what it cost. What landed:

- `ResidentLabs::evaluate_scan` (crate): the patches bring the LAB up to
  date through the sync step now shared with `evaluate`; the candidate's
  facts are held in view at each bel of the order as a trial would be,
  with recomputed counts, and the first legal index comes back. Patch
  shape 8 in the module's list; the oracle compares 2,000 and more scans
  with the capture path answering bel by bel, asserting that scans
  finding a later bel and scans finding none both occur.
- `npnr_mistral_resident_v2_scan` (FFI): envelope checks, the poisoned
  handle, a malformed scan answers `BAD_SNAPSHOT` with the LAB untouched
  so the caller asks per bel. One test.
- `ResidentLabLegality::scan`, `scan_lab_tile`, `capture_cell_v2_keyed`
  (arch): facts for a cell bound nowhere, the batch builder shared with
  `evaluate`, and a hook that declines what it does not cover (legacy
  mode, carry and LUTRAM cells, bels of two LABs or of the wrong kind).
- `PlacerHeapCfg::scan_tile_first_legal` (HeAP): in `try_place_cell`
  the filters became a pure predicate, the batch is asked once per tile
  scan after a first refusal of an available bel, refused bels ahead of
  the named one are skipped, the named bel is bound and certified by
  `isBelLocationValid` as before, and the ripup draw for occupied bels is
  where it was. In shadow and verify nothing is skipped and every
  prediction is compared with the live answer (`scan_tile_advisory`,
  `note_lab_tile_prediction`; a mismatch is fatal in verify).
- On by default in the Rust legality modes; `--no-lab-tile-scan` for the
  A/B. `scans`, `scan-bels`, and `scan-hits` on the resident stats line
  and in the telemetry file.

| Check | Result |
| --- | --- |
| Probe placement, default options, scan on against off | Checksum `0x7f9f8105` both; per-bel queries 1,489,160 against 5,570,583; 307,568 scans over 5,959,604 bels; strict legalisation 0.78 s against 1.10 s (the solver dominates the probe's placement, which does not move) |
| Probe placement, recipe options, scan on against off | Checksum `0xa99e0f68` both and equal to the run before the change; 31,854 scans; strict legalisation 1.40 s against 1.36 s, as the run lengths predicted |
| Probe placement, `--lab-legality verify` (advisory scan) | Same checksum; 307,568 scans, every prediction compared with the live check of the same bel: `mismatches=0 errors=0` |
| Full core placement, recipe options, scan on against the per-bel Rust path | Checksum `0x2d44a02e` both; strict legalisation 538.7 s against 964.7 s, HeAP 595.1 s against 1,020.1 s, placement 669.4 s against 1,092.4 s; per-bel queries 256,042,738 against 5,348,124,234; 167,119,070 scans over 5,119,980,450 bels, of which 572,943 named a bel; legal answers 23,986,231 in both |
| Full core placement, legacy C++ authority, same binary, same day | Checksum `0x2d44a02e`; strict legalisation 662.1 s, HeAP 717.6 s, placement 781.1 s: the Rust default with the scan places the core in 0.86 of the legacy time, where the per-bel path took 1.40 |
| Default-path probe identity (the gate) | `0xbb18ede9` / `0xbc1365c6`, report hash unchanged, with the scan on |
| gtest | 65 in `build/rust-enabled` (the scan against bind, check, and unbind on a filling LAB, what it declines, the mismatch path), 52 in `build` |
| Verify-mode placement of the core | Recorded below ("The tile scan certified on the core"): zero mismatches over 5.35 billion evaluations and 167 million advisory scans |

What remains in the core's strict legalisation is 167 million scans at
about 3.2 microseconds, a hundred nanoseconds per bel evaluated, and
99.7% of them find nothing. The next lever is inside the scan: a
LAB-wide refusal (the input limit, a control set no slot can take)
decided once instead of per bel. It is not started.

### 2026-09-21: The tile scan, refined: cheapest first, asked first after a refused tile

The entry above left 167 million scans at a hundred nanoseconds per bel
evaluated. A profile of the core's legaliser with the scan on and a
temporary count of the refusal reasons inside the scans (not landed)
said where that went (design section 14.1):

| Refusal of a bel evaluated in a scan, first 805 million on the core | Share |
| --- | ---: |
| `ODD_FF`: the second register bel of an ALM half, refused for any register | 55.4% |
| `DATAIN_PATH`: no route-through LUT or E/F input left for the register's data | 21.0% |
| `LAB_INPUT_LIMIT` | 11.9% |
| `CONTROL_CONFLICT` | 11.8% |
| Legal | 0.001% |

Three bels in four fail the ALM rule alone, and the verdict recomputed
the input counts before asking it. Four refinements, none of which
changes an answer:

- The scan asks the verdict's predicates in order of cost and stops at
  the first refusal (`ResidentLabs::bel_legal`): the ALM rule, then the
  input total with counts recomputed for the trial ALMs only, then the
  control rules on the mirror. The per-bel path and its reasons are
  untouched.
- An MLAB-capable LAB takes the same path, and the full verdict, which
  owns the MLAB group rule, is asked only for a bel the cheaper
  predicates have passed; before, every bel of such a LAB took it.
- A register is not evaluated at the second register bel of a half.
  The shortcut is a fact of `check_alm`, and a property test holds it to
  the rule: 8,000 random ALMs with a register there, all refused.
- The legaliser asks the batch before its first bind when the previous
  tile's first available bel was refused
  (`StrictLegaliser::scan_prev_refused`). The batch answers what the
  live check would, so when it is asked changes cost and never the
  result; on the core nine tile visits in ten follow a refused one.

| Check | Result |
| --- | --- |
| Crate tests | 13 (the scan oracle unchanged and passing, the property test new); clippy and fmt clean |
| Full core placement, recipe options | Checksum `0x2d44a02e`, legal answers 23,986,231, as before. Strict legalisation 391.3 s, HeAP 451.5 s, placement 529.6 s, against 538.7 / 595.1 / 669.4 s for the first scan build (486.6 / 545.1 / 624.1 s with the cheapest-first order alone) and 662.1 / 717.6 / 781.1 s under the legacy C++ authority; per-bel queries 88,799,779 against 256,042,738; 167,516,520 scans. Measured with the verify-mode run sharing the machine |
| Probe placement, default and recipe options, against the first scan build | Checksums `0x7f9f8105` and `0xa99e0f68`, unchanged; in `--lab-legality verify` under both option sets every prediction compared with the live check of the same bel, 355,103 scans over 7,076,786 bels and 41,220 over 831,854: `mismatches=0 errors=0` |
| Default-path probe identity, gtest, gate | `0xbb18ede9` / `0xbc1365c6` and the report hash unchanged; 65 tests in `build/rust-enabled`, 52 in `build`; `mistral/tests/gate.sh` passed |

The finding behind the first row of the reasons table is larger than
the scan. Twenty of a LAB's forty register bels are never legal for a
register (upstream's checker refuses them with "TODO: why are these FFs
broken?"), they are always free, so the legaliser scans them at every
tile visit, and HeAP's cut spreader takes a tile's capacity from the
number of bels in its bucket, so it spreads registers as if a LAB held
forty. Correcting the capacity changes placements and is a unit of its
own, opt-in, with its own quality evidence; it is the next one.

### 2026-09-21: The tile scan certified on the core in verify mode

The follow-up the tile scan's entry left pending. The core's placement
under the recipe's options with `--lab-legality verify`, on the first
scan build (`a905f6f5`), artefacts in
`build/stage6-fullcore/verify-scan/`:

| Check | Result |
| --- | --- |
| Placement | Checksum `0x2d44a02e`, the scan build's and the legacy authority's; HeAP 13,279 s, the harness's price |
| Per-bel evaluations | 5,348,124,234, each compared across the resident session, the capture path in C++ and in Rust, and the live check: `mismatches=0 errors=0 stale-cache=0 stale-revision=0` |
| Advisory scans | 167,119,070 over 5,119,980,450 bels, nothing skipped, every prediction compared with the live answer of the same bel, a difference fatal: the run finished normally |
| Pack admission in verify | 15,181 pairs and 5,360 registers, `mismatches=0` |

The refined scan (`e0e1cdac`) changed the scan's evaluation after this
run had started. It has verify-mode evidence on the probe under both
option sets; its own core run was started on 2026-09-21 in
`build/stage6-fullcore/verify-scan-refined/` and is to be recorded when
it ends. (It did not end: it was stopped after 31 of 105 iterations with
no mismatch when the run over units 16.6, 16.2, and 16.1 superseded it;
that run covers the refined scan and is recorded below, "The refined
scan and the units of design 16 certified on the core in verify mode".)

### 2026-09-21: Time Profiler run of the full core flow

At the user's request the flow was profiled with Instruments' Time
Profiler from the command line (`xcrun xctrace record --template 'Time
Profiler' --launch`), the whole run from load to signoff under the
recipe, exported (`xctrace export`, schema `time-profile`) and
aggregated by phase from each sample's own stack. 715,373 samples,
715.4 s of CPU. Artefacts, outside git, in
`build/stage6-fullcore/profile/`: `core.trace` (opens in Instruments;
moved on 2026-09-22 to the Envoy drive, `archive/nextpnr/stage6-fullcore-profile/`, as `.trace.tar.zst` with a README and `SHA256SUMS`), the exported samples, `aggregate.py` and `inclusive.py`, and the two
text reports.

| Phase | Time | Share |
| --- | ---: | ---: |
| HeAP strict legalisation | 389.4 s | 54.4% |
| router2 | 143.3 s | 20.0% |
| Annealer refinement | 82.1 s | 11.5% |
| HeAP solver | 74.8 s | 10.5% |
| HeAP cut spreading and other | 20.1 s | 2.8% |
| Device load, netlist load, pack, the rest | 5.7 s | 0.8% |

Inside strict legalisation, inclusive: `try_place_cell` 282.8 s, of
which the tile scan 157.7 s (the Rust scan 131.9 s: `rules::evaluate`
40.9, `check_alm` 37.4, the mirror's trial rows 15.1, the scan's own
bookkeeping 38) and the C++ loop about 100 s (`try_place_cell` self
63.9, `checkBelAvail` 35.0); `try_place_cluster` 90.9 s, of which the
transaction callback 85.1 s (`freeze_placement_candidate` 52.0 with the
whole-LAB capture 34.7 s self, the C++ evaluation 13.2, the Rust
cross-check 13.8); the per-bel dispatch 15.0 s. In router2: the
priority queue 35.9 s, `route_arc` 21.5 s, and three hash lookups per
wire 33 s (`dict<WireId, int>::at` 14.5, `dict<PipId, NetInfo *>`
lookups 12.0, `dict<WireId, WireInfo>::at` 6.4). In the solver: building
the system 57 s (`build_solve_direction` 33.8,
`vector<pair<int, double>>::insert` 16.7, `build_equations` 6.2) against
12 s in Eigen's conjugate gradient. The annealer has no function above
1.5% of the run.

The probe's profile is a different program: annealer 47%, solver 32%,
strict legalisation 3%. Nothing about the legaliser can be sized from
it.

Six paths were judged worth work and are designed in section 16 of the
design document. The control-rule design's premise, that a register's
control verdict is the same at every bel of a LAB, was checked by a
property test before the design was written down (3,000 random LABs,
more than 50,000 bels compared, no difference), and was wrong: the
test's generator could not reach the case, and the full core disproved
it when the unit was built (entry "Unit 16.4", below).

### 2026-09-21: The register capacity the rules admit: built, measured negative, removed

The finding of the refined tile scan's entry, acted on and recorded as
unit 6d was. Twenty of a LAB's forty register bels are never legal for a
register, the legaliser scans them at every visit, and HeAP's spreader
counts them as capacity. `--usable-register-bels` (design section 15)
filed those bels under a bucket no cell type maps to and refused them in
`isValidBelForCellType`, so the bel lists, the spreader's capacity, the
scans, and the annealer's proposals saw twenty register bels per LAB. A
test held the predicate to the rule: on an empty LAB, under the legacy
rules and the Rust evaluator, a lone register is refused at exactly the
twenty bels the option hides.

Exec probe, full flow, three seeds, without and with the option:

| Options | Fmax, seeds 1 to 3 (MHz) | Mean | Wires | router2 iterations |
| --- | --- | ---: | --- | --- |
| Default, all bels | 35.87 / 34.98 / 32.96 | 34.60 | 149,576 / 149,154 / 150,377 | 20 / 46 / 18 |
| Default, usable only | 34.75 / 30.12 / 33.95 | 32.94 | 148,144 / 148,771 / 147,914 | 88 / 23 / 16 |
| Recipe, all bels | 32.32 / 33.64 / 34.73 | 33.56 | 131,026 / 132,126 / 129,728 | 9 / 16 / 13 |
| Recipe, usable only | 34.93 / 35.25 / 32.41 | 34.20 | 130,093 / 130,431 / 130,606 | 15 / 10 / 12 |

Inside the seed spread on the probe, which is not crowded. The full
core, recipe options, full flow:

| Seed | Register bels | Strict legalisation | Placement | Bels asked in scans | router2 | Wires | Fmax |
| --- | --- | ---: | ---: | ---: | --- | ---: | ---: |
| 1 | all | 379 s | 534 s | 5.29 billion | 45 iterations | 767,087 | 11.39 MHz |
| 1 | usable only | 319 s | 442 s | 1.91 billion | 48 iterations | 773,142 | 11.05 MHz |
| 2 | all | 374 s | 522 s | 4.49 billion | 51 iterations | 778,792 | 10.94 MHz |
| 2 | usable only | 412 s | 559 s | 2.69 billion | 78 iterations | 766,253 | 10.89 MHz |
| 3 | all | 331 s | 499 s | 4.26 billion | 64 iterations | 770,569 | 11.87 MHz |
| 3 | usable only | 392 s | 543 s | 2.36 billion | **not routed**: 1,387 overused at the cap of 100 | 819,557 | - |

The scans ask about half as many bels, and that is all the option
delivers. Seed 1, the first measured, was the flattering one: on seeds
2 and 3 the legaliser is slower, not faster, the router needs half
again as many iterations, and seed 3 does not route at all where the
default routes it in 64 iterations. The default runs of seed 1
reproduce the recorded core result (placement `0xe0b15557`, routing
`0x681553a4`). A reading, not measured further: the overstated
capacity lets the spreader leave a register beside the logic it
belongs to and the legaliser then finds the nearest real bel, while the
true capacity makes the spreader move registers out before the
legaliser has a say, which costs wires (6% more on seed 3) where the
core has none to spare.

The option landed in `5407ae7b` and is removed in the commit after it. What stays: the
finding, the design section with its outcome, and a test that the
second register bel of every half refuses a register under both
authorities, which is the fact the tile scan's shortcut and any later
attempt rest on. The cost of scanning those bels is already mostly
gone: the refined scan skips them in Rust without evaluating, and
design 16.2 removes the loop's share.

### 2026-09-21: Unit 16.4: the control rules in scans: a wrong premise caught by the core, and no gain from the right one

The first of the six hot-path units (design 16.4), and a negative
result with a lesson in it.

**The first form was wrong.** The design argued that a candidate
register's control verdict is the same at every bel of a LAB, so a scan
could ask the control rules once, keep the answer, and end at a
refusal. A property test written during design agreed (3,000 LABs,
50,000 bels). Built, it passed the crate's 14 tests including the scan
oracle, clippy, the probe's placement checksums under both option sets,
and verify mode on the probe with zero mismatches over 7.9 million
compared predictions. The full core's placement did not: checksum
`0xea133043` where `0x2d44a02e` is required, with strict legalisation
at 305 s. The unit stopped there.

**Why.** `rules.rs` walks the registers twice. The first walk's pools
are disjoint per kind, so it fails by the set of signals alone. The
second walk gives data lines by first fit over lists that overlap
between kinds in different orders (enables `Datain2, Datain3, Datain0`;
clears `Datain3, Datain2`), taking the first free line without looking
ahead for one that already carries the signal. A net that serves two
kinds fits or not by which resource the first walk gave it, which
follows the registers' physical order: a `DatainConflict` is a refusal
of one bel, not of the LAB. The design's analysis took the two walks to
be alike; the property test gave each kind nets of its own and could
not meet the case; the probe has no such LABs.

**What the wrong form got past, and what now stops it.** A mutation
check put the wrong early exit back: the main scan oracle still passed.
A second oracle, `scans_over_hostile_control_labs_equal_the_capture_path`
(one palette of nets for every control kind, clocks that are not
global, registers the control rules alone decide), compares every scan
with the capture path bel by bel, asserts that it contains scans whose
first legal bel follows a control refusal, and fails on the mutant in a
fraction of a second.

**The right form does not pay.** Ending a scan only at a first-walk
refusal (clock, sload, sclr conflict, clear or enable capacity), which
does hold at every bel, was built with a hostile property test that had
to find position-dependent LABs to pass, and did.

| Core placement, recipe options | Checksum | Legal answers | Strict legalisation | Placement |
| --- | --- | ---: | ---: | ---: |
| Before (`e0e1cdac`) | `0x2d44a02e` | 23,986,231 | 391.3 s | 529.6 s |
| First form, wrong | `0xea133043` | - | 305.4 s | - |
| Corrected form | `0x2d44a02e` | 23,986,231 | 391.8 s | 534.7 s |

Inside the noise: first-walk refusals are rare among the core's scans,
and the control refusals that are common are the kind that must not be
shortcut. The early exit is reverted, as the plan rules. Kept: the
hostile scan oracle, and the split of the scan's legality check into
the ALM-and-total part and the control part, behaviour-neutral, which
unit 16.1 reuses. The tree as committed was run on the core once more:
checksum `0x2d44a02e`, 23,986,231 legal answers, strict legalisation
386.2 s.

### 2026-09-21: Unit 16.6: the scan's bookkeeping in constant time

The second hot-path unit (design 16.6), in the crate only and with no
change of any answer. Each LAB's session keeps a 60-bit occupancy mask
beside its facts, set where commits are applied and cleared at a reset,
and the scan decides whether the bels it was given are free by bit tests
on that mask overlaid with the call's patches, where it searched the
patch list and read an `occupied` field out of the 3.8 KB facts record
for each of up to forty bels. The candidate travels beside the trials
(`view_with`, and the same parameter through `bel_passes_alm_and_total`
and `control_legal`), so the array of trials in view is built once per
scan and the full array only on the rare paths that ask `verdict`. The
ALM rule is generic over where a refusal goes (`Reject`, implemented by
the assessment and by `Discard`), so a scan's refusing branch is a
return and the scan no longer carries a scratch assessment.

| Check | Result |
| --- | --- |
| Crate tests | 14, the main scan oracle now asserting after every step that the occupancy mask equals the facts, the hostile scan oracle, both property tests; clippy and fmt clean |
| Probe placement, default and recipe options | Checksums `0x7f9f8105` and `0xa99e0f68`, scans and hits unchanged (355,103 / 165,558 and 41,220 / 27,292) |
| Probe placement, `--lab-legality verify`, both option sets | Same checksums, `mismatches=0 errors=0` |
| Core placement, recipe options | Checksum `0x2d44a02e`, 23,986,231 legal answers, 167,516,520 scans, 661,994 hits: all unchanged |
| Core strict legalisation | 378.4 s and 377.1 s, against 391.3, 391.8, and 386.2 s for the tree before it: about 12 s, 3% |

Kept. The estimate was 15 s; the measured 12 s is mostly the occupancy
mask, which removes a scattered read of each LAB's facts from every one
of 167 million scans.

### 2026-09-21: Unit 16.2: the legaliser's scan loop evaluates its filters once

The third hot-path unit (design 16.2), in `try_place_cell` only. The
tile scan's look-ahead, which lists the bels that remain for the batch,
evaluated the region test, the control-set filter, and the availability
of every bel from its start to the end of the tile, and the loop then
evaluated them again as it reached each bel. The look-ahead now records
two flags per position (`scan_flags`: passes the filters, available) and
the loop reads them from the look-ahead's start onward. The filters are
pure, and availability changes during a scan only through the scan's
own binds, each undone before the scan continues or ending it, so the
ripup draw is made for exactly the same bels.

| Check | Result |
| --- | --- |
| Probe placement, default and recipe options | Checksums `0x7f9f8105` and `0xa99e0f68`, scans and hits unchanged |
| Probe placement, `--lab-legality verify` (the flags are read in the advisory scan too) and `--lab-legality legacy` (no batch, flags never read) | `0x7f9f8105` in both, `mismatches=0` |
| Core placement, recipe options | Checksum `0x2d44a02e`, 23,986,231 legal answers, 167,516,520 scans, 661,994 hits: all unchanged |
| Core strict legalisation | 367.1 s and 373.3 s, against 378.4 s and 377.1 s with unit 16.6 alone: about 7 s, 2% |

Kept, at its measured size. The estimate was 25 to 35 s, from
`checkBelAvail`'s 35 s in the profile; the duplicate was the smaller
share of that, since most of the loop's availability tests belong to
tile visits where no batch is asked and to the positions before the
look-ahead. Every run with the change is below every run without it,
which is the most two runs a side can say.

### 2026-09-21: Unit 16.1: cluster candidates through the resident session

The fourth hot-path unit (design 16.1) and the largest. Every pair and
register cluster HeAP tries goes to `place_cluster_transaction`, 19.6
million times on the core with 96.6% rejected. The Mistral callback
froze each candidate as one whole-LAB overlay capture per edited bel,
evaluated every record in C++, and evaluated it again in Rust as a fatal
cross-check: 91 s in the profile, and the C++ verdict the authority in
every legality mode. In the Rust mode the candidate is now answered by
the session that already holds the LAB.

- Crate: `ResidentLabs::evaluate_edits` (patch shape 9). The patches
  bring the LAB up to date through the shared sync step; the candidate's
  edits of the LAB, the facts of the cell each places or empty facts
  where one is displaced, are held in view together, an edit overriding
  a pending trial of the same bel; the answer is the conjunction the
  frozen path computes per edit, asked once per distinct predicate: the
  ALM rule for each ALM an edit touches, the input total once, the
  control rules once if any edit is a register bel, the full verdict
  for an MLAB. The pending trials and the edits share the places in
  view; a candidate with more edits is a malformed call.
- FFI: `npnr_mistral_resident_v2_edits`, with the envelope checks and
  the poisoned handle of its siblings.
- Arch: `ResidentLabLegality::edits` over the shared batch builder;
  `placement_candidate_resident` groups a prepared transaction's edits
  by LAB, builds each edit's facts with `capture_cell_v2_keyed`, and
  declines what the session does not cover (a bel outside a LAB, a
  LUTRAM cell, a LAB over the budget, which is a carry chain).
- Callback: `rust` mode asks the session and rejects or commits;
  `shadow` and `verify` run the frozen path as before, which decides,
  and compare the two, a difference fatal in verify; `legacy`, the
  lookahead's worker threads, and what the session declines are
  unchanged. `--no-lab-tile-scan` turns this off with the tile scan.
  `edit-calls` and `edit-bels` join the resident stats line and the
  telemetry file.

| Check | Result |
| --- | --- |
| Crate | The main oracle compares edit sets (placements, removals, edits overriding a pending trial, over-budget sets) with the capture path answering edit by edit, coverage asserted; two mutants about removals (a removed bel's ALM unchecked; the control rules skipped for a removed register) both fail it within a hundred steps; FFI test for the round trip and the envelopes |
| gtest | The resident answer equals the frozen evaluation's for a pair with a register on each half, a pair with one, a single LUT with its register, the same with a displaced register among the edits, and a register forced in against the control rules; legacy declines |
| Probe placement, default and recipe options | Checksums `0x7f9f8105` and `0xa99e0f68`; transaction counters exactly the baseline's (2,892 / 1,632 / 1,260 and 251,392 / 153,720 / 97,672 attempted, committed, rejected) |
| Probe placement, `--lab-legality verify`, both option sets | Same checksums and counters; the resident answer compared with the frozen evaluation on 624 and 249,880 candidates: `mismatches=0 errors=0` |
| Probe, recipe, strict legalisation | 0.64 s against 1.44 s with `--no-lab-tile-scan` |
| Core placement, recipe options | Checksum `0x2d44a02e`, 23,986,231 legal answers, 167,516,520 scans, and the transaction counters 19,561,195 / 668,514 / 18,892,681: all unchanged. 19,528,999 candidates (99.8%) answered by the session over 56,087,279 edited bels, the rest carry chains on the frozen path. Strict legalisation 282.4 s against 370 s with units 16.6 and 16.2, HeAP 342.4 s, placement 425.7 s against 503 s; the legacy authority takes 781 s |

### 2026-09-21: Unit 16.3: the equation system appending then merging: bit-identical, no gain, not kept

The fifth hot-path unit (design 16.3), in upstream's `EquationSystem`.
The profile put 57 s of the solver phase into building the system
(`build_solve_direction` 34 s with `add_coeff` inlined, a vector insert
17 s) against 12 s in Eigen's conjugate gradient, and the design blamed
`add_coeff`'s binary search and shifting insert. The unit appended
contributions and merged each column once in `solve` by a stable sort
and a fold in arrival order.

| Check | Result |
| --- | --- |
| Probe placement, default, recipe, and legacy | Checksums `0x7f9f8105`, `0xa99e0f68`, `0x7f9f8105`: the matrix is bit-identical, as the design argued |
| Probe HeAP time | 6.0 s against 5.8 s and 8.2 s against 7.5 s: slower, if anything |
| Core placement, recipe options | Checksum `0x2d44a02e`; HeAP 342.40 s against 342.41 s; strict legalisation 287.1 s against 282.4 s and the remainder 55.3 s against 60.0 s, which is noise moving between the two |

Not kept; `placer_heap.cc` is as it was. The argument for identity held
and the argument for speed did not. The original keeps each column small
by merging a contribution the moment it arrives, so its search is over
a handful of entries and its insert shifts almost nothing; appending
holds every duplicate until the end, and the sort of those larger
columns (with `std::stable_sort`'s buffer per column) costs what the
searches saved. The 34 s of `build_solve_direction` are the walk over
the nets and the arithmetic of the bound-to-bound model, not the
container. Making that cheaper is a different unit, in upstream's
algorithm and not in its data structure, and it is not started.

### 2026-09-21: Unit 16.5: bindings on the wire records kept; a flat wire index in router2 measured negative

The sixth hot-path unit (design 16.5), in two steps measured apart.

**Step A, kept.** `WireInfo` carries the wire's bound net and the source
of the pip that drives it, and the arch answers the binding API from
that record: `bindWire`, `unbindWire`, `bindPip`, `unbindPip`,
`getBoundWireNet`, `getBoundPipNet`, the two conflict queries,
`checkWireAvail`, `checkPipAvail`, `checkPipAvailForNet`, each the base
arch's function line for line with its two hash maps replaced, and the
existing side effects (the routing epoch, the placement revision) where
they were. `checkPipAvailForNet`, which router2 asks for every pip it
considers, fetches the destination wire's record once for the blocked
and reserved-route test and the binding test, where it fetched the
record and then looked the pip up in `base_pip2net`. The base maps are
not written. ecp5 and machxo2 already replace them the same way;
nothing outside `BaseArch` reads them; checkpoints and route reuse bind
through the API; router2's worker threads only read availability and
binding is serial. About 44 MB more for the 2.74 million wire records.

**Step B, not kept.** router2's `dict<WireId, int> wire_to_idx` as a flat
open-addressing table, a power of two at most half full, probed from a
multiplicative mix of the wire's hash. Byte-identical and slower: the
mix that avoids collisions scatters neighbouring wires across a 44 MB
table, where the dict's hash, the raw wire id modulo a prime, keeps
wires that are near in the fabric near in memory, and the router visits
wires in neighbourhoods. `router2.cc` is as upstream wrote it.

| Check | Result |
| --- | --- |
| gtest | 67 in `build/rust-enabled` and 54 in `build` (the contract test and the second-register-bel test had sat inside a Rust-only region of the test file and were moved out, so the native build runs them too): a new test of the binding contract over the wire records (a pip bound by itself, its wire bound with it, a rival pip into that wire not thereby bound, unbinding the wire unbinding its pip, a blocked wire refusing), and the checkpoint round-trip and route reuse tests, which restore bindings through the API, unchanged |
| Probe, full flow with `--alm-pairing 1 --register-packing` | Checksums `0x8d5fc97c` / `0x956e97b5`, report `5c670e0139b66090…`: unchanged, for step A and for both steps |
| Default-path probe identity (the gate) | `0xbb18ede9` / `0xbc1365c6`, report hash unchanged |
| Core, full flow, recipe, seed 1, every variant | Placement `0xe0b15557`, routing `0x681553a4`, 45 iterations, 767,087 wires, 11.39 MHz: unchanged |
| Core router2 time, binaries run side by side | First pair: 92.2 s with step A against 103.1 s before. Three together: 99.2 s before, 90.8 s with step A, 99.8 s with steps A and B |

Step A is about 10 s of router2, 10%, measured twice under equal
conditions; it also makes every bind, unbind, and availability test in
the other phases one lookup cheaper, which was not measured apart.

### 2026-09-21: The six hot-path units closed: the core flow's profile before and after

A second Time Profiler recording of the full core flow under the recipe,
with the tree at `a1ac20e9`, against the one the six units were designed
from (`e0e1cdac`). Routed result unchanged: placement `0xe0b15557`,
routing `0x681553a4`. Artefacts in `build/stage6-fullcore/profile/`
(`after_profile.txt`, `after_legaliser_inclusive.txt`; `after.trace`
moved on 2026-09-22 to the Envoy drive, `archive/nextpnr/stage6-fullcore-profile/`, as `.trace.tar.zst` with a README and `SHA256SUMS`).

| Phase | Before | After | Change |
| --- | ---: | ---: | ---: |
| HeAP strict legalisation | 389.4 s | 279.7 s | -109.7 s |
| router2 | 143.3 s | 120.0 s | -23.3 s |
| Annealer refinement | 82.1 s | 79.8 s | -2.3 s |
| HeAP solver | 74.8 s | 71.6 s | -3.2 s |
| HeAP cut spreading and other | 20.1 s | 18.8 s | -1.3 s |
| Loads, pack, the rest | 5.7 s | 5.4 s | - |
| Whole run | 715.4 s | 575.2 s | -140.2 s, 19.6% |

| Unit | Outcome | What the profile shows of it |
| --- | --- | --- |
| 16.4 control rules in scans | Negative: the first form wrong and caught by the core's checksum, the sound form no gain; the hostile scan oracle kept | `control_legal` still 50.7 s inside scans |
| 16.6 scan bookkeeping | Kept, 12 s | `evaluate_scan` self 38.1 s to 17.1 s |
| 16.2 scan loop flags | Kept, 7 s | `checkBelAvail` 35.0 s to 15.5 s |
| 16.1 clusters through the resident session | Kept, 88 s | `try_place_cluster` 90.9 s to 18.0 s |
| 16.3 equation system | Negative: bit-identical, no gain | `build_solve_direction` 33.8 s to 31.7 s, which is noise |
| 16.5 bindings on the wire | The arch step kept, about 10 s of router2; the flat wire index slower, dropped | `dict<PipId, NetInfo *>` lookups gone from router2's top entries; `dict<WireId, int>::at` still 14.1 s |

Three kept, three negative, and the design's sum of estimates (200 s)
against 140 s measured. The estimates that held were inclusive times of
functions that stopped running (16.1, 16.5's first step); those that
did not were shares of a loop's self time (16.2) or a reading of what a
hot function was doing (16.3, 16.4).

What the profile names next, none of it started:

- The tile scan is still 152 s, and it is now the rules themselves:
  the ALM rule and the input total 59.6 s (`check_alm` 37.5 s), the
  control rules 50.7 s (`rules::evaluate` 39.8 s, the mirror's trial
  rows 14.8 s). Unit 16.4 showed the control rules cannot be asked less
  often; they can be made cheaper to ask, for instance by evaluating the
  mirror incrementally instead of walking forty registers per bel.
- `try_place_cell`'s own loop, 53.5 s of self time: forty bels per tile
  visit, 167 million visits.
- router2: the priority queue 33.9 s, `route_arc` 18.1 s, its wire
  index 14.1 s, where a replacement must keep the locality upstream's
  hash has.
- The solver's system building, 48 s, which unit 16.3 showed is the walk
  and the arithmetic and not the container.
- The annealer, 80 s with nothing above 1.5% of the run.

### 2026-09-21: CPU Counters run of the full core flow: where the pipeline stalls

The Time Profiler runs say where the time goes and not why. This run
records the core flow (recipe, seed 1, tree at `a1ac20e9`) under
Instruments' CPU Counters template, which on Apple silicon reports per
millisecond the share of the core's pipeline capacity that retired work
(useful), that the back end could not take (processing: execution limits
and, in pointer-chasing code, above all waiting for data), that the
front end could not deliver (instruction fetch, branch targets), and
that was discarded after a mispredicted branch. The same trace carries a
1 ms time profile; `build/stage6-fullcore/profile/bottleneck.py` joins
the two by millisecond (`record_counters.sh` beside it records, exports,
and joins; report in `counters_bottlenecks.txt`; the recording,
`counters.trace`, moved on 2026-09-22 to the Envoy drive, `archive/nextpnr/stage6-fullcore-profile/`, as `.trace.tar.zst` with a README and `SHA256SUMS`). Result unchanged:
`0xe0b15557` / `0x681553a4`. M1 Ultra: 128-byte lines, 128 KB of L1D,
12 MB of L2 per four cores, 16 KB pages. The verify run of the entry
above shared the machine on another core.

| Phase | CPU | Useful | Back end | Front end | Discarded |
| --- | ---: | ---: | ---: | ---: | ---: |
| HeAP strict legalisation | 288.0 s | 55.6% | 13.3% (38 s) | 15.8% (46 s) | 15.3% (44 s) |
| router2 | 117.8 s | 23.0% | 57.2% (67 s) | 1.6% | 18.2% (21 s) |
| Annealer refinement | 77.1 s | 20.6% | 56.9% (44 s) | 5.3% | 17.2% (13 s) |
| HeAP solver | 72.9 s | 39.5% | 31.0% (23 s) | 5.4% | 24.1% (18 s) |
| HeAP cut spreading | 10.8 s | 20.3% | 37.5% | 9.8% | 32.4% |
| Whole run | 580.5 s | 40.9% | 31.7% (184 s) | 9.9% (57 s) | 17.5% (102 s) |

What it resolves and what it does not: a millisecond holds thousands of
iterations of a tight loop, so every function of one loop gets the
loop's shares (the legaliser's functions all read 54 to 56% useful, the
router's all 23 to 25%). Phases, and stretches longer than a
millisecond, are resolved: inside the solver, system building is 32%
useful with 28% discarded while the coefficient insert is 57% useful,
and the timing analyser's walks inside routing are 82% back end. The
back-end share does not separate a cache miss from a busy execution
port, so it bounds memory stalls from above and is not a measurement of
cache usage: the legaliser's 38 s is a ceiling, and a finding; the
router's and the annealer's 111 s is a ceiling, and a hypothesis until
the miss events are counted. That needs a counters template in manual
mode (L1D, L2, and TLB miss events with event-triggered samples), which
has to be saved once from the Instruments window and cannot be made
from the command line; nothing below is certified as memory bound. The
table sizes quoted below are computed from the declarations, not
measured. (Both were done the next day: the miss events counted per
function, and the sizes measured, in "Cache and TLB misses of the core
flow, counted per function".)

Findings:

- Strict legalisation, half the run, is not waiting for data: 13% back
  end. One LAB of the resident session is about 6 KB (3.8 KB of facts,
  2.2 KB of control mirror) and a scan evaluates some thirty bels
  against it, so the first touch is paid once per scan and the
  neighbourhood in play fits in L2. A better layout there has a ceiling
  of 38 s and a realistic yield of a few. Its losses are the front end
  (46 s: many small functions and an FFI crossing per scan) and
  mispredicted branches (44 s: the rules are chains of data-dependent
  early exits). The levers that fit are rules evaluated as masks and
  tables instead of branches, and fewer calls per bel; not data layout.
- router2 and the annealer are waiting for data: 57% back end each, 111 s
  between them, and a fifth to a quarter useful. The router touches, per
  neighbour it looks at, the arch's wire record through a hash lookup
  (`dict<WireId, WireInfo>`: 2.74 million entries of about 120 bytes,
  330 MB, each with three heap vectors behind it), its own
  `dict<WireId, int>` (33 MB of entries and a bucket table beside
  them), `flat_wires` (52-byte records, about 140 MB), and the net's own
  wire dict: five dependent reads across three tables, all far past L2
  and past what a second-level TLB of a few thousand 16 KB pages maps.
  Sizes are from the declarations, not measured. Its priority queue is a
  binary heap of 20-byte entries, 33.5 s of self time.
- The solver is mixed: 23 s back end and 18 s discarded. System building
  reads a cell's position through `cell_locs.at(cell->name)`, a hash
  lookup six to eight times per port per net, which is what unit 16.3
  found when the container turned out not to be the cost.

Candidates, by the measured ceiling of their phase, none started and
none designed:

| Candidate | Phase, stalled | Shape |
| --- | --- | --- |
| The router's per-wire state in one compact record per wire, in creation order (flags, bound net, bound source, congestion, the visit fields), and the adjacency as one flat array with offsets | router2, 67 s | The arch's, plus an index the router can share; unit 16.5's second step showed the index must keep the fabric's locality |
| A four-way heap for the router's queue: four 20-byte children on one 128-byte line and half the depth | router2, part of the 33.5 s | Upstream's file; pops in the same order wherever scores do not tie exactly, which the routed checksum decides |
| The annealer's per-move reads: `dict<IdString, ArchPinInfo>` by port name, the timing analyser's `dict<CellPortKey, PerPort>`, net bounds through `CellInfo` records spread over the heap | Annealer, 44 s | Dense per-cell and per-port arrays beside the dicts; bit-identical by construction |
| Cell positions in a dense array indexed per cell for the solver's system building | Solver, 23 s | Upstream's file; the same arithmetic in the same order |
| A free-bel mask per tile kept on bind and unbind, read once per tile visit instead of sixty 96-byte bel records for one pointer each | Strict legalisation, a share of 38 s | Small, and the measurement says the phase is not memory bound, so expect little |

### 2026-09-21: The refined scan and the units of design 16 certified on the core in verify mode

The follow-up the plan left pending: one verify-mode placement of the
core for everything that changed how the Rust session answers since the
first scan build. The core's placement under the recipe's options with
`--lab-legality verify`, tree at `fa67bc80` (the refined scan, unit 16.6's
scan bookkeeping, unit 16.2's scan loop flags, unit 16.1's cluster
candidates; unit 16.4 is not in it, having been reverted), binary and
artefacts in `build/stage6-fullcore/verify-units-16/`:

| Check | Result |
| --- | --- |
| Placement | Checksum `0x2d44a02e`, the Rust authority's and the legacy authority's; HeAP 13,193 s, strict legalisation 13,136 s, the harness's price |
| Per-bel evaluations | 5,348,124,234, of which 23,986,231 legal (the baseline's count), each compared across the resident session, the capture path in C++ and in Rust, and the live check: `mismatches=0 errors=0 stale-cache=0 stale-revision=0` |
| Advisory scans | 167,516,520 over 5,289,619,878 bels, 661,994 naming a bel, nothing skipped, every prediction compared with the live answer of the same bel, a difference fatal: the run finished normally |
| Cluster candidates | 19,561,195 attempted, 668,514 committed, 18,892,681 rejected, none unsupported: the baseline's counters exactly. 19,528,999 of them answered by the resident session over 56,087,279 edited bels and each compared with the frozen evaluation, which decides in this mode; a difference fatal |
| Pack admission in verify | 15,181 pairs and 5,360 registers, `refused=0 mismatches=0 errors=0` |

The refined scan's own run (`verify-scan-refined/`) was stopped after 31
of 105 iterations with no mismatch when this run superseded it. Unit
16.5's arch step landed after this binary; it moves the storage of wire
and pip bindings and touches no legality path, and the gate's routed
identity covers it.

### 2026-09-22: Cache and TLB misses of the core flow, counted per function

The follow-up the CPU Counters entry left open: its back-end shares bound
memory stalls from above and did not count a miss. This entry counts
them. Instruments' CPU Counters template in manual mode, generated from
the command line by `build/stage6-fullcore/profile/make_counters_template.py`
(the instrument's configuration is a JSON document under `optionsEncoded`
in the template's keyed archive; manual mode sets `configurationType`,
`sampleByTime`, `pmiEventAliasOrMnemonic`, `pmiThreshold`, and
`allEventsAndFormulas`, each event a base64 keyed archive of an
`XRCountersSetupEventOrFormula`). Three recordings of the core flow
(recipe, seed 1, tree at `0a494709`, `record_misses.sh`), each sampling on
one event, each reading the same eight counters (cycles, instructions,
L1D load misses, L1D store misses, L1D TLB misses, L2 TLB data misses,
MMU table walks for data, load-unit uops):

| Trigger | Every | Samples of the process | Result |
| --- | ---: | ---: | --- |
| L1D load misses | 250,000 | 296,553 | `0xe0b15557` / `0x681553a4`, 45 iterations, 767,087 wires |
| L2 TLB data misses | 4,000 | 197,338 | the same |
| Cycles | 6,000,000 | 288,167 | the same |

A sample lands in a function in proportion to its trigger's count there,
so samples times the interval estimates each function's count of that
event; the three runs are byte-identical, so their functions join.
`misses.py` joins them (report `misses_report.txt`). Per-phase totals are
the counter deltas of the cycles run; per-function counts are the
estimates, which exceed the summed deltas by 8 to 10% on every run
(sampling skid), so rates per function carry that error in both numerator
and denominator.

What the counters cannot say: this CPU's event database
(`/usr/share/kpep`, 60 events) has no L2-cache-miss or memory event. The
deepest counts are L1D misses and TLB misses. The cost of a miss is
therefore calibrated separately: a dependent-load chase over a random
cyclic permutation of 128-byte lines (`scratchpad` `chase.c`, idle
machine, load average 3.5 from other sessions) measured

| Working set | 128 KB | 256 KB to 1 MB | 4 MB | 8 MB | 12 to 16 MB | 32 MB | 64 MB to 1 GB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| ns per load | 1.0 | 5.7 | 7.4 | 21 | 52 to 63 | 96 | 126 to 146 |

An L2 TLB miss means the page lies beyond what the TLBs map, where the
chase pays 126 to 146 ns. Counting every L2 TLB miss as one serialized
trip at 140 ns gives an upper bound on a phase's far-memory waiting; the
real figure is lower by whatever the core overlaps. L1D misses that stay
near cost 5.7 ns serialized and are mostly overlapped (the legaliser
retires 4.3 instructions a cycle with 30 billion of them).

| Phase | CPU | IPC | L1D load misses | per 1k instructions | of loads | L1D TLB MPKI | L2 TLB misses | Far memory, at most |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Strict legalisation | 288 s | 4.34 | 30.1 G | 8.95 | 3.5% | 0.54 | 0.14 G | 19 s, 7% |
| router2 | 118 s | 1.66 | 11.8 G | 21.7 | 6.1% | 8.66 | 0.23 G | 32 s, 28% |
| Annealer | 77 s | 1.54 | 9.4 G | 28.8 | 8.2% | 13.5 | 0.25 G | 35 s, 46% |
| HeAP solver | 73 s | 3.24 | 12.8 G | 20.6 | 5.9% | 1.27 | 0.02 G | 3 s, 4% |
| Whole run | 580 s | 3.17 | 66.7 G | 13.4 | 4.7% | 2.49 | 0.70 G | 98 s, 17% |

(CPU seconds are the CPU Counters run's; misses and IPC are the cycles
run's deltas.)

Findings, replacing the hypothesis of the entry before:

- The legaliser takes 44% of the run's L1D load misses and pays little
  for them. `check_alm` alone is 13% of the run's misses (86 per 1,000
  cycles), the tile scan inclusive 28%, `try_place_cell` 7.5%, the bel
  records read by `checkBelAvail` 4.7%; yet the phase misses the L2 TLB
  0.04 times per 1,000 instructions, a tenth of the router's rate, and
  retires 4.3 instructions a cycle. The facts are streamed from nearby
  caches and the misses overlap. Its largest TLB-miss sites are not the
  rules but `Arch::update_alm_input_count` (50 M) and
  `next_random_location` (27 M). A layout change to the resident session
  would cut misses, not time.
- router2 is at most 28% far-memory bound. Its L2 TLB misses: the
  priority queue's own heap array 59 M (the largest single site in the
  run: the queue grows large enough that each pop walks pages),
  `route_arc` itself 39 M (the per-wire state reads are inlined there), the wire index `dict<WireId, int>`
  35 M, `score_wire_for_arc` 22 M, the arch's wire record in
  `is_pip_blocked` 15 M, the timing analyser's port dict 9 M. A layout
  of dense per-wire state reached by index, not hash, and a queue with
  fewer levels are what this points at, within 32 s.
- The annealer is the most far-memory-bound phase per cycle: at most 46%,
  with 1,175 L2 TLB misses per million cycles. Its sites are cell and
  net records reached through pointers: `random_bel_for_cell` 44 M,
  `add_move_cell` 30 M, `update_alm_input_count` 25 M, the resident
  session's batch capture (`build_batch`, `capture_cell_v2_keyed`) 24 M,
  `memcmp` 13 M, `compute_cost_changes` 9 M, `is_alm_legal_overlay`
  8 M, `dict<IdString, ArchPinInfo>` 6 M. `CellInfo` is 448 bytes and
  `NetInfo` 224 (measured), each on its own heap allocation.
- The solver's misses are streams: 12.8 G L1D misses and 14.8 store
  misses per 1,000 instructions, from the coefficient inserts (5.4 G) and
  Eigen's sparse products (3.6 G), with almost no TLB misses and 3.2
  instructions a cycle. Unit 16.3 already showed the insert path is not
  the cost.

Sizes, measured with the build's flags: `WireInfo` 104 bytes (116 as a
dict entry, about 320 MB for the device), `BelInfo` 96 (not 88 as the
entry before said), `dict<WireId, int>` entry 12, `CellInfo` 448,
`NetInfo` 224. Artefacts: `misses_*.xml`, `misses_*.log`,
`misses_report.txt` in `build/stage6-fullcore/profile/`; the recordings,
`misses_*.trace` (18 GB together), moved the same day to the Envoy drive, `archive/nextpnr/stage6-fullcore-profile/`, as `.trace.tar.zst` with a README and `SHA256SUMS`.

### 2026-09-22: Design 17 landed: router2 reaches wires by slot, not by hash

Units 17.1 (first step), 17.2, and 17.3 of design section 17, measured
from the core's route-prepared checkpoint
(`build/stage6-fullcore/ckpt/core_prepared.json`, written by the
baseline binary under the recipe's options; a resumed router reproduces
the full flow's routing checksum `0x681553a4`, 45 iterations, 767,087
wires). `resume.sh` beside it runs one binary from it. Every run below
ends at that checksum, and the routed JSON (without its `creator` line)
and the report are byte-identical to the baseline's.

| Binary | router2, resumed | Runs |
| --- | ---: | --- |
| Baseline (`7d5bfb64`, before the units) | 89.8 s | 89.4, 89.7, 90.0, 92.4 |
| 17.1 first step (`dcb9b87f`): the queues keep their storage | 88.7 s | 88.3, 88.7, 91.3 |
| + 17.2 (`1e3ad68e`): wire slots in the arch | 78.7 s | 75.3, 76.4, 81.0, 83.2 (and 96.5, see below) |
| + 17.3 (`5686a671`): router2's index through the slot | 67.6 s | 66.0, 67.5, 67.6, 70.7 |

(Medians; the machine carried other work during these runs, load
average 4 to 11 from two Verilator simulations and a stuck system
service; one 17.2 run at 96.5 s under a load of 11.5 is an outlier and
is shown, not used.)

- 17.1, first step: the two search queues are cleared between arcs
  instead of being replaced; about 1 s. The second step, a four-way
  heap, was built and moves the route (routing checksum `0x823226a7`, 54
  iterations): the router's seed entries all carry the random tag 0, and
  seeds at one location tie on score, so the pop order among them
  depends on the heap's shape. Not kept, as the design's identity rule
  says.
- 17.2: the device's 2,739,969 wires numbered into 2,809,423 slots
  (the table spans 228,780 type-and-tile groups, 85,108 of them
  occupied); `WireRoute` (16 bytes: bound net, bound source, flags) by
  slot, `WireInfo` down from 104 to 80 bytes. The flags left `WireInfo`;
  the checkpoint writer and restore, the Stage 1 control edits, and 31
  test sites use `wire_flags` / `set_wire_flags`.
- 17.3: `Router2Cfg::wire_slot` and `wire_slot_count`, set by the
  Mistral arch; router2 keeps a vector from slot to its own index and
  fills its dictionary only for arches without slots.
  `score_wire_for_arc` takes the index `route_arc` already has.

Together 22 s, a quarter, of router2 on the core. The gtest suites (67 and 54),
the gate's routed probe identity, and the checkpoint and control-edit
tests pass.

### 2026-09-22: Design 18: chain moves through the swap seam; the cached timing weight measured and dropped

**18.2, the timing weight computed once per timing update: negative.**
Built bit-identical (core placement `0x2d44a02e`, the seam's counters
equal), and the annealer took 79.8 s against 78.4 s, with HeAP, whose
code did not change, moving 5 s between the same two runs: no gain
inside the noise. The 11.4 G cycles the cycles recording credited to
`powf` were most likely skid: a counter's interrupt lands a few
instructions after the event that fires it, and `powf` follows
`predict_arc_delay`'s record reads, which is where the annealer waits.
Reverted. The rule for reading event-sampled profiles gains this: a
cheap leaf right after a stalling load inherits the load's samples.

**18.1, chain moves through the swap seam (`e6dc9299`): kept.** `try_swap_chain`
plans the live walk on an occupancy overlay (`plan_chain`), the arch
assesses the planned move certifying only the moved cells' new bels
(`Placer1SwapEdit::certify`, `overlay_bels_legal` over a list of bels;
`BelOverlay::MAX` 4 to 16), the cost delta comes from the position
overlay in the live code's `moved_cells` order (hashlib's dict iterates
newest first), and a refused move binds nothing but leaves every cell
the live revert would have moved with `STRENGTH_WEAK`. A move the seam
cannot assess runs the live path as before.

| Check | Result |
| --- | --- |
| gtest | The seam test's overlay patterns cover all six slots now (the overlay held four before), and a new loop certifies the first ALM's bels over the whole overlay with one clock: 64 patterns agree with binding and asking, and the generator reaches subsets that are legal where the whole is not. 67 and 54 tests pass |
| Shadow, probe, default options | 15,011 chain moves compared, 0 mismatches; 5,665,634 single swaps, 0 |
| Shadow, probe, recipe options | 644,483 chain moves, 0 mismatches (Rust authority); the same under `--lab-legality legacy`, 0 |
| Shadow, core | 7,024,566 chain moves and 6,933,122 single swaps compared, 0 mismatches (placement `0x2d44a02e`) |
| Identity with the seam on | Probe `0x7f9f8105` and `0xa99e0f68`; core placement `0x2d44a02e`; core full flow placement `0xe0b15557`, routing `0x681553a4`, 45 iterations, 767,087 wires, 11.39 MHz, routed JSON and report byte-identical to the baseline's |
| Chain counters, core | planned 7,024,566, failed in the walk 46,349, assessed 6,978,217, illegal 2,998,572, rejected 3,928,426, committed 51,219, unsupported 210,943 (moves longer than 16 bels, which run live) |
| Core annealer | 69.0 s placement-only and 71.0 s in the full flow, against 78.4, 79.8, and 81.5 s before (the last two runs of unchanged annealer code): about 10 s. Router2 in the same full flow 62.9 s with design 17 |

The probe's annealer runs 5 to 10 s and its repeats spread by more than
a second, so its times are not a measurement; the core's are.

### 2026-09-23: The quality baseline over five core seeds (design 19)

The reference every unit of design 19 is judged against: the core
recipe (`mistral/tests/quality.py`, configuration `core`) at `faf70aa0`,
seeds 1 to 5, four at a time, with seed 1 repeated.

| Seed | Routed | Fmax | Iterations | Wires | ALMs | LABs | Rows per net | LABs per net |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | yes | 11.39 MHz | 45 | 767,087 | 28,652 | 4,189 | 1.917 | 1.608 |
| 2 | yes | 10.94 MHz | 51 | 778,792 | 28,716 | 4,183 | 1.893 | 1.618 |
| 3 | yes | 11.87 MHz | 64 | 770,569 | 28,780 | 4,187 | 1.908 | 1.609 |
| 4 | **no** | - | cap of 100 with 1 wire overused | 785,042 | | | | |
| 5 | yes | 10.92 MHz | 46 | 774,185 | 28,677 | 4,184 | 1.907 | 1.612 |

Median Fmax of the routed seeds 11.17 MHz, range 10.92 to 11.87 (spread
0.95). Seed 1 repeated is identical to the byte, and seeds 1 to 3
reproduce the earlier records exactly (11.39, 10.94, 11.87 MHz at 45, 51,
64 iterations). Shape measures are nets of up to 64 sinks with every pin
in a LAB, without the nets that route-through buffers (`MISTRAL_BUF`,
inserted after placement in a register's own ALM) drive: each splits a
fabric net at its register and would add a net of one row and no entry
that the netlist Quartus fits does not have (the first count included
them: 1.80 rows and 1.40 entries per net).

Seed 4 does not route under the recipe: router2 reaches its cap of 100
iterations one wire short and hands over to router1, which never
finishes on the core; the run was stopped after 80 minutes. The rule's
"every seed routes" is therefore a bar the baseline itself misses, so a
candidate must do better than the baseline there. The harness now stops
such a run (`--timeout`, 30 minutes by default) and records it as not
routed. The delay-cost reference (`--drop=--router2-unit-cost`) was not
run: at the recipe's cap of 100 it would not converge (it needed 125
iterations under a cap of 200, 2026-09-18), so it belongs to 19.3's
design with its cap.

### 2026-09-23: The core against Quartus, measured the same way

Quartus 17.0.2's fit of the identical core netlist (WYSIWYG hand-over,
`core_probe_20260917/q33` on NAS01, production settings, 33 MHz
constraint) set beside the design 19 baseline. Quartus's placement was
extracted with the exec probe's TimeQuest script (`dump_loc.tcl`, run
against the fitted database; 51,297 cell locations) and measured with
the harness's definitions by `build/quality/quartus-core/shape.py`
(nets of up to 64 sinks with every pin in a LAB); its LAB count matches
the fit report exactly (3,219). Wires by type: the fit report's
"Fitter Resource Usage Summary" against nextpnr's routed JSON (seed 1),
matched by the graph's own counts (V2/C2 117,357 against 119,108, the
LAB input lines TD against block 285,892 against 289,320, local LD
84,580 against 84,580).

| Measure | nextpnr (baseline, median of routed seeds) | Quartus 17.0.2 | nextpnr / Quartus |
| --- | ---: | ---: | ---: |
| Fmax (each tool's own model) | 11.17 MHz (10.92 to 11.87; seed 4 does not route) | 25.18 MHz (slow 1100 mV 100 C) | 0.44 |
| ALMs | 28,696 occupied | 23,885 used in final placement; 20,576 needed after dense-packing recovery | 1.20 (used), 1.39 (needed) |
| LABs | 4,186 of 4,191 (100%) | 3,219 (77%) | 1.30 |
| Registers | 15,647 | 14,590 (duplicates merged) | 1.07 |
| Rows a net touches | 1.91 | 1.44 | 1.33 |
| LABs a net's sinks enter besides the driver's | 1.61 | 0.84 | 1.92 |
| Fabric wires (H3/R3, H6/R6, H14/R14, V2/C2, V4/C4, V12/C12), nextpnr seed 1 | 300,517 | 128,832 | 2.33 |
| of which row wires H3, H6, H14 | 194,268 | 87,405 | 2.22 |
| of which column wires V2, V4, V12 | 106,249 | 41,427 | 2.56 |
| LAB input lines (TD / block) | 128,583 | 76,045 | 1.69 |
| Local lines (LD / local, a LAB's outputs back into itself) | 3,462 | 18,515 | 0.19 |
| Placement and routing time | about 8.5 min, one thread, M1 Ultra | fitter 26.6 min (placement 6.2), `--parallel=2`, Ryzen 7 2700 | different machines |

Reading. The gap is placement, as 9.5 argued from the exec probe, and
the core says it more sharply. Quartus puts a net's sinks in the
driver's own LAB so often that its average net enters fewer than one
other LAB, and it reaches those sinks through the LAB's local lines,
the cheapest wire the fabric has (18,515 in use against nextpnr's 3,462).
nextpnr's nets enter 1.9 times as many other LABs, and every entry costs
a fabric wire and a LAB input line, which is where its 2.3 times the
fabric wires and 1.7 times the input lines come from; it also touches a
third more rows, and column wires, the stairs, are its largest excess
(2.6 times). It fills every LAB of the device where Quartus leaves a
quarter empty. The Fmax ratio mixes the timing models (the exec probe's
1.8 times decomposed as rewrite 1.12, timing model 1.22, placement and
routing 1.32; the core's is not decomposed), so the placement shape and
the wires are the comparable measures, and design 19.2's terms price
exactly the two where the difference is largest.

### 2026-09-23: Unit 19.1 negative; unit 19.2 screened

**19.1, router2's four-way heap: negative, removed.** Route-prepared
checkpoints of seeds 1 to 5 written by the baseline binary
(`quality.py prepare`); each seed routed from its checkpoint one run at a
time by the same binary with and without `--router2-quad-heap`. The runs
without it reproduce the full-flow baseline exactly (resumed routing
equals the uninterrupted run).

| Seed | Binary heap: Fmax, iterations, router2 | Four-way heap |
| ---: | --- | --- |
| 1 | 11.39 MHz, 45, 69 s | 11.36 MHz, 54, 71 s |
| 2 | 10.94 MHz, 51, 81 s | not routed (1 overused at 100) |
| 3 | 11.87 MHz, 64, 89 s | 10.77 MHz, 53, 70 s |
| 4 | not routed | not routed |
| 5 | 10.92 MHz, 46, 72 s | 10.73 MHz, 64, 91 s |

Speed rule: REJECT (a seed that routed stops routing, the median of the
routed seeds 11.17 to 10.77 MHz, the worst below the baseline's worst).
About 12% faster per iteration (1.31 to 1.42 s against 1.39 to 1.59 s),
on a machine carrying other work. Reverted; the queues keep their
storage (17.1).

**19.2, rows and LAB entries in the annealer: screened.** Seeds 1 and
2, two at a time; the baseline's seeds 1 and 2 are 11.39 and 10.94 MHz,
767,087 and 778,792 wires, 1.91 and 1.89 rows, 1.61 and 1.62 LABs per
net.

| Weights | Fmax, seeds 1 and 2 | Iterations | Wires | Rows per net | LABs per net |
| --- | --- | --- | --- | ---: | ---: |
| entry 1 | 11.67, 11.15 | 40, 55 | 758,714, 769,129 | 1.92, 1.89 | 1.53, 1.53 |
| entry 2 | 12.06, 10.25 | 46, 36 | 754,299, 765,267 | 1.92, 1.90 | 1.50, 1.50 |
| entry 4 | 11.42, 11.59 | 58, 59 | 755,028, 764,569 | 1.93, 1.91 | 1.47, 1.48 |
| row 2 | 11.79, 11.52 | 52, 70 | 759,310, 774,738 | 1.81, 1.82 | 1.59, 1.61 |
| row 4 | 11.74, 11.80 | 45, 45 | 761,470, 769,081 | 1.79, 1.77 | 1.58, 1.59 |
| row 8 | 11.74, 11.10 | 74, 72 | 758,945, 769,668 | 1.75, 1.74 | 1.58, 1.60 |

Each term moves its own measure and nothing else: the entry weight
brings LABs per net from 1.61 to 1.47, the row weight rows per net from
1.91 to 1.74, and wires fall 1 to 2%. Quartus is at 0.84 and 1.44: a
greedy refinement of HeAP's placement moves the shape by about a sixth
of the distance, as the design said it might. Row 4 and row 4 with entry
4 go to five seeds.

The harness's compare now takes medians over routed seeds and fails a
candidate with an unrouted seed, instead of refusing to compare.

### 2026-09-23: Unit 19.2 on five seeds: every seed routes, Fmax inside the spread

Row weight 4, and row weight 4 with entry weight 4, on the core recipe
over seeds 1 to 5 (`sa-r4-5s`, `sa-r4e4-5s`), against the baseline. Medians
over routed seeds.

| Set | Routed | Fmax median (range) | Worst seed | Wires | Row wires | Column wires | Input lines | Local lines | Rows per net | LABs per net | Cells per LAB |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline | 4 of 5 | 11.17 (10.92 to 11.87) | 10.92 | 772,377 | 197,876 | 106,996 | 128,934 | 3,447 | 1.907 | 1.611 | 13.16 |
| Row 4 | **5 of 5** | 11.74 (10.77 to 11.98) | 10.77 | 764,462 | 193,456 | 103,900 | 128,491 | 3,591 | 1.782 | 1.587 | 13.17 |
| Row 4, entry 4 | **5 of 5** | 11.43 (11.13 to 11.99) | 11.13 | 753,156 | 188,819 | 101,585 | 124,418 | 4,029 | 1.815 | 1.477 | 13.16 |
| Quartus 17 | | 25.18 | | | 87,405 | 41,427 | 76,045 | 18,515 | 1.435 | 0.839 | 15.93 |

Quality rule: REJECT for both (the gain is below the baseline's spread
of 0.95 MHz; row weight 4 alone also puts seed 4 at 10.77, below the
baseline's worst). Both route seed 4, which the baseline does not, so
routability improves; seed 4 routes in 69 and 59 iterations. Decision:
the options stay, off by default, unpromoted: they pay on routability
and every shape and wire measure, and the levers that change how full
HeAP makes each LAB are expected to compose with them.

### 2026-09-23: Density sweep: spreading denser does not fill LABs

HeAP's spreading knobs on top of unit 19.2 (rows 4, entries 4), seeds 1
and 2 (`build/quality/sweep_density.sh`): the cut spreader's target
occupancy `MISTRAL_HEAP_BETA` (default 0.5) and the congestion
inflation's threshold `MISTRAL_SPREAD_CONGESTION_K` (default 2).

| Setting | Routed | Fmax, seeds 1 and 2 | Wires | LABs used | Cells per LAB |
| --- | ---: | --- | --- | --- | ---: |
| beta 0.5, K 2 (the 19.2 set) | 2 | 11.99, 11.13 | 747,590, 760,411 | 4,190, 4,184 | 13.2 |
| beta 0.6 | 2 | 11.27, 10.15 | 753,554, 768,049 | 4,161, 4,096 | 13.3 |
| beta 0.7 | 0 | cap of 100 on both | 763,569, 826,970 | | |
| beta 0.85 | 1 | 10.31 | 772,092, (826,522) | 4,180 | 13.2 |
| beta 0.7, no `--spread-congestion` | 0 | stopped at 40 minutes | | | |
| K 3 | 2 | 11.59, 11.13 | 750,700, 766,067 | 4,188, 4,130 | 13.2 |
| beta 0.7, K 3 | 0 | | | | |

Reading. A denser spreading target does not make LABs fuller: at beta
0.85 cells per LAB stay at 13.2 and 4,180 LABs are used; it only
crowds the routing (more wires, seeds that do not route) and costs
Fmax. How full a LAB gets is set where the legaliser meets the LAB's
limits (input lines, control sets) with the unrelated cells the spreader
brings into it; Quartus fills its LABs to 15.9 cells with cells that
share inputs. That is design 19.6's lever, not the spreader's. The knobs
stay at their defaults.

### 2026-09-23: Unit 19.4 step 1: Quartus writes the second register of a half as nextpnr does

The differential of design 19.4, step 1. `ff2test` is twelve cells in
LAB (85, 27):

- In ALM 0 each half's 4-input LUT drives both registers of its half.
- In ALM 1 each half's 2-input LUT drives the first register, and the
  second register takes a pin through E/F.

nextpnr placed it by BEL attributes, with the refusal lifted by a
test-only override (`MISTRAL_TEST_ALLOW_SECOND_FF`, not committed). It
also needed a second test-only tolerance: HeAP's constraint placer
checks the ALM after every bind, so a register bound before its half's
LUT looks fabric-fed. Quartus 17.0.2 fitted a WYSIWYG twin with location
assignments (NAS01 `fabi386_jobs/ff2test_20260923`). It honoured every
assignment and reported no warning on them. Both bitstreams were decoded
with libmistral (`mistral-cv decomp`), and the LAB's settings were
compared (`build/stage6-fullcore/ff2test/{q,np}.lab`).

| Setting, ALMs 0 and 1 | Quartus | nextpnr |
| --- | --- | --- |
| `BMODE`/`TMODE`, `CLK_SEL`, `ACLR0_SEL`, `EN0_EN` | as nextpnr | as Quartus |
| `PKREG0.1` (both halves) | 1 | 1 |
| `SCLR_DIS.1` (both halves) | 1 | 1 |
| `SCLR_DIS.0` (both halves; no register uses the sync clear) | not set | 1 |
| `EF_SEL.1` (second register of ALM 1 from the fabric) | not set (E) | F |
| `LUT_MASK.0`, `.1` | differ | differ |
| Register outputs used | `FF[TB]0`, `FF[TB]1L` | `FF[TB]0`, `FF[TB]1`, `FFB1L` |

Every register setting agrees. The differences are choices, not model
gaps:

- the E or F line for the fabric-fed register;
- the LUT masks, which follow each tool's input permutation;
- the sync-clear disable on an ALM with no sync clear;
- which of the second register's two outputs is used. `FF*1` goes to
  the fabric and `FF*1L` to the local line. Quartus used the local one
  because it packed the output XOR into the same LAB (ALMs 2 and 3).

No bit is missing from the model for either pattern. Step 2, the
silicon test, is built and waits for the board. The board did not
answer at `192.168.25.19`, and its MAC is on neither subnet.

- **Test design.** `build/stage6-fullcore/ff2silicon/gen.py`, seed 194,
  in LABs x 16 to 17, y 20 to 27, six ALMs per LAB. Three ALMs of the
  first pattern and three of the second, 192 second registers in all.
  Every LUT and E/F input is a state bit or an LFSR bit. A 32-bit MISR
  folds the 384-bit state for 20,000 cycles.
- **Golden.** `e73a4c8d`. The Python model and an iverilog run of the
  generated Verilog on Yosys's primitive models agree (they agreed on
  the first draft's `cd9c43bf` as well). Sticking any one of three
  sampled second registers at 0 moves the signature.
- **nextpnr.** Every one of the 576 constrained cells is on its bel.
  34 more second-register bels are taken by free cells under the rule
  with only the refusal lifted. The test tolerance is limited to
  BEL-constrained cells whose LUT is unbound. Fmax is 164 MHz against the
  board's 50 MHz clock.
- **Control.** `ff2sil_ctl.rbf` is the same netlist with the golden
  off by one bit and build id F5. It must read `match=0` with the same
  `sig_lo`.
- **Run.** `build/stage6-fullcore/ff2silicon/run_board.sh`, through
  openflow-test's `run_hybrid.sh`.

### 2026-09-23: Unit 19.6 screened: LAB affinity shortens routing, cells per LAB unchanged

LAB affinity (commit c240bdff) on top of unit 19.2 (rows 4, entries 4),
seeds 1 and 2 (`build/quality/sweep_affinity.sh`, binary
`nextpnr-mistral.u19-6`). The reach is in weighted tiles: at a row
weight of 4 (read as 2 by `hpwl_scale_y`, an `int`), a reach of 2.5
covers two columns and one row, and a reach of 5 covers five columns and
two rows. The design's 1 and 2 would have admitted no other row, so the
screening used 2.5 and 5.

| Weight, reach | Routed | Fmax | Iterations | Wires | LABs used | Entries per net | Route s |
| --- | ---: | --- | --- | --- | --- | --- | --- |
| 19.2 set, no affinity | 2 | 11.99, 11.13 | 29, 79 | 747,590, 760,411 | 4,190, 4,184 | 1.48 | 52, 96 |
| 1, 2.5 | 2 | 11.69, 11.70 | 53, 66 | 746,779, 753,512 | 4,183, 4,180 | 1.45 | 86, 129 |
| 2, 2.5 | 2 | 11.92, 11.42 | 32, 50 | 747,484, 748,233 | 4,168, 4,176 | 1.45 | 61, 89 |
| 4, 2.5 | 2 | 11.99, 11.66 | 63, 44 | 751,864, 753,892 | 4,183, 4,164 | 1.45 | 151, 87 |
| 1, 5 | 2 | 11.57, 11.58 | 45, 37 | 743,853, 745,219 | 4,167, 4,179 | 1.43 | 86, 68 |
| 2, 5 | 2 | 11.57, 12.26 | 27, 28 | 737,553, 743,925 | 4,174, 4,167 | 1.41 | 61, 62 |
| 4, 5 | 1 | 11.57, cap of 100 | 34, 100 | 742,204, 748,059 | 4,149 | 1.41 | 101 |

Reading:

- Affinity pulls nets together. LAB entries per net fall from 1.48 to
  1.41, wires by 1 to 2%, and at weight 2, reach 5 the router needs 27
  and 28 iterations, against 29 and 79 without affinity and 45 and 51
  on the baseline.
- It does not fill LABs. Cells per LAB stay at 13.2 and LABs used fall
  by 20 at most. A cell pulled into a neighbour's LAB leaves room behind
  it that another cell takes.
- Weight 4 lost seed 2.
- Weight 2, reach 5 goes to five seeds.

### 2026-09-23: Unit 19.6 on five seeds: every seed routes, the gain inside the spread

Weight 2, reach 5, on top of unit 19.2 (rows 4, entries 4), seeds 1 to 5
three at a time, seed 1 repeated (`build/quality/run_19_6.sh`, tag
`a-w2-r5-5s`).

| Seed | Fmax | Iterations | Wires | LABs | Rows per net | Entries per net | Place s | Route s |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 11.57 | 27 | 737,553 | 4,174 | 1.812 | 1.404 | 277 | 60 |
| 2 | 12.26 | 28 | 743,925 | 4,167 | 1.819 | 1.413 | 308 | 58 |
| 3 | 11.75 | 52 | 734,123 | 4,178 | 1.795 | 1.394 | 223 | 74 |
| 4 | 11.52 | 42 | 751,786 | 4,159 | 1.813 | 1.399 | 208 | 79 |
| 5 | 11.98 | 52 | 731,958 | 4,163 | 1.805 | 1.408 | 366 | 58 |

Median 11.75 MHz, range 11.52 to 12.26. Every seed routes, including
seed 4, which the baseline does not. Seed 1 repeated is identical.

Against the baseline:
- **Quality rule: REJECT.** The median gains 0.58 MHz, below the spread
  of 0.95; the floor holds (worst seed 11.52 against 10.92).
- **Speed rule: ACCEPT on its criteria.** The median is inside the
  baseline's range and the median wall time falls from 505 to 337 s.
  The baseline ran four at a time and this set three at a time, so the
  times are an orientation, not the serial measurement a speed
  acceptance needs.

Against unit 19.2 alone (median 11.43): +0.31, inside that set's spread
of 0.86. Fabric wires fall about 4.5% against the baseline (medians of the routed seeds 772,377 and 737,553), LAB entries
per net from 1.61 to 1.40, local lines from 3,446 to 4,360. Cells per
LAB stay at 13.2.

The harness now names only the failed criteria in a verdict; it
listed every criterion before.

### 2026-09-23: Unit 19.3 negative in its first form: no seed routes

`--router2-crit-cost` (commit 7131d306), resumed from the baseline's
route-prepared checkpoints (`build/quality/ckpt/core-base`), seeds 1 to 5
three at a time (`build/quality/run_19_3.sh`, tag `crit-5s`).

| Seed | Routed | Iterations | Wires | Overused at the cap |
| ---: | --- | ---: | ---: | ---: |
| 1 | no | cap of 100 | 870,311 | 264 |
| 2 | no | cap of 100 | 881,911 | 452 |
| 3 | no | cap of 100 | 876,659 | 46 |
| 4 | no | cap of 100 | 897,352 | 1,244 |
| 5 | no | cap of 100 | 875,218 | 230 |

router1's fallback was stopped by the harness at 40 minutes on every
seed. Wires rise 13 to 16% over the baseline's routed seeds (767,087 to
778,792). Blending by `1 - crit²` hands too many arcs to the delay cost.
They crowd the fast wires as the full delay cost did (842,300 wires,
125 iterations under a cap of 200, 2026-09-18). The option stays opt-in
and unpromoted. The design's next settings are a steeper blend or a
criticality threshold that leaves all but the most critical arcs on the
unit cost.

### 2026-09-23: Where the critical path's time goes: detours as large as distance

The baseline's routed seeds (1, 2, 3, 5), re-routed from their
checkpoints to identical results, with every arc's routing delay dumped
(`MISTRAL_DUMP_ARC_DELAYS`, about 217,000 arcs per seed; the timing
analyser's own delay). The reference for an arc is the delay this router
achieves for arcs of the same span over the four designs: the 10th
percentile of that span's arcs, widening the span until 30 arcs are
found (`build/quality/crit_split.py`, `arcs_s*.csv`).

| Span (columns, rows) | Arcs | 10th percentile | Median |
| --- | ---: | ---: | ---: |
| (0, 0) | 121,666 | 0.02 ns | 0.02 ns |
| (1, 0) | 36,588 | 0.33 | 0.55 |
| (0, 1) | 32,492 | 0.79 | 0.90 |
| (5, 0) | 6,725 | 0.60 | 0.62 |
| (0, 5) | 2,728 | 1.19 | 1.57 |
| (10, 0) | 1,851 | 0.88 | 1.15 |
| (0, 10) | 670 | 1.39 | 2.12 |
| (20, 0) | 546 | 1.39 | 2.08 |
| (30, 5) | 244 | 1.96 | 3.25 |

Each seed's critical path, split:

| Seed | Path | Distance | Detour | Logic | Arcs |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 87.8 ns | 39.4 | 35.4 | 12.5 | 53 |
| 2 | 91.4 | 38.3 | 40.5 | 12.1 | 69 |
| 3 | 84.3 | 40.0 | 30.4 | 13.3 | 56 |
| 5 | 91.6 | 30.4 | 49.1 | 11.6 | 50 |

Over the four: detour 44%, distance 42%, logic 14%.

- **The median reference.** Against the median delay of the span
  instead, the detour is still 31% of the paths' routing delay (94 of
  303 ns).
- **Fanout is not the cause.** At a span of 10 columns, fanout raises
  the median delay from 0.88 ns (one sink) to 1.44 ns (65 sinks or
  more). Single-sink arcs on the paths carry 31 ns of the detour.
- **Examples:** a 31-column arc of 4.38 ns (reference 1.24), a 5-by-3
  arc of 7.85 ns (1.06), and a one-column arc of 6.14 ns (0.33).

The critical path's arcs are routed by wire count, not delay. The
routing side of Fmax is as large as the placement side, which the first
form of 19.3 did not show because it failed to converge.

(The report's `detailed_net_timings` are arrival times at endpoints, not
arc delays; a first split from them was wrong and is not used.)

### 2026-09-23: Unit 19.3b: the criticality cost at the unit's own scale routes every seed

The first form of 19.3 scaled every wire to nanoseconds (0.184 ns per
unit). That changed the balance against router2's to-go estimate for
every arc, not only the critical ones. 19.3b keeps a non-critical arc's
wire at exactly one unit and prices a critical arc's wire at
`w + (1 - w) * delay / U`. `--router2-crit-threshold T` keeps the unit
cost for arcs below criticality T. All runs resume from the baseline's
route-prepared checkpoints (binary `nextpnr-mistral.u19-3b`).

Screening, seeds 1 and 2:

| Threshold | Fmax | Iterations | Wires |
| --- | --- | --- | --- |
| 1.01 (control) | 11.39, 10.94 | 45, 51 | 767,087, 778,792: the baseline's routes exactly |
| 0.97 | 11.31, 11.70 | 67, 72 | 767,972, 779,439 |
| 0.9 | 11.64, 11.70 | 43, 72 | 768,626, 779,439 |
| 0 | 11.65, 11.64 | 58, 98 | 766,473, 776,854 |

Five seeds:

| Seed | Baseline | T = 0.9 | T = 0 | Iterations at T = 0 | Wires at T = 0 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 11.39 | 11.64 | 11.65 | 58 | 766,473 |
| 2 | 10.94 | 11.70 | 11.64 | 98 | 776,854 |
| 3 | 11.87 | 11.06 | 11.86 | 64 | 768,672 |
| 4 | not routed | 10.57 | 10.84 | 80 | 784,482 |
| 5 | 10.92 | 10.77 | 11.40 | 66 | 773,043 |
| Median | 11.17 | 11.06 | 11.64 | | |

- **T = 0.** Every seed routes, including seed 4, which the baseline
  does not. The median rises 0.48 MHz, and seeds 1, 2 and 5 gain 0.26 to
  0.70. Wires are unchanged; iterations rise to 58 to 98 against 45 to
  64.
- **T = 0.9.** Worse: the median falls to 11.06, and seeds 3 and 5 lose
  Fmax.
- **Quality rule, T = 0: REJECT.** The gain is inside the spread, and
  seed 4 (10.84) is below the baseline's worst (10.92), though the
  baseline has no seed 4 to compare.
- **Size of the gain.** It is small against the 24 ns of detour above
  the median on each path. Routing the worst path better makes the next
  one critical, and the core has many paths near the worst.

Kept opt-in (`--router2-crit-cost --router2-crit-threshold 0`). It is
next measured together with the placement settings.

### 2026-09-23: Timing-driven placement: HeAP's timing weight screened

HeAP weighs an arc in its solver by `1 + timingWeight * crit^e`. These
were never tuned for the core. Screened on top of 19.2 and 19.6 (rows 4,
entries 4, affinity 2, reach 5), seeds 1 and 2
(`build/quality/sweep_tmg.sh`).

| Setting | Routed | Fmax | Iterations | Wires | Entries per net |
| --- | ---: | --- | --- | --- | --- |
| weight 10 (default) | 2 | 11.57, 12.26 | 27, 28 | 737,553, 743,925 | 1.40, 1.41 |
| weight 30 | 1 | cap of 100, 11.77 | 100, 33 | 742,335, 732,886 | 1.40 |
| weight 100 | 2 | 12.80, 12.39 | 59, 64 | 738,738, 734,590 | 1.41, 1.41 |
| exponent 4 | 2 | 11.57, 12.26 (the default's placements exactly) | 27, 28 | 737,553, 743,925 | 1.40, 1.41 |
| weight 30, exponent 4 | 1 | the weight-30 runs exactly | | | |

- **Weight 100** gives the best seeds so far, a median of 12.60 on the
  two seeds.
- **Weight 30** lost seed 1. The response is not monotonic, so it is
  judged over five seeds.
- **The exponent does nothing** because the arch overrides it:
  `mistral/arch.cc` sets `criticalityExponent = 7` after the settings
  are read, so `--placer-heap-critexp` never takes effect on this arch.
  At 7, only arcs very close to the worst slack get the timing weight,
  which is why a weight of 100 is needed to see an effect. Sweeping the
  exponent needs an arch option.

### 2026-09-23: The Fmax set passes the quality rule: median 11.17 to 12.59 MHz

Units 19.2, 19.6 and 19.3b with HeAP's timing weight at 100, seeds 1 to
5 three at a time, seed 1 repeated (`build/quality/run_combo.sh`, tag
`f-w100-crit-5s`, binary `nextpnr-mistral.u19-3b`). The options on top
of the recipe are:

    --sa-row-weight 4 --sa-entry-weight 4 --heap-lab-affinity 2 --heap-lab-reach 5
    --placer-heap-timingweight 100 --router2-crit-cost --router2-crit-threshold 0

| Seed | Baseline | Fmax set | Iterations | Wires | LABs | Entries per net |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 11.39 | **13.40** | 45 | 736,786 | 4,157 | 1.412 |
| 2 | 10.94 | **12.41** | 26 | 733,044 | 4,174 | 1.410 |
| 3 | 11.87 | **12.93** | 63 | 733,038 | 4,134 | 1.406 |
| 4 | not routed | **12.59** | 30 | 726,337 | 4,165 | 1.404 |
| 5 | 10.92 | **12.48** | 43 | 730,877 | 4,146 | 1.409 |

**Quality rule: ACCEPT.**

- The median gains 1.42 MHz (12.7%), above the baseline's spread of
  0.95.
- Every seed routes, including seed 4. The worst seed (12.41) is above
  the baseline's best (11.87).
- Seed 1 repeated is identical.
- Wires fall 5% (median 733,038 against 772,377), iterations are 26 to
  63, and placement takes 257 to 351 s against 376 to 440 (orientation
  only: parallel runs).

The three parts add up:
- the placement units (19.2, 19.6) gave 11.75;
- the timing weight gave 12.60 on seeds 1 and 2;
- the routing cost adds 0.6 on seed 1 (12.80 to 13.40) and nothing on
  seed 2 (12.39 to 12.41).

A timing weight of 300 lost seed 2 (cap of 100) after 12.94 on seed 1,
so 100 is the setting.

Every option in the set is opt-in. Making the set the core recipe, or a
default, is a promotion and needs its own decision row.

### 2026-09-23: HeAP's criticality exponent, now an option

`--heap-crit-exp E` (default 7, the value the arch fixed) replaces the
override, so `--placer-heap-critexp` is still ignored on this arch. On
19.2 and 19.6, seeds 1 and 2 (`build/quality/sweep_cexp.sh`, binary
`nextpnr-mistral.cexp`):

| Weight, exponent | Routed | Fmax | Iterations | Wires |
| --- | ---: | --- | --- | --- |
| 100, 7 (control) | 2 | 12.80, 12.39 | 59, 64 | 738,738, 734,590: the weight-100 runs exactly |
| 100, 4 | 2 | 12.85, 12.67 | 69, 51 | 741,240, 739,002 |
| 100, 2 | 0 | cap on seed 2; seed 1 stopped at iteration 71, 836,142 wires | | |
| 10, 2 | 2 | 12.45, 12.10 | 38, 65 | 732,217, 732,748 |

- **Weight 100, exponent 2** pulls too many arcs and does not route.
- **Weight 10, exponent 2** comes near weight 100 at exponent 7.
- **Exponent 4 at weight 100** is 0.16 MHz better on the median of two
  seeds, inside the noise. It goes to five seeds with the full set.

Exponent 4 in the full set on five seeds (tag `f-w100-e4-crit-5s`):
12.51, 12.36, 12.66, 12.82, and seed 5 not routed (cap of 100). The
median of the four routed seeds, 12.59, equals the set at exponent 7,
and iterations climb to 96 and 99 on seeds 3 and 4. REJECT against the
set; the set keeps exponent 7.

### 2026-09-24: Units 19.7 and 19.8 screened: annealer constants negative, delay table neutral

On the core recipe with the Fmax set (promoted, 5563eb6d), seeds 1 and
2, binary `nextpnr-mistral.u19-78` (`build/quality/sweep_19_78.sh`). The
reference is the set's own seeds 1 and 2: 13.40 and 12.41.

| Setting | Routed | Fmax | Iterations | Wires |
| --- | ---: | --- | --- | --- |
| Fmax set (reference) | 2 | 13.40, 12.41 | 45, 26 | 736,786, 733,044 |
| `--placement-delay table` (19.8) | 2 | 12.86, 12.98 | 34, 36 | 734,043, 728,594 |
| `--sa-timing-lambda 0.7` (19.7) | 2 | 12.59, 12.41 | 89, 34 | 746,964, 743,035 |
| `--sa-timing-lambda 0.9` | 0 | cap of 100 on both | | 801,284, 796,445 |
| `--sa-crit-exp 4` | 2 | 12.38, 12.07 | 52, 51 | 736,141, 733,218 |

- **19.7 is negative.** A larger timing share in the annealer costs
  wires, and at 0.9 routability. A softer exponent loses Fmax. The
  defaults (0.5, 8) stay.
- **19.8 is neutral on two seeds:** median 12.92 against 12.91, seed 1
  down 0.54 and seed 2 up 0.57. On the uncrowded probe it cost 18%
  (35.87 to 29.49 MHz, one seed). Both options stay opt-in. The table is
  next judged on the current core.

### 2026-09-24: The detour the Fmax set leaves (the measurement of design 19.9)

The Fmax set's routed seeds 1, 2, 3 and 5 were resumed from new
route-prepared checkpoints (`build/quality/ckpt/core-fset`, binary
`nextpnr-mistral.u19-78`). They reproduce 13.40, 12.41, 12.93 and 12.48
exactly. Every arc was dumped (`fset_arcs_s*.csv`) and the critical
paths split as before.

| Seed | Path | Distance | Detour | Logic | Above the span's median |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 74.7 ns | 37.1 | 23.2 | 13.8 | 13.6 |
| 2 | 80.6 | 37.1 | 30.8 | 12.1 | 20.1 |
| 3 | 77.3 | 34.6 | 28.5 | 13.6 | 18.8 |
| 5 | 80.1 | 40.7 | 23.9 | 15.1 | 15.1 |

Over the four: distance 48%, detour 34%, logic 17%. The detour above
the median averages 16.9 ns per path, against 24 on the baseline. The
worst arcs are still plain detours: 11.48 ns for a 3-by-16 arc
(reference 1.55), and 7.64 ns for 29 by 6 (reference 2.12). That is
enough to build 19.9.

### 2026-09-24: Unit 19.9: timing repair raises every seed, 12.59 to 13.28 MHz

`--router2-repair-rounds N --router2-repair-crit C`. It resumes from the
Fmax set's route-prepared checkpoints (`core-fset`, binary
`nextpnr-mistral.u19-9`), so every seed has the set's own placement, and
the router is identical up to convergence.

- **Probe.** Two rounds at C = 0.9 on the default path: 1,372 critical
  arcs, 107 and then 14 faster, 35.87 to 36.69 MHz. Under the unit cost:
  34.56 to 37.48. router1's check passes.
- **Core, C = 0.9.** Only 3 arcs qualify and none is faster, so every
  seed is unchanged. On the core the critical paths' arcs sit between 0.5
  and 0.9 in router2's criticality. At C = 0.5, seed 1 repairs 32,730
  arcs (3,904 faster). At C = 0.1 it repairs 133,313, with the same
  Fmax.

| Seed | Fmax set | 1 round, C = 0.5 | 2 rounds, C = 0.5 |
| ---: | ---: | ---: | ---: |
| 1 | 13.40 | 14.19 | 14.20 |
| 2 | 12.41 | 13.18 | 13.76 |
| 3 | 12.93 | 13.24 | 13.28 |
| 4 | 12.59 | 13.08 | 13.21 |
| 5 | 12.48 | 12.61 | 12.68 |
| Median | 12.59 | 13.18 | 13.28 |

- **Every seed gains** at two rounds, from 0.20 to 1.35 MHz on the same
  placement, and routes legally. Router time grows by 4 to 7 s.
- **Against the Fmax set: REJECT.** The median gains 0.69, below that
  set's seed spread of 0.99.
- **Against the original baseline: ACCEPT** (+2.11 MHz).
- **The rule's reference is placement variance.** The spread the rule
  compares against is how much placements differ from seed to seed. A
  change that leaves every placement alone and gains on all five of them
  is judged against a variance it does not have. That is a question for
  the rule's owner.

The option stays opt-in.

### 2026-09-24: The current Fabi386 core does not place: LAB input lines, not ALMs

The fabi386 session reported (2026-09-24) that today's core fails HeAP
under both recipes. Its netlist, copied to
`build/stage6-fullcore/core-20260924/`, has 44,325 `MISTRAL_COMB` (52%)
and 15,244 registers, against the 2026-09-17 netlist's 39,448 and 15,647:
12.4% more logic. All runs use `--no-route` and seed 1 unless named.

| Run | Result |
| --- | --- |
| Old recipe (pairing, register packing) | Fails in HeAP's first strict pass over the LUTs: "Unable to find legal placement" on an ALUT5 after 7.6 million attempts (73 s) |
| Old recipe, seed 2 | Same, on another cell |
| Without `--alm-pairing` | Same |
| Without `--register-packing` | The LUTs place; the stall exit (6a) reports 1,169 registers with no legal location |
| Limit 46 or 50 (`MISTRAL_LAB_INPUT_LIMIT`), with register packing | Same early failure |
| Limit 46, without register packing | Places (624 s) |
| Full Fmax recipe, limit 46, without register packing, seed 1 | Places (252 s). router2 falls to 13,855 overused wires at iteration 10, then rises to 20,316 at 30 (stopped at 45 minutes) |
| Same, seed 2 | 997 cells unplaced |

The stall report without register packing:
- 1,661 of 4,191 LABs are at the input-line limit of 42, and 1,377 more
  are within 4 of it (37.5 inputs per LAB on average);
- 19,159 ALMs hold two LUTs and 6,007 hold one (60% of 41,910 in use);
- the stuck cells are all registers, with 3.3 unique input nets each.

The old core already used 4,186 of 4,191 LABs. The new one has no LAB
left, and the limit that binds is the input lines, not ALMs. The count
of 42 is conservative (`lab.cc`):
- it sums each ALM's unique inputs, so a net that enters several ALMs
  of one LAB counts once per ALM, though it uses one input line;
- it counts nets driven inside the LAB, which arrive on local lines.

Raising the limit to 46 places seed 1, but the router cannot realise the
denser LABs, as `lab.cc` warns. Register packing fails separately, in
the first pass. Quartus fitted the old core in 3,219 LABs. The Fmax work
continues on the 2026-09-17 netlist; the current core needs a density
unit (an input-line count that matches the hardware, and 19.4).

### 2026-09-24: How many input lines a LAB really uses (the measurement of design 19.10)

On the routed Fmax set of the 2026-09-17 core, seed 1, from each net's
`ROUTING` attribute (`build/quality/lab_lines.py`):
- nextpnr's count averages 36.3 per LAB;
- distinct data nets from outside the LAB average 23.4, and register-driven
  nets inside it 7.3;
- the route uses 29.0 input lines and 1.1 local lines per LAB.

In the 1,539 LABs at the count's limit of 42, the route uses 35.2 lines
on average and at most 44 of 46. The distinct-net demand (every net on a
data pin not driven by a LUT of the same LAB) tracks the lines used within
two lines at the median, on seeds 1 and 2; the table is in design 19.10.
A per-class bound from the netlist's pin names reaches 61, above anything
the route used, so pin names are not physical pins.

### 2026-09-24: Unit 19.10 phase A negative: denser LABs do not route, lower limits cannot place chains

`--lab-input-model nets` (C++ only, `--lab-legality legacy`). The demand is
kept incrementally per LAB in `bindBel`/`unbindBel`; the overlay form
answers from the overlay's changes, and cluster candidates take HeAP's
live path. `MISTRAL_CHECK_LAB_DEMAND=1` recounts every LAB from scratch
after each change and each overlay answer. A full probe placement passed
it. Its first form flagged every unbind, because the recount ran while the
leaving cell still sat on its bel; the check was wrong, not the count.

Probe, recipe router settings (`--router2-unit-cost --router2-reroute 20
--router2-reroute-contested`), seed 1:

| Model, limit | Result |
| --- | --- |
| count, 42 | routes in 9 iterations, 32.32 MHz |
| nets, 42 | 10 wires overused at the cap of 100, almost all LAB input lines (TD) |
| nets, 40 | routes in 11 iterations, 33.61 MHz |
| nets, 38 and 36 | placement not finished in 5 minutes |

Cores, full recipe, seeds 1 and 2 (binary `nextpnr-mistral.u19-10a`):

| Core, model, limit | Result |
| --- | --- |
| 2026-09-17, nets, 40 | placement 1,008 and 680 s; router2 at the cap with 497 and 437 overused wires |
| 2026-09-17, nets, 38 | no placement: the divider's 65-cell carry chain (`divisor_shifted`) has a LAB-sized segment with 40 distinct input nets |
| 2026-09-24, nets, 40 | no placement: register packing's early failure, as under the count model |
| 2026-09-24, nets, 40, without register packing | no placement: the same carry chain |

The measurement behind the design (input lines used tracking the demand)
held for placements the per-ALM count made. LABs packed to the new
count's limit ask for more input lines than router2 finds. They fail on
the lines' class structure (section 9.2), which the count cannot see.
Below 40, a carry chain's own segment no longer fits. Kept opt-in at the
user's direction; phase B (Rust) is not built.

Register packing's early failure on the 2026-09-24 core, under the count
model: `MISTRAL_DEBUG_CLUSTER_REJECT` shows the address adder's carry
chain (`computed_ea`, 34 cells, a 38-input first segment) refused over
and over for the LAB input limit (45 counted in the LABs tried), and then
a microcode LUT hits the per-cell attempt limit. The cause is not yet
known.

### 2026-09-25: The cause: global clocks were never flagged, and every LAB lost a DATAIN line

Register packing's early failure on the 2026-09-24 core, traced:

- **Not a search limit.** Raising the per-cell attempt limit fourfold and
  eightfold (`--placer-heap-cell-placement-timeout 2` and `1`; the option
  is a divisor) fails on the same cell at the same point.
- **A control conflict in every LAB.** `MISTRAL_DEBUG_CLUSTER_REJECT`
  now takes a count, and `MISTRAL_DEBUG_CLUSTER_REJECT_LAB` lists a
  rejected LAB's registers and the cluster's targets. The failing
  cluster (a 5-input LUT, a 2-input LUT, and a register with a fabric
  enable and the `eflags_we` clear, shared by 4,462 registers) was
  refused in 4,061 distinct LABs, every time for LAB control reason 21,
  a DATAIN conflict.
- **The cause is a model omission.** A LAB has four DATAIN lines for
  fabric-driven control signals. The control model gives one to the
  clock unless the clock net is global. The mistral arch never sets
  `NetInfo::is_global`, and neither does upstream nextpnr, so the core
  clock, which the global router carries to the LAB's clock input,
  holds `DATAIN[0]` in every LAB. With the clear on a second line, a LAB
  admits one other enable, and the core has 270.

**Quartus agrees that `DATAIN[0]` can carry an enable.** The test design
has three registers in LAB (85, 27), each with its own enable from a pin,
and a clock on pin V11 (`build/stage6-fullcore/en3test/`; NAS01
`fabi386_jobs/en3test_20260925`).
- **nextpnr without the flag** refuses the placement.
- **With the flag,** it routes the enables into `DATAIN.0`, `.2` and
  `.3`, and the clock into `CLKIN.0` from `XCLKB2A.085.027.0005`.
- **Quartus 17.0.2** uses the same four inputs. The clock and enable
  settings agree bit for bit (`BCLK_SEL.0 CLK1`, `TCLK_SEL.0 CLK0`,
  `TCLK_SEL.1 CLK2`, `EN0/1/2_NINV 0`). The differences are the
  registers' data-path encoding (`PKREG` against `MODE`) and where each
  tool put the output XOR.

**`--lab-global-clocks`** flags every net driven by a clock buffer, a
clock enable block, or a PLL counter as global. The flag is set in
`assignArchInfo`, so it holds after packing and on every checkpoint
restore.
- **Probe:** verify mode has 2,080,187 evaluations and 0 mismatches. A
  run resumed from its route-prepared checkpoint equals the
  uninterrupted run (`0xb890eb09`, 32.73 MHz).
- **2026-09-24 core:** the old recipe now places seeds 1 and 2 (163 s,
  314 s), where it failed in the first pass before.

### 2026-09-25: Global clocks measured: today's core places but does not route; the old core loses seeds

`--lab-global-clocks` (binary `nextpnr-mistral.u19-11`), with the full
recipe.

2026-09-24 core, seeds 1 and 2 (`core0924/gclk`, binary
`nextpnr-mistral.gclk`: the same flag, set by the experiment's
environment variable):

| Seed | Placement | Routing |
| ---: | ---: | --- |
| 1 | 416 s | 1,685 overused wires at iteration 10, 1,833 at 30, 2,449 at the cap of 100 (873,800 wires) |
| 2 | 259 s | 9,569 at 10, 17,021 at 30, 18,532 at 56, where the 60-minute limit stopped it |

2026-09-17 core, five seeds, against the recipe's result
(`rep2-c05-5s`, median 13.28):

| Seed | Recipe | With the flag | Iterations | Wires | Entries per net |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 14.20 | 13.39 | 72 | 734,165 | 1.366 |
| 2 | 13.76 | 13.39 | 72 | 727,771 | 1.346 |
| 3 | 13.28 | not routed (cap of 100) | | 747,905 | |
| 4 | 13.21 | not routed | | 741,311 | |
| 5 | 12.68 | 12.48 | 27 | 731,440 | 1.368 |

- **Quality rule: REJECT.** Two seeds do not route, and the worst seed
  (12.48) is below the recipe's worst.
- Placement takes 186 to 236 s, against about 280 to 350. LAB entries
  per net fall from 1.41 to 1.35. Seed 1 repeated is identical.
- With the clock's line freed, a LAB takes a third distinct enable. The
  enables ride input lines as the data does, and the placer packs to the
  new capacity without pricing what it costs the router.

The flag stays opt-in. It is the hardware's truth and today's core needs
it to place. What the core now needs is routing capacity around full
LABs, which none of the density levers so far provide.

### 2026-09-25: Design 20.0, step 0.4: why local lines go unused

In the recipe's routed core (`rep2-c05-5s`, seed 1), each net with a sink
in its driver's LAB was classified by how it reaches that LAB's inputs:

| Driver | Inside the ALM only | Local line (LD) | Out and back in (TD) | Both |
| --- | ---: | ---: | ---: | ---: |
| LUT | 17,035 | 3,886 | 3,863 | 720 |
| First register of a half | | | 4,667 | |

`MISTRAL_DUMP_LAB_LINES` now also dumps local lines and ALM inputs. For
LAB (40, 30):
- each of the 20 local lines (one per LUT output) reaches 42 ALM inputs;
- each ALM input accepts 8 to 12 of the 20 local lines, beside 21 to 25
  of the 46 input lines.

A LUT's output therefore reaches a sink in its own LAB locally only if
the sink LUT's physical pin accepts that local line. The pin is fixed by
the netlist, so half of the intra-LAB LUT nets leave the LAB and re-enter
by an input line. The first register of a half has no local output at
all. Two units of design 20 address these directly: LUT input permutation
(20.2) lets the router choose a pin the local line reaches, and the
second register's local output (19.4, 20.3) serves the registers.

### 2026-09-25: Design 20.0 decided: two LAB rules block 86% of Quartus's clustering

**The oracle fit.** Quartus 17.0.2 placed our netlist as given (NAS01
`core_oracle_20260925`: physical synthesis off, no merging), handed over
by `mistral/tests/quartus_handover.py` with a name map; locations joined
by `mistral/tests/oracle_join.py`. Results:
- 20,391 ALMs, 3,219 LABs, and 13,532 of 15,647 registers kept;
- **16.71 MHz**, against 25.18 for the production fit of the same
  netlist and 13.28 for our recipe;
- 49,756 cells mapped to a LAB, 1,619 not.

Quartus's lead thus splits roughly into netlist optimisation (physical
synthesis and the rest, 1.5x) and placement and routing (1.26x), mixing
two timing models.

**Hinted runs (step 0.3).** Old core, seeds 1 to 3, pairing and register
packing off, `--lab-global-clocks`:

| Run | Result |
| --- | --- |
| `--lab-hint` (Quartus's LABs), `--no-sa-refine` | 42 to 46% of hinted legalisations honoured; no seed routes (989,583 to 996,773 wires at iterations 20 to 28, stopped at 60 minutes) |
| Control, no hints | all route: 11.75, 12.19, 11.85 MHz; 786,800 to 793,317 wires |

A placement 42% Quartus and 58% scattered by the fallback search tests
neither clustering.

**Why nextpnr refuses Quartus's LABs.** `mistral/tests/rules_gap.py`
maps every Quartus LAB cell by cell onto nextpnr's bels (the N numbering
verified by ff2test and en3test) and checks nextpnr's rules as `lab.cc`
and the control model state them, with the clock global.

| Rule broken | LABs of 3,219 | Cells |
| --- | ---: | ---: |
| Second register of a half | 2,224 | 36,110 |
| LAB input count above 42 | 1,883 | 36,483 |
| Register fed from the fabric, no E/F left | 324 | 7,269 |
| ALM inputs above 8 (two shared inputs) | 159 | 3,001 |
| ALM LUT bits above 64 | 93 | 1,765 |
| Control lines | 1 | 20 |
| Legal as packed | 346 | 2,817 |

Quartus's LABs have a nextpnr input count of median 50, 90th percentile
72, maximum 92. The share of Quartus's LABs legal under nextpnr's rules,
with rules lifted:

| Rules lifted | Legal LABs |
| --- | ---: |
| none | 346 (11%) |
| the second register | 1,288 (40%) |
| the input count | 938 (29%) |
| both | 2,771 (86%) |
| both and the E/F rule | 3,059 (95%) |

**Phase order.** Quartus's clustering cannot be tested or emulated until
two rules change:
- **The second-register refusal (20.3):** the Quartus differential
  agrees; it waits on silicon.
- **The input count (20.2):** a routability proxy that LUT input
  permutation must replace. Quartus routes LABs of count 50 and more
  because it permutes; 19.10 showed that loosening the count without
  permutation fails in the router.

20.3 and 20.2 therefore come before 20.1 (clustering).

### 2026-09-25: Unit 20.2 first measurement: LUT input permutation, 13.28 to 14.09 MHz on the recipe's placements

`--lut-permutation` (design 20.2).
- **Pseudo-wires.** Every ALM half gets five logical-input pseudo-wires
  (`LPERM`), each fed from the half's five physical pins, created with
  the device.
- **Pre-route.** `reassign_alm_inputs` maps a plain L5 half's logical
  inputs to them, except the nets both halves share, which stay on A
  and B.
- **After routing.** `lut_permutation_fixup` moves each logical pin
  onto the physical pin the router chose and unbinds the pseudo-wire.

The first form also permuted the shared nets. On the probe that left
115 overused wires at the cap (165,263 wires against 149,576 without
permutation): two five-input halves fit only with their common nets on
A and B, and negotiation did not find it. Pinning them made that 41,
and router1 finished. Under the recipe's router settings the probe
routes in 41 iterations against 16, at 35.98 MHz against 34.56 (router2
counts the pseudo-wires, so its wire totals read high).

Core, recipe plus `--lut-permutation`, five seeds (tag `lperm-5s`). The
placements match the recipe's (`rep2-c05-5s`) seed for seed:

| Seed | Recipe | Permuted | Gain | Iterations |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 14.20 | 14.42 | +0.23 | 50 |
| 2 | 13.76 | 13.60 | -0.16 | 45 |
| 3 | 13.28 | 14.33 | +1.06 | 82 |
| 4 | 13.21 | 14.09 | +0.88 | 32 |
| 5 | 12.68 | 13.58 | +0.90 | 39 |
| Median | 13.28 | 14.09 | +0.81 | |

- **Router.** It moves 73,833 LUT inputs on 30,995 LUTs.
- **Wires.** Median local lines rise from 4,626 to 6,740 (+46%); row wires
  fall from 181,499 to 164,824, column wires from 95,038 to 86,557, and
  input lines from 120,566 to 115,550.
- **Routing rule: REJECT,** because seed 2 loses 0.16 MHz.
- **Quality rule: REJECT,** because the gain is below the 1.52 spread.

The option stays opt-in. Its purpose is the denser LABs of 20.0, which
come next.

**The correctness guard** (`mistral/tests/lut_perm_check.py`) is
independent of `compute_lut_mask`. It reads each ALM input's source from
the RBF decoded by `mistral-cv decomp`, matches it to a net through the
routed JSON (the ROUTING wires for an input line; the half's LUT or
second register for a local output, by `xDFF1L NLUT`), and checks every
plain L5 LUT's function against its mask. On the probe:
- without permutation: 7,341 of 7,341 LUTs correct;
- with permutation: 7,341 of 7,341 correct.

A mutation check shows it fails on errors: flipping one mask bit in 20
ALMs is caught in 15 (the other five hit halves it does not check), and
swapping the A and C sources in 20 ALMs is caught in 18. Silicon follows
when the board is back.

### 2026-09-25: The input limit relaxed under permutation: no density on the old core, not enough for today's

The per-ALM LAB input limit (`MISTRAL_LAB_INPUT_LIMIT`) raised under
`--lut-permutation`, full recipe (binary `nextpnr-mistral.u20-2`).

**Old core, seeds 1 and 2:**

| Limit | Seed 1 | Seed 2 |
| --- | --- | --- |
| 42 (`lperm-5s`) | 14.42 | 13.60 |
| 44 | 14.75 (42 iterations) | 14.08 (57) |
| 46 | 7 overused at the cap of 100, 7 at 200 | 2 at 100, 2 at 200 |
| 50 | placement failed | 45 overused at the cap |

**Limit 44 on five seeds (`lp-l44-5s`):**
- **Fmax:** 14.75, 14.08, 14.33, 14.10, 13.61; median 14.10, against
  14.09 at 42 and 13.28 for the recipe. Seed 1 repeated is identical.
- **Density:** LABs used are 4,142 to 4,156 against about 4,150; cells
  per LAB stay at 13.3; local lines are 6,986.
- **Quality rule against the recipe: REJECT** (+0.82, spread 1.52).

On the old core the placer does not fill LABs to the limit, so raising
it changes almost nothing. Density has to come from deciding which cells
share a LAB (20.1), not from a looser limit. Limit 46 is a hard wall:
the same wires stay overused with twice the iterations.

**Today's core (`core0924`), with `--lab-global-clocks`, seeds 1 and 2:**

| Limit | Seed | Placement | Overused at iteration 10, 30, 60 | Last |
| --- | ---: | ---: | --- | --- |
| 42 | 1 | 226 s | 516, 432, 508 | 406 at the cap |
| 42 | 2 | 121 s | 4,437, 5,495, 9,267 | 10,829 at 96 (time limit) |
| 44 | 1 | 100 s | 5,645, 5,954, 5,921 | 6,366 at the cap |
| 44 | 2 | 87 s | 3,858, 4,293, 6,560 | 7,477 at the cap |

Without permutation, seed 1 ended at 2,449 overused wires and seed 2 at
18,532. Permutation is a sixfold improvement on seed 1, but the core
still does not route, and a tighter limit only adds congestion. What
remains is fabric capacity around full LABs: fewer, denser LABs
(20.1), and the second register (20.3).

### 2026-09-26: A Linux runner, 20.2 confirmed there, and 20.1's first form negative

**The runner.** `nextpnr-runner`, an m8a.4xlarge spot instance (16 Zen 5 cores, 64 GiB, eu-central-1, about $0.45 an
hour, stopped between batches). nextpnr-mistral is built in an Ubuntu 24.04 container (`nextpnr-build`, Rust
enabled, Release) from `git archive` of the commit under test. Fifteen core runs in parallel take about 10 minutes
(placement 330 to 440 s, router2 120 to 230 s), against about 15 minutes for three on the workstation.

**A crash the runner found.** The first Linux build segfaulted at HeAP's first LAB query on the probe.
`Arch::assign_comb_info` set `combInfo.wclk` and `combInfo.we` for MLAB cells only; for every other LUT they were
the union's leftover bytes (they overlay `ffInfo`), and both LAB captures read them for every LUT. On macOS the
bytes happen to be zero; on Linux they held string data (the faulting pointer was `0x5f465f315f515f36`, ASCII). Fixed
in `77f1abd9` by clearing both for every cell; the probe is byte-identical on macOS (checksums `0xbb18ede9` /
`0xbc1365c6`), and Linux runs the probe in 29 s at 31.07 MHz.

**Platform.** Linux and macOS place differently from the same inputs (the C++ libraries' `std::sort` orders ties
differently), so results are compared within one platform. The Linux baseline of the recipe (`aws-base`, commit
`77f1abd9`):

| Seed | 1 | 2 | 3 | 4 | 5 | Median |
| --- | --- | --- | --- | --- | --- | --- |
| Fmax (MHz) | 13.01 | 13.17 | 12.90 | 13.11 | 13.14 | 13.11 |
| Iterations | 70 | 69 | 64 | 60 | 52 | |

Every seed routes, spread 0.26 MHz (1.52 on macOS), about 4,160 LABs, and seed 1 repeated is byte-identical.

**20.2 on Linux** (`aws-perm`, `--lut-permutation`, same placements): 13.64, 13.15, 14.03, 13.86, 13.69 MHz, median
13.69. The quality rule accepts it (+0.58 against a spread of 0.26, worst 13.15 above 12.90). The routing rule rejects
it: per seed +0.64, -0.02, +1.13, +0.75, +0.55, and seed 2 does not gain. The pattern matches macOS (seed 2 -0.16
there). Row and column wires fall 10% and 9%, local lines rise 46%. Not promoted; the promotion is the user's call
on two platforms' evidence.

**20.1, the clusterer** (`--lab-clustering`, `mistral/lab_clustering.*`), statistics on the old core with the
recipe's packing (31,278 units, 51,819 cells):

| Setting | Clusters | Cells per cluster | Absorbed nets | Closed at fill | Rule refusals |
| --- | --- | --- | --- | --- | --- |
| price 0.5, 8 tries | 13,616 | 3.81 | 17,668 | 290 | 22,289 |
| price 0, 8 tries | 7,477 | 6.93 | 19,357 | 637 | |
| price 0, 64 tries | 6,956 | 7.45 | 19,186 | 834 | 168,259 |
| price 0, 64 tries, distinct-net inputs | 6,811 | 7.61 | 19,621 | 2,364 | 65,275 |
| price 0, 64 tries, no input limit | 6,891 | 7.52 | 19,590 | 2,465 | 59,337 |
| price 0, 64 tries, second register allowed (test only) | 6,955 | 7.45 | 19,186 | 834 | 168,255 |
| price 0.1, 32 tries (the defaults now) | 8,283 | 6.26 | 20,405 | 712 | 84,465 |

A refusal for want of a bel is rare (at most 799). The C++ and the Rust authority give identical statistics.

**20.1, placement from the clusters**, five seeds on the runner, price 0.1, 32 tries:

| Tag | Options beyond the recipe | Routed | Fmax median (range) | LABs |
| --- | --- | --- | --- | --- |
| `aws-base` | | 5 | 13.11 (12.90 to 13.17) | 4,145 to 4,174 |
| `aws-cl-a` | clustering, hints | 5 | 12.15 (11.12 to 12.51) | 4,159 to 4,178 |
| `aws-cl-p1` | plus `--lab-cluster-pull 1` | 3 | 12.38 (12.37 to 12.48) | 4,168 to 4,177 |
| `aws-cl-p4` | plus `--lab-cluster-pull 4` | 3 | 12.09 (11.57 to 12.37) | 4,157 to 4,162 |
| `aws-perm` | `--lut-permutation` | 5 | 13.69 (13.15 to 14.03) | as base |
| `aws-cl-perm` | clustering, hints, permutation | 5 | 12.93 (11.86 to 13.45) | 4,159 to 4,178 |
| `aws-pn` | permutation, `--lab-input-model nets` | 0 | 95 to 116 overused at 100 | |
| `aws-cl-pn` | clustering on top of `aws-pn` | 0 | 98 to 151 overused at 100 | |

The hints are honoured on 37 to 51% of the legalisations that ask. No form moves the LAB count, and wires rise 2 to
4%. The workstation's seed-1 runs agree: clustering alone ends at 227 overused wires, and with permutation and
distinct-net inputs at 125.

**Conclusion.** Under the present LAB rules clustering before placement cannot raise density: the legaliser already
fills LABs to what the rules admit, and the clusters only pull members from their solver positions. The
distinct-net input model admits denser clusters and then does not route, as in 19.10. Design 20.1's outcome records
this; the pass stays opt-in for when 20.3 changes the rules.

**Today's core on the runner** (`core0924`, `--lab-global-clocks`, `--router2-max-iter 200`, five seeds each, stopped
after an hour with every run diverging):

| Tag | Options | Best overused wires (iteration) per seed | After an hour |
| --- | --- | --- | --- |
| `aws-gp200` | plus `--lut-permutation` | 6,574 (8), 1,211 (18), 3,572 (9), 1,719 (85), 2,324 (11) | 5,082 to 9,654 |
| `aws-g200` | | 9,899 (5), 4,511 (7), 7,239 (6), 6,772 (8), 5,318 (7) | 15,014 to 20,370 |

Permutation halves the overuse and no seed routes; the 406 wires seed 1 reached on the workstation is not typical.
Today's core needs density the rules do not yet admit (20.3), not more router iterations.

### 2026-09-26: Unit 19.4 step 2 on silicon: the second register works beside its LUT

The DE10-Nano was power-cycled and came back at `192.168.25.30` (DHCP; MAC `02:03:04:05:06:07`, MiSTer's default).
openflow-test's `run_hybrid.sh` now takes the address from `BOARD` (default unchanged). Every build is the 19.4 test
design (`build/stage6-fullcore/ff2silicon/`) loaded through `run_hybrid.sh` with the md5 checked on the board and
read by `measure_ff2.sh`; the full table and the classes are in `MISTRAL_GAPS.md` G9.

- **The prepared test fails, and not on the test registers.** F4 and its control read `done=0`: the 20,000-cycle
  counter never stops (the signature changes between reads seconds apart). The rule was lifted for every cell, and
  34 harness registers took second-register bels, nine of them counter bits; 33 of the 34 had no first register
  beside them and several no LUT in the half.
- **With the lift limited to the 192 constrained test registers** (F7), the board reads `done=1 match=1
  sig_lo=4c8d`, and the control (F8, golden off by one bit) `done=1 match=0` with the same `sig_lo`. Patterns P and
  Q, as the Quartus differential of step 1 had them, work on silicon.
- **Alone in its half** (F9, `gen.py --lone`, golden `bac4514b`): `done=1 match=1 sig_lo=514b`.
- **What fails:** the F4 netlist rebuilt with harness registers admitted by class (FB, FC, FD) fails whenever a
  second register sits in a half with no LUT; FD fails the checksum with exactly one such register. A first rule for
  `--alm-both-registers` (no route-through into a second register, which instead takes E/F) still fails (FE,
  13 such registers); requiring a LUT in the half (FF) makes the counter work and the checksum fail; admitting only
  the verified envelope (E0) fails again because the annealer moved a LUT away from a harness second register and
  checks only the bels it fills (design 18.1).
- **Carry halves** were not reached: the carry variant did not place (post-placement check).

**Conclusion.** The refusal in `lab.cc` guards a real failure, but a narrower one than "every second register": a
second register works beside a LUT of its half, fed by that LUT or through E/F. A free-placement rule that depends on
the LUT's presence is unsound under the annealer's move check, so 20.3 admits the second register only inside a
pack-time cluster with the LUT that drives it. The work-in-progress option (`--alm-both-registers`, C++ rule only) is
saved as `build/stage6-fullcore/ff2test/u20-3-wip.patch`, not committed.

### 2026-09-26: Unit 20.3: the second register's clock select, and `--alm-both-registers`

The board stayed up (`192.168.25.30`), so the LUT widths register packing produces were tested first:
`gen.py --widths` (build E1), LUT widths 5, 3, 2, 1, 5, 3 by ALM, half 0 with both registers fed by its LUT, half 1
with the second only, golden `db9d7d18`: `done=1 match=1 sig_lo=7d18`; its control E2 `match=0 sig_lo=7d18` (a
first control was void: the build-id `sed` also rewrote an ALUT3 mask of `8'hE1`).

**The option.** `--alm-both-registers` (with `--register-packing`): the packer gives a LUT a second register as a
cluster child at relative z 3 or 5 (the second register bel of its half), not beside a six-input LUT. The rule,
in `lab.cc`, the detached V2 evaluator (`lab_v2.cc`), and Rust (`v2.rs`), admits a register on a second register
bel only when it carries `NPNR_LAB_FF_SECOND_REGISTER` (set by the capture for such cluster children,
`is_second_register_child`), is fed by the LUT of its half, is not in a carry half or beside a six-input LUT, and
every register of its ALM shares its control set. `NpnrLabFfV2` gained `flags` (ABI v3; 56 bytes, the facts 3,968,
the bel patch 152); the tile scan skips second register bels only for unflagged candidates. The rule depends on the
LUT, but only inside a cluster, which moves as one unit.

**Silicon, end to end.** The ff2sil netlist without bel constraints through the option (96 second registers)
failed (E3 `match=0`) where the same netlist without the option passes on seeds 1 to 3 (E4, E6, E7). Neither MLAB
tiles (E5) nor LUT input sharing (E8) was the cause; the decoded bitstreams showed ALMs with registers in one half
only, which no passing build had. Mirroring the second register's control selects onto the other half passes (E9);
the synchronous-clear select alone does not (EA); the clock select alone does (EB). The writer now sets the other
half's selects from a second register, and the rules require one control set per such ALM. The option's build
passes (EC `done=1 match=1 sig_lo=4c8d`, control ED `match=0`), Rust authority, MLAB tiles allowed. `MISTRAL_GAPS`
G9 records the cause.

**Guards.** Rust: the generators put flagged and unflagged registers on second register bels; a dedicated test
scans flagged registers beside their LUT against the capture path (at least 100 of 400 scans answer on a second
register bel); a mutation that keeps the old skip for every register fails it and the random-sequence oracle;
the random-sequence test runs 16,000 steps (the new generator made the 8,000-step coverage floors marginal). Verify
mode on the probe with the option: 2,553,417 evaluations, zero mismatches, before and after the control-set clause
(it had first caught the detached evaluator still refusing). gtest 67 of 67. The probe packs one second register;
the core is next.

### 2026-09-26: Unit 20.3 on the core: no room, and a routability limit

Five old-core seeds with `--alm-both-registers` on the Linux runner (binary of `d934aafb`, recipe with permutation):
**no seed routed**. router2 stopped on its first iteration with an arc from a LUT output that had no path at all
(`u_core.decoder.ru_u[30]`, `ru_u[32]` and two others across seeds). Five seeds of today's core with global clocks
failed the same way. The same binary without the option reproduces `aws-perm` seed for seed (13.64, 13.15 MHz), so
the default path is unchanged.

- **Why routing failed.** The route-prepared core (seed 1) packed two second registers. At 27.49 ALM 0 half 0 the
  LUT feeds both registers of its half and a LUT in another LAB. A half leaves the ALM through three ports: the
  first register's (fabric), the second register's (fabric), and the second register's local line. Both
  registers' outputs have fabric sinks (16 and 29), so they hold both fabric ports, and the LUT's own fabric sink has
  none. The packer now gives a LUT a second register only when everything its output drives is a register
  (`register_packing.cc`); the silicon test design is unchanged by that (96 second registers, its LUTs drive only
  registers).
- **Why there is little to pack.** The 3,980 registers refused for "LUT slot already taken" (19.4's motivation)
  come from about 400 LUTs that each drive around ten registers. A half holds two, so a second register could add
  at most one register per such LUT, and on the core only three of those LUTs drive registers that share one control
  set. Pack admission with the option refused 3,220 of 8,582 candidates (the ALM's one-control-set clause); with the
  register-only condition the core packs **0** second registers: 2,024 LUT slots taken, 583 control-set conflicts,
  1,665 refusals of 7,025 admissions.
- Verify mode on the core: pack admission agreed on all 8,582 (zero mismatches); the placement phase was stopped
  with the routing result known.

**Conclusion.** The second register works in silicon and the option is correct (the clock-select fix, `MISTRAL_GAPS`
G9), but it cannot move this core: its multi-register LUTs are broadcast LUTs with mixed control sets. Design 19.4's
estimate counted registers, not LUT halves. The option stays off by default; 20.3's density lever is closed for the
Fabi386 core.

### 2026-09-26: Unit 20.4a built: LUT inputs timed by physical pin

`--lut-pin-delays off|report|on` (design 20.4). `getCellDelay` timed a LUT input by its logical pin and the LUT's
size, as if the last logical input sat on F; `reassign_alm_inputs` puts a five-input LUT's logical A to E on C, E,
F, B, A. With the option a plain LUT input is timed by its physical pin (the table's quads re-keyed: A 0.605, B
0.583, C 0.510, D 0.512, E 0.400, F 0.097 ns); under 20.2's permutation an `LPERM` pin costs nothing in the cell and
its pseudo-pip carries the chosen pin's delay (`Arch::lut_input_class`). Pins are assigned at routing preparation,
so placement is unchanged and every comparison is a routing comparison. `report` routes under the old table and
redoes the post-route timing (and the report) by pin: the yardstick for the old routes.

Exec probe, seed 1, recipe router settings with `--lut-permutation`: `off` 39.48 MHz (the old model), `report`
34.83 MHz (the same routing, checksum `0x8b2f05a9`, timed by pin), `on` 36.38 MHz (routed by pin): +4.4% on the
same yardstick. Signoff (`--rbf`) agrees: 33.71 re-timed against 35.45 routed by pin. Without permutation the
option cannot choose pins and the one seed moved the other way (35.60 re-timed, 33.40), inside probe noise; the
core over five seeds decides.

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
| 2026-09-18 | 6h | Make the spreader and the legaliser weigh a vertical tile like the solver does, behind `--row-cost W` | Probe: sink classes move to Quartus's, fabric wires 9% fewer, router slower to settle, Fmax a few percent down; **core: routes to completion for the first time** (0 overused at iteration 45 with the unit cost and periodic re-routes, at 125 with the delay cost), signoff 9.8 to 11.9 MHz against Quartus's 25.2 |
| 2026-09-18 | Rust | Reopen the concluded crate for a performance revision at the user's direction: resident LAB snapshots patched one ALM at a time replace the per-query capture in every non-legacy legality mode; the capture path stays as the harness | Probe: the Rust authority at wall parity with legacy with the annealer on the overlay seam (30.6 s against 30.7 s), verify mode zero mismatches over 14.6 million queries; core: placement 1,065 s against 750 s (1.4 times, from 6 times on the first resident build and an order of magnitude on the capture path), byte-identical, 5.36 billion queries; the 59 ns per query that remain are the call, the marshalling, and the rules, which a per-tile query would halve |
| 2026-09-19 | rules | Five coding rules, each with its check: the crates deny `unwrap`, `expect`, `panic`, and `unreachable` outside tests; the oracle generates every patch shape the arch sends; `--telemetry` writes the counters as JSON beside the unchanged `--report`; the fixture fails a test that leaks cells or nets; `mistral/tests/gate.sh` runs before every commit | One `expect` removed from the FFI; the leak check found two tests to fix; telemetry on the probe leaves the report and both checksums byte-identical; the gate runs in 58 s |
| 2026-09-19 | monitor | Add the live dashboard as Rust surface at the user's direction: a pure renderer crate and a monitor module in the existing FFI crate (a second static library would carry a second Rust runtime); the session owns the terminal, keeps the `--log` stream, and reads only atomics and the phase clock off the owner thread | Probe byte-identical with and without it; the resident totals of a Rust-mode run equal the run without it; 96 frames over the 24 s probe |
| 2026-09-19 | promotion | The complete LAB evaluator defaults to the Rust authority in Rust builds, with the annealer on the overlay seam; legacy stays the fallback in every build and the default where Rust is not built; the control-plan authority stays legacy because the complete evaluator owns the control check | User's direction on the recorded evidence: byte-identical on the probe and the core, verify harness zero mismatches on both, probe wall at parity (29.4 s against 30.0 s), core placement 1.4 times legacy; the gate's probe identity unchanged on the new default path |
| 2026-09-19 | concurrency | Enable no concurrency path with the promotion: `--threads` for placement, `--placer-lookahead`, and `--sa-batch` stay off | Threads change nothing on the default path (router2 partitions on its own; placement has no parallel section); the lookahead is byte-identical, 10% slower, 1.8 times the CPU; the batched annealer is not byte-identical and owner-bound past two workers |
| 2026-09-21 | admission | The pack-time rules are not ported: they stay as search filters, and the placer's LAB legality authority admits each cluster before the packer commits it, through the overlay seams, with no new Rust surface | The pairing rule equals the checker's input rule on plain LUTs and runs millions of times per pack; one admission per committed cluster costs 20,541 evaluations on the core; zero refusals, zero verify mismatches, packing and results byte-identical |
| 2026-09-21 | tile scan | Measure the query stream before designing the per-tile query; build the scan as "first legal bel in scan order, asked after a first live refusal" rather than a mask over the tile | The probe under the recipe is 91% single legal answers, the core 99.55% refusals in runs of 30; the scan ends at the first legal bel, so a mask would evaluate bels the scan never reaches, and an unconditional batch would double the cost of uncrowded tiles |
| 2026-09-21 | tile scan | On by default in the Rust legality modes, advisory in shadow and verify, absent in legacy; `--no-lab-tile-scan` keeps the per-bel path for comparison | Byte-identical placements on the probe under both option sets and on the core; verify mode zero mismatches on the probe; core placement 669 s against 1,092 s per bel; the legacy authority takes 781 s on the same binary the same day |
| 2026-09-21 | tile scan | Inside the scan, ask the predicates cheapest first, skip the second register bel of a half under a property test, keep MLABs on the cheap path, and ask the batch before the first bind after a refused tile | 55% of the bels a scan evaluates on the core are second-in-half register bels and 76% fail the ALM rule alone; the batch equals the live check, so its timing is a cost choice; core placement 530 s against 669 s, byte-identical, 0.68 of the legacy authority's 781 s |
| 2026-09-21 | profile | Size further work from a whole-run Time Profiler recording of the core, never from the probe; six hot paths designed before any is built (design 16), ordered small Rust changes first, the cluster path next, upstream's files last | Strict legalisation 54% of the core's 715 s, router2 20%, annealer 11.5%, solver 10.5%; the probe is 47% annealer and 3% legaliser; estimates are for ordering only and each unit is measured by the same recording when it lands |
| 2026-09-21 | register capacity | Hiding the never-legal register bels from the placer (`--usable-register-bels`) is a negative result; landed for the record, then removed; never size a change from one seed | Probe inside the seed spread; core: the legaliser slower on two seeds of three, router2 iterations up by half, and seed 3 unrouted at the cap where the default routes in 64 iterations |
| 2026-09-21 | 16.4 | The scan does not shortcut the control rules: the first form (one verdict per LAB) was wrong and the sound form (end at a first-walk refusal) gains nothing; a shortcut in the evaluator now needs a hostile property test, a mutation check of the oracle, and the core's checksums | The core's checksum moved with the first form where the probe, its verify mode, and the scan oracle all passed; the corrected form holds identity at 391.8 s against 391.3 s; the new hostile scan oracle fails on the wrong form |
| 2026-09-21 | 16.6, 16.2 | Keep both at their measured size, two core runs a side: the occupancy mask and the candidate beside the trials (12 s), the scan loop's flags (7 s); estimates from a profile's self time overstate what a change can recover | Identity on the probe in three modes and on the core; strict legalisation 390 s to 378 s to 370 s; 16.2 was estimated at 25 to 35 s |
| 2026-09-21 | 16.1 | In the Rust legality mode a cluster candidate is answered by the resident session and that answer decides; the detached C++ evaluation of frozen records stays the authority in legacy, the harness in shadow and verify, and the path for what the session declines; one crate function and one FFI call of new surface | The frozen path captured a whole LAB per edited bel and evaluated it twice, 91 s of the core's legaliser, with the C++ verdict deciding even after the promotion; core placement 426 s against 503 s, byte-identical with the same candidates accepted and rejected; verify mode zero mismatches on the probe over 250,504 candidates |
| 2026-09-21 | 16.3 | The equation system keeps upstream's sorted insert; append-and-merge is bit-identical and no faster | Core HeAP 342.40 s against 342.41 s with the checksum unchanged; the columns are small because contributions merge on arrival, which the design's reading of the profile missed |
| 2026-09-21 | 16.5 | The arch records a wire's binding on the wire and answers the binding API from it (kept); router2 keeps upstream's dict for its wire index (a flat mixed-hash table was slower) | Routed identity on the probe and the core in every variant; router2 90.8 s against 99.2 s with step A and 99.8 s with both, side by side; a hash that preserves the fabric's locality beats one that avoids collisions |
| 2026-09-21 | closing | The six hot-path units are closed at three kept and three negative; further legaliser work is on the cost of the rules per bel, not on how often they are asked | Core flow 715 s to 575 s of CPU with every checksum unchanged; strict legalisation 389 s to 280 s, router2 143 s to 120 s; estimates held only where they were inclusive times of functions that stopped running |
| 2026-09-22 | 17 | The Mistral arch numbers its wires into slots and keeps their routing state by slot; router2 reaches a wire's index through the slot when an arch offers one, and keeps its search queues between arcs; the four-way heap is not kept | Byte-identical on the core and the probe; resumed router2 on the core 89.8 s to 67.6 s; the four-way heap moves the route through tied seed entries |
| 2026-09-22 | 18 | The annealer's cluster moves go through the swap seam whenever it is on (the default): planned on an overlay, certified on the moved cells' bels, committed only when accepted; the cached timing weight is not kept | Byte-identical on the probe and the core, shadow over 7 million core chain moves with no mismatch, core annealer about 80 s to 70 s; the timing weight gained nothing, its profile share was skid |
| 2026-09-23 | policy | Lift byte identity as the acceptance rule for placement and routing changes, at the user's direction; determinism and legality stay exact; judge changes over five core seeds with `mistral/tests/quality.py`: every seed routes, no seed below the baseline's worst, quality changes lift the median Fmax by more than the baseline's spread, speed changes stay inside its range and are faster; opt-in until a promotion row | The refactor through design 18 held every result to the byte and so could not move the Quartus gap (11.39 MHz on 28,652 ALMs against 25.18 MHz on 20,576) |
| 2026-09-23 | 19.1 | The four-way router heap is not kept: over five seeds it routes one seed fewer and lowers the median Fmax; reverted | Speed rule REJECT; 12% faster per iteration, more iterations |
| 2026-09-23 | 19.2 | Keep `--sa-row-weight` and `--sa-entry-weight` opt-in and unpromoted: five seeds route where the baseline routes four and every gap measure moves the right way, but the Fmax gain (+0.27 to +0.58 MHz) is inside the baseline's spread | Quality rule REJECT for promotion; kept because it pays on routability and composes with the density levers |
| 2026-09-23 | 19.6 | Keep `--heap-lab-affinity`/`--heap-lab-reach` opt-in: five seeds route and entries per net fall to 1.40, but the gain (+0.58 MHz) is inside the spread and cells per LAB do not rise | Quality rule REJECT on its own; part of the Fmax set |
| 2026-09-23 | 19.3 | The first criticality cost (every wire in nanoseconds) is negative; 19.3b keeps a non-critical wire at one unit, routes every seed, and is kept opt-in | First form: no seed routed; 19.3b: median +0.48 MHz, inside the spread |
| 2026-09-24 | Fmax set | **Promote** 19.2 (rows 4, entries 4), 19.6 (affinity 2, reach 5), HeAP timing weight 100, and 19.3b (threshold 0) into the core recipe (`quality.py` configuration `core`, `core_probe_flow.sh`), at the user's direction; defaults unchanged. The set's five-seed run (`f-w100-crit-5s`) is the new core baseline | Quality rule ACCEPT: median 11.17 to 12.59 MHz, every seed routes, worst 12.41 above the old best; exponent 4 lost a seed and is not in the set |
| 2026-09-24 | policy | A routing-only change, measured from the base's route-prepared checkpoints so every seed keeps its placement, is judged seed by seed (`quality.py compare --kind routing`): every seed the base routes must still route and gain Fmax on its own placement, and the placements must match; at the user's direction | The seed spread of the quality rule measures placement variance, which such a change does not have; 19.9 gained on all five seeds and failed the spread rule |
| 2026-09-24 | 19.9 | **Promote** the timing repair (`--router2-repair-rounds 2 --router2-repair-crit 0.5`) into the core recipe at the user's direction; defaults unchanged | Routing rule ACCEPT: +0.80, +1.35, +0.35, +0.62, +0.20 MHz; median 12.59 to 13.28 |
| 2026-09-25 | 20.0 | Order design 20: complete register packing (20.3) and LUT input permutation (20.2) before LAB clustering (20.1) | nextpnr's rules admit 346 of Quartus's 3,219 LABs; lifting the second-register refusal and the input count admits 2,771; hinted runs with the rules as they are do not route |
| 2026-09-26 | platform | Compare quality only within one platform; the Linux runner has its own baseline (`aws-base`, median 13.11 MHz, spread 0.26) | Linux and macOS place the same inputs differently; determinism holds on each |
| 2026-09-26 | 20.2 | **Promote** `--lut-permutation` into the core recipe (`quality.py` configuration `core`, `core_probe_flow.sh`), at the user's direction; default unchanged; baselines with it are `lperm-5s` (macOS) and `aws-perm` (Linux); the silicon checksum waits for the board | Quality rule ACCEPT on both platforms (13.28 to 14.09 MHz, 13.11 to 13.69); routing rule REJECT on both for seed 2 alone (-0.16, -0.02); `lut_perm_check.py` 7,341 of 7,341 LUTs correct, with a mutation check |
| 2026-09-26 | 19.4 | Step 2 passes for the second register beside a LUT of its half (fed by the LUT, 4 inputs, or through E/F beside 2 inputs; with or without the first register); every failing build has one in a half without a LUT; 20.3 admits the second register only in a pack-time cluster with the LUT that drives it, never as a free placement | F7 `match=1`, control F8 `match=0`, F9 (alone) `match=1`; FB to FE and E0 fail; E0 shows the annealer leaving a register alone after moving its LUT |
| 2026-09-26 | 20.3 | Admit the second register of a half behind `--alm-both-registers` (off by default), only as a register-packing cluster child beside the LUT that drives it, one control set per ALM; the writer sets the other half's clock, clear, and synchronous-clear selects from it | Silicon: E1 (widths 1 to 5), EC (the option's own build) pass with controls; the cause of every earlier failure, the other half's clock select, found (E9, EB against EA); verify mode zero mismatches |
| 2026-09-26 | 20.3 | Close 20.3 for the core: `--alm-both-registers` stays opt-in, restricted to LUTs that drive only registers (a LUT with both registers of its half taken may have no free fabric port); it packs 0 registers on the core | 3,980 candidate registers sit on about 400 broadcast LUTs, one extra each at most, almost all with mixed control sets; unrestricted, no core seed routed |
| 2026-09-26 | 20.1 | The first form of LAB clustering (hints, solver pull) is negative; keep `--lab-clustering` and `--lab-cluster-pull` opt-in and revisit after 20.3 | LABs unchanged at about 4,160 in every form; median 12.15 against 13.11, and two seeds lost with the pull; the input count refuses most additions |
| 2026-09-16 | 3a | Compute a reuse plan with reasons before applying anything, and validate each decision again when applying | Plans for both controlled edits name exactly the edited cells with the right reason |
| 2026-09-16 | 3b | Region expansion releases transplants by growing radius around the dirty cells, then everything, each retry from the pre-placement RNG state | Forced ladder: 3,606 then 5,844 then 2,126 then the rest; the last rung is the clean placement |
| 2026-09-16 | 3a | Typed build states in C++ with runtime adoption at the legacy boundary; a bitstream needs a validated build | `--rbf` on an unrouted design is refused instead of writing a meaningless file |

## Stage gates and promotion

| Gate | Status | Promotion state |
| --- | --- | --- |
| Stage 1: Rust preparation plans | Complete | Legacy default; Rust preparation authority available only by explicit mode, and only with `--lab-legality legacy` (the complete evaluator owns the control check) |
| Stage 2: boundary optimization | Complete (2C performance target rejected) | Single-search capture, reduced decoder temporaries, and direct output promoted |
| Stage 3: complete LAB evaluation | Complete; **promoted 2026-09-19** | The Rust authority is the default in Rust builds (`--lab-legality rust`), legacy in Rust-disabled builds and by `--lab-legality legacy`; shadow and verify remain the harness |
| Stage 4: transactions and reuse | Complete for the Stage 4 scope (4A–4E); cross-build checkpoints and artifact provenance are the next design | Serial transaction authority and owned frozen batches enabled; `--placer-lookahead`, `--lab-reuse`, and `--reuse-placement` available, all off by default and not promoted |
| Stage 5: seams and checkpoints | Candidate list complete: 1c, 4b (retired), 2a, 2b, 3c, 3b, 3a; closing measurement recorded; 3c-2 (router2 bind order), 3c-3 (route survival), and 3c-4 (history seeding) landed from it | `--sa-seam on` is the default since 2026-09-19 (byte-identical, 8% faster annealing); `--sa-batch`, `--checkpoint`, `--resume`, `--route-prepare-only`, `--reuse-routes`, `--reuse-routes-history`, `--reuse-plan-out`, `--reuse-dry-run` available, off by default, unpromoted; 3c-2 is a default-path fix that is byte-identical for the clean flow |
| Stage 6: density | 6a (legaliser stall exit), 6b (ALM pairing), 6c (demand-weighted spreading), 6e (congestion-driven spreading), 6f (the router's share measured; re-route and unit cost), 6g (register packing), and 6h (the row cost) complete; 6d (line pre-assignment) built, measured negative, removed; the crate stays concluded; **the full core places and routes to completion** (2026-09-18, 15 minutes wall, 9.8 to 11.9 MHz signoff against Quartus's 25.2), and the next units are a hybrid base cost, the LAB-level assignment, and timing-driven placement quality | `--alm-pairing`, `--spread-demand`, `--spread-congestion`, `--router2-reroute`, `--router2-unit-cost`, `--register-packing`, `--row-cost` available, off by default, unpromoted; the stall exit is on the default path and byte-identical for designs that fit |
