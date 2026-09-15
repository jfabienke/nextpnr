# Staged Design for Parallel and Incremental nextpnr

Status: proposal. The types, APIs, files, and options below are proposed additions, not existing capabilities. Initial implementation targets Mistral; other architectures retain their existing paths.

## Objectives and sequence

Make repeated FPGA builds reuse valid work and make independent optimization work safe to execute concurrently. Preserve existing placement/routing algorithms while replacing the state boundaries that limit both capabilities.

Rust adoption is primarily an ownership, concurrency, type-safety, and state-machine decision. Lower memory consumption is a design objective; faster execution is a possible benefit, not a prerequisite. Memory safety alone does not guarantee lower consumption or prevent all leaks. Measure resource use separately from safety guarantees.

| Stage | Main implementation | Usable outcome |
| --- | --- | --- |
| 1 | C++ placement snapshots and commits | Evaluate supported placement moves without mutating the live design |
| 2 | C++ checkpoint persistence and restoration | Resume a completed phase with complete Mistral state |
| 3 | Rust phase ownership and matching/dependency engine plus C++ repair adapters | Enforce supported build transitions and reuse placement/routing after an edit |
| 4 | Evaluate an isolated Rust-owned computation subsystem | Assess safety, memory use, maintainability, and execution cost |

The first incremental path consumes a newly synthesized netlist and repacks it completely. Incremental RTL synthesis and incremental packing are separate projects. Thus a large Yosys cost can remain even when nextpnr iteration becomes faster.

## Shared contracts

1. **One authoritative live design.** C++ owns committed cells, nets, physical bindings, and backend state. Rust initially owns analysis graphs and worker scratch space. Workers cannot mutate C++ objects.
2. **Snapshots have explicit lifetimes.** Capture at a phase barrier; freeze all referenced data for the computation. Changes are published between epochs. `const Context*` is not a snapshot because interning and lazy caches can mutate it.
3. **Separate identity from indexing.** Durable entity keys identify persisted objects. Dense indices address one snapshot. Neither `IdString::index`, pass-specific `udata`, pointer values, nor copied native struct bytes serve as cross-build identity.
4. **Configuration is data.** Resolve relevant environment switches, constraints, device/model versions, and algorithm settings before work starts. Include them in phase compatibility checks. No worker reads environment variables or interns names.
5. **Changes carry dependencies.** A change record names affected cells, clusters, LABs, nets, pin maps, and shared resources. Version changes cover every input read by an evaluator, including control-net properties and configuration.
6. **Compatibility is explicit.** Unsupported cases use the legacy serial path. Stale proposals are retried, ambiguous matches are rebuilt, and incomplete checkpoints are rejected. A search budget exhaustion is not proof of infeasibility.
7. **Types express phase and access rights.** Distinguish cells, nets, BELs, wires, and versioned snapshot identities. A worker receives read access to a frozen snapshot and exclusive ownership of its scratch state. Phase transitions consume the previous phase handle. Runtime checks still validate resource legality, provenance, and freshness.

### Memory ownership and budgets

Treat memory efficiency as a cross-stage requirement:

- Share immutable device topology rather than cloning it for each worker or build candidate. Mistral currently mixes topology, occupancy, and reservation flags; separate those representations before treating the graph as shareable. Initial snapshot adapters may only extract a supported subset.
- Use compact arrays and typed indices for bulk analysis data. Avoid a heap allocation per entity or iterator where a contiguous representation suffices. These layout improvements are possible in C++ too; Rust helps enforce the intended ownership and borrowing rules.
- Keep snapshots as a shared immutable base plus bounded changed-region data where practical. The initial copied Rust metadata is an explicit migration cost, not a permanent mandate to duplicate the entire design.
- Give workers reusable, capacity-limited scratch buffers. Measure per-worker growth and avoid duplicating device-wide arrays when the task only needs an active region.
- Release phase-local state once consumers finish. Retain only the provenance, reusable physical results, and bounded caches needed by subsequent builds; do not keep every epoch alive through shared ownership.
- Track peak process footprint, live allocations, retained capacities, snapshot duplication, and bytes per entity/worker. Allocator retention means releasing ownership need not immediately lower process RSS.

