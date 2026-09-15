# Mistral LAB control FFI

This static library exports the synchronous `npnr_mistral_eval_controls_v1` function declared in [lab_control_abi.h](../../mistral/lab_control_abi.h). It depends only on the safe evaluator and Rust's standard library, without the older `nextpnr` Rust wrapper.

## Buffer contract

Each call evaluates 0–64 records. Count zero succeeds without inspecting pointers. Other calls check count, output capacity, null pointers, alignment, address overflow, and overlap, in that order. Envelope errors leave outputs untouched; only the first `count` output slots are written. Extra capacity does not participate in the operation.

For envelopes that pass these checks, the caller must supply fully initialized inputs in one live allocation and exclusive, writable output storage in a disjoint active range within one live allocation. Outputs may initially be uninitialized. No concurrent input mutation or output access is allowed. Numeric checks cannot establish pointer liveness or allocation bounds. The bridge retains no pointer and transfers no ownership; callers may reuse or release buffers after return.

Raw transport structs contain integers and fixed arrays. Semantic validation happens in the safe crate. Outputs use raw pointer writes so uninitialized host storage is never exposed through a Rust reference. Unsafe operations and their caller obligations are documented beside each use.

## Failure policy

The call status describes transport success; each result independently reports legal, illegal, bad snapshot, or unsupported rules. A Rust panic invalidates the whole batch: all active outputs are reset to `InternalError`, with request and epoch echoed. Even a panic while dropping a panic payload is contained; a secondary payload is intentionally leaked in that exceptional case. Normal calls retain and allocate no memory.

The bridge requires `panic=unwind` and rejects abort profiles at compile time. Aborts, invalid memory, allocation failure, foreign exceptions, and an aborting/panicking global panic hook cannot be recovered by `catch_unwind`. No C++ callbacks run inside the boundary.

## Host modes

Build Mistral with `-DBUILD_RUST=ON`; Corrosion imports this library only when Mistral is enabled. Rust-disabled builds preserve the legacy path and reject Rust mode selection explicitly.

| CLI mode | Behavior |
| --- | --- |
| `--lab-controls legacy` | Default; original worker, no snapshot/FFI overhead. |
| `--lab-controls shadow` | Compare Rust, detached C++, and the live worker; return the legacy decision after disagreements. |
| `--lab-controls verify` | Same comparisons; disagreement or boundary failure ends the run. |
| `--lab-controls rust` | Experimental Rust control verdict, checked result shape/provenance; explicit unsupported rules fall back to legacy and increment a counter. |

Preparation still applies the original C++ worker's allocation in every mode. Shadow/verify compare preparation plans first. Full LAB checks, LUTRAM control reservations, routing, and mutation remain in C++. `--verify-lab-controls` retains the separate C++-only verification option and cannot be combined with a Rust mode.

Nonlegacy modes report query/preparation counts, outcomes/reasons, errors, disagreements, and fallbacks. At most four full mismatch records are emitted per Context; records use snapshot-local numeric identities, carry both results and the first differing field, and are not retained in memory. Stats use atomics, but this does not authorize capture from a concurrently changing design. Live capture is synchronous; epoch zero grants no freshness or cache certificate.

## Validation

```sh
cargo test --manifest-path rust/Cargo.toml -p npnr_mistral_lab -p npnr_mistral_lab_ffi
cargo clippy --manifest-path rust/Cargo.toml -p npnr_mistral_lab -p npnr_mistral_lab_ffi --all-targets -- -D warnings
cmake -S . -B build/rust-enabled -DARCH=mistral -DBUILD_RUST=ON -DBUILD_TESTS=ON -DMISTRAL_ROOT=/path/to/mistral
cmake --build build/rust-enabled --target nextpnr-mistral nextpnr-mistral-test nextpnr-mistral-lab-ffi-replay --parallel
ctest --test-dir build/rust-enabled --output-on-failure
python3 mistral/tests/compare_lab_controls.py --ffi-driver build/rust-enabled/mistral/nextpnr-mistral-lab-ffi-replay
```

Rust tests exercise envelope failures, uninitialized output ownership, mixed/maximal batches, input immutability, panic recovery, and concurrent calls with separate outputs. An isolated allocation counter tests 100,000 repeated calls. Native host tests additionally cover full-result parity, malformed result rejection, mode failure policies, the diagnostic cap, and MLAB reservations. See the [implementation plan](../../docs/mistral-lab-legality-plan.md) for live-run evidence and remaining gates.
