# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this checkout is

A fork of YosysHQ/nextpnr (remote `origin`; push target is `fork` = jfabienke/nextpnr) whose active
work is the **Cyclone V (`mistral`) backend** on branch `cyclonev-compress-default` (~120 commits
ahead of `main`). Most sessions touch `mistral/`, `common/place/placer_heap.*`, `rust/`, and `docs/`.
The untracked `AGENTS.md` carries the same generic repo guidelines; the essentials are folded into
the "Repository guidelines" section below so this file stands alone.

## Build

Out-of-tree only (CMake rejects in-tree). Submodules must be initialised first
(`git submodule update --init --recursive`). Three configured build trees already exist:

| Dir | Config | Use |
| --- | --- | --- |
| `build/` | `-DARCH=mistral -DBUILD_TESTS=ON -DBUILD_RUST=OFF -DCMAKE_BUILD_TYPE=Release` | native C++ reference / fallback behaviour |
| `build/rust-enabled/` | same plus `-DBUILD_RUST=ON` | Rust LAB evaluator, FFI tests, benchmarks |
| `build/rust-generic/` | `-DARCH=generic -DBUILD_RUST=ON` | Rust bindings smoke build |

Both mistral trees use `-DMISTRAL_ROOT=/Users/jvindahl/Development/ext/mistral` (the libmistral
checkout, `nextpnr-latest` branch). Reconfigure example:

```sh
cmake -S . -B build/rust-enabled -DARCH=mistral -DBUILD_TESTS=ON -DBUILD_RUST=ON \
      -DCMAKE_BUILD_TYPE=Release -DMISTRAL_ROOT=/Users/jvindahl/Development/ext/mistral
cmake --build build/rust-enabled -j4            # nextpnr-mistral, nextpnr-mistral-test, benches
cmake --build build --target clangformat        # format with root .clang-format
```

Other arches: `-DARCH=ice40|ecp5|nexus|machxo2|generic|himbaechel` with the matching
`*_INSTALL_PREFIX` / `HIMBAECHEL_UARCH` options (see README.md). `BUILD_GUI`, `USE_OPENMP`,
`BUILD_PYTHON` (default ON) are the other relevant options in the top-level `CMakeLists.txt`.

## Tests

```sh
# C++ unit tests (gtest). TEST_SOURCES per arch are declared in <arch>/CMakeLists.txt.
cmake --build build/rust-enabled --target nextpnr-mistral-test -j4
./build/rust-enabled/nextpnr-mistral-test                     # all
./build/rust-enabled/nextpnr-mistral-test --gtest_filter='LabControlModel.*'   # one suite / test
ctest --test-dir build --output-on-failure                    # same tests via ctest

# Rust workspace (rust/Cargo.toml: nextpnr, example_printnets, npnr_mistral_lab, npnr_mistral_lab_ffi)
cargo test   --manifest-path rust/Cargo.toml --offline --workspace
cargo clippy --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab -p npnr_mistral_lab_ffi --all-targets -- -D warnings
cargo fmt    --manifest-path rust/Cargo.toml --all -- --check
git diff --check

# Backend self-check, arch regressions (tests/ is a submodule)
build/nextpnr-generic --uarch example --test
make -C tests/<arch>/regressions NPNR=$PWD/build/nextpnr-<arch>
```

Mistral-specific harnesses (all under `mistral/tests/`, built only with `BUILD_TESTS` + `BUILD_RUST`):
`nextpnr-mistral-lab-bench`, `nextpnr-mistral-lab-frozen-bench`, `nextpnr-mistral-lab-ffi-replay`,
and Python drivers `compare_lab_controls.py` / `profile_lab_controls.py` /
`validate_lab_controls_live.py` (Python 3.9+, need a C++17 compiler and cargo, no chipdb).
Fixtures live in `mistral/tests/fixtures/`. Benchmark and validation outputs go under `build/`
(`build/stage*-validation/`, `build/lab-profile-fabi386/`) and are never committed. When comparing
routed JSON across runs, strip the `creator` line and any settings lines that legitimately differ
(`threads`); a run that interns an extra `IdString` before the netlist is read shifts every net id
by one, so compare reports and log checksums as well.

