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

Since the Darwin 27 update, `xcrun` resolves the SDK to the Command Line Tools copy (MacOS 27.0),
whose `.tbd` stubs the installed linker cannot parse (`tapi error: malformed file ... unknown
architecture`). Xcode's own SDK still matches the linker, so export
`SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`
before any `cmake --build`; no reconfigure is needed.

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

# Rust workspace (rust/Cargo.toml: nextpnr, example_printnets, npnr_mistral_lab, npnr_mistral_lab_ffi, npnr_mistral_monitor)
cargo test   --manifest-path rust/Cargo.toml --offline --workspace
cargo clippy --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab -p npnr_mistral_lab_ffi -p npnr_mistral_monitor --all-targets -- -D warnings
cargo fmt    --manifest-path rust/Cargo.toml -p npnr_mistral_lab -p npnr_mistral_lab_ffi -p npnr_mistral_monitor -- --check  # not --all: it reformats upstream rust/nextpnr
git diff --check
mistral/tests/gate.sh   # all of the above, both gtest suites, the probe identity, clang-format on changed files; 58 s

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
by one, so compare reports and log checksums as well. Compare reports only between runs with the
same `--rbf` presence: bitstream generation re-runs a signoff timing analysis before the report is
written, so a `--rbf` run's report differs from a run without it.

The silicon-work designs (PLL, two-PLL, CLKBUF, M10K, M10K dual-clock, MLAB shapes, DSP,
IO register, SDRAM IO) live in the sibling checkout `/Users/jvindahl/Development/ext/openflow-test/`
(Verilog plus `constraints/slot2*.qsf`; flow: `yosys -q -p "read_verilog X.v; synth_intel_alm
-family cyclonev; write_json X.json"`). Their synthesised JSON and the checkpoint validation
driver `validate_design.sh` sit in `build/stage5-validation/fixtures/`, outside git.

The end-to-end regression design is **Fabi386** (i386 core, ~48k ALUTs, DE10-Nano
`5CSEBA6U23I7`). Its JSON/QSF inputs live outside git in `build/fabi386-inputs/`
(`f386_exec_probe_nodsp.json`, `exec_probe.qsf`). They were reconstructed on 2026-09-16 after a
reboot wiped the original `/private/tmp` copies; the tracker's baseline section records how and
proves the rebuilt pair reproduces the reference artifacts. Never keep them only in a temporary
directory. A/B runs are compared with `cmp` on `--write` JSON and `--report` JSON, e.g.

