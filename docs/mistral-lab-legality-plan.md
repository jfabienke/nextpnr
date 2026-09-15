# LAB Legality Evaluator Implementation Plan

This plan implements the [LAB evaluator design](mistral-lab-legality-design.md) in reviewable increments. Each increment preserves the existing legality rules until a separately reviewed rule change. C++ retains authoritative placement and preparation state.

The [four-stage follow-on design](mistral-lab-next-stages-design.md) expands the
remaining D–F work into preparation tickets and edits, boundary-cost experiments,
full-LAB query contracts, and versioned transactions for parallel/incremental use.
It is a design proposal; the completion statuses below remain unchanged.

## Milestone A: capture and independent C++ reference (complete)

Deliver a usable verification path before introducing Rust:

- [x] Define a C-compatible, versioned input/result layout with fixed arrays, explicit status/reason codes, and compile-time layout assertions.
- [x] Capture all forty physical FF slots from normalized `ffInfo`, preserving disconnected polarity, first-encounter net identity, and global-net classification.
- [x] Implement a detached C++ evaluator with input validation, legacy-order greedy allocation, structured conflicts, and no allocations in evaluation.
- [x] Export legal allocations from the existing worker without changing its algorithm; compare verdicts and all twelve allocation entries.
- [x] Add `--verify-lab-controls` as a C++ reference verification option. Ordinary builds continue using the existing worker. Verify both placement control queries and preparation checks.
- [x] Add a named-field JSON replay writer, reader, and fixtures. Record numeric IDs as snapshot-local data; preserve 64-bit request/epoch values without JSON number precision loss.
- [x] Register Mistral unit tests for normalization, all physical slots, capacities, greedy order, malformed inputs, deterministic snapshots, and generated differential cases.
- [x] Build the Mistral executable and tests; run verification on the Fabi386 execution-stage slice, checking queried candidates and preparation plans.

Files: [C ABI](../mistral/lab_control_abi.h), [detached evaluator](../mistral/lab_model.cc), [capture and verification](../mistral/lab_legality.cc), [replay reader/writer](../mistral/lab_replay.cc), and [tests](../mistral/tests/lab_legality.cc). The shared C header is under `mistral/`; Rust crates are introduced in B/C. Fixtures live in `mistral/tests/fixtures` so this change does not modify the separate `tests` submodule. The existing JSON library is used only in replay/diagnostic code, outside the evaluation kernel.

Completion evidence: no live/reference mismatches, exact legal-plan agreement, passing boundary/rule tests, a successful verification run, and recorded limitations. Do not promote the detached evaluator into the default legality decision in this milestone.

## Milestone B: pure Rust evaluator (complete)

- [x] Add `npnr_mistral_lab` with no dependency on the live `nextpnr` wrapper and `#![forbid(unsafe_code)]`.
- [x] Decode transport values into private net/slot/polarity types and immutable owned snapshots.
- [x] Implement the same bounded two-pass rules and structured failures.
- [x] Replay A's fixtures, enumerate reduced domains, and compare randomized cases against the independent C++ reference. Test deterministic results and bijective ID renaming; FF permutation must preserve reference behavior, not necessarily the verdict.
- [x] Assert representation budgets and compile-fail checks for invalid construction/access.

The [Rust crate](../rust/npnr_mistral_lab/README.md) allows `std`, has no third-party runtime dependencies, and exposes value evaluation separately from FFI. Its typed assessments borrow the source snapshot; callers cannot construct legal allocations or substitute another snapshot when converting results. Raw records remain explicitly unvalidated value transport. The initial `no_std` choice was removed during C: there is no embedded target requirement, and heap use/crate selection should follow practical needs and measurements. The small current evaluator still uses fixed storage.

The [differential runner](../mistral/tests/compare_lab_controls.py) builds the actual detached C++ reference and Rust replay driver as separate processes. It compares every result field and native layout without depending on the unfinished bridge or a device database. JSON and subprocess allocation are confined to test tools. A backend-local ignore exception now keeps the existing JSON fixtures visible to Git.

Gate: exact verdict/allocation parity, input immutability, no kernel allocations, and reported resource costs.

