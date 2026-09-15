# LAB control evaluator performance and memory

## Scope and method

This profiles the Milestone C implementation on the Fabi386 execution-stage
ALUT-only slice, not the complete CPU. It compares the original live C++ worker,
the detached C++ reference, and the Rust evaluator. Rules and dispatch policy
were not optimized for this measurement.

The host is macOS 26.6.2, arm64; Apple Clang 17 builds C++17 with `-O3` and
ThinLTO, and Rust 1.98 builds the bridge in Cargo's release profile. The full
P&R comparison uses one Rust-enabled executable, switching only `--lab-controls
legacy` / `rust`. It therefore excludes binary/link-layout differences between
Rust-enabled and Rust-disabled builds. C++ still applies preparation allocations
in both modes.

Device/settings: `5CSEBA6U23I7`, seed 1, `--threads 1`, HeAP, Router2, 12 MHz,
`--router2-max-iter 100`. Inputs:

- JSON SHA-256: `3cdb742d5b5b85b2905e7957d622c2ec64ab3adc8053ddad80ec7d4c23f54bd3`.
- QSF SHA-256: `67c27631ac1b6eac92e056914d758fd842b09db9a765b8fe46f904338098bcca`.
- Corpus SHA-256: `ee54b557fada0614caa5b5c9daef5a86f5a35bd6b6973235ae92bcfa46bb4e86`.

An opt-in deterministic reservoir captures 4,096 of 1,513,411 live FF-control
queries. It includes 1,825 legal records, 2,271 illegal records, and 16 preparation
records. Sampling uses a private hash of query order, without advancing the
placement RNG. This samples queries with their observed multiplicity, not unique
LAB states. Sampling and JSON output are disabled in every timed comparison run.
The illegal sample contains 42 synchronous-clear conflicts and 2,229 DATAIN
conflicts; other rejection categories remain covered by correctness tests, not
this workload's performance measurements.

The C++ microbenchmark reconstructs each record using real bound FFs and nets in
LAB0. Before timing, it checks exact snapshot reconstruction, recorded live
verdict/plan agreement, detached C++ agreement, complete C++/Rust result agreement,
and host acceptance. Rebinding, parsing, and correctness checks are excluded from
timing. Each phase runs 128 times per record over seven rounds; phase order rotates
by record and round, with compiler barriers and full-result consumption.

These are **warm-input microbenchmarks**, with compact, reused live objects. They
do not reproduce the cache behavior of a dispersed live design. Components are
measured separately; their times need not add exactly because compilation,
inlining, temporaries, and harness boundaries differ. Builds and benchmarks run
serially, with no deliberate competing workload or CPU affinity control.

## Component timings

Median over seven rounds, weighted by the captured query distribution:

| Operation | ns/query |
| --- | ---: |
| Existing live C++ verdict | 146.09 |
| Existing C++ worker plus plan export | 149.59 |
| Capture live state into transport and net map | 160.28 |
| Detached C++ validation, rules, and result | 132.74 |
| Rust through the actual C++→Rust ABI, one record | 174.66 |
| Host result validation | 24.13 |
| Complete Rust dispatch, including capture, validation, counters | 363.27 |
| Empty C++ harness | 0.45 |

The complete experimental Rust path is **2.49×** the existing live C++ query in
this warm workload. Comparing detached evaluators on the same transport records,
Rust through FFI is **1.32×** the detached C++ reference.

A separate Rust harness splits the same corpus further:

| Rust component | ns/query |
| --- | ---: |
| Decode and validate transport into typed snapshot | 116.98 |
| Evaluate an already validated snapshot | 28.55 |
| Encode an existing assessment into the result DTO | 11.82 |
| Combined safe wire evaluator | 167.91 |
| Complete ABI entry called from Rust | 174.52 |