The target is fewer allocations, less duplicated state, and shorter necessary lifetimes. A C++/Rust migration that retains two complete representations can initially increase memory use and must report that cost.

## Stage 1: C++ placement snapshots and controlled commits

### Boundary and representation

Start at `StrictLegaliser::try_place_cluster` in [placer_heap.cc](../common/place/placer_heap.cc), then integrate the same machinery with [detail_place_core.cc](../common/place/detail_place_core.cc). Today both can modify live architecture bindings to find out whether a move is legal.

Extract the Mistral LAB rules from [lab.cc](../mistral/lab.cc) into functions over explicit data:

- `CellFacts`: LUT input/output net keys, LUT capacity/mode, FF control signals and polarity, FF data inputs, MLAB grouping, and relevant cluster facts.
- `LabSnapshot`: occupancy for all ten ALMs, cell-fact references, input usage, control-resource information, and a version.
- `MoveProposal`: cells, expected original BELs and strengths, proposed BELs, and displaced cluster members.
- `MoveEvaluation`: legality, a structured rejection reason, recomputed resource summaries, and the versions of everything inspected.
- `ChangeSet`: the accepted physical changes and their dependency consequences.

The snapshot must cover source and destination LABs and every displaced cluster. Carry chains can span LABs. Global clocks/PLLs and any resource not represented by this contract initially remain on the serial legacy path. Geometry alone does not establish independence.

Conceptual interfaces:

```cpp
MoveEvaluation evaluate_move(const PlacementSnapshot &snapshot,
                             const MoveProposal &proposal,
                             EvalScratch &scratch);

CommitResult try_commit_move(Context &ctx,
                             const MoveProposal &proposal,
                             const MoveEvaluation &evaluation);
```

`evaluate_move` has no `Context` access. It evaluates an overlay of the proposed occupancy, using thread-local reusable scratch space. It must check vacated as well as occupied resources. Initially retain small bounded ALM recomputations; optimize them into deltas only after parity is established.

`try_commit_move` runs under the commit coordinator. It verifies dependency versions and expected ownership, then applies all bindings and derived-count changes as one operation. Rejected/stale moves leave no observable changes. Preflight allocations and retain rollback information where the apply path can fail. Observers and workers cannot see intermediate bindings. Publish exactly one `ChangeSet` after success.

Every mutation path used during an epoch must either participate in version tracking or be excluded until the epoch ends. A version counter attached only to the new API would miss legacy mutations.

### Delivery increments

1. Extract the evaluator and capture real candidate snapshots. In a serial comparison mode, run both the snapshot evaluator and the existing bind/check/revert sequence on equivalent starting states.
2. Switch supported serial legalizer moves to detached evaluation. Keep candidate ordering, RNG use, and acceptance heuristics unchanged where possible. Update affected ALM summaries once per accepted transaction.
3. Connect parallel refinement to the same evaluator. First use serial commits; later evaluate batches on frozen state and commit in a deterministic order. Conflicting read/write sets cause reevaluation in the next epoch.

Keep legality changes separate from extraction. Existing behavior is a compatibility reference, not proof that every existing rule is correct.

Likely new files: `common/place/placement_transaction.h`, `mistral/lab_snapshot.h`, and `mistral/lab_model.cc`. Shared algorithms access a backend capability adapter; backend snapshots need not have one universal representation.

### Acceptance and measurements

- Snapshot and legacy evaluation agree on captured valid/invalid candidates, including multi-LAB carry moves, FF controls, MLABs, and displacement chains.
- Rejected and stale transactions preserve canonical state exactly. Accepted transactions pass existing backend checks plus focused rule tests.
- A controlled conflict test proves that two proposals cannot claim the same shared resource.
- Track candidate evaluations, speculative bind/unbind calls, ALM recomputations, allocation counts/bytes, snapshot and scratch capacities, evaluator time, snapshot time, commit time, and retries.
- Compare Fabi386 placement success, timing, resource use, and wall-time. Keep the previously infeasible large carry-chain case as a bounded-failure regression rather than requiring it to become placeable.

