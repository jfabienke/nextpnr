# Upstream candidates from the Cyclone V branch

Status: inventory, 2026-09-15. Nothing here has been offered upstream; this
records what could be, in what shape, and what must stay on this branch.

Against upstream `main`, the branch changes eight files under `common/`
(542 insertions, 136 deletions). Everything else is under `mistral/`,
`rust/`, `docs/`, and `build/`-side tooling. The generic changes fall into
four pieces. Each is independent of the Mistral backend, keeps the default
behaviour byte-identical (evidence in the tracker), and could be extracted as
its own pull request. They are interleaved with Mistral commits in history,
so extraction means a fresh branch off upstream `main` with the hunks below
applied, not a cherry-pick.

## 1. Object-carrying mutation hooks (kernel)

Files: `common/kernel/basectx.h`, `common/kernel/nextpnr_types.cc`,
`common/kernel/basectx.cc`. Branch commits `1261e6d` (kind-only hook),
`7fd9482` (object-carrying forms).

`BaseCtx` gains a `notifyContextMutation(ContextMutationKind)` hook, raised by
the kernel's own mutators (port and parameter changes, connect/disconnect,
renames, constraints, object creation), plus `notifyCellMutation(CellInfo *,
kind)` and `notifyNetMutation(NetInfo *, kind)` whose defaults forward to the
kind-only hook. Every existing backend sees no-ops. A backend that caches
anything derived from the netlist can override them to invalidate precisely;
Mistral uses them for per-LAB legality reuse and for revision stamps on
speculative placement transactions.

Upstream value: a documented invalidation point for architecture-side caches,
which today rely on convention. Risk: none for backends that do not override.
Extraction: about 60 lines; the tracker's Stage 4A and 4E entries are the
audit of which mutators raise which kind.

## 2. HeAP strict legaliser: rollback fix, candidate seam, lookahead

Files: `common/place/placer_heap.h`, `common/place/placer_heap.cc`. Branch
commits `1261e6d`, `bf30a82`, `9c7e56f`, plus `9c49b79` and `940ab61`.

Four separable parts, in order of general usefulness:

- **Rollback strength fix.** `try_place_cluster` restored displaced cells
  with `STRENGTH_WEAK` regardless of their original strength. The branch
  records the original strength in `HeAPDisplacedBinding` and restores it
  (`restore_heap_cluster_bindings`). This affects later rip-up eligibility and
  therefore the search trajectory; it is a correctness fix worth offering on
  its own, with the note that it changes results for any arch using clusters.
- **Linear cell-placement timeout and progress heartbeat.** The per-cell
  attempt limit was quadratic in design size (`cells^2 / divisor`), which
  measured as a 45-minute-plus livelock on one infeasible carry cluster at 54k
  cells; it is now linear (`cells * 1024 / divisor`, floor 10,000). A heartbeat
  every 2,000 legalised cells reports queue depth and rip-up radius. Both are
  small and independent.
- **Candidate/commit split.** `try_place_cluster` is factored into
  `build_cluster_candidate` (no mutation), the live bind/check/revert path,
  and `finish_cluster_move` (bookkeeping), and the random location step into
  `next_random_location`. Behaviour is unchanged; this is what makes the next
  part possible and is the natural home for any future move-evaluation hook.
- **Transaction and lookahead seams.** `PlacerHeapCfg` gains two optional
  callbacks: `place_cluster_transaction` (evaluate and commit one candidate
  without provisional binding) and `place_cluster_transactions` (a batch, with
  `clusterLookahead` candidates speculated ahead in exact serial order and the
  RNG/radius state restored after the commit). With neither set, the placer is
  byte-identical to before. An arch that can evaluate a move against a frozen
  snapshot gets deterministic parallel legalisation for free.

Upstream value: the fix and the timeout are bug fixes; the seams are a
general capability with no cost when unused. The Mistral coordinator behind
the seams stays on this branch.

## 3. router2: congestion knobs, iteration cap, overuse dump

Files: `common/route/router2.h`, `common/route/router2.cc`,
`common/kernel/command.cc`. Branch commits `571ee4e`, `f6ac2f1`, `00cf98c`.

- `--router2-init-curr-cong`, `--router2-hist-cong`,
  `--router2-curr-cong-mult`, `--router2-estimate-weight`: the existing
  hard-coded weights exposed as settings.
- `--router2-crit-weight-floor` (default 0.05, the old constant) and
  `--router2-present-cong-floor` (default 0, the old behaviour): present
  overuse is a legality constraint, but its cost was scaled by criticality,
  so a non-critical net barely felt an overused wire and could oscillate
  forever with spare capacity. The floor decouples legality pressure from
  timing. Measured on wide HPS interfaces; documented in the gaps file.
- `--router2-max-iter` (default 0, unlimited): give up with a warning instead
  of iterating forever on a stuck design.
- `NEXTPNR_ROUTER2_DUMP_OVERUSE`: name the still-overused wires at an
  iteration, which is how the TD-mux deadlock was diagnosed.

Upstream value: all defaults preserve behaviour; the present-congestion
floor is the one substantive router change and should be offered with the
measurement, not as a default.

## 4. A scheduling observation for `parallel_refine`

Not a diff. `common/place/parallel_refine.cc` partitions work statically per
thread and spawns threads per iteration. The branch's frozen-evaluator
benchmark showed that on heterogeneous cores (Apple M1 Ultra: 16 performance
plus 4 efficiency) a static partition scales to 12.2x at 16 workers because
the slowest thread finishes 17 to 43 percent after the fastest, while dynamic
claiming of coarse units reaches 14.8x at 16 and 16.1x at 20. The lesson
transfers; the benchmark itself (`mistral/tests/lab_frozen_bench.cc`) does
not, because it measures the Rust FFI.

## What stays on this branch

- Everything under `mistral/`: the LAB legality pipeline (V1/V2 ABIs,
  detached evaluators, preparation tickets and edits, transactions, revision
  stamps, the lookahead coordinator with parallel freezing, assessment and
  placement reuse), IO-register packing, DSP, dual-clock M10K, PLL delivery,
  HPS bridges, and the `VUP_*` and `MISTRAL_*` environment knobs. All of it
  depends on libmistral's `nextpnr-latest` branch and on the DE10-Nano
  silicon results in `MISTRAL_GAPS.md`.
- The Rust crates under `rust/npnr_mistral_lab*`, concluded at their current
  ABI as a parity oracle (tracker: "Rust evaluator concluded").
- The validation corpus and scripts under `build/stage*-validation/`, which
  are deliberately not committed.

## How to extract a piece

```sh
git diff main...cyclonev-compress-default -- common/kernel/basectx.h \
    common/kernel/basectx.cc common/kernel/nextpnr_types.cc      # piece 1
git diff main...cyclonev-compress-default -- common/place/placer_heap.h \
    common/place/placer_heap.cc                                    # piece 2
git diff main...cyclonev-compress-default -- common/route/ common/kernel/command.cc  # piece 3
```

Apply the hunks to a fresh branch off upstream `main`, rebuild `generic` and
one Lattice target, and confirm byte-identical output on their example
designs before and after. For piece 2, expect the rollback fix to change
placements on cluster-heavy designs; that is the point of it.