The end-to-end regression design is **Fabi386** (i386 core, ~48k ALUTs, DE10-Nano
`5CSEBA6U23I7`). Its JSON/QSF inputs live outside the repo (ephemeral `/private/tmp/fabi386-pnr.*`;
sha256 of the baseline inputs is pinned in the tracker). A/B runs are compared with `cmp` on
`--write` JSON and `--report` JSON, e.g.

```sh
./build/rust-enabled/nextpnr-mistral --device 5CSEBA6U23I7 --json F.json --qsf F.qsf \
   --seed 1 --threads 1 --placer heap --router router2 --freq 12 --router2-max-iter 100 \
   --lab-controls legacy --write out.json --report out.report.json
```

## Architecture (big picture)

- `common/kernel/` holds the netlist (`Context`, `CellInfo`, `NetInfo`, `IdString` per-context pool),
  `common/place/` the placers (HeAP = `placer_heap.cc`, whose `StrictLegaliser` is the main
  Mistral hot path), `common/route/` the routers (`router2.cc` is the one used). Each arch dir
  implements the Arch API (`docs/archapi.md`): camelCase API methods, snake_case arch helpers,
  `pack()`/`place()`/`route()` plus `isBelLocationValid`-style legality checks. nextpnr packs
  lightly and relies on placement-time legality checking instead (`docs/coding.md`).
- `mistral/` is the Cyclone V arch on top of libmistral. Beyond the stock files, this fork adds:
  `dsp.cc` (MUL18X18), `m10k.cc`, `pll.cc`, IO-register packing in `io.cc`/`pack.cc`, `globals.cc`
  clock attestation, `vup_cram.cc`, and the **LAB legality pipeline**:
  `lab.cc` (legacy control-set worker) -> `lab_control_plan/edits/preparation` (Stage 1 ticket ->
  validated plan -> preflighted edits) -> `lab_model`/`lab_v2*` (V1/V2 detached evaluators and
  ABI) -> `placement_revision` (session/revision stamps, Stage 4A) -> `placement_transaction`
  (serial frozen transactions hooked into HeAP via `PlacerHeapCfg::place_cluster_transaction`,
  Stage 4B) -> `lab_frozen_batch` (RAII owner of Rust-owned immutable batches, Stage 4C).
- `rust/npnr_mistral_lab` is the pure evaluator (`model.rs`, `rules.rs`, `v2.rs`, wire formats);
  `rust/npnr_mistral_lab_ffi` is the C ABI consumed by `mistral/lab_v2_abi.h` /
  `lab_control_abi.h`. Rust owns only validated values and scratch; C++ owns the live design,
  all mutation, and signoff. No live C++ pointer may cross FFI or sit in a cache.
- Evaluator authority is opt-in via `nextpnr-mistral --lab-controls` and `--lab-legality`, each
  `legacy|shadow|verify|rust` (`mistral/main.cc`). **Legacy is and must stay the default**;
  promotion needs its own recorded evidence.
- Stage 4D parallel evaluation is opt-in via `--placer-lookahead N` (candidates speculated per
  clustered HeAP move) with `--threads W` workers. HeAP generates candidates in serial order and
  restores its RNG/radius state after a commit (`legalise_cluster_lookahead` in `placer_heap.cc`);
  `mistral/placement_coordinator.*` prepares on the owner, then workers freeze each candidate's
  overlay, create their own Rust handle, evaluate, and cross-check (parallel freezing); the owner
  commits strictly in proposal order. Workers may read the live design only because the owner is
  blocked during the parallel section and the capture path writes no shared state. Results are byte-identical to the
  serial search. The option travels in `ArchArgs`, not `ctx->settings`, on purpose: interning a
  new settings key shifts `IdString` indices and changes both the log checksums and the routed
  JSON net numbering, which would break A/B comparisons against retained artifacts.
- Stage 4E reuse is also opt-in: `--lab-reuse shadow|on` caches LAB-level legality sub-results
  behind per-LAB binding versions and a global facts epoch (`mistral/lab_reuse.*`, active only
  inside `Arch::place()`); `--reuse-placement prev.json` transplants previous BELs onto cells
  with an identical name and signature (`mistral/placement_reuse.*`), and HeAP's constraint
  placer validates them. Reuse runs re-route from a different RNG state, so compare quality,
  not bytes.