## Stage 2: Complete checkpoint persistence

### Phase boundaries

Define explicit completed phases:

```text
mapped -> packed -> placed -> route-prepared -> routed
                                                |
                                  bitstream configuration + signoff
```

`placed` includes placement-dependent PLL fixups. `route-prepared` includes LAB control assignment, physical pin mapping, inserted route-through cells/nets, and global routing. Split these actions out of the current monolithic `Arch::route()` so restoration does not run them a second time.

Start with packed checkpoint support, then placed, then route-prepared/routed. This release checkpoints completed phases, not an arbitrary interrupted solver iteration. Mid-search resumption would additionally require solver/search queues, congestion history, and other algorithm state.

### Format and contents

Add a versioned checkpoint envelope with a logical-netlist payload and typed backend metadata. Keep ordinary Yosys JSON import available. A missing versioned backend section must not be treated as a complete Mistral checkpoint.

| Section | Contents |
| --- | --- |
| Manifest | Schema/backend-state versions, completed phase, device/package/speed grade, model/tool build fingerprints, effective settings, constraints, input and parent-checkpoint digests |
| Logical design | Cells, types, full property values, ports, connectivity, aliases, hierarchy/provenance, clocks and region constraints |
| Packing | Cluster roots/members, relative and absolute placement constraints, absorbed pin constants/inversions, custom logical-to-physical pin maps, global-net roles |
| Physical state | BEL bindings/strengths, route trees/strengths, committed global/PLL choices, LAB/ALM modes, control allocations, design-dependent routing reservations |
| Reproducibility | Seed, current RNG state where relevant, and defined canonical traversal order |

Audit each Mistral field as either persisted authoritative state, reproducible derived state, or disposable scratch. For example, pointer-based `combInfo`/`ffInfo` should normally be reconstructed from restored netlist, pin, and cluster metadata. Do not persist raw object layouts or attempt to restore pointers.

Store immutable device-model data by fingerprint/reference. This stage does not eliminate device-graph startup cost. Store the packed parent checkpoint alongside later physical checkpoints, because Stage 3 needs to compare equivalent logical phases before accounting for generated route-through logic.

### Restore ordering

The current frontend invokes `attributesToArchInfo()` while importing the top module; introduce a loading mode that defers architecture reconstruction.

1. Validate manifest and requested operation before adopting the result. Define whether an option is compatible, invalidates a phase, or is rejected; do not silently let imported settings override the invocation.
2. Construct logical objects and resolve durable references.
3. Restore packing/cluster metadata, pin states/maps, and authoritative control/global choices.
4. Rebuild pointer-based cell facts before restoring bindings, since current bind operations consult derived cell information.
5. Restore placements and phase-appropriate backend allocations/reservations; rebuild aggregate occupancy consistently.
6. Restore routed resources, if present, against the restored endpoint mappings and restrictions.
7. Run structural and backend checks, then publish the loaded context. Rebuild bitstream configuration before signoff when needed.

Build a fresh candidate context so a failed restore cannot damage the current design. Reuse immutable device data once its ownership has been separated; until then, include the transient cost of two contexts in the memory budget. Publish checkpoint files atomically after the complete payload has been written and validated. Never use a second pack pass as a metadata-recovery mechanism.

### Acceptance and usable result

The Fabi386 packed round-trip must preserve the values of all 1,226 clustered-cell records and all 7,813 non-default pin-state entries found by the audit, not merely their counts. Add M10K, clock/PLL, and IO-register fixtures because that execution-stage probe does not cover them.

