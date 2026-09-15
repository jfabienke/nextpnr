# Mistral LAB next-stages tracker

Execution record started 2026-09-15. Architectural rationale belongs in
[`mistral-lab-next-stages-design.md`](mistral-lab-next-stages-design.md); this
file records implementation state and evidence.

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
| 4D: deterministic parallel evaluation | 4C | In progress | Reproducible decisions, zero stale commits, bounded retries, measured scaling | — |
| 4E: incremental reuse | 4D | Blocked | Incremental results match full recomputation and final signoff | — |

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

Unit 4D is active. The next boundary is deterministic proposal sequencing using
the existing C++ scheduler: freeze proposals and RNG decisions serially, evaluate
owned batches concurrently, consume in sequence order, and reject stale commits
against the global revision with two bounded retries before synchronous fallback.
The first 4D throughput benchmark is now in place; live scheduling and stale-retry
integration remain outstanding.

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
1,280,000 record evaluations per phase. Two independent seven-round runs produced
the following midpoint of their run medians:

| Path | Workers | Median ns/record | Throughput | Speedup vs frozen 1-worker |
| --- | ---: | ---: | ---: | ---: |
| Direct V2 FFI, validates every call | 1 | 807.66 | 1.24 M/s | 0.49x |
| Rust-owned frozen V2 | 1 | 392.19 | 2.55 M/s | 1.00x |
| Rust-owned frozen V2 | 2 | 199.21 | 5.02 M/s | 1.97x |
| Rust-owned frozen V2 | 4 | 102.67 | 9.74 M/s | 3.82x |
| Rust-owned frozen V2 | 8 | 54.28 | 18.42 M/s | 7.22x |

Creation plus destruction costs 504.92 ns per retained record, or 32.32 us for
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
| `./build/rust-enabled/mistral/nextpnr-mistral-lab-frozen-bench build/stage4-validation/frozen-v2-scaling.csv 7 20000` | Complete; 1/2/4/8-worker scaling measured |
| Independent seven-round repeat | Complete; medians reproduced within 3.0% |
| `/usr/bin/time -l ... frozen-v2-scaling-rss.csv 3 20000` | Complete; peak RSS 4,227,072 bytes |

CSV SHA-256: primary
`2ef2385c2d8601f3de58de2744b354103d8c4ab43868fed882b4bd34cee70ddb`;
repeat `a3fb6cf20e4341abb732549ab386ec09b3f031e51a54e16d6172105b6b62847d`;
resource run
`41cacb1abac1c0cdfec9f896853838e555d6b0d34d1d3799646a862db723717a`.

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
| 2026-09-15 | 4D | Retain validated V2 facts when a frozen batch will be reused | 807.66 ns/record direct versus 392.19 ns frozen; creation amortizes after two serial evaluations |
| 2026-09-15 | 4D | Continue with bounded parallel scheduling | Frozen evaluation scales 1.97x, 3.82x, and 7.22x at 2/4/8 workers; live determinism remains gated |

## Stage gates and promotion

| Gate | Status | Promotion state |
| --- | --- | --- |
| Stage 1: Rust preparation plans | Complete | Legacy default; Rust preparation authority available only by explicit mode |
| Stage 2: boundary optimization | Complete (2C performance target rejected) | Single-search capture, reduced decoder temporaries, and direct output promoted |
| Stage 3: complete LAB evaluation | Complete | Explicit shadow, verify, and Rust authority modes; legacy remains default |
| Stage 4: transactions and reuse | In progress (4A–4C complete; 4D active) | Serial transaction authority and owned frozen batches enabled; parallel scheduling not promoted |