Capture and typed decoding dominate this small evaluator's cost. The agreement
between the C++-called and Rust-called ABI measurements supports the attribution;
the ABI envelope adds much less than materializing and validating each snapshot.
The 28.55 ns rules number excludes boundary work and must not be compared directly
with the full live C++ query as evidence of a Rust speedup.

## Repeated full P&R

The runner collects seven measured pairs after one excluded warmup pair,
alternating order between pairs. Wall time uses a monotonic clock; CPU time and
per-child peak RSS use POSIX `wait4`. Each process starts fresh. Routed JSON hashes,
timing/utilization reports, and Rust error/fallback counters are checked on every
run. The separate capture run is excluded.

| Mode | Median wall (s) | Wall range (s) | Median peak RSS (MiB) | RSS range (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Legacy C++ | 34.981 | 34.828–35.224 | 1,966.17 | 1,963.59–1,977.17 |
| Rust | 35.453 | 35.299–36.881 | 1,968.12 | 1,964.48–1,977.98 |

Rust was slower in all seven pairs. The median **paired** wall-time increase is
**1.27%**, with a paired median-ratio bootstrap interval of **+0.65% to +2.12%**
(10,000 resamples, fixed seed). This small single-host sample supports a modest
regression on this design; the interval does not cover systematic machine effects
or predict other designs. The ratio of the two unpaired medians is a different
statistic. The largest slowdown coincided with slower Router2 as well as placement,
so it cannot all be attributed to the evaluator.

The median paired RSS difference is **+2.05 MiB** for Rust, with individual
differences from **−9.05 to +11.44 MiB**. There is no demonstrated memory saving.
Median user CPU time is 41.12 s for C++ and 41.55 s for Rust; median system time is
about 0.70 s for both. `--threads 1` does not disable HeAP's independent X/Y solver
threads, which explains why accumulated CPU time can exceed wall time.

The existing stage timers provide useful context:

| Stage | C++ median (s) | Rust median (s) |
| --- | ---: | ---: |
| HeAP placement | 7.22 | 7.47 |
| Simulated-annealing refinement | 12.08 | 12.27 |
| Router2 | 9.41 | 9.39 |

These stage medians do not cover initialization, packing, all routing passes, or
output, and their independent medians need not sum to median total time. Multiplying
the warm-query overhead by 1.51 million calls suggests roughly 0.33 s of additional
work, of the same order as the full-run change; this is an extrapolation, not a
live accumulated evaluator timer.

All 16 comparison runs (including warmups) and the capture run produced routed
SHA-256 `7ed738af6eedd35d36e826ef8364918777d2b2ded50cebe895903b2bf57c9fb0`.
Timing and utilization matched, with 35.871864 MHz achieved. Each Rust run reported
1,513,411 evaluations, including 4,191 preparation checks, with zero errors,
fallbacks, or diagnostics. Prior strict verification and per-record comparisons
establish evaluator parity; Rust-authority counters alone do not.

Results are recorded in `build/lab-profile-fabi386/summary.json`, `stage-times.json`,
and the full per-run manifest.

## Memory

Every measured phase recorded **zero hot-path allocations** across 3,670,016 calls
per phase. The C++ harness replaces scalar/array/aligned `operator new`; the Rust
harness wraps `GlobalAlloc`, including allocation, zeroed allocation, and
reallocation. These are separate allocator domains: the C++ counter does not
intercept Rust allocation, and neither counter is a whole-process malloc tracer.
Setup, parsing, corpus storage, diagnostics, and panic paths are excluded. The
evaluated normal C++ paths contain no direct malloc calls.

Only full P&R peak RSS is comparable between implementations here. The C++
microbenchmark loads a device database to reconstruct live state; the Rust
component harness does not, so their whole-process RSS figures measure different
setup workloads.

| Object | Bytes on this host |
| --- | ---: |
| Original C++ worker | 192 |
| Rust worker | 72 |
| Host capture, including pointer translation map | 3,552 |
| Wire input, included in host capture | 1,952 |
| Additional typed Rust snapshot | 1,024 |
| Typed assessment | 56 |
| Wire result | 216 |

The smaller Rust worker does **not** establish a memory reduction: the original
worker already avoids heap allocation, while the boundary introduces additional
temporary representations. The profiling reservoir itself reserves 8,912,896
bytes; it is used only in the excluded capture run.

Static arm64 stack reservations were inspected in the exact measured P&R binary
using `llvm-nm` and `llvm-objdump --disassemble-symbols=...`. Saved prologues are in
`build/lab-profile-fabi386/profile-disassembly.txt`:

| Function | Own stack reservation, bytes |
| --- | ---: |
| Shared `Arch::is_lab_ctrlset_legal` entry | 3,824 |
| Original `LabCtrlSetWorker::run` | 128 |
| Detached C++ evaluator | 544 |
| Host capture | 128 |
| Rust ABI entry | 336 |
| Rust `evaluate_wire` | 2,160 |
| Rust snapshot decoder | 1,328 |
| Rust rules evaluator | 208 |
| Rust result encoder | 240 |

The shared entry reserves space for the inlined Rust dispatch even when legacy
mode is selected. Its legacy worker call has 3,952 bytes of these application
frames active; its Rust decoder call has at least 7,648 bytes active
(3,824 + 336 + 2,160 + 1,328). These are static frame observations, not measured
dynamic peak stack usage or whole-process stack bounds: callees, callers,
library routines, and exceptional paths can add more. Frame sizes include saved
registers and compiler temporaries and must not be added again to object sizes.

## Implications for the plan

Keep C++ as the default. This change provides a detached, typed, testable boundary;
the current serial implementation does not demonstrate a performance or memory
win. Rust memory safety alone cannot reduce allocations that C++ already avoids.

The next performance experiment should reduce repeated capture and decoding,
while retaining boundary validation and exact error semantics. Candidate work is
eliminating duplicate net-map searches in capture, then reusing validated frozen
snapshots within a correctly versioned transaction. Cache reuse requires the
freshness tracking in Milestone F; copying an epoch into a result is insufficient.
Parallelizing a roughly 29 ns rules pass by itself is unlikely to amortize
scheduling overhead. Broader candidate work or batches are the meaningful units
to investigate. These are proposed experiments, not measured optimizations.

## Reproduction

Build the existing Release configuration with Rust and tests enabled, then:

```sh
cmake --build build/rust-enabled --target nextpnr-mistral nextpnr-mistral-lab-bench --parallel 8
cargo build --manifest-path rust/Cargo.toml --offline --release \
  -p npnr_mistral_lab_ffi --example lab_control_bench
python3 mistral/tests/profile_lab_controls.py \
  --binary build/rust-enabled/nextpnr-mistral \
  --cpp-bench build/rust-enabled/mistral/nextpnr-mistral-lab-bench \
  --rust-bench rust/target/release/examples/lab_control_bench \
  --input "$LAB_TEST_JSON" --qsf "$LAB_TEST_QSF"
```

The output directory must be fresh. Defaults are seven pairs, seven microbenchmark
rounds, and 128 calls per record/phase. The manifest preserves exact commands,
input/corpus/binary/source hashes, the tracked working-tree patch, counters,
CPU/wall time, RSS, and output hashes. CSV files retain per-round and per-rejection
reason timings and allocation counts. The synthesized input/corpus and bulk
outputs remain under ignored `build/`, outside source control.

Validation passed: 20 native tests with Rust enabled, 16 with Rust disabled,
22 Rust tests/doctests, strict Clippy for both crates/all targets, and formatting
checks. The new reservoir regression checks bounded sampling, reproducibility,
live-state preservation, and unchanged placement RNG state. Final benchmark
validation also replays all 4,096 records and checks malformed-input diagnostics.
`validation.json` records these checks. The measured C++ benchmark executable and
source are preserved because a subsequent malformed-JSON error-path fix changed
the tool, without changing timed operations or the measured P&R executable.