## Milestone C: synchronous Rust bridge and shadow mode (complete)

- [x] Add `npnr_mistral_lab_ffi` with audited pointer/length handling, output initialization, and panic containment.
- [x] Import/link the bridge only for Mistral through Corrosion. Exercise actual Rust-enabled and Rust-disabled executables.
- [x] Introduce `legacy`, `shadow`, `verify`, and experimental `rust` dispatch modes, retaining `--verify-lab-controls` for C++-only verification.
- [x] Compare Rust and live results on Fabi386 and dedicated clock/control/MLAB host fixtures. Cap diagnostic recording and report aggregate counters.
- [x] Test ABI layouts, malformed batches, buffer ownership, error distinctions, and repeated-call memory use.

The [bridge README](../rust/npnr_mistral_lab_ffi/README.md) specifies ownership, panic behavior, modes, and validation commands. C++ remains the default and owns preparation allocations/mutations in all modes. The experimental Rust mode decides only the FF-control verdict; it does not certify a complete LAB. No live-state capture concurrency or caching was introduced.

Gate: zero verification mismatches, bounded memory, working cross-language linkage, and no unhandled panic/exception crossing.

## Milestone D: serial authority and preparation

Profiling is complete for the Fabi386 execution-stage slice; broader validation
and Rust preparation-plan consumption remain pending. The
[profiling report](mistral-lab-legality-profile.md) records captured-query component
timings, allocation counts, static stack reservations, and seven paired full P&R
runs. Complete warm Rust dispatch is 363 ns versus 146 ns for the live C++ check;
median paired full P&R overhead is 1.27%, with no demonstrated memory saving.
The measured overhead is primarily capture and typed decoding. This evidence
does not justify default-mode promotion or parallelizing the rules pass alone.

Validate the experimental Rust FF-control authority across broader designs and profile capture, result validation, FFI, and kernel costs. In a separate change, allow C++ preparation to consume verified Rust allocations. Preserve selection order, disconnected-signal comparisons, and existing mutation loops. Recompute immediately before application until complete freshness tracking exists. Default-mode promotion requires this gate; adding the opt-in `rust` dispatcher in C does not promote preparation.

Gate: final placement, preparation, routing, timing, and resource validation; compare serial candidate ordering and deterministic artifacts. Report capture/FFI/kernel/end-to-end costs against both C++ implementations. A speedup is not required.

## Milestone E: complete LAB evaluator

Add a separate V2 snapshot for LUT/FF data accessibility, ALM bit/input/carry rules, LAB input accounting, and MLAB groups. Keep legacy scoped BEL queries distinct from whole-LAB assessment. Resolve input-limit policy in the host; recompute bounded input counts and compare them with the legacy cache.

Gate: rule-specific and scoped-query parity, including null/duplicate inputs, odd FF slots, E/F consumption, MLAB shortcuts, and signed policy boundaries.

## Milestone F: detached transactions and incremental reuse

Add frozen candidate overlays covering source/destination/displaced LABs and clusters. Track occupancy, cell facts, net properties, rules, and external move constraints. Serialize commits with freshness checks and deterministic ordering. Start with a conservative epoch; refine read sets only after all active mutation paths participate.

Then add dirty-LAB tracking and a byte-bounded cache. Keep legality, prepared state, and routed state separate; unchanged legality alone does not permit reusing reservations or pin maps.

Gate: rejected/stale proposals preserve committed state; competing proposals cannot claim the same resource; validated results at 1/2/4/8 workers where supported; clean-build comparison for incremental edits.

## Validation record