```sh
./build/rust-enabled/nextpnr-mistral --device 5CSEBA6U23I7 --json build/fabi386-inputs/f386_exec_probe_nodsp.json \
   --qsf build/fabi386-inputs/exec_probe.qsf \
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
  `checkpoint.cc` (Stage 5, 2a/2b) persists and restores the packed, placed, route-prepared, and
  routed phases.
- `rust/npnr_mistral_lab` is the pure evaluator (`model.rs`, `rules.rs`, `v2.rs`, wire formats);
  `rust/npnr_mistral_lab_ffi` is the C ABI consumed by `mistral/lab_v2_abi.h` /
  `lab_control_abi.h`. Rust owns only validated values and scratch; C++ owns the live design,
  all mutation, and signoff. No live C++ pointer may cross FFI or sit in a cache. The crates were
  **concluded** at this contract in Stage 4 (tracker: "Rust evaluator concluded") and reopened
  once, on 2026-09-18 at the user's direction, for a performance revision: `ResidentLabs`
  (`rust/npnr_mistral_lab/src/resident.rs`) keeps a snapshot per LAB that the arch patches one bel
  at a time (`BelPatchV2`, net ids as run-stable keys, trial patches held in view for one
  evaluation, commits on second sight, the arch's ALM input count carried as a fact and recomputed
  only in the harness modes) with the control rules on a resident mirror of the control model's
  snapshot, through `npnr_mistral_resident_v2_*` and `mistral/lab_resident.*` (`Arch::lab_bel_dirty`
  and `lab_bel_refacts` are marked from the LAB-version hooks); `--lab-legality rust|shadow|verify`
  all evaluate through it, and the capture path stays as the parity harness in shadow and verify.
  The live monitor (`rust/npnr_mistral_monitor`, a pure renderer, and the `monitor` module of the
  FFI crate) is the one other Rust surface, added 2026-09-19 at the user's direction; it reads
  counters and renders, and never touches the netlist. Add no other Rust surface without a
  recorded decision.
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
  serial search. On the full core (2026-09-18) the lookahead is byte-identical and 10% slower,
  and `--lab-legality rust` on the capture path was an order of magnitude slower than the serial
  C++ path (the whole-LAB value capture per check, about 1.6 µs, not Rust); the resident path
  (2026-09-19, tracker "Rust legality at parity") brings it to the legacy wall time on the probe and
  1.4 times the legacy placement on the core (59 ns per query over 5.36 billion), byte-identical,
  and the annealer should run on the overlay seam (`--sa-seam on`) in that mode. The option travels in `ArchArgs`, not `ctx->settings`, on purpose: interning a
  new settings key shifts `IdString` indices and changes both the log checksums and the routed
  JSON net numbering, which would break A/B comparisons against retained artifacts.
- Stage 5 (1c-A): `--sa-seam off|shadow|on` routes `placer1` refinement swaps through a detached
  assessment (`Arch::overlay_bels_legal` on a `BelOverlay`, cost delta from the annealer's position
  overlay). Do not assess swaps by freezing V2 records: measured 3.8x slower. Shadow mode is the
  oracle for this path. `--sa-batch N` (1c-B) is the batched, deterministic-across-workers policy;
  it is not byte-identical to serial (acceptance draws share the RNG stream with location draws),
  quality is inside the seed spread, and it does not scale past two workers because the annealer
  is owner-bound. The worker pool both consumers use is `common/place/placement_pool.*`.
- Stage 4E reuse is also opt-in: `--lab-reuse shadow|on` caches LAB-level legality sub-results
  behind per-LAB binding versions and a global facts epoch (`mistral/lab_reuse.*`, active only
  inside `Arch::place()`); `--reuse-placement prev.json` transplants previous BELs onto cells
  with an identical name and signature (`mistral/placement_reuse.*`), and HeAP's constraint
  placer validates them. Reuse runs re-route from a different RNG state, so compare quality,
  not bytes.
- Stage 5 (2a/2b) checkpoints: `--checkpoint file.json` writes the output JSON plus a
  `nextpnr_checkpoint` object for the last completed phase (packed, placed, route-prepared with
  `--route-prepare-only`, or routed); `--resume file.json` replaces `--json`, restores it, and runs
  the remaining phases (a routed resume goes straight to signoff and `--rbf`). `Arch::route()` is
  split into `prepare_route()` and the router for this. `mistral/checkpoint.cc` implements the
  `BaseCtx` hooks. A resumed run is byte-identical to the uninterrupted run because
  the checkpoint replays the IdString table and every `dict`/users iteration order; a plain reload
  of nextpnr's own JSON reproduces neither (design doc section 2.6). Nothing between a checkpoint
  write and the netlist import may intern a string, which is why `write_module` looks up
  `"module"` without interning.
- Stage 5 (3c) route reuse: `--reuse-routes prev.json` (a routed checkpoint or any routed output)
  preserves a previous run's route for every net whose endpoints, wires, and pips still check out
  under the current design, bound strong and registered with router2 as pre-routed arcs. router2
  still rips a pre-routed arc up when it becomes overused (about 18% of applied routes are
  re-routed on the controlled edits), so the reuse report counts applied and surviving routes
  separately and a `--reuse-plan-out` plan is rewritten after routing with `survived` per net;
  invalid routes are never preserved (`mistral/route_reuse.*`). `--reuse-routes-history H` seeds
  router2's history cost on every preserved wire so dirty nets route around them from the first
  iteration; at H = 8 the edits keep 99% of applied routes and route in a third of the time (off
  by default, `Router2Cfg::prerouted_hist_cost`). It composes with
  `--reuse-placement`. On router failure every preserved route is dropped and the router reruns
  from the same RNG state (`MISTRAL_ROUTE_REUSE_FORCE_FALLBACK` forces that path).
- Stage 5 (3a/3b): both reuse paths compute a `ReusePlan` (`mistral/reuse_plan.*`) before applying
  it; `--reuse-plan-out plan.json` writes every cell and net decision with its reason and
  `--reuse-dry-run` applies nothing. Placement reuse retries on placer failure by releasing
  transplants within a growing radius of the dirty cells, then everything
  (`MISTRAL_PLACEMENT_REUSE_FORCE_FALLBACK=n` forces n failures). `mistral/build_state.*` is the
  typed phase machine: `Arch::place()`/`route()` and the bitstream writer adopt the context into the
  phase they need and run the typed transitions; `--rbf` on an unrouted design is now an error.
- Stage 6 (6a): HeAP's strict legaliser stops with a report once the rip-up radius covers the
  device and the queue stops shrinking (`PlacerHeapCfg::stall_rounds`, `report_infeasible`; the
  Mistral report prints LAB input occupancy and ALM pairing density). It is on the default path
  and never triggers on a design that fits.
- Stage 6 (6b): `--alm-pairing 1|2|3` pairs plain LUTs into ALM clusters at pack time under the
  checker's rule (`mistral/alm_pairing.*`, `Arch::getClusterPlacement` puts a pair on one ALM).
  Level 1 (shared inputs) is the one to use; 1.37 to 1.74 cells per ALM on the probe, and the full
  core places with it. Off by default. The remaining wall is the LAB input-line classes (A/C 25,
  B/D 21, E 22, F 24 of 46), which the count of 42 cannot see; see the design doc section 9.
- Stage 6 (6c): `--spread-demand` makes HeAP's cut spreader weigh comb cells by unique inputs
  (`PlacerHeapCfg::get_cell_spread_units`, four units per bel). Off by default; it clears the paired
  probe's stubborn wire and trims the full core's routing plateau by 10% without converging it.
- Stage 6 (6e): `--spread-congestion` closes a placement-routing loop inside HeAP: every spreading
  pass rebuilds a bounding-box wire-density estimate from the current positions
  (`PlacerHeapCfg::on_spread_begin`, `Arch::rebuild_spread_inflation`) and inflates LAB cells in
  tiles above `MISTRAL_SPREAD_CONGESTION_K` times the mean. Best measured configuration on the full
  core with pairing (router plateau 35% below pairing alone), not convergent; off by default. Do not
  stack it on `--spread-demand`: the two inflations together leave the legaliser without room.
- Stage 6 (6f): the router's share of the Quartus gap is measured, not assumed. On the identical
  exec-probe netlist nextpnr uses 2.8 times Quartus's fabric wires with a placement of lower
  wirelength: LAB input lines are fed by row wires (88% of their inputs), so a vertical hop is a
  two- or three-wire stair, and registers seldom pack with their LUTs (16% against 95%). The
  full core sits at 67% fabric use against Quartus's 24%; six router2 negotiation variants from one
  checkpoint move the plateau 13% either way, a unit wire cost halves it, and none converges. `--router2-reroute N` (every arc re-routed
  every N iterations, `--router2-reroute-contested` limits it to nets on wires with history) and
  `--router2-unit-cost` (one unit per wire instead of its delay) are landed opt-in; router2's
  heatmap set gained `_utilisation_by_tile_<iter>.csv`. A checkpoint resumes only under the same
  common command-line options (`--router2-max-iter` and the other `--router2-*` settings intern
  before the table replays), so router experiments from a checkpoint use the `MISTRAL_R2_*`
  environment block in `Arch::run_router_phase`. The next unit is the placement cost model
  (design doc section 9.5).
- Stage 6 (6g): `--register-packing` packs a register into the ALM half of the LUT that drives it
  as a cluster child (`mistral/register_packing.*`), placed by the same `Arch::getClusterPlacement`
  override as 6b's pairs; one register per LUT (the arch admits one per half), plain LUTs only. It
  runs after `assignArchInfo` because the packer asks the LAB control model whether a cluster's own
  registers can share a LAB (a pair whose registers cannot is unplaceable and was rejected at every
  ALM until HeAP's timeout). With it, `PlacerHeapCfg::cluster_units_by_bucket` makes the cut
  spreader weigh a cluster by the members of the pass's bucket only. Off by default. On the core it packs 5,360 of 9,632 LUT-driven registers; the router plateau is 15% worse
  under the delay cost and a third of 6f's unit-cost plateau (1,964 against 5,694) under
  `--router2-unit-cost`.
- Stage 6 (6h): `--row-cost W` weighs a vertical tile W horizontal tiles in HeAP's solver, cut
  spreader (cut along the axis longer in cost units), and strict legaliser (box W times wider than
  tall, candidates scored by weighted distance) through `PlacerHeapCfg::anisotropic`; the fabric
  enters LABs through row wires and a vertical hop costs 2.5 times a horizontal one. On the probe
  the placement's shape moves to Quartus's and column wires fall by a fifth to a half, at more
  router iterations and a few percent of Fmax. Off by default. On the full core it is the lever that
  turns the router's plateau into a descent: with pairing, congestion spreading, register packing, the
  unit wire cost, and `--router2-reroute 20 --router2-reroute-contested`, **the core routes to
  completion** (0 overused wires at iteration 45; first time, 2026-09-18), 15 minutes wall, signoff
  9.8 to 11.9 MHz against Quartus's 25.2; the recipe is `build/stage6-fullcore/core_probe_flow.sh`
  (outside git). router1's fallback never finishes on the core; the periodic re-route is the finisher.
- Many experimental knobs are `getenv`-driven (`MISTRAL_LAB_INPUT_LIMIT`, `MISTRAL_HEAP_BETA`,
  `NEXTPNR_ROUTER2_DUMP_OVERUSE`, the signoff report switches `MISTRAL_SIGNOFF_TEMP|EST|BOUND`,
  the router2 experiment block `MISTRAL_R2_*`, the graph dump `MISTRAL_DUMP_LAB_LINES=x,y`, the
  cluster rejection log `MISTRAL_DEBUG_CLUSTER_REJECT`,
  and ~35 `VUP_*` clock/IO/PLL debug switches in `mistral/`).
  `rg getenv mistral` before adding another.
- `--telemetry file.json` writes the run's phase, checksum (the one the log prints), device, the
  Stage 5 and 6 options, the LAB legality, resident, and control-set counters, cell and net
  counts, and the placement and routing wall times as JSON after placement and again after
  routing (`mistral/telemetry.*`, an `ArchArgs` option). It is the machine-readable record;
  `--report` is untouched by it.
- `--monitor` is the live dashboard of a run (needs a terminal on stdout and the Rust build; it
  warns and runs without itself otherwise): phases with their clock, the LAB legality, resident,
  and control-set counters with the query rate, the placer's and router's progress read from
  their log lines (router2's overused wires as a sparkline), and the log tail. The session
  (`mistral/monitor.*`) replaces the log's terminal streams with a hook, keeps the `--log` file
  stream, and renders four times a second from a ticker thread that reads only atomics and the
  phase clock; the frame is Rust (`rust/npnr_mistral_monitor`). The last frame stays on the
  terminal when the run ends. The run itself is unchanged: the probe's checksums and report are
  byte-identical with and without it.

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
- `docs/mistral-checkpoint-design.md` — Stage 5 units 2a/2b: checkpoint field audit, format, restore
  order, section 2.6 (what a reload of nextpnr's own JSON loses), and the byte-identity gate: a
  resumed run's `--write` JSON, `--report`, and log checksums equal the clean run's.
- `MISTRAL_GAPS.md` — silicon-verified findings for the DE10-Nano flow (PLL, IO registers, HPS
  bridges, DSP, M10K, router deadlock mechanisms). Several "obvious fixes" recorded there were
  tried and reverted; check it before re-deriving one.

State: Stages 1–3 and 4A–4E complete for the Stage 4 scope; Stage 5 has 1c (swap seam, batched
refinement), 4b (retired), 2a/2b (checkpoints for all four phases), 3c (route reuse), 3b
(placement region expansion), 3a (reuse plan, typed build states), and 3c-2 (router2 binds only
after every net's previous binding is ripped up, which removed every bind-time failure on the
reuse runs and is byte-identical for the clean flow) and 3c-3 (route survival measured and
reported) and 3c-4 (`--reuse-routes-history`, history seeding for preserved routes) done; Stage 6
(density) has 6a (legaliser stall exit), 6b (`--alm-pairing`), 6c (`--spread-demand`), and 6e
(`--spread-congestion`), 6f (the router's share measured; `--router2-reroute`,
`--router2-unit-cost`), 6g (`--register-packing`), and 6h (`--row-cost`) done; 6d (LAB input-line pre-assignment) was built, measured negative, and
removed, its record and landing commit in the tracker. Every new capability
is off by default and unpromoted. Hard rules that still apply: the
serial search order and RNG stream are the reference, every reuse path must be validated against
full recomputation, and nothing may silently certify a partial result.

## Conventions

- Commit subjects are `<area>: <summary>` (`mistral:`, `router2:`, `heap:`, `docs:`,
  `MISTRAL_GAPS:`). Rust-touching changes run the full test/clippy/fmt block above in
  `build/rust-enabled`; changes to `common/` or fallback paths also run the Rust-disabled `build/`.
- Assertions in `mistral/delay.cc` and similar signoff guards are correctness guards with silicon
  evidence behind them; do not downgrade them to warnings.
- Run `mistral/tests/gate.sh` before every commit. A change on a legality path (the LAB rules,
  the resident protocol, `lab_v2*`, `lab_resident.*`) also records a verify-mode placement of the
  probe, or of the core when the protocol changed, in the tracker; a follow-up commit is fine.
- The two lab crates deny `unwrap`, `expect`, `panic`, and `unreachable` outside tests: answer
  with typed errors and call statuses, and keep the FFI's `catch_unwind` as the backstop. A new
  shape of patch the arch sends goes into the resident module's list and the oracle generator in
  the same change.
- Stats lines are `name: key=value` and stable across builds, and nothing logs per query;
  `--telemetry` is the machine-readable form. `--report` stays byte-identical for the same run.
- A test that creates cells or nets in the shared fixture context takes them down again; the
  fixture's teardown fails the test otherwise.
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
