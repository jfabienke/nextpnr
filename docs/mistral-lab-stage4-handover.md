# Mistral LAB Stage 4 handover

## Purpose and current state

This document is the restart guide for the Mistral LAB legality and placement
work. Detailed evidence and architectural decisions remain in
[`mistral-lab-next-stages-tracker.md`](mistral-lab-next-stages-tracker.md), and
the original rationale remains in
[`mistral-lab-next-stages-design.md`](mistral-lab-next-stages-design.md).

Handover point:

- Branch: `cyclonev-compress-default`
- Commit: `16a233e0c19b732554594c5f4d4df52b18ef4dba`
- Stages 1, 2, and 3 are closed.
- Stage 4A, 4B, and 4C are complete.
- Stage 4D is active.
- Stage 4E is blocked on 4D.
- Legacy LAB legality remains the default. Rust authority is opt-in.
- Parallel placement evaluation has not been enabled in production.

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
```

Artifact hashes and the resource run are recorded in the tracker. Benchmark
outputs belong under `build/` and are intentionally not source-controlled.

## Remaining critical path

### Stage 4D: deterministic parallel evaluation

This is the immediate next unit. The implementation must preserve candidate and
RNG order exactly; concurrency is allowed only for detached evaluation.

Required sequence:

1. Generate candidate proposals and all RNG decisions serially in the existing
   HeAP order.
2. Prepare and freeze complete candidate transactions while the serial owner has
   the necessary live context.
3. Form Rust-owned batches of no more than 64 candidates.
4. Submit immutable ranges through the existing C++ scheduler. Each worker must
   own its output and scratch.
5. Consume assessments strictly by proposal sequence, never completion order.
6. Before commit, check the global session/revision stamp and every expected
   binding owner and strength.
7. On stale work, regenerate and retry at most twice. After two stale retries,
   evaluate that proposal synchronously against current state.
8. Record proposals, accepted/rejected decisions, stale results, retry counts,
   synchronous fallbacks, and commits.

Important constraint: preparing many candidates from one revision means the
first successful commit invalidates later candidates under the current global
revision policy. Batching must therefore include a deliberate retry strategy;
benchmark throughput alone does not solve commit serialization. Do not introduce
fine-grained dependency versions until global-revision correctness is proven.

Stage 4D exit evidence:

- Identical decision traces for the selected deterministic policy.
- Zero stale commits.
- No more than two asynchronous stale retries per proposal.
- Identical final placement/routing artifacts at 1, 2, 4, and 8 workers.
- 12/16-worker measurements where the workload supplies enough parallel work.
- Fabi386 plus carry, MLAB, odd-FF, E/F-input, and policy-boundary fixtures.
- End-to-end wall time, placement time, routing time, RSS, retry counters, and
  final timing/utilization—not evaluator microbenchmarks alone.

### Stage 4E: incremental reuse

Start only after Stage 4D is reproducible and stable.

Required work:

1. Track `DirtyLab`, `EvaluatedLab`, `PreparedLab`, and routed dependencies as
   distinct states.
2. Begin with one current versioned assessment per LAB.
3. Add reverse incidence from net and cell changes to affected LABs.
4. Invalidate precisely on controlled connectivity, fact, constraint, binding,
   generated-object, and routing changes.
5. Reuse placement outside conservative dirty repair regions.
6. Compare unchanged and controlled-edit incremental builds with full
   recomputation.
7. Add a bounded content cache only after versioned per-LAB reuse is correct.
8. Continue full control preparation and routing until artifact provenance and
   teardown rules are implemented.

Stage 4E exits only when incremental and full recomputation agree on legality,
connectivity, timing, utilization, routing, and final artifacts.

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

## Default-mode and promotion policy

Legacy remains the default. Completing a stage gate does not automatically
promote Rust authority, parallel evaluation, performance QoS, or incremental
reuse. Each promotion requires its own evidence-backed decision, with a simple
fallback path retained until full P&R parity and operational stability are
established.