- Many experimental knobs are `getenv`-driven (`MISTRAL_LAB_INPUT_LIMIT`, `MISTRAL_HEAP_BETA`,
  `NEXTPNR_ROUTER2_DUMP_OVERUSE`, and ~35 `VUP_*` clock/IO/PLL debug switches in `mistral/`).
  `rg getenv mistral` before adding another.

## Current work and its documents

Read these before touching the LAB/placement work; they are the record of what has been
measured and decided:

- `docs/mistral-lab-stage4-handover.md` — the restart guide (current stage, key files,
  required validation, promotion policy).
- `docs/mistral-lab-next-stages-tracker.md` — implementation state and evidence log. **Update it
  in the same commit that changes a unit's status**; do not mark 4D/4E complete without the
  recorded exit evidence.
- `docs/mistral-lab-next-stages-design.md` — architectural rationale (with
  `mistral-lab-legality-design/plan/profile.md` and `parallel-incremental-design.md`).
- `MISTRAL_GAPS.md` — silicon-verified findings for the DE10-Nano flow (PLL, IO registers, HPS
  bridges, DSP, M10K, router deadlock mechanisms). Several "obvious fixes" recorded there were
  tried and reverted; check it before re-deriving one.

State: Stages 1–3 and 4A–4E complete for the Stage 4 scope; every new capability is off by
default and unpromoted. Next is a design for cross-build checkpoints and physical artifact
provenance (design doc section 6.8, levels three and four). Hard rules that still apply: the
serial search order and RNG stream are the reference, every reuse path must be validated against
full recomputation, and nothing may silently certify a partial result.

## Conventions

- Commit subjects are `<area>: <summary>` (`mistral:`, `router2:`, `heap:`, `docs:`,
  `MISTRAL_GAPS:`). Rust-touching changes run the full test/clippy/fmt block above in
  `build/rust-enabled`; changes to `common/` or fallback paths also run the Rust-disabled `build/`.
- Assertions in `mistral/delay.cc` and similar signoff guards are correctness guards with silicon
  evidence behind them; do not downgrade them to warnings.
- Untracked `AGENTS.md` and `mistral/tests/__pycache__/` are not part of the LAB work; leave them.

## Repository guidelines (from AGENTS.md)

- **Layout.** Shared code is `common/kernel`, `common/place`, `common/route`, `frontend`, `json`;
  backends are `ice40/`, `ecp5/`, `nexus/`, `machxo2/`, `mistral/`, `generic/`, `himbaechel/`;
  GUI in `gui/`, chipdb assembler in `bba/`, utilities in `python/`, docs in `docs/`, regression
  flows in `tests/` (submodule). Third-party code is vendored under `3rdparty/` or pulled as
  submodules. Build outputs stay in out-of-tree `build/`.
- **Style.** `clang-format` with the root `.clang-format`; match surrounding names and include
  order. Keep the Arch API's `camelCase`; arch-local helpers are `snake_case`. Keep architecture
  changes inside their backend and shared behaviour in `common/`.
- **Commits and PRs.** One focused commit per change with a `<area>: <summary>` subject. PRs state
  the behaviour change, affected architecture(s), external database requirements, and the
  validation run; link issues, add screenshots for GUI changes, and keep the relevant CI arch
  jobs green. PRs from this fork target `main`; `fork` is the push remote, `origin` is upstream.

## Documentation ownership for the LAB work

- `docs/mistral-lab-next-stages-design.md` owns architectural rationale. Do not put design
  arguments in the tracker.
- `docs/mistral-lab-next-stages-tracker.md` is the execution record (started 2026-09-15): unit
  status, exit evidence, hashes, commands, decision log. Every status change lands here.
- `docs/mistral-lab-stage4-handover.md` is the restart guide and must reflect the current branch,
  commit, and active unit when a stage boundary is crossed.
- `MISTRAL_GAPS.md` is for silicon-verified findings on the DE10-Nano flow, dated per entry.