Milestone A was validated with the existing Release Mistral configuration, `BUILD_RUST=OFF`, and the repository's pinned test submodules initialized:

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build --target nextpnr-mistral nextpnr-mistral-test --parallel 8
ctest --test-dir build --output-on-failure
```

All 14 tests passed. Differential coverage includes 1,080 exhaustive reduced-domain cases, 3,000 seeded randomized cases, all forty FF slots, the full 200-net capture bound, packed normalization, and exact legal allocations. The C header also passed a C11 syntax/layout check: input 1,952 bytes; result 216 bytes.

Fabi386 validation used the execution-stage ALUT-only JSON and its clock/probe QSF, with seed 1, one thread, HeAP, Router2, a 12 MHz constraint, and a 100-iteration routing cap. Run the following twice, adding `--verify-lab-controls` for the second run and selecting separate output paths:

```sh
build/nextpnr-mistral --device 5CSEBA6U23I7 \
  --json "$LAB_TEST_JSON" --qsf "$LAB_TEST_QSF" \
  --seed 1 --threads 1 --placer heap --router router2 --freq 12 \
  --router2-max-iter 100 --write "$LAB_TEST_OUTPUT" --report "$LAB_TEST_REPORT"
```

Both runs completed without a verification mismatch. Routed JSON was byte-identical; timing and utilization matched. The reported maximum frequency was 35.871864 MHz. Observed wall times were 33.79 s for legacy and 31.69 s for verification; one pair of runs is correctness evidence, not a performance comparison.

- Source input SHA-256: `3cdb742d5b5b85b2905e7957d622c2ec64ab3adc8053ddad80ec7d4c23f54bd3`.
- QSF SHA-256: `67c27631ac1b6eac92e056914d758fd842b09db9a765b8fe46f904338098bcca`.
- Identical routed JSON SHA-256: `7ed738af6eedd35d36e826ef8364918777d2b2ded50cebe895903b2bf57c9fb0`.
- Local run manifest, logs, reports, source/binary hashes, and outputs: `/private/tmp/nextpnr-lab-validation-6xc2qtz_/`.

The Fabi386 test covers a slice, not the complete CPU; milestone A does not claim hardware validation or faster execution. Verification captures synchronously and makes no reusable epoch/freshness guarantee. The default decision and all preparation mutations still use the legacy worker.

### Milestone B evidence

```sh
cargo test --offline --manifest-path rust/Cargo.toml -p npnr_mistral_lab --target-dir build/lab-rust
cargo clippy --offline --manifest-path rust/Cargo.toml -p npnr_mistral_lab --all-targets --target-dir build/lab-rust -- -D warnings
cargo check --offline --manifest-path rust/Cargo.toml --workspace --all-targets --target-dir build/lab-rust
python3 mistral/tests/compare_lab_controls.py --offline --cxx /usr/bin/c++
```

All 15 Rust tests/doctests passed: seven unit tests, three replay/layout tests, one API example, and four compile-fail cases. The unit suite includes eight concurrent readers performing 1,000 evaluations each on one frozen snapshot. Clippy passed with warnings denied for the new crate. Workspace checking passed with three existing warnings in the older `nextpnr` wrapper.

The cross-language runner passed **15,137 records** with exact status, reason, allocation, blocker, and provenance agreement. Coverage includes 1,080 exhaustive cases, 10,000 randomized inputs, 1,000 bijective renamings, 1,000 FF permutations, 2,000 mutated records, fixed malformed cases, all FF slots, maximum net count, and both golden fixtures. It reached every implemented rejection and boundary-error category. Native sizes, alignments, and offsets matched for all five transport structures on this host.

Measured object sizes on this host:

| Representation | Bytes |
| --- | ---: |
| C-compatible input | 1,952 |
| Validated Rust snapshot | 1,024 |
| Rust worker state | 72 |
| Typed assessment | 56 |
| C-compatible result | 216 |

The typed snapshot is an additional bounded copy while decoding/evaluating the transport. These sizes do not measure peak stack use, whole-process memory, or a memory reduction relative to C++. Evaluation uses fixed values/arrays, with no heap allocations on its normal return paths; JSON drivers and the test harness do allocate.

- Corpus seed: `0x4c4142`.
- Corpus SHA-256: `08d66a67cd4ab239259a93bf81fdf81320f67b22f6697e2069b60c9e3215f7a6`.
- Local validation summary, source hashes, layout metadata, and compiler/build logs: `build/lab-rust-parity/`.

B established algorithm/type/layout parity through test processes. The following C evidence adds native linkage and live integration; it does not turn owned snapshot thread safety into permission to read a changing C++ design.

### Milestone C evidence

Validated on this macOS/Apple Silicon host with Apple C++17 and Rust 1.98.0. Both new crates also pass library checking on the declared Rust 1.85 minimum. Strict Clippy passes. The bridge deliberately fails compilation with `panic=abort`.

- **22 Rust tests/doctests pass**, including six bridge tests and one isolated allocation test. Coverage includes uninitialized outputs, 64-record mixed batches, invalid envelopes, input immutability, panic recovery, a panicking payload destructor, and eight concurrent callers with independent outputs.
- **100,000 four-record calls (400,000 evaluations) allocate zero times** in the isolated Rust allocator counter, covering legal, illegal, bad-snapshot, and unsupported-rule results. This is normal-call behavior, not a prohibition on using the heap elsewhere.
- **15 native tests pass with Rust disabled; 19 pass with Rust enabled.** Existing exhaustive/random live-capture tests exercise all three active Rust modes and complete detached-result parity. New tests verify failure policies, a four-record diagnostic cap, broken-capture detection, result-shape/provenance validation, MLAB group rejection, and unchanged LUTRAM write reservations.
- **15,137 corpus cases match through the actual C++→Rust ABI**, as well as the independent Rust and C++ replay processes. Every result field and native layout is compared; summary/source hashes are in `build/lab-ffi-parity/summary.json`.
- Rust-enabled **generic** also builds and its CLI runs. Its targets/link line include the existing example Rust integration and exclude the Mistral bridge. Pinned Corrosion's injected CLT SDK path is removed on current native macOS so the compiler's selected Xcode SDK supplies system libraries.

Reproduce the native corpus and live runs after building both configurations:

```sh
python3 mistral/tests/compare_lab_controls.py --offline --cxx /usr/bin/c++ \
  --ffi-driver build/rust-enabled/mistral/nextpnr-mistral-lab-ffi-replay \
  --output-dir build/lab-ffi-parity