For later phases, compare canonical physical state, endpoint connectivity, and uncompressed configuration bits after restoration. Compare resumed optimization with a clean run under a controlled seed/order; report placement/routing quality as well as runtime. Unsupported versions/configurations and missing metadata must produce explicit errors.

The usable result is phase reuse for the same design: packing can be reused for placement experiments, placement for router experiments, and routed state for reporting/signoff. Small-edit reuse follows in Stage 3.

## Stage 3: Rust matching and dependency analysis

### Ownership and FFI

Add a Rust crate such as `npnr_incremental` to the existing Cargo/Corrosion workspace. Give it an owned, architecture-neutral graph of entity keys, connectivity, semantic properties, and dependency relationships. The C++ backend exports architecture-specific dependency facts; Rust does not infer physical sharing solely from RTL module boundaries.

Introduce a small bulk bridge with versioned fixed-width records and explicit buffer ownership. Initially copy graph metadata into Rust-owned storage once per build. Device-wide routing adjacency is unnecessary for matching. Return an owned plan through a handle with a matching release function. Borrowed views must not outlive their owner, and language exceptions/panics must not unwind across this boundary.

The existing live-`Context` Rust wrapper is excluded from worker access. Before exposing it more broadly, fix its iterator lifetimes, prevent unsupported `Send`/`Sync`, and replace borrowed rotating-buffer names with owned strings. Its separate global mutex is not the synchronization authority for this design.

Conceptual Rust interface:

```rust
fn plan_reuse(previous: &PackedDesign,
              current: &PackedDesign,
              physical: &PhysicalSummary,
              changes: &ConstraintChanges) -> IncrementalPlan;
```

The result contains validated matches, dirty units, reusable physical artifacts, required phase invalidations, and human-readable reasons for each decision. C++ validates every plan before applying it.

### Typed build state machine

Make the supported Rust-controlled lifecycle explicit, even while phase computation remains in C++. A private `Build<State>` owns the backend handle and exposes only the operations legal in that state. Conceptual transitions are:

```rust
fn place(build: Build<Packed>) -> Result<Build<Placed>, FailedBuild>;
fn prepare(build: Build<Placed>) -> Result<Build<Prepared>, FailedBuild>;
fn route(build: Build<Prepared>) -> Result<Build<Routed>, FailedBuild>;
fn validate(build: Build<Routed>) -> Result<Build<Validated>, FailedBuild>;
```

These are interface sketches, not implemented functions. Transitions consume their input handles so safe callers cannot continue using an obsolete phase. Final artifact publication requires `Build<Validated>`; validation may internally construct provisional bitstream configuration for Mistral signoff. A failed phase produces a failed state with diagnostic/recovery operations, not an allegedly intact prior state unless rollback was actually guaranteed.

Raw checkpoint bytes begin as untrusted/unvalidated input. Only the checked loader constructs the appropriate phase type. An edit transitions a routed build into an explicit dirty state carrying its invalidation set; reusing physical artifacts does not preserve a stale routed/validated status.

Keep the backend handle thread-confined. Workers receive frozen snapshot types that can be shared safely and owned scratch/proposal values that can be transferred. They receive no live `Context` access. Distinct `CellId`, `NetId`, and resource IDs prevent category mix-ups; snapshot/version checks handle stale identities that types alone cannot distinguish.

The Rust compiler enforces these rules only where the safe API fully encapsulates ownership. C++ callbacks, FFI declarations, externally loaded data, and physical resource conflicts still require audited contracts and runtime validation. The initial implementation applies this coordinator to the new incremental entry path; legacy entry paths remain explicitly outside that compile-time guarantee.

### Matching and invalidation

First match packed designs from the same logical phase. Use hierarchy/provenance to propose matches, then verify types, parameters, pin semantics, and mapped connectivity. Structural signatures can help resolve renamed objects; ambiguous duplicates remain unmatched. Hashes accelerate matching but are not the sole correctness evidence.

Dependencies include:

```text
cell semantics/connectivity -> packed cluster
cluster placement           -> source and destination LABs
LAB occupancy/control state -> physical pin maps and preparation artifacts
pin maps/endpoints           -> route trees and shared resource reservations
route/cell delay changes    -> timing dependencies
clock or device changes     -> potentially much wider invalidation
```

Record why each dependency was discovered. Include shared/global resources even when the dependent objects are physically distant. Changed timing constraints can permit a legal placement/route seed while requiring fresh timing analysis and optimization; they do not make old timing results reusable.

### Delivery increments

1. **Phase ownership and analysis:** introduce the typed coordinator and compile-fail checks for invalid phase/access combinations. Synthesize as usual, repack the new netlist completely in a candidate context, and emit a reuse plan without applying it. Compare plans against controlled edits.
2. **Placement reuse:** transplant validated placements, add an explicit active-cell/cluster set to legalization, and anchor the rest as reusable preferences. The current HeAP strict pass unbinds its solve set, so merely loading BEL attributes is insufficient. Expand the active region when local repair fails; preserve actual user constraints.
3. **Routing reuse:** reuse preparation artifacts only where dependencies and endpoints still match. Keep provenance for generated cells/rewiring; otherwise regenerate the affected region from its packed representation. Extend Router2's pre-routed-arc support with an active-net set and resource reservations, then remove its all-net final rebinding where safe.

Release invalid routes before moving their endpoints. Route changes can trigger conflict-driven expansion to neighboring nets. Keep original reusable state available until the candidate repair is accepted. If repair exceeds its budget or quality degrades beyond policy, enlarge the region and eventually run the full legacy phase.

Initially perform complete final legality and timing analysis and regenerate the full bitstream. In particular, incremental software compilation does not imply partial FPGA reconfiguration or patching a running device.

### Acceptance and usable result

Use unchanged rebuilds, comment/name-only changes, small ALU changes, carry-chain changes, added/removed cells, and clock/pin/constraint changes. Verify semantic matching and resource legality rather than expecting identical placement after every edit.

Report reused cells/routes, dirty clusters/LABs/nets, reasons for invalidation, repair expansion, fallback rate, peak memory, and end-to-end runtime. Compare against clean builds over fixed seeds, including final timing/resource use. Only claim iteration improvements after counting synthesis, full repacking, matching, restoration, and final validation costs separately.

Compile-fail cases should reject routing a packed handle, publishing an unvalidated artifact, using a consumed phase handle, sending the live backend handle to a worker, and mixing cell/net ID types. Runtime cases must still reject stale proposals and corrupted or incompatible checkpoints.

## Stage 4: An isolated Rust ownership and computation experiment

### First kernel and controls

Use batched LAB legality evaluation as the first bounded experiment: Stage 1 supplies an explicit input contract, real candidate corpus, and a C++ reference. Rust owns validated evaluation values and scratch state; it returns legality/reason/resource-summary records. The caller retains candidate selection and commit policy. Evaluate the strength of the ownership boundary and the necessary unsafe surface as well as execution cost.

The [detailed Mistral LAB evaluator design](mistral-lab-legality-design.md) starts with FF control-set evaluation and exact allocation parity. Its serial capture/shadow implementation can begin before the complete Stage 1 transaction boundary; detached move evaluation and parallel commits still depend on that boundary. It specifies the snapshot, C ABI, Rust types, rule order, preparation integration, tests, and rollout.

Compare:

1. Current legacy bind/check/revert behavior.
2. Detached C++ evaluation using the new data layout.
3. Detached Rust evaluation using equivalent inputs and rules.
4. Parallel batch evaluation at controlled worker counts.

Comparison 1 versus 2 measures the architecture improvement; 2 versus 3 isolates the language/kernel implementation more closely. Candidate batches remain tied to one frozen epoch. Commit accepted work in a defined order and reevaluate candidates invalidated by preceding commits.

Capture data once per batch/epoch and reuse immutable arrays. Measure conversion, allocation, FFI, rejected speculative work, and commit overhead alongside kernel time. Also compare live bytes, peak process footprint, worker scratch growth, duplicated buffers, and retained state between repeated builds. Avoid per-cell or per-PIP callbacks into C++.

