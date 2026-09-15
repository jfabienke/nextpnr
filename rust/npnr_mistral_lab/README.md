# Mistral LAB control evaluator

This crate implements `LegacyControlRulesV1` from nextpnr's Mistral backend. It checks FF control sharing and the existing greedy DATAIN allocation for forty physical FF slots. It does not check complete LAB legality or apply placement/routing changes.

The library allows `std`, forbids unsafe code, and has no third-party runtime dependencies. The current bounded algorithm uses fixed storage and does not allocate during evaluation. This is a local implementation choice, not a restriction on using the heap or useful crates elsewhere. JSON support is a development dependency used by test/replay drivers.

```rust
use npnr_mistral_lab::{ControlLabSnapshot, evaluate, wire::LabControlsV1};

let raw = LabControlsV1::default();
let snapshot = ControlLabSnapshot::try_from(&raw)?;
let assessment = evaluate(&snapshot);
let result = assessment.to_wire();
```

`ControlLabSnapshot` owns validated data; later changes to the raw record cannot affect it. Typed assessments borrow their source snapshot, and callers cannot construct a legal allocation or substitute another snapshot during serialization. The result's epoch is copied provenance, not a freshness check.

`evaluate_wire` combines decoding and evaluation for value records. It returns `BadSnapshot` or `UnsupportedRules` separately from `ControlIllegal`. It accepts no raw pointers and is not an FFI entry point. The [synchronous bridge](../npnr_mistral_lab_ffi/README.md) isolates pointer handling and panic containment in a separate crate.

## Validation

From the repository root:

```sh
cargo test --manifest-path rust/Cargo.toml -p npnr_mistral_lab --target-dir build/lab-rust
cargo clippy --manifest-path rust/Cargo.toml -p npnr_mistral_lab --all-targets --target-dir build/lab-rust -- -D warnings
python3 mistral/tests/compare_lab_controls.py --cxx /path/to/c++
```

Add `--offline` to use cached Cargo dependencies. The differential runner needs a C++17 compiler; on this macOS development host, use `--cxx /usr/bin/c++`. It builds the actual C++ kernel and the Rust replay example as separate processes, compares every result field, and checks native sizes, alignments, and offsets. It does not require a device database. Pass `--ffi-driver` with the CMake-built `nextpnr-mistral-lab-ffi-replay` executable to additionally test native C++/Rust calls on the same corpus.

The corpus includes the repository's golden fixtures, exhaustive reduced domains, random controls, invalid transport values, net-ID renaming, and FF permutations. Renaming must preserve results after translating IDs. FF permutations are compared separately to the C++ reference because the legacy greedy rule can change verdict with physical order.

`build/lab-rust-parity/summary.json` records case counts, source/input hashes, compiler information, and layouts. A disagreement writes `mismatch.json` with the input and both results; generation is reproducible from the recorded seed. Temporary bulk inputs/results are removed after the run. The workspace retains its existing policy of ignoring `Cargo.lock`; the local resolved lockfile hash is included in the summary.

Unit tests cover boundary errors, resource limits, provenance, input independence, and eight readers of a frozen snapshot. Compile-fail doctests check invalid construction, bypassing validation, mutation, and snapshot lifetimes. Representation assertions are object-size checks; peak stack use and process RSS require separate measurements.

See the [implementation plan](../../docs/mistral-lab-legality-plan.md) for completed gates and the remaining integration work.

## Profiling

The [profiling report](../../docs/mistral-lab-legality-profile.md) compares the live
C++ worker, detached C++ reference, and Rust bridge on captured Fabi386 queries,
including allocation counts and repeated full P&R runs. Build the
`nextpnr-mistral-lab-bench` CMake target and the bridge crate's `lab_control_bench`
release example, then run `mistral/tests/profile_lab_controls.py` as documented
there. JSON parsing and live-state reconstruction are outside component timings.

`--lab-controls-profile <corpus.jsonl>` enables bounded live query sampling for
serial profiling runs. Its 4,096-record reservoir and JSON output add overhead;
leave this option off when comparing full P&R wall time and peak RSS. Generated
corpora contain numeric snapshot-local identities and belong under `build/`.