python3 mistral/tests/validate_lab_controls_live.py \
  --off-binary build/nextpnr-mistral --rust-binary build/rust-enabled/nextpnr-mistral \
  --input "$LAB_TEST_JSON" --qsf "$LAB_TEST_QSF"
```

The Fabi386 execution-stage slice used A's input/QSF, device, seed, and routing settings. All five full P&R runs completed. Routed JSON was byte-identical across Rust-off legacy and Rust-on legacy/shadow/verify/rust; timing and utilization matched, with 35.871864 MHz achieved against 12 MHz. The Rust-off executable rejects each Rust-dependent mode before loading the device.

| Run | Wall time (s) | Peak RSS (MiB) |
| --- | ---: | ---: |
| Rust-off legacy | 34.20 | 1980.9 |
| Rust-on legacy | 36.44 | 1974.5 |
| Rust-on shadow | 34.55 | 1970.8 |
| Rust-on verify | 34.90 | 1963.8 |
| Rust-on rust | 33.88 | 1969.5 |

Each active Rust mode reports **1,513,411 evaluations**, including **4,191 preparation checks**: 650,567 legal and 862,844 illegal; zero errors, mismatches, fallbacks, or diagnostic records. Rejection counts are 16,275 synchronous-clear conflicts and 846,569 DATAIN conflicts. These are candidate checks, not that many distinct LABs. Shadow/verify compare against C++; authoritative Rust-mode counters alone do not prove agreement.

One run per mode is correctness evidence, not a speed or memory improvement claim. Peak RSS is measured per child with POSIX `wait4`; no memory savings are established by the small variations. Logs, input/source/binary hashes, timings, peak RSS, reports, routed outputs, and replayable commands are in `build/lab-live-4p1kcvio/manifest.json`. A final strict verification run after the diagnostic regression fix is recorded separately in `final-verification.json` in that directory.

The next milestone is D: broader authority/preparation validation and consuming verified Rust plans in the existing C++ application loops. Cross-platform ABI testing, peak stack measurements, Miri, full-CPU/hardware validation, complete LAB rules, transactions, and incremental reuse remain outside the completed C gate. Miri is not installed in the available toolchains; native tests do not constitute a formal proof of FFI soundness.