### Timing as the next candidate

If phase profiling identifies timing propagation as material, export a complete timing graph with cell/net arcs, delays, constraints, clock domains/edges, skew relationships, and start/endpoints. Rust can own arrival/required-time propagation and its dirty queues. Keep architecture delay acquisition, Mistral analogue simulation, and final signoff under the existing C++ path initially.

Support both increasing and decreasing delays: reducing an extremal predecessor requires recomputing the affected maximum/minimum, not just propagating increases. Structural or clock-domain changes rebuild the relevant topology/domain analysis. Unsupported loop/domain cases use full analysis. Validate every supported incremental result against a full recomputation within the existing delay representation's tolerance.

Do not select this kernel solely from the earlier unexplained runtime remainder; first distinguish propagation from delay acquisition, bitstream work, and reporting.

### Promotion gate

Promote a Rust subsystem after semantic equivalence and boundary validation pass, and its ownership/type/concurrency guarantees justify the migration with acceptable execution and memory costs. A speedup is not required. Equal execution time with stronger enforceable invariants, or lower peak memory with a modest measured runtime tradeoff, can be a successful result. Set resource budgets from the baseline and report any regressions explicitly.

Require reproducible runs, bounded retries, and validated behavior at 1/2/4/8 workers where supported. Keep a reference implementation and runtime selection during rollout. Review compile-fail coverage, FFI/unsafe contracts, phase-state coverage, allocation lifetimes, peak memory, runtime, and output quality as separate criteria.

Retain a C++ kernel when the Rust version does not provide enough ownership, maintainability, or resource benefit to justify migration; equal speed alone is not a reason to reject Rust. A whole-router migration is a later ownership and engineering decision requiring bulk graph/resource interfaces and evidence about its safety, memory, and runtime tradeoffs.

## Reviewable implementation units

| Order | Change | Completion evidence |
| --- | --- | --- |
| 1a | Explicit config and LAB evaluator extraction | Captured-candidate parity and focused legality cases |
| 1b | Serial placement transactions | No mutation on rejection; one update per accepted change |
| 1c | Parallel refinement adapter | Resource-conflict tests and reproducible commits |
| 2a | Packed checkpoint envelope/loader | Fabi386 metadata round-trip plus backend fixtures |
| 2b | Placed and prepared/routed checkpoints | Resume, connectivity, and configuration equivalence |
| 3a | Rust phase coordinator, bulk bridge, and reuse-plan engine | Invalid transitions rejected; conservative matching and explainable invalidation |
| 3b | Placement reuse | Local edits repaired with clean-build validation |
| 3c | Route reuse and region expansion | Preserved routes remain valid; fallback demonstrated |
| 4a | Rust LAB evaluator experiment | C++/Rust parity; ownership, memory, and runtime assessment |
| 4b | Optional timing kernel experiment | Incremental/full timing equivalence and validated ownership/resource tradeoffs |

Stages 1 and 2 can overlap after shared identity/configuration contracts are settled. Stage 3 analysis can start against captured packed graphs, but applying reuse depends on reliable restoration and mutation tracking. A Stage 4 experiment can begin after Stage 1; promoting it into normal builds requires the relevant correctness, ownership, and resource gates.

## Rust language references

- [The Rustonomicon: resource leaks](https://doc.rust-lang.org/nomicon/leaking.html) explains why memory safety does not imply leak freedom.
- [The Rust Book: Send and Sync](https://doc.rust-lang.org/book/ch16-04-extensible-concurrency-sync-and-send.html) describes how thread-transfer and sharing rules depend on the types and sound unsafe implementations.
- [The Rust Book: encoding states and behavior as types](https://doc.rust-lang.org/book/ch18-03-oo-design-patterns.html#encoding-states-and-behavior-as-types) illustrates restricting operations to the states in which they are valid.
