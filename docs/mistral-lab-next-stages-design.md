# Mistral LAB Integration, Optimization, and Transactions Design

Status: proposed implementation design, 2026-09-15. This document specifies the
four steps following FF-control parity and profiling. It does not claim that
preparation-plan consumption, complete LAB evaluation, or transactions exist yet.

Read alongside the [original evaluator design](mistral-lab-legality-design.md),
[implementation record](mistral-lab-legality-plan.md),
[profiling report](mistral-lab-legality-profile.md), and
[broader parallel/incremental design](parallel-incremental-design.md).
The contracts below refine the remaining milestones D–F. Existing V1 wire
semantics remain authoritative; proposed functions and types are interface
sketches, not declarations already available in the repository.

## 1. Decisions and measured starting point

| Stage | Deliverable | Existing milestone |
| --- | --- | --- |
| 1 | Rust control allocation plans consumed by C++ preparation | D |
| 2 | Lower capture/decoding overhead without semantic changes | D, with later reuse work in F |
| 3 | Detached evaluation of all currently modeled LAB legality rules | E |
| 4 | Versioned candidate transactions, parallel evaluation, and incremental assessments | F |

We retain C++ ownership of the live design, physical mutation, candidate policy,
and final signoff. Rust owns validated values and pure computation. Both Rust
crates may use `std` and heap storage where useful; there is no embedded target
requirement. Workers never receive the existing live `nextpnr` Rust wrapper.

The Fabi386 execution-stage slice establishes the following baseline, not a
claim about every Mistral design:

| Measurement | Observation |
| --- | ---: |
| Original live C++ FF-control query | 146 ns, warm inputs |
| Complete Rust dispatch | 363 ns, warm inputs |
| Capture / Rust decode / Rust rules | 160 / 117 / 29 ns |
| Detached C++ / Rust through FFI | 133 / 175 ns |
| Median paired full P&R overhead | Rust +1.27%, seven pairs |
| Hot-path allocations | Zero in both measured implementations |
| Median peak process RSS | C++ 1,966 MiB; Rust 1,968 MiB |

The serial boundary overhead is measurable; no memory saving is established.
Stage 2 targets conversion work. Stage 4 amortizes capture across substantial
candidate work rather than scheduling a 29 ns rules call independently.

Cross-cutting invariants:

1. Preserve current conservative rules, greedy order, null-signal polarity, and
   distinct failure classes. Rule improvements require another rules version.
2. A well-formed result is not proof of algorithmic correctness. Differential
   validation establishes parity; host checks establish transport/provenance
   integrity. Neither substitutes for freshness checks.
3. A legal FF-control plan is not a complete LAB, move, prepared-state, or routing
   certificate. Each has a distinct type and validation gate.
4. No cached result or worker-owned value contains a live C++ object pointer.
5. Default behavior stays legacy during rollout (ended 2026-09-19 for the complete
   LAB evaluator, which defaults to the Rust authority in Rust builds; the legacy
   rules remain the fallback and the harness reference). Unsupported capabilities select
   an explicit reference path before mutation; malformed data and internal errors
   fail the operation and cannot silently become placement rejection.

## 2. Architecture and ownership

```text
C++ owner: cells, nets, BELs, constraints, routes, timing, revisions
    |
    +-- Stage 1: capture -> Rust control plan -> host validation
    |                                        -> C++ preparation edits
    |
    +-- Stage 2: cheaper capture/decode, same contracts and results
    |
    +-- Stage 3: full LAB facts -> Rust scoped/whole-LAB assessment
    |
    +-- Stage 4: frozen facts + occupancy overlay + dependency versions
                         |
                  coordinator schedules batches
                         |
                 pure detached evaluators
                         |
              ordered results, including rejections
                         |
               C++ freshness + external checks
                  /                       \
         stale: recompute              commit edits
                                           |
                           update revisions and dirty artifacts
```

| Owner | Authoritative data | Lifetime |
| --- | --- | --- |
| C++ design owner | Object identity, connectivity, placement, constraints, physical reservations | Context/build session |
| C++ capture | Wire DTO and local-ID-to-net translation | One stable synchronous preparation operation |
| Rust evaluator | Validated snapshot, assessment, private scratch | One call, or an explicitly owned frozen batch in stage 4 |
| Coordinator | Frozen publications, proposal order, dependency records, worker limits | Placement session/epoch |
| Assessment cache | Pointer-free semantic result plus exact dependencies | Bounded; evictable without correctness impact |

Mutable architecture caches must be materialized by the owner before freezing
facts or excluded from the worker interface. Atomic statistics do not make the
rest of `Arch` safe for concurrent access. The existing profiling reservoir is
also an owner-side serial tool, not a worker-side collection mechanism.

## 3. Stage 1 — consume Rust preparation plans

### 3.1 Current behavior and intended change

[`assign_control_sets`](../mistral/lab.cc) currently invokes the Boolean Rust
dispatcher, discards its allocation, and runs `LabCtrlSetWorker` again. It then
uses that worker's DATAIN assignment to reserve routes and set selection indices.
[`lab_pre_route`](../mistral/lab.cc) performs this per LAB before
`reassign_alm_inputs`, which may insert route-through cells and rewire FF inputs.

Change preparation to retain the selected evaluator's twelve-signal allocation.
Keep the physical application logic in C++. Placement's Boolean control subcheck
continues to use the same dispatch policy.

### 3.2 Host types and entry points

Introduce a host-only `LabControlAllocation` containing the twelve named
`ControlSig` fields in V1 order. It describes an allocation independent of the
legacy worker. Export the worker into this type before adding Rust consumption.

Use a private, move-only preparation ticket that owns its `LabControlCapture`,
selected result, LAB ID, and request ID. It cannot be constructed from a Boolean
verdict or retained in `Arch` between placement and preparation.

```cpp
// Proposed host API. Errors distinguish illegal, unsupported, and broken data.
PreparationOutcome evaluate_controls_for_preparation(Arch &, uint32_t lab);
ValidatedControlPlan validate_and_translate(PreparationTicket &&);
PreparedControlEdits plan_control_edits(const Arch &, ValidatedControlPlan &&);
void apply_control_edits(Arch &, PreparedControlEdits &&);
```

These are consuming transitions with private constructors. The translated plan
may contain C++ net pointers only inside this synchronous owner operation; none
cross FFI or escape into a cache. The ticket's snapshot map remains alive until
all identities have been translated and edits resolved.

Validation checks the call status, complete result header, expected request,
rules, legal status, canonical unused fields, and every allocated signal against
the capture. Preserve polarity even when `net_id == 0`. Translate a connected
ID through `capture.nets[id - 1]`, never through a name lookup or pointer cast.
Retain all twelve entries for comparison even though current application primarily
consults `datain[4]`.

Stage 1 uses uninterrupted serial capture/evaluation/planning/application. No
placement or connectivity change may intervene, including through callbacks.
Epoch zero remains provenance only. If that ownership interval cannot be held,
discard the ticket and recapture; retained tickets require stage 4 revisions.

### 3.3 Preserve application semantics exactly

Extract the existing application loops into a common consumer first, using a C++
plan. Then supply a Rust plan through the same consumer:

- Preserve ascending ALM, FF, enable-selection, and clear-selection order.
- Match signals using equality of net identity and polarity. Do not use the
  currently defective `ControlSig::operator!=` during extraction.
- Preserve `ena_datain` and `aclr_datain` lookup order and the first matching
  selection, including matches between disconnected signals. Do not invent a
  default index when the original loop makes no assignment.
- Reset `aclr_used[2]` as today; update `clk_ena_idx` and `aclr_idx` only where the
  original loop writes them.
- Keep MLAB WCLK/WE reservations on CLK0/ENA0, including their original ordering.
  Rust's FF-control plan does not describe MLAB write-port legality.
- Preserve the order relative to LUT pin mapping, route-through insertion,
  global routing, and normal routing. No LUT mapping or global routing moves
  into the Rust evaluator in this stage.

[`reserve_route`](../mistral/arch.cc) finds the source in the destination's uphill
list and **assigns** `RESERVED_ROUTE | index` to the destination flags. It does
not OR this into the old flags. A parity refactor must preserve this assignment,
including repeated writes and final selection. Treat improvements to flag
preservation or conflicting reservation policy as separately validated changes.

### 3.4 Preflight, application, and failures

Build an ordered `PreparedControlEdits` list before the first write. Resolve all
wire references and uphill indices; reject a missing route edge as an integration
error. Record index/flag writes and expected original values. Allocate edit and
undo storage before publication. The collection is small but may use reusable
heap storage; the no-allocation evaluator property need not cover preparation.

Apply under the owner with no foreign callbacks or allocating operations. Keep a
journal of original values for unexpected application failure; repeated writes
to one field restore its original value exactly once. Do not advertise rollback
for unrelated later preparation work. If `reassign_alm_inputs` subsequently
fails after creating cells or rewiring nets, the preparation phase is failed;
that context cannot be presented as an intact placed design or routed artifact.

```text
Placed -> CapturedControls -> ValidatedPlan -> PreflightedEdits
               |                    |                 |
           evaluation error     validation error   preflight error
               +--------------------+-----------------+
                       no control edits published

PreflightedEdits -> AppliedControls -> remaining preparation -> Prepared
                                          |
                                     FailedPreparation
```

This stage does not make preparation reentrant. Do not rerun `lab_pre_route` on
an already transformed design to repair an error; reconstruct from clean packed
or placed state until generated-object provenance and teardown exist.

### 3.5 Dispatch and rollout

| Existing mode | Evaluation and comparison | Plan applied |
| --- | --- | --- |
| `legacy` | C++ worker | C++ |
| `shadow` | Compare Rust, detached C++, and live C++ results; bounded diagnostics | C++ |
| `verify` | Exact plan comparison; mismatch/error stops before writes | Rust, after agreement |
| `rust` | Rust with host validation | Rust |

Changing `verify` to exercise Rust application is intentional and must be
documented. A valid explicit unsupported-rules outcome in experimental Rust mode
may use a newly evaluated C++ plan under the existing fallback policy and counter.
An illegal result during preparation is a failed placement/preparation invariant.
Invalid snapshots, malformed plans, and panic/call failures must not fall back
after any edits. Rust-disabled builds reject Rust-dependent modes as today.

For shadow preparation, construct both edit lists without applying either,
compare ordered operations and resulting touched state, then apply only C++.
This catches translation/application defects that twelve-signal parity alone
cannot reveal.

### 3.6 Delivery and gate

1. Extract allocation value type and common consumer; keep C++ authoritative.
2. Separate evaluation from Boolean dispatch and retain preparation tickets.
3. Add preflight/edit comparison and failure injection before any mode consumes
   Rust output.
4. Enable verified Rust application, then experimental Rust application.

Require exact allocation, reservation flags, and selection-index agreement;
unchanged state on rejected plans; MLAB and disconnected-polarity fixtures; and
Fabi386 placement/preparation/routing/timing parity. Compare named physical state
across separate runs, not pointer addresses. Add FF control and LUTRAM designs
beyond the Fabi slice. Software parity is insufficient evidence for new hardware
rules, which remain out of scope.

## 4. Stage 2 — reduce capture and decoding costs

### 4.1 Optimize within the existing contract first

Retain V1's 1,952-byte input, 216-byte result, complete validation, and exact error
precedence. Run each experiment separately against the archived corpus and a
fresh live capture. Measure live C++, detached C++, Rust FFI, and complete dispatch
so layout and language effects remain distinguishable.

| Experiment | Concrete change | Constraint |
| --- | --- | --- |
| Single net-map search | Have capture's first-encounter lookup return the ID directly; avoid the second search in `encode_existing` | Byte-identical DTO and net map, including null inversion |
| Reduce temporary copies | Write directly into the final transport buffer; inspect optimized code before assuming a source rewrite helps | Every wire byte remains initialized; C caller ownership unchanged |
| Decoder storage | Construct typed storage with fewer temporary aggregate copies; keep the same checks and their order | No unchecked public constructor or borrowed live pointer |
| Control-only capture | Experiment with a translation-map workspace whose unused tail is not retained after a verdict-only call | Keep bounded net lookup scratch; full map remains for preparation and diagnostics |
| Dispatch stack/counters | Outline large preparation/error paths; measure owner-local counters where ownership proves serialization | Keep per-worker statistics for future workers; never remove synchronization while callers can overlap |

The map search uses tiny bounded domains; a heap hash map is not an assumed
improvement. Preserve the reference `encode_existing` use for exporting a legacy
plan. Zero-filling a transport record can be efficient; avoid unsafe
uninitialized-memory techniques merely to reduce apparent source-level writes.

Use safe Rust for decoder changes. If a borrowed view of *validated wire values*
is later justified, encapsulate validation and lifetimes in the pure crate and
retain equivalent type guarantees. It must not become a view of mutable C++
state or a caller assertion that bypasses checks. First prefer the current owned
model with improved construction.

### 4.2 Avoid premature caching and batching

V1 already accepts up to 64 independent records. Batch independent frozen inputs
when a caller naturally has them; its bridge processes the batch serially. Do
not collect calls across intervening speculative mutations or move selection
decisions simply to fill a batch. In particular, do not pre-capture all LAB
preparation inputs while earlier LAB preparation is allowed to rewire the design.

The reusable validated snapshot belongs to stage 4. A repeated LAB number,
matching net names, matching hash, or unchanged occupancy alone is insufficient
for reuse: normalization, net global status, policy, and cell facts can change.
Stage 2 performs no persistent memoization and introduces no Rust-owned global
handle registry.

### 4.3 Measurements and acceptance

Use the existing [profiling runner](../mistral/tests/profile_lab_controls.py),
[C++ microbenchmark](../mistral/tests/lab_bench.cc), and
[Rust component benchmark](../rust/npnr_mistral_lab_ffi/examples/lab_control_bench.rs).
Preserve benchmark binary/source/input hashes. Also measure naturally ordered
query streams and bounded batches; warm repeats alone cannot establish live
cache behavior.

For each experiment report distributions of capture, decode, rules, result
validation, FFI, and dispatch cost; allocations and requested bytes in each
language; static frames and, where available, dynamic stack high-water marks;
and full P&R wall/CPU time and RSS. Object size and static frame size are not
dynamic peak memory measurements. `--threads 1` still permits HeAP's X/Y solver
threads; prevent nested evaluator oversubscription separately.

Proposed engineering target: reduce complete warm dispatch by at least 20% from
363 ns, without changing results or worsening the measured full P&R baseline.
This is a target, not an achieved result or an automatic default-promotion gate.
Retain a change only with a reproducible component benefit or a documented
maintainability/stack benefit and acceptable measured runtime. Do not combine
several experiments before attributing their effects.

Gate: full named-field parity on valid and malformed corpora; identical capture
mapping where V1 applies; no new normal evaluator allocations; unchanged serial
candidate/RNG behavior; and repeated full P&R with equivalent artifacts. If the
target cannot be reached safely, proceed with the explicit measured overhead
rather than weakening validation.

## 5. Stage 3 — complete modeled LAB legality

### 5.1 Scope and query contract

Expand the input to facts read by `is_alm_legal`, `update_alm_input_count`,
`check_lab_input_count`, `check_mlab_groups`, and FF control evaluation. C++ still
normalizes packed cell facts. Rust does not independently infer controls,
constants, chain sharing, or MLAB groups from logical ports.

Expose distinct safe APIs and a tagged C ABI query:

```rust
enum LabQuery { CombBel(AlmIndex), FfBel(AlmIndex), WholeLab }
fn evaluate_lab(snapshot: &ValidatedLabSnapshot, query: LabQuery) -> LabAssessment;
```

| Query | Ordered checks |
| --- | --- |
| `CombBel(alm)` | Queried ALM, LAB input count, MLAB groups |
| `FfBel(alm)` | Queried ALM, LAB input count, FF controls, MLAB groups |
| `WholeLab` | ALMs 0–9, LAB input count, FF controls, MLAB groups |

The first two preserve [`isBelLocationValid`](../mistral/arch.cc), including
queries of empty BELs and invalid unrelated occupants. Whole-LAB validation is a
new, stronger composition of existing rules. Do not install it as a transparent
replacement for either scoped query. A full candidate may still violate cluster
geometry, regions, BEL ownership, clock/PLL compatibility, timing policy, or
physical routing constraints.

### 5.2 Proposed V2 value schema

Add `mistral/lab_abi_v2.h` and new versioned symbols. Keep V1 layouts, symbols, and
`LegacyControlRulesV1` frozen. Separate transport version from the whole-LAB
semantic rules version; embedding V1 controls does not give V1 full-LAB meaning.

Use fixed-width integers, explicit occupancy, signed fields where the host uses
signed values, zeroed reserved fields, and bounded arrays. Do not expose C++ or
Rust containers. Initially send a bounded full snapshot per LAB; optimize only
after parity. Proposed fields:

| Record | Fields |
| --- | --- |
| Header | ABI/size, LAB rules version, control rules version, request/epoch, query tag and ALM index |
| LAB facts | `is_mlab`, signed resolved input limit, net count and net global classifications |
| 20 LUT slots | Occupied, seven ordered input IDs, output ID, nominal/used input counts, LUT-bit count, chain-shared input count, carry flag, signed MLAB group and cluster Z |
| 40 FF slots | Occupied, five normalized controls, DATAIN ID, SDATA ID |
| Result | Status, echoed query/provenance, first failing check/location, ten recomputed counts and total, tagged optional control allocation/conflict |

The seven-input capacity follows `ArchCellInfo::combInfo.lut_in`, even though
some current cell types use fewer. Snapshot bounds cover 20 × (7 inputs + 1
output) + 40 × (5 controls + 2 data inputs) = **440 net occurrences**, so 440
distinct connected nets suffice for this schema. MLAB WCLK/WE are not included:
their normalized group identity is what the current legality check reads, and
their physical reservation remains in stage 1. Adding other net fields requires
revisiting this bound and schema.

Assign IDs deterministically in documented slot/field order, with zero for
disconnected. Use one namespace across LUT outputs and FF data so driver identity
comparisons remain meaningful. The host retains reverse IDs only for immediate
translation or diagnostic materialization. Stable session IDs in stage 4 are a
separate identity domain.

Validate all slots and metadata before constructing the typed snapshot, even for
scoped queries; validation errors are not hardware illegality. Validate array
bounds and known fact ranges from the normalization audit, preserve allowed
signed values, and accumulate counts in a wide signed temporary before checked
conversion. Pass the resolved input limit unchanged, including zero/negative
values. Do not silently clamp policy or replace current environment parsing as
part of this port.

Resolve `MISTRAL_LAB_INPUT_LIMIT` once into an explicit host policy used by both
reference and Rust. The current function-local static is unsuitable for testing
multiple policies in one process or changing policy revisions. Preserve current
single-run behavior during extraction; parsing/range-policy changes are separate.

### 5.3 Reuse the proven control evaluator

Do not pass V2 net IDs directly to V1's 200-net decoder. Initially project FF
controls to a dense, first-encounter V1 control domain, retaining a bounded
control-ID-to-LAB-ID map. Invoke the existing pure control path and translate
legal allocations and conflict signals back into the V2 domain. This projection
is deterministic, adds no live capture, and provides a clear parity oracle.

The result carries a presence tag: a successful combinational query has no
control-plan certificate. A whole-LAB failure exposes no applicable plan even if
an earlier control subcheck succeeded. Recomputed counts are diagnostic/derived
values with an explicit validity indicator; callers cannot mistake uncomputed
short-circuit fields for zero counts.

Measure projection overhead. A later private control-view abstraction may share
typed net facts directly, but must preserve V1 behavior and keep local-ID domains
distinct. Avoid two independently maintained control-rule implementations.

### 5.4 Exact algorithms to port

Port in the following order, retaining a detached C++ reference for each unit:

1. **ALM capacity and sharing.** Sum declared LUT bits and reject above 64. Sum
   nominal LUT inputs. If above eight, preserve the current asymmetric nested
   sharing scan, including null equality and repeated inputs; do not substitute
   mathematical set union. Reject when the adjusted count exceeds eight.
2. **Carry and FF accessibility.** Reject mixed carry/non-carry LUT halves.
   Carry mode disables route-through LUT use. Keep the existing half/FF order
   and rejection of odd FF slots. Preserve normalized control-set comparison.
   E/F availability depends on the opposite LUT's used-input count being at
   most two. SDATA consumes E/F first; external DATAIN consumes a permissible
   route-through LUT first, then E/F. A DATAIN driven by its associated LUT
   output needs neither external path. These are capacity assessments, not
   physical pin-map construction.
3. **ALM input accounting.** Preserve the whole-ALM zero shortcut for a LUT with
   `mlab_group != -1` and `constr_z > 2`. Otherwise sum used LUT inputs minus
   chain-shared inputs. Apply the existing asymmetric non-null sharing scan,
   capped at two for the current non-MLAB branch; clamp the intermediate result
   at zero. Count each SDATA use and each external DATAIN use as today.
4. **LAB input limit.** Sum all ten recomputed ALM counts and compare `<=` the
   resolved signed limit. This is a conservative usage count, not a count of
   distinct LAB nets or a routing proof.
5. **MLAB compatibility.** On MLAB-capable LABs, all occupied LUTs must have the
   same group, including ordinary-LUT group `-1`. A nonnegative group excludes
   all FF occupants. Preserve the empty-LAB and ordinary-LUT cases.
6. **Composition.** Invoke the proven controls where the query contract requires
   them and return the first failing check in the defined order.

Recompute counts from input facts rather than trusting `unique_input_count`.
During shadowing compare recomputed counts with the live cache separately from
verdict comparison. A stale cache is a host-maintenance defect, not automatically
a Rust rule error; diagnose it before allowing authoritative consumption.

### 5.5 Diagnostics, migration, and gate

V2 reason categories distinguish boundary errors from ALM bit/input capacity,
carry mixing, unsupported FF position, data accessibility, LAB input limit,
control conflicts, and MLAB group/FF conflicts. Include the first ALM/slot,
computed counts or consumed resource mask, and bounded witnesses. Diagnostics
describe the current algorithm's first failure, not a minimal unsatisfiable set.
Use a new named-field replay schema with query, policy, normalized facts, cached
host counts, and complete expected result. Do not dump native memory.

Introduce per-subcheck shadow validation, then compose scoped queries, then add
the explicit whole-LAB API. Existing `--lab-controls` retains its FF-only scope;
propose a separate `--lab-legality legacy|shadow|verify|rust` for composed queries.
Reject ambiguous combinations of independent control/full-LAB modes rather than
double dispatching. The full-LAB mode selects its own control subcheck internally.

Gate: exhaustive small sharing/accessibility domains; odd FFs; constants, nulls,
duplicate inputs; carry chains crossing LABs; MLAB grouping and Z shortcuts;
signed policy boundaries; stale host-cache fixtures; malformed V2 records; and
live scope-by-scope C++/Rust parity. Test cases where an unrelated invalid ALM
makes `WholeLab` fail while a legacy scoped query succeeds. Reprofile the wider
boundary and repeat Fabi386 plus feature-specific full P&R. Complete modeled LAB
legality does not eliminate conservative rejection or guarantee routeability.

## 6. Stage 4 — versioned candidate transactions

### 6.1 Deliver serial transactions before parallel execution

Today the HeAP strict legalizer and refinement paths perform provisional
bind/check/revert operations. Introduce a backend capability adapter for
detached LAB candidates without rewriting their search policy first. Unsupported
cell classes and global/PLL dependencies remain on the existing owner-only path
until their constraints can be captured or explicitly checked at commit.

Start with placement-phase designs, before control preparation or route-through
insertion. Incremental editing of already prepared/routed designs has additional
teardown requirements in section 6.8.

```cpp
// Proposed common interface; Mistral owns the concrete snapshot representation.
FrozenPlacement freeze_for_candidates(OwnerGuard &, const CandidateScope &);
CandidateAssessment evaluate(const FrozenPlacement &, const CandidateProposal &);
CommitOutcome try_commit(OwnerGuard &, CandidateAssessment &&);
```

The common header exposes capabilities and opaque backend-owned values, not a
universal architecture snapshot. A proposed
`common/place/placement_transaction.h` does not exist yet. Integrate HeAP first;
adapt `parallel_refine.cc` only after serial transactions pass their gate.

There is an existing compatibility hazard in `StrictLegaliser::try_place_cluster`:
its rollback records cells but rebinds displaced occupants with `STRENGTH_WEAK`,
rather than their saved original strength. A transaction that restores strengths
exactly can therefore change later search behavior. First add a fixture exposing
this case, then correct the legacy rollback in a separate reviewed change and
refresh its baseline. Do not weaken the transaction's rollback invariant to
reproduce that leak, or claim exact trajectory parity against the unfixed path.

### 6.2 Identity, snapshots, and overlays

Assign separate session-scoped `CellKey`, `NetKey`, `BelKey`, and `LabKey` domains.
Use generations or non-reused monotonic IDs; recreating a same-named object must
not revive its old identity. Cross-process identity is outside this stage's
first delivery. Retained values include a build-session ID and publication ID.

A frozen publication owns immutable cell/net facts and base occupancy for the
needed scope. Once stage 3 expands the facts, retain one normalized fact record
per entity in a publication rather than one graph copy per worker. Share frozen
chunks across proposals; candidate overlays store only changed occupancy and
fact overrides. Initial per-worker materialization into reusable full LAB
scratch is acceptable and measurable.

Proposal contents:

| Field | Meaning |
| --- | --- |
| Proposal identity | Session, base publication, deterministic sequence number |
| Ordered edits | Cell, expected old BEL/strength, target BEL/strength, displacement/unplacement actions |
| Affected closure | Every source, target, displaced-cell LAB, and displaced cluster member |
| External constraints | Cluster geometry, regions, fixed bindings, alias/global dependencies where supported |
| Query schedule | Exact compatibility queries, or explicitly selected full-candidate policy |
| Dependency record | Versions of all facts consumed, including negative occupancy reads |
| Search metadata | Owner-issued candidate parameters and policy/RNG provenance; optional score dependency versions |

Normalize the full overlay before evaluating. Reject duplicate target claims,
contradictory edits, mismatched expected occupants, and incomplete cluster moves.
Every displaced occupant must be explicitly moved or left unplaced and scheduled
for legalization; it cannot disappear from the transaction. A carry cluster can
span several LABs. Evaluate the resulting state of source LABs as well as targets:
removing a LUT can invalidate a remaining FF's data path.

There are two explicit commit policies:

- **Compatibility:** replay the original caller's scoped query schedule against
  the complete overlay, preserving its observed order and decisions. Full-LAB
  checks may run diagnostically but cannot silently reject additional moves.
- **Full candidate:** require whole-LAB assessments for the affected closure plus
  external move checks. This deliberately strengthens legality composition and
  gets its own correctness/quality baseline.

Promoting the second policy is an algorithm-policy change, independent of
choosing C++ or Rust for evaluation.

### 6.3 Freshness and dependency tracking

Begin with a single monotonic design revision and a mutation audit. Increment it
for every relevant live mutation, including legacy speculative bind/unbind and
restoration. Returning to identical occupancy does not return to an old revision.
Disable retained assessments during any phase whose mutation paths are unaudited.
On revision exhaustion, invalidate the session rather than wrapping identifiers.

After conservative correctness is established, introduce explicit read versions:

| Dependency | Invalidating mutations |
| --- | --- |
| LAB occupancy | Any occupant binding, removal, move, or relevant strength change |
| Cell facts | Connectivity, pin polarity/constants, data inputs, LUT metadata, chain/MLAB annotations |
| Net facts | Global classification and any normalization inputs used by evaluation |
| Cluster/region constraints | Membership, relative locations, hard constraints, control-group selection policy |
| BEL/alias ownership | Occupation or release of target, source, alias, or expected-empty resources |
| Rules/device/policy | Model fingerprint, rule version, signed input limit, applicable query policy |
| Scoring facts, if retained | Net endpoints/bounds, timing costs, temperature or acceptance-policy state |

The read set includes absent occupants and all facts traversed by short-circuit
decisions. Start with all facts in each captured LAB rather than attempting a
minimal reason-dependent set. Sharing no moved cells does not prove independence:
two LABs can depend on the same net property or cluster constraint.

Audit `bindBel`/`unbindBel`, direct binding writes, port connect/disconnect,
pin-state changes, `assign_ff_info`/`assign_comb_info`, group/cluster changes,
global promotion, constraint changes, and generated-cell creation/removal.
Invalidation must occur when authoritative data changes, not only when its
derived annotations are eventually refreshed. Mark stale annotations unusable
until rebuilt. `update_bel` alone does not cover this dependency space.

Maintain owner-side reverse incidence from nets/cells to dependent LABs. A
clock's global-status change can dirty distant LABs without moving any cell.
Initially broad invalidation is preferred to a partial incidence implementation.

### 6.4 Owned batch boundary

Do not extend V1 by retaining its C++ buffers. For persistent frozen evaluation,
introduce a separately versioned bulk API with an opaque Rust-owned batch handle:

```text
create_frozen_batch(versioned facts) -> owned handle or boundary error
evaluate_candidates(handle, proposals, caller-owned results) -> call status
destroy_frozen_batch(handle) -> release all retained Rust storage
```

Creation copies and validates the supplied data; no C++ pointer is retained.
Internally a safe immutable batch owns typed facts. Candidate inputs refer to
session keys validated against that batch. Results use session keys or a defined
batch-local namespace; they never carry an expired V1 capture map.

C++ wraps the handle in move-only RAII ownership. Concurrent evaluations may
borrow the same immutable handle, with disjoint outputs and per-call scratch.
The coordinator joins all readers before destruction. Calling destroy twice,
destroying during evaluation, or passing a foreign handle violates the audited
FFI caller contract; numeric IDs alone cannot prove pointer liveness. If runtime
stale-handle detection is required later, add a generational registry explicitly
and measure its locking cost. Do not imply that the initial RAII wrapper protects
arbitrary unsafe foreign calls.

Each call bounds proposals, references, and output capacity before creating
slices. Reuse V1's panic-containment principle: no unwind across FFI, an error
invalidates the active batch, and no partially computed output can be committed.
Allocation failure is a process/runtime failure, not `Illegal`. No callbacks,
internal thread pool, or per-cell FFI calls are added. Publish exact numeric
layout/limits with cross-language offset tests before implementation is enabled.

### 6.5 Commit protocol and failure atomicity

```text
Proposed -> Evaluated(Legal | Illegal | Error)
                 |
        owner checks provenance and dependencies
                 |
         stale -> reevaluate against current state
                 |
       current illegal -> reject without publication
       current legal   -> external checks + acceptance policy
                                   |
                              Preflighted
                                   |
                         publish under owner guard
                            /             \
                      Committed      rollback or FailedContext
```

Freshness applies to **illegal results too**. Earlier commits can remove a
conflict; silently dropping a stale rejection changes search behavior. Errors
follow explicit fallback/failure policy, not the legal/illegal branch.

For a current legal assessment, the owner:

1. Verifies session, proposal, query policy, rules, dependencies, expected
   bindings/strengths, and all target claims.
2. Performs external checks not certified by the LAB evaluator. Recomputes
   score/acceptance from current state unless all score dependencies were frozen
   and checked; a current legality result does not make an old score current.
3. Resolves edit targets and allocates journals, dirty lists, and derived-state
   storage before mutation. Checks that every displaced cell remains accounted
   for in placer queues and cluster state.
4. Applies the complete write set while observers/workers cannot access live
   state. Updates both cell-to-BEL and BEL-to-cell mappings, strengths, derived
   ALM counts, placer occupancy structures, costs, and legalization queues.
5. Publishes dirty-artifact state and incremented revisions, then releases the
   owner guard and emits notifications/diagnostics.

Initially use existing binding APIs under the owner and journal all effects.
Only after parity introduce a private batched binding operation that recomputes
each touched ALM once. It must preserve all existing update responsibilities;
direct assignments to `data.bound` are not a complete transaction implementation.

Rejected and stale candidates do not alter canonical design state. Recoverable
application failure restores journaled state before it becomes observable. If a
callback/allocation/exception cannot be made recoverable, mark the context failed
and stop rather than claiming rollback. Revisions advance after any provisional
live mutation/restoration, so prior certificates remain invalid even after a
successful rollback. Counters and proposal scheduling state are separate from
the canonical placement whose equality is tested.

### 6.6 Deterministic parallel evaluation

The C++ coordinator is the sole scheduler and commit authority. Rust evaluates
each assigned batch serially; workers own scratch. Start with workers only doing
detached legality, then include other pure candidate work if profiles justify it.

There are two reproducibility contracts:

1. **Serial compatibility:** preserve the existing candidate stream, RNG draws,
   and acceptance ordering. If generation of the next candidate depends on an
   earlier result, it cannot be speculated without reproducing that dependency.
   Parallelism is limited to naturally independent work in this mode.
2. **Frozen-epoch search:** the owner creates a bounded ordered proposal batch
   from one publication, assigns random choices independent of worker identity,
   evaluates in parallel, and consumes results in sequence order. Worker
   completion order never selects the winning move. This is a new search policy;
   require reproducibility across worker counts, but not byte identity with the
   old serial search trajectory.

With a global revision, every accepted commit makes all later results stale.
This is an intentional first correctness implementation with limited speedup.
Fine-grained read sets permit results unaffected by earlier writes to survive.
For stale proposals, regenerate or reevaluate at the same defined sequence point
and recheck acceptance policy. Use a deterministic retry limit, initially two
speculative attempts, then evaluate synchronously under the owner to guarantee
progress. Record retries and fallback frequency.

Bound outstanding work by both bytes and proposals. Initial experimental limits
are 64 candidates per submitted batch, at most two batches per worker, and a
64 MiB aggregate budget for frozen retained facts, overlays, results, and scratch.
These are proposed defaults to validate, not established optimal values. Admit
smaller batches when the byte budget is reached; oversized proposals use serial
evaluation. Never truncate a cluster to satisfy a buffer limit. Divide the
budget among workers rather than multiplying a per-worker cache without limit.

Integrate with the existing parallel-refinement scheduler instead of running a
second pool beside it. Account for HeAP solver threads and other active phases
when sizing workers. Test 1/2/4/8 workers where the host supports them; report
throughput, end-to-end time, stale work, commit bottlenecks, and peak memory.

### 6.7 Incremental assessment reuse within a session

Begin with one most recent accepted-state assessment per LAB; add a bounded
content cache only after versioned reuse works. Track these states separately:

```text
DirtyLab -> EvaluatedLab(dependencies, query, outcome)
                |
        independently prepared artifact
                |
            PreparedLab -> RoutedDependenciesValidated

relevant edit -> DirtyLab + dirty preparation/routes/timing as applicable
```

An evaluated legal state does not install pin maps or reservations. Dirty status
is published with mutation; clean results become reusable only after matching
facts, query scope, rules, and dependencies have been checked. Net-property edits
follow reverse incidence even when occupancy is unchanged.

Cache keys include evaluator/rules, policy, query, normalized semantic content,
and session/dependency identity. Request IDs and changing epochs are provenance,
not semantic content; reissue them when instantiating a current result. A hash
accelerates lookup, but compare complete normalized fields before using a hit.
Keep cached allocations in their recorded local namespace and rebuild a current
translation, or use validated session keys. Never retain a capture's raw net
pointers or blindly attach an old plan to a new map.

Eviction or allocation pressure reduces reuse, not correctness. Record live and
retained bytes, hits, misses, invalidations, and hit cost. The cache is disabled
for every mutation path lacking complete dependency tracking.

### 6.8 From assessment reuse to iterative builds

Deliverables are deliberately separated:

| Level | Reused work | Additional requirement |
| --- | --- | --- |
| Same placement session | Immutable facts and unaffected LAB assessments | Revisions, dependencies, bounded cache |
| Edited packed design | Validated placements outside a dirty repair region | Entity matching, active cluster set, incident-route invalidation |
| Prepared/routed design | Physical artifacts whose complete dependencies match | Provenance and removal/rebuild of reservations, pin maps, generated cells, routes |
| Separate process/build | Prior packed/physical state | Versioned checkpoints, compatible model/settings, durable matching |

Stage 4 first delivers the first level and a conservative dirty-placement repair
adapter. For edited designs, begin from a clean packed representation, match
entities conservatively, transplant valid placements, and legalize an explicit
active set. Unchanged placements are reusable preferences; actual user constraints
remain hard constraints. Expand the repair region or use the full placer when
local repair exceeds deterministic search/work budgets.

Do not move an endpoint while leaving its incident routes bound. Before physical
artifact reuse, track the ownership of reservations and every generated
route-through cell/net and pin rewrite. Remove or rebuild all affected artifacts
before re-preparing. Stage 1 edit tickets do not supply this wider provenance.
Until that machinery exists, perform full preparation/routing from the clean
packed design with reused placement. A plain output JSON is not assumed to be a
complete restorable Mistral checkpoint.

Persistent checkpoint restoration and cross-build semantic matching follow the
[parent design](parallel-incremental-design.md). Ambiguous matches are dirty.
Changes to device, rules, clocks, packing semantics, or constraints can invalidate
large regions or whole phases. Always run complete final legality/timing checks
and generate a complete bitstream initially; incremental compilation does not
imply partial FPGA reconfiguration. Measure synthesis, packing, matching,
restoration, repair, preparation, routing, and signoff separately before claiming
an iterative-build speedup.

### 6.9 Transaction and reuse acceptance gate

- Serial detached and bind/check/revert traces agree under compatibility policy,
  including rejected candidates, displacement queues, strengths, and RNG state.
- Rejected/stale candidates preserve canonical placement, counts, queues, and
  routes. Inject failure before and during publication and verify rollback or
  explicit failed-context handling.
- Exercise ABA-style move-and-restore, net-global changes without moves, stale
  illegal results, altered scores, shared targets, source-LAB invalidation,
  multi-LAB carry displacement, and direct legacy mutation while results exist.
- Independently evaluate frozen candidates with C++ and Rust; compare complete
  results before validating commit behavior.
- Require deterministic ordered decisions for the selected search policy across
  worker counts; test delayed/out-of-order workers, cancellation, and handle
  destruction only after readers finish. Use race/lifetime tooling where
  available and record its limits.
- Compare incremental outcomes with full recomputation on unchanged rebuilds,
  local logic changes, carry edits, added/removed objects, global/clock changes,
  and policy/region changes. Require final legality, connectivity, timing, and
  resource quality; edited designs need not route byte-identically to clean runs.
- Validate budget exhaustion, eviction, retry fallback, unsupported capability
  fallback, and full rebuild. No optimization may turn a supported build into a
  silently partial or incorrectly certified result.

## 7. Implementation sequence and file boundaries

Only the initial sequence is linear. Stage 2 experiments can overlap stage 3
schema work after the stage 1 boundary is stable; retained decoding and parallel
authority wait for stage 4's mutation audit.

| Unit | Proposed change | Review evidence |
| --- | --- | --- |
| 1a | Host allocation type and C++ application extraction | Existing behavior unchanged |
| 1b | Retained dispatch result, private ticket, complete translation checks | Result/ticket misuse and malformed-plan tests |
| 1c | Ordered preparation edits, preflight/journal, shadow comparison | Exact touched-state equality, failure injection |
| 1d | Verified/experimental Rust plan consumption | Feature fixtures and complete P&R parity |
| 2a | Single-search capture | Byte-identical DTO/map and isolated timings |
| 2b | Decoder/temporary/stack experiments, one at a time | All error-order parity and measured resource effects |
| 3a | Explicit input policy, V2 capture/replay, detached C++ reference | Cached/recomputed counts and schema validation |
| 3b | Rust ALM rules, input accounting, MLAB checks | Per-rule differential corpus |
| 3c | Scoped composition and separate whole-LAB API | Live scoped parity, deliberate scope-difference cases |
| 4a | Mutation audit, session IDs, global revision, frozen capture | Every active mutation invalidates correctly |
| 4b | Legacy rollback-strength correction, then serial overlays and journaled HeAP commit adapter | Separate fix/baseline, compatibility traces, rejection/rollback invariants |
| 4c | Immutable owned batch boundary and bounded worker scheduling | Lifetime/error tests; deterministic results |
| 4d | Fine-grained dependencies, ordered parallel commits | Conflict/staleness tests and worker scaling |
| 4e | Dirty-LAB assessments and conservative placement repair | Reuse versus full recomputation, bounded fallback |
| Later | Prepared artifact provenance and checkpoint-backed build reuse | Teardown/restore equivalence and end-to-end edit benchmarks |

Proposed source organization:

```text
mistral/lab_control_plan.h/.cc       host tickets, translation, preparation edits
mistral/lab_legality.cc             dispatch shared by verdict and plan callers
mistral/lab.cc                      legacy reference and physical application
mistral/lab_abi_v2.h                separate full-LAB transport contract
mistral/lab_snapshot_v2.h/.cc       normalized full-LAB capture and replay support
mistral/lab_model_v2.cc             detached full-LAB C++ reference
mistral/lab_transaction.h/.cc       backend overlays, dependencies, commit adapter
common/place/placement_transaction.h  common capability boundary
common/place/placer_heap.cc         first serial transaction caller
common/place/parallel_refine.cc     existing scheduler integration
rust/npnr_mistral_lab/src/          typed full-LAB model, rules, frozen batches
rust/npnr_mistral_lab_ffi/src/      V2/batch exports and ownership boundary
mistral/tests/                     replay, preparation, transaction, reuse tests
```

Keep reference implementations usable during rollout, including Rust-disabled
Mistral builds. Avoid adding Mistral dependencies to generic or other backends.
Update CMake, Cargo, CLI documentation, fixtures, and the implementation record
with each unit. A capability adapter may fall back for an unsupported architecture;
it must not weaken that architecture's existing constraints.

## 8. Release decisions and unresolved empirical choices

Each stage ships as a reviewable opt-in capability with its own gate. Default
promotion requires broader device/design evidence, exact applicable semantics,
explicit preparation and transaction guarantees, and an accepted runtime/memory
tradeoff. Successful microbenchmarks alone are insufficient. Rust's type and
ownership benefits remain valid reasons to retain a component with a documented
small cost; no stage promises a language-driven speedup or heap reduction.

Decisions deliberately deferred to measured prototypes are the optimal V2 memory
layout, whether decoder views beat owned values, batch size, worker threshold,
cache size, and whether fine-grained revisions repay their maintenance cost.
Their safe fallback is already defined: bounded owned values, serial evaluation,
coarse invalidation, cache eviction, and full recomputation. Whole-LAB query
scope, validation, deterministic ordering, and complete mutation coverage are
correctness contracts and are not tunable performance shortcuts.

## 9. Stage 6 — density

The first full-core attempt (tracker, 2026-09-17) measured why a 55k-cell
design does not build: at 1.4 LUT cells per ALM the design needs 67% of
the ALMs and the strict legaliser finds no legal home for the last
cells; Quartus packs the same cells at 1.92 per ALM into 49%. The
architecture asks for pairing: a Cyclone V ALM takes two LUTs only when
they fit its shared-input structure, and a LAB feeds its ALMs from 46
input lines.

### 9.1 Pairing as clusters

nextpnr packs lightly and legalises at placement time; on this family
that leaves ALM pairing to whatever the legaliser's random search
happens to find. Stage 6 keeps the philosophy and adds one pack-time
decision: LUTs that share input nets are paired under the checker's own
rule (64 LUT bits; eight unique inputs with only A and B shareable; a
6-input LUT never pairs) and emitted as two-cell clusters, so the placer
moves a pair as a unit and every legality check sees both cells. The
cluster placement is overridden in the arch so a pair lands on the two
halves of the ALM its root bel is in; the base cluster mechanism cannot
express "any ALM, first half" because an absolute z pins the root to one
ALM. Pairs survive checkpoints because they are ordinary cluster fields.
The packer is opt-in (`--alm-pairing`) and stays unpromoted; its level
selects how far from shared inputs it will look for a partner.

### 9.2 Where the wall moves to

With pairs, the full core places; its routing then plateaus at twice
the overuse of the unpaired placement. The LAB input limit of 42 counts
lines; the lines have structure. Measured from the routing graph: every
LUT pin can be fed by 21 to 25 of the 46 lines, the A and C pins from
one group of 25 and the B and D pins from a disjoint group of 21, the E
pins from a group of 22 and the F pins from a disjoint group of 24, each
line in one group of each pair. A net that reaches pins of two classes
in one LAB needs two lines, or a LUT whose inputs are permuted so it
does not. The count cannot see this, and no lower count fixes it: with
pairs, limits of 36 and 30 never legalise at all.

### 9.3 Input lines cannot be assigned ahead of the router

The per-class rule was never written: under the input count of 42, a
line is never more than one pin use and the tightest quadrant needs two
per net, so the rule cannot bind. The count already implies that a line
assignment exists for every LAB it admits; what fails is router2 finding
it. Assigning the lines ahead of the router (matching from the routing
graph, pins bound through their line's pip at placer strength) was built
and measured: it finds a perfect matching in every LAB and costs 40 to
60% more wires, because a line decides which fabric muxes feed it and
that choice belongs with the fabric route. The negotiation has to be
fixed inside the router with the fabric in view; the tracker holds the
numbers and the design.

### 9.4 Spreading by demand, by congestion, and what is left

The overuse after pairing is 59% short and medium fabric wires around
the packed LABs and 23% input lines. `--spread-demand` gives the cut
spreader an arch hook: a comb cell weighs its unique input count and a
bel offers four units, so input-heavy regions spread thinner. Measured,
it redistributes without thinning: the paired probe's stubborn wire goes
away, the core's plateau drops 10%, and a uniform thinner factor still
does better and saturates at the design's own occupancy. The loop that
lets placement hear from routing is closed inside HeAP by
`--spread-congestion`: every spreading pass rebuilds a bounding-box
wire-density estimate over the current positions and inflates the units
of LAB cells in tiles above a threshold, so the spreader thins where the
router would fail; it is the best configuration measured on the core and
it still does not converge, because the estimate is a proxy and the
fabric at this density is short of the real thing. Per-class feasibility
in the LAB checker was analysed and not built: nets on
A/C pins at most 25, on B/D at most 21, on E at most 22, on F at most
24, a net that must reach two classes counted in each, using the pin
assignment the legaliser already makes and permuting plain LUT inputs
so a net keeps one class across the LAB. That is a change to the rules
that V1, V2, and the Rust evaluator implement in parity, so it is the
rules revision the concluded crate reopens for, validated on the probe
first (router1 should no longer be needed at level 1) and then on the
full core's routing.

### 9.5 Where the wires go: the fabric's entry structure and the demand it sets

Unit 6f asked the router to close what spreading could not, and the
measurement turned the question around. On the identical exec-probe
netlist handed to Quartus as WYSIWYG primitives, nextpnr's routing uses
2.8 times the fabric wires Quartus's does, while the routing graph's
wire counts match Quartus's resource table within a few percent (the
local and block interconnect counts exactly). The placement is not the
cause: its half-perimeter wirelength per sink is below Quartus's. The
cause is what one LAB entry costs. A LAB's 46 input lines are fed by row
wires: at a central LAB, 88% of the line inputs are H3 or H6, 2.5% are
column wires, and the LAB directly above drives 64 column wires of
which one reaches a line of the LAB below. A connection to a vertical
neighbour is therefore a stair of two or three wires (row, column, row)
where a horizontal neighbour costs one, and a net whose sinks sit in
several rows pays that stair per row. Quartus's placement knows it:
7% of its sinks sit in the driver's column against nextpnr's 15%, it
touches 1.52 rows per net against 1.80, it puts 95% of LUT-to-register
pairs in one ALM against 16%, and so it enters a LAB with about one
fabric wire where nextpnr spends 1.83. The delay-based base cost adds
its share: it prefers long wires for short hops (13 times Quartus's use
of the length-12 column wires) and a unit wire cost buys 18% fewer
fabric wires for 4% of Fmax.

On the full core this demand is 67% of the fabric device-wide at the
router's best iteration, against Quartus's 24% average and 66% peak for
the same core; the median tile is at 77% and the congested quarter of
the device at 91%. No negotiation resolves that: six router2 variants
from the same placement (criticality-independent legality pressure, no
timing-driven routing, a periodic full re-route, a contested-only
re-route, an admissible A* estimate) move the plateau by 13% either
way; a unit wire cost halves it, because a router that counts wires
stops paying for stairs and long wires it does not need, and still
none converges. The re-route and the unit cost are landed as opt-in
Mistral options because they are measured and cheap; the record's
conclusion is that the remaining gap is a placement
cost model, not a router: column hops must cost what they cost, the
register must pack with its LUT the way pairing packs two LUTs, and a
net's sinks must be drawn into fewer rows and LABs. The router's own
lever after that is the base cost.

### 9.6 The register packs with its LUT

The first placement-side lever from 9.5 is the one Quartus applies to
95% of LUT-driven registers: the register sits in the ALM half of the
LUT that drives it, so the data path is the half's internal wire and
costs no LAB input line, no fabric wire, and no route-through LUT. The
arch already admits this (the checker's data-in rule) but nothing asked
for it: HeAP places registers and LUTs as independent cells and lands 7%
of them together. `--register-packing` runs after `assignArchInfo` and
attaches one register to its LUT's cluster as a child at relative z 2
(the root's half) or 4 (a pair partner's half); `Arch::getClusterPlacement`
puts it on the register bel of whichever half the LUT lands on, and the
same override now places pairs and register clusters alike. The arch
admits one register per half, so a LUT with several registers keeps one.

Two rules the packer must respect came out of the first run. The cut
spreader weighs a cluster by all its members, so a LUT-plus-register
cluster counted as two LUTs in the LUT pass;
`PlacerHeapCfg::cluster_units_by_bucket` counts only the members of the
pass's bucket. And a pair whose two registers cannot share a LAB's
control lines (the model's DATAIN allocation: a non-global clock, two
enables and a synchronous clear do not fit) can never be placed; HeAP's
detached transaction rejected it at every ALM until the cell placement
timeout. The packer therefore asks the LAB control model
(`registers_share_a_lab`) whether the cluster's own registers fit an
empty LAB before attaching one, and reports the conflicts it kept apart.

### 9.7 The row is the cheap direction

Section 9.5 measured the fabric's asymmetry: a LAB's input lines are fed
by row wires, so a connection to a vertical neighbour costs 2.3 fabric
wires where a horizontal one costs 0.9, and the ratio holds at every
distance (2.5 to one). HeAP's solver knew half of it (`hpwl_scale_y` is
2 for Mistral, and the annealer's delay estimate weighs a vertical tile
2.7 times), but the two stages that move cells after the solve did not:
the cut spreader alternates its cut axis regardless of cost, and the
strict legaliser searches a square box and picks among legal candidates
by unweighted Manhattan distance to the drivers. `--row-cost W` makes
the three consistent: the solver's vertical scale becomes W, the
spreader cuts a region along the axis that is longer in cost units (so
cells move along rows until the region's shape matches the ratio), and
the legaliser's box is W times wider than tall with candidates scored
by weighted distance (`PlacerHeapCfg::anisotropic`). On the probe the
placement's sink classes move to Quartus's (same column 15% to 9%, rows
per net 1.80 to 1.55 at W = 4) and the fabric wires fall 9%, most of it
column wires; the router takes more iterations to settle the busier
rows, and a few percent of Fmax go with the longer horizontal paths.
The weight is a knob, not a model: the true cost of a connection
depends on where the LAB input lines are reached from, which is the
LAB-level assignment of the next unit.

With the row cost the full core's routing changed character: instead of
a plateau of thousands of diffuse, churning wires it falls at every
iteration to a few dozen, and those are the structured conflicts 6d
diagnosed, LAB input lines in LABs at the top of the input distribution,
which negotiated congestion keeps swapping and router1's fallback cannot
finish. 6f's periodic re-route of the nets on wires with history is the
finisher: every twenty iterations it re-routes the contested nets
together under the accumulated costs, and two rounds took the core from
65 overused wires to none. That is the first complete routing of the
core, under the unit wire cost. The delay cost with the same recipe is
the next measurement, because the unit cost's routes are delay-blind and
the signoff Fmax it produces is a lower bound.

## 10. The Rust evaluator at parity: resident LAB snapshots

The crate was concluded in Stage 4 as a parity harness on a value
contract: every query captured the whole LAB into a record, Rust
validated and evaluated the record, and no live pointer crossed the
boundary. That contract made the evaluator independent and safe, and it
made the authority mode slow by construction: on the exec probe the
capture was 62% of every query and the record's validation another 19%,
and the full core's legaliser asks tens of millions of times, so the
mode ran an order of magnitude behind the live C++ check. The user
directed a performance revision to reach parity (2026-09-18), the one
reopening the concluded entry had not foreseen.

The resident session keeps the contract and removes the capture. Rust
owns one snapshot per LAB, validated once at reset and then patched one
ALM at a time (`AlmPatchV2`: the ALM's LUT and register facts, net ids
as run-stable keys, since the rules compare ids only for equality).
The arch marks the ALMs whose bindings or facts changed from the same
hooks that version LABs for reuse (`Arch::lab_alm_dirty`), so a query
sends only what changed since the LAB's last query, usually one ALM,
and the session evaluates over cached per-ALM input counts and a cached
control projection; the verdict equals `evaluate_lab_v2` over the same
facts field for field (the crate's oracle test says so over random
patch sequences), which keeps the C++ detached evaluator and the
capture path as the parity harness in the shadow and verify modes.

Two placement patterns shaped the protocol, and the core's profile
shaped the granularity. The legaliser binds a candidate, asks, and
usually unbinds; the annealer swaps, asks, and usually reverts. So a
changed bel travels as a trial the session holds in view for that
evaluation only, and is committed when the next query of that LAB finds
it unchanged; a bel whose occupant is back to the one the committed
facts were built from sends nothing, and a LAB just reset or a chain
bound before any query travels as commits. The patch is one bel, not
one ALM, because the placer changes one bel per query and the core's
legaliser spent a third of each query building and comparing the other
five. Register queries run the control-set rules on a resident mirror
of the control model's own snapshot, with the trial rows substituted on
a copy, instead of projecting forty registers into a record and
validating it each time; the mirror maps the control net keys to the
model's ids with a reference-counted table. The ALM rules read the six
slots by reference with the trial substituted, so nothing is copied for
a trial, and the ALM's unique input count travels in each patch as the
arch keeps it, one fact among the others the arch supplies; the
authority mode takes it, the shadow and verify modes recompute it from
the facts and report a difference, as the capture path always did. What
remains per query is the FFI call, the ALM rules for the queried ALM,
and the control rules for a register query, and on the probe the Rust
authority with the annealer on the overlay seam runs at the legacy
path's wall time; the core's number is the measurement in the tracker's
parity entry.

## 11. Coding rules and what enforces them

The intent of the Rust work is fast iteration by agents on the LAB
rules, with safe idioms and concurrency as the by-products. A rule
serves that intent only when a check enforces it inside the iteration
loop, in seconds for the crate and in a minute for the flow; a rule in
prose is a rule the next session forgets. The five rules landed on
2026-09-19 are each paired with their check.

The crates already forbade `unsafe` and returned typed errors at every
boundary, but nothing stopped an `unwrap` from turning a boundary error
into an abort, and an audit found one `expect` in the FFI. The lints
`clippy::unwrap_used`, `expect_used`, `panic`, and `unreachable` are
denied in both crates, with tests exempt, because a test's panic is its
failure report. Indexing by an id the boundary has validated is the
panic source the lints cannot see; the FFI's `catch_unwind` and the
poisoned handle remain for that, as the backstop rather than the rule.

The resident protocol's oracle compared the resident evaluator to the
capture path over random traffic, but the traffic it generated was
shaped by the test's author and not by the caller: the burst a chain
bind sends was missing, and the core's verify run found the defect
hours after the crate's tests had passed. The module now lists the
patch shapes the arch sends, the generator produces each of them, and
the test asserts that it did; a new shape in `mistral/lab_resident.cc`
is a change to that list and the generator in the same commit.

Telemetry is a file of its own, `--telemetry`, rather than fields in
`--report`, because the report's byte identity across builds is the
gate for every resume and reuse path and any new field would break it.
The file carries what the record has been copying from logs by hand:
the checksum the log prints, the options that shaped the run, the
counters the stats lines print, and the phase times. It is written at
the end of each phase, so a run that dies in the router leaves the
placement's file behind. The option travels in `ArchArgs` for the same
reason every Stage 5 and 6 option does: a settings key would intern a
string before the netlist is read.

The gtest fixture shares one context across the suite for the chip
database's sake, so a test that leaves a cell or a net behind changes
the tests that run after it, and the failure appears somewhere else.
The fixture now counts cells and nets before and after each test and
fails the test that leaked; two tests had, without any symptom yet.

The gate script exists because the conventions listed six commands and
three trees, and a session under time pressure ran the subset it
remembered. `mistral/tests/gate.sh` is the whole list, with the probe
identity checked against the recorded checksums and report hash, in
58 seconds; a deliberate change to the default path updates those
constants in the same commit, which makes such a change visible in the
diff.

## 12. The live monitor

The telemetry file gave the record its numbers at the end of a run;
the monitor gives them during it. The core's strict legaliser runs
twelve minutes without a log line, and the router's plateau argument
of unit 6h was made by reading iteration lines after the fact; an
operator, or an agent watching a run, wants the query rate, the phase
clock, and the overused-wire curve while they happen. `--monitor` is
that view, and it is the user's request that it be Rust.

The renderer is a pure crate: a snapshot of counters and the log tail
in, lines of text out, so every panel is asserted line by line in the
crate's tests and the terminal is not part of the tested surface. Its C
ABI lives in the existing FFI crate as a module rather than in a
second static library, because two Rust static libraries linked into
one binary each carry the runtime and collide; the module keeps the
crate's rules, a locked handle poisoned by a panic, envelope checks on
every pointer, and layout asserts on both sides of the two records.

The terminal cannot carry both nextpnr's log and a frame. The session
removes the log's terminal streams for the run and keeps the file
stream, so `--log` keeps the text and the tail panel replaces the
scroll; every message reaches the tail through the log hook, warnings
and errors included, and the last frame stays on the terminal when the
run ends, with the cursor handed back below it. No alternate screen,
for that reason: the frame is the run's summary.

The ticker thread reads nothing of the netlist. The legality and
control-set counters were atomics already; the resident session's five
were plain and are now single-writer atomics bumped with a relaxed
load and store, which is the plain add the hot path paid before, and
the resident pointer is read with `atomic_load` because the owner
assigns it once on the first dispatch. Cell and net counts are
snapshotted on the owner thread at each phase entry, and the phase
clock is the owner's report. The run is therefore unchanged by being
watched: the probe's checksums and report are byte-identical with and
without the monitor, and a Rust-mode run's resident totals are the
same either way.

The monitor does not replace the telemetry file. It shows this run;
the file, one per run, is the record across runs, and a recent-runs
view would read the files, not the process.

## 13. Pack-time admission by the placer's authority

Two packers form clusters the placer must later put on one ALM: the
ALM pairing of unit 6b and the register packing of unit 6g. Each
decides with a rule of its own. The pairing rule is a structural
restatement of the checker's input rule (three exclusive lines per
half, two shared), kept at least as strict as the checker by a unit
test; the register rule asks the C++ twin of the control model. A
cluster that the packer admits and the placer's authority refuses is
the worst failure the flow has: it is rejected at every ALM of the
device until HeAP's timeout, which is how unit 6g found its
control-set conflict. With the Rust evaluator promoted to the default
authority the split is also one of implementations: the packer asks
C++ and the placer asks Rust.

The packers' rules stay, as what they are: search filters. The
pairing search evaluates its rule for every candidate on every shared
net, millions of times on the full core, and the rule is a few
integer operations; routing that through an evaluator call would cost
more than the packing it guards. What changes is who admits the
result. Before a packer commits a cluster it asks the question the
placer will ask: is this cluster legal on a clean ALM under the run's
LAB legality authority? The members are laid on the first clean LAB
through the same cluster placement the placer uses and evaluated
through the seams the placer's paths already have, the bel overlay
for the C++ rules and the overlay capture for the Rust evaluator,
with nothing bound and nothing new on the Rust side. In the shadow and
verify modes both answer and are compared, as everywhere else. A
refusal undoes the cluster, is counted in the packer's report, and
the cells go on unclustered, which is always placeable.

The cost is one evaluation per committed cluster, some twenty
thousand on the full core, against millions for the search. The
invariant the unit test asserted about two functions becomes a
property of every packed design: no cluster leaves the packer that
the placer's authority has not admitted. The exit criterion is that
the admission never fires on the recorded designs, so the pairings,
the register packings, and the results after them are byte-identical
to those before it; if a future rules revision makes the two
disagree, the run says so in its pack report instead of in a
placement that never finishes.

## 14. The tile scan query

The parity entry left the Rust authority at 59 ns per query over the
full core's 5.3 billion and named a per-tile query as what would halve
it. The design began with a measurement of how those queries cluster,
and the measurement decided both whether to build it and what to
build. On the exec probe under the recipe's options nine queries in
ten are a single legal answer, and a per-tile batch would save
nothing. On the core itself 99.55% of the 5,348,124,234 queries are
refusals, in runs on one LAB that average thirty queries, 96% of them
in runs of seventeen or more: an unclustered cell, usually a register,
bound to bel after bel of a crowded LAB, asked about, and unbound,
forty times per tile visit. The cost of that pattern is not mainly the
call. It is the bind, the dirty marks, the patch, the restore, and the
unbind around each of the forty questions.

The strict legaliser's scan of a tile ends at the first available bel
the arch accepts; refused bels are skipped, and the one random draw in
the loop is made only for occupied bels. So the batch question is not
a mask over the tile but the scan itself: given this cell's facts and
these free bels in this order, which is the first the rules accept?
The session answers by holding the candidate in view at each bel in
turn, exactly as a trial patch would put it there, and stops at the
first legal one, so it evaluates the rules no more often than the
per-bel scan would and never binds anything. Counts for the
candidate's ALM are recomputed from the facts, as the harness modes
always did, since the arch keeps a count only for what is bound. The
answer is equal by construction to the per-bel answers, and the
crate's oracle asserts it against them.

The placer uses the answer without changing its search. The scan
order, the filters, the ripup draws for occupied bels, and the
acceptance of a bel are as before: the bel the batch names is still
bound and still certified by the ordinary validity check, which stays
the only thing that accepts a placement. What the batch removes is
the bind, check, and unbind of the bels before it, which the same
rules have just refused. The batch is asked only after a first live
refusal in the tile, for the bels that remain, so a tile whose first
free bel is legal, the common case on an uncrowded design, costs what
it costs today. In the shadow and verify modes the batch is advisory:
nothing is skipped, every bel is checked live as before, and each
prediction is compared with the live answer, which is the harness for
this path. The legacy mode does not use it and is untouched.

The hook is one optional callback in HeAP's configuration, like the
cluster transaction before it, and one call in the FFI beside the
resident evaluate. The serial search order and the RNG stream are the
reference: the exit criterion is a byte-identical placement on the
probe under both option sets and on the core, with the verify harness
at zero mismatches, and the measured placement time of the core
against both the per-bel Rust path and the legacy path.

### 14.1 The scan asks cheapest first

The first build of the scan held the candidate in view at each bel and
took the session's full verdict there. A profile of the core's
legaliser showed the scan's own evaluation at three fifths of the
time, the control rules only an eighth of that, and the recomputation
of input counts, which the verdict does before anything else, as
large as the ALM rule. A count of the refusal reasons inside the scans
said why: of the bels a scan evaluates on the core, 55% are the second
register bel of an ALM half, which the ALM rule refuses for any
register (upstream's checker carries the note "why are these FFs
broken?"), 21% have no path left for the register's data, and the
LAB's input limit and the control rules refuse 12% each. Three bels in
four fail the ALM rule alone.

A scan needs the answer and not the reason, and the verdict's
predicates are independent, so inside the scan they are asked in order
of cost and the first refusal ends the bel: the ALM rule on the view,
then the input total with counts recomputed only for the trial ALMs,
then the control rules on the mirror. An MLAB, or a control set larger
than the mirror, takes the full verdict as before. The per-bel path
and its verdicts, reasons included, are untouched. The oracle that
holds every scan against the capture path bel by bel is the proof
that the order changes nothing.

## 15. The register capacity the rules admit

An ALM has four register bels, two per half, and the ALM rule refuses a
register in the second of each pair whatever else the ALM holds; the
upstream checker's line reads "TODO: why are these FFs broken?". The
reason count of section 14.1 made the consequence visible: more than
half of what the legaliser asks on the full core is whether a register
may sit in one of those bels. The answer never varies, so the question
is the defect. It has two costs. The bels are always free, so every
tile visit of every register scans them. And HeAP's cut spreader takes
a tile's capacity from the number of bels in the cell's bucket, so it
spreads registers as if a LAB held forty where it holds twenty; the
registers it over-assigns are the ones the legaliser then carries from
tile to tile, which is the stuck-register pattern of unit 6a.

The rule itself is not this unit's subject. Whether the second bel of a
half can be made usable is a question about the device and the
bitstream, with silicon behind it, and it stays where upstream left it.
What this unit does is tell the placer what the rule already says. With
`--usable-register-bels` the second register bel of each half leaves
the register bucket: `getBelBucketForBel` files it under a bucket no
cell type maps to and `isValidBelForCellType` refuses it for a
register, so HeAP's bel lists, the spreader's capacity, the
legaliser's scans, and the annealer's proposals all see twenty register
bels per LAB. Nothing can be placed that could not be placed before,
since no register was ever legal there; what changes is where the
spreader sends registers and what the search spends its time on, so
the placement changes and the option is opt-in, with its own quality
evidence on the probe and the core.

The bucket for the hidden bels is an identifier the arch already has
and nothing maps to (`MISTRAL_MCOMB`, the bel type of LUTRAM-capable
LUT bels, which themselves file under `MISTRAL_COMB`), because a new
identifier would be interned ahead of the netlist and shift every net
index of the default path.

**Outcome (2026-09-21): negative, removed.** On the probe the option is
inside the seed spread. On the core it halves what the scans ask and
nothing else improves: the legaliser is slower on two seeds of three,
the router needs half again as many iterations, and one seed no longer
routes. The overstated capacity appears to do useful work: it lets the
spreader leave a register beside its logic and leaves the choice of
the real bel to the legaliser. The argument above, that telling the
placer the truth can only help, was wrong for this placer, and the
record of it is the tracker's entry. The scanning cost that motivated
the unit is addressed where it arises, in the scan (section 14.1) and
in the legaliser's loop (section 16.2).

## 16. Six hot paths of the full core flow

A Time Profiler recording of the whole core flow under the recipe
(tracker, "Time Profiler run of the full core flow") put strict
legalisation at 54% of the run, router2 at 20%, the annealer at 11.5%,
and HeAP's solver at 10.5%, and named six paths worth work. Each design
below states what the profile shows, the change, why the result does
not move, what holds it, the expected gain, and what it costs. None
changes a placement or a routing: the exit criterion of every one is
the recorded checksums on the probe and the core, and the gain is
whatever the core's Time Profiler run says afterwards, not the
estimate given here.

The order of work is 16.4, 16.6, 16.2, 16.1, 16.3, 16.5: the two small
Rust changes first because 16.1 builds on both, then the C++ loop,
then the largest unit, then the two that touch upstream's code. Each
lands as its own commit with its own core timing.

### 16.1 Cluster candidates through the resident session (Rust and C++)

**Profile.** `try_place_cluster` is 91 s, 23% of strict legalisation.
HeAP offers every pair and register cluster to
`PlacerHeapCfg::place_cluster_transaction`, 19.6 million times on the
core with 96.6% rejected, and the Mistral callback prepares the edits,
freezes them (`freeze_placement_candidate`, 52 s: one whole-LAB overlay
capture of 3.8 KB per edited bel, four for a pair with two registers),
evaluates every record in C++ (`evaluate_placement_candidate`, 13 s),
and evaluates every record again in Rust as a fatal cross-check
(`placement_candidate_rust_matches`, 14 s). The authority on this path
is the detached C++ evaluator in every legality mode, the promotion
notwithstanding.

**Design.** In the Rust legality mode the candidate is answered by the
resident session, which already holds the LAB.

- Crate: `ResidentLabs::evaluate_edits(lab, patches, edits, recompute)
  -> Result<bool, ResidentError>`. `patches` bring the LAB up to date
  through `sync`, as for `evaluate` and `evaluate_scan`. `edits` are
  bel patches held in view together and never applied: the facts of
  the cell an edit places, or empty facts where an edit displaces one.
  The answer is the conjunction the frozen path computes per edit,
  asked once per distinct predicate instead of once per edit: the ALM
  rule for every distinct ALM an edit touches (a removal can break an
  ALM too: a register loses the LUT that fed it and needs a data path),
  the LAB's input total once with counts recomputed for the touched
  ALMs, the control rules once if any edit is a register bel (the
  edits name their bels, so one evaluation is exact here and owes
  nothing to 16.4), and the full verdict for an MLAB. For a pair with two registers that
  is one ALM check, one total, and one control evaluation, where the
  frozen path makes four captures and eight evaluations. It is patch
  shape 9 in the module's list.
- Budget: the pending trials of the sync and the edits share the
  `MAX_TRIALS` slots in view, so the batch builder is given
  `MAX_TRIALS - edits` and sends more changed bels as commits, as it
  does for the scan. A candidate with more edits in one LAB than the
  budget, which is a carry chain, is not covered and takes the frozen
  path as today.
- FFI: `npnr_mistral_resident_v2_edits(handle, lab, patches,
  patch_count, edits, edit_count, flags, legal)`, with the envelope
  checks and the poisoned handle of its siblings.
- Arch: `ResidentLabLegality::edits(...)` sharing `build_batch` and
  `note_sent`; `placement_candidate_resident(arch, transaction)` groups
  the prepared transaction's edits by LAB, builds each edit's facts with
  `capture_cell_v2_keyed` (or empty facts for a displaced cell), and
  declines what it does not cover (a bel outside a LAB, a LUTRAM cell,
  an over-budget LAB).
- Callback: in `rust` mode, prepare, ask the resident session, then
  reject or commit; the freeze, the C++ evaluation, and the cross-check
  are not run. In `shadow` and `verify` both paths run and are compared,
  a difference fatal in verify, which keeps the existing cross-check's
  meaning. In `legacy`, and for the lookahead's worker threads, which
  need frozen values by construction, nothing changes.

**Identity.** The verdict per candidate is the same conjunction of the
same predicates, so HeAP sees the same accept and reject sequence. The
commit path (`commit_placement_transaction`) and the revision stamps are
untouched; the resident answer is synchronous on the owner thread, so
it cannot be stale.

**Held by.** The oracle gains edit sets: several placements and
removals over one or more ALMs, registers included, compared with the
capture path's per-edit conjunction, with coverage asserted for
accepted sets, rejected sets, removals, and over-budget refusals. A
gtest compares `placement_candidate_resident` with the frozen
assessment on the register packing fixture's clusters. Verify mode on
the probe and the core compares every candidate in the flow.

**Gain.** 91 s to an estimated 15 to 25 s.

**Cost and decision.** One function in the crate and one FFI call: new
Rust surface, so a decision row. The decision it records is larger
than the surface: in the Rust mode the cluster path's authority moves
from the detached C++ evaluator to the Rust session, consistent with
the promotion, and the C++ evaluator remains the legacy authority and
the harness.

**Outcome (2026-09-21): kept.** Identity on the probe and the core with
the transaction counters exactly the baseline's; 99.8% of the core's
19.6 million candidates answered by the session; strict legalisation
282 s against 370 s. The estimate held because it was the inclusive
time of functions that stopped running, not a share of a loop.

### 16.2 The legaliser's scan loop evaluates its filters once (C++)

**Profile.** `try_place_cell` is 64 s of self time and
`Arch::checkBelAvail` 35 s beneath it, a quarter of strict
legalisation. The loop visits up to forty bels per tile visit and for
each evaluates the region test, the control-set filter, and the
availability. Since the tile scan, the look-ahead that lists the
remaining candidates for the batch evaluates the same three for every
bel from the current position on, and the main loop then evaluates
them again.

**Design.** The look-ahead records what it computes. A member vector of
one byte per tile position holds two flags, passes-the-filters and
available, filled for the positions from the batch's start onward; the
main loop reads the flags for those positions instead of calling
`passes` and `checkBelAvail`. Positions before the batch's start are
evaluated live as today, and a scan in which no batch is asked never
fills or reads the flags.

**Identity.** The filters are pure functions of the cell and the bel.
Availability can change during a scan only through the loop's own
binds, and every one of those is either undone before the loop
continues (a refused trial, a refused ripup with the displaced cell
bound back) or ends the loop. So a flag read later equals the call it
replaces, and the ripup draw, which is made only for a bel the flag
says is unavailable, is made for exactly the same bels.

**Held by.** The probe's routed identity in the gate and the core's
placement checksum. No new test: the change has no behaviour of its
own to assert beyond identity.

**Gain.** An estimated 25 to 35 s of the 100 s. With
`--usable-register-bels` the list itself is a third shorter.

**Cost.** Twenty lines in `placer_heap.cc`, which this fork already
changes in this function.

**Outcome (2026-09-21): kept, smaller than estimated.** Identity on the
probe in three modes and on the core; strict legalisation 370 s against
378 s, about 7 s. The duplicate was the smaller share of the loop's
availability tests.

### 16.3 HeAP's equation system appends, then merges (C++)

**Profile.** The solver phase is 75 s and Eigen's conjugate gradient is
12 s of it. Building the system is 57 s: `build_solve_direction` 34 s
with `EquationSystem::add_coeff` inlined, and
`std::vector<std::pair<int, double>>::insert` another 17 s. `add_coeff`
keeps every column sorted by row: a binary search per coefficient, an
addition where the entry exists, and otherwise an insert that shifts
the column's tail.

**Design.** `add_coeff` appends `(row, value)` to the column. `solve`
begins with a finalise pass per column: `std::stable_sort` by row, then
one forward pass that folds each run of equal rows into its first
entry by adding the later values in order. The matrix is built from
the merged columns exactly as now, sorted by row.

**Identity.** The present code sums a coefficient's contributions in
arrival order: the first creates the entry and each later one is added
to it. A stable sort keeps equal rows in arrival order, and the fold
adds them in that order, so every coefficient is the same sequence of
the same floating-point additions and the matrix is bit-identical.
Nothing reads the columns between `reset` and `solve`.

**Held by.** The solver feeds everything after it, so any changed bit
moves the placement checksum: the probe's identity in the gate and the
core's `0x2d44a02e` are the test.

**Gain.** An estimated 35 s: appends replace the searches and the
shifting inserts, and one sort per column replaces a shift per new
entry.

**Cost.** Twenty lines inside one struct of upstream's file. It is
upstreamable as it stands.

**Outcome (2026-09-21): bit-identical, no gain, not kept.** The identity
argument held on the probe and the core. The speed argument did not:
core HeAP 342.40 s against 342.41 s. Upstream's columns are small
because a contribution merges the moment it arrives, so the search is
short and the insert shifts little; appending defers that and pays it
back in the sort. What is expensive in `build_solve_direction` is the
walk over the nets and the bound-to-bound arithmetic, not the container.

### 16.4 A scan ends at a control refusal that holds everywhere (Rust)

**Profile.** Inside scans the control rules cost 56 s: `rules::evaluate`
41 s and the mirror's trial rows 15 s, asked for every bel that has
passed the ALM rule and the input total.

**The first form of this design was wrong, and the record of it
matters.** It argued that a candidate register's control verdict is
the same at every bel of a LAB, so the rules could be asked once per
scan, and it cited a property test that agreed. The probe agreed too,
under both option sets and in verify mode. The full core's placement
checksum did not: `0xea133043` where `0x2d44a02e` was required, and the
unit stopped there as the plan says it must. The argument had taken
the rules' two walks to be alike. They are not. The test had given
each control kind nets of its own, so it could not meet the case; the
scan oracle's LABs almost never meet it either, which a mutation test
confirmed.

**Analysis, corrected.** The worker (`rules.rs`, `Worker::run`) walks
the registers in physical order twice. The first walk gives each
connected control signal the first resource of its kind's pool that
holds the same signal or is empty. The pools are disjoint between
kinds (one clock, one sload, one sclr, two clears, three enables), so
the walk fails exactly when a kind has more distinct signals than
resources: a property of the set of registers, not of the bels they sit
in. The second walk gives the signals held in those resources their
data lines, again by first fit, but over lists that overlap between
kinds in different orders (enables may take `Datain2`, `Datain3`,
`Datain0`; clears `Datain3`, `Datain2`; sclr `Datain3`; the clock
`Datain0`). First fit takes the first free line without looking ahead
for a line that already carries the same signal. When one net serves
two kinds, whether it finds its line depends on which resource the
first walk put it in, which depends on the order the registers were
met, which depends on the bel. A `DatainConflict` is therefore a
refusal of this bel only.

**Design.** The scan classifies the control verdict
(`ControlAnswer`): legal; refused here (`DatainConflict`), and the scan
goes on to the next bel; refused everywhere (any first-walk reason:
clock, sload, or sclr conflict, clear or enable capacity), and the scan
ends with "no bel". Nothing is cached: a legal answer ends the scan
anyway. The per-bel path and its verdicts are untouched.

**Held by.** Three things, the second and third added because the
first form got past everything that existed.
- A property test with a hostile generator, one small palette of nets
  for every control kind and clocks that are not global, holds that a
  first-walk refusal at one bel is a refusal at every bel, and must
  itself find LABs where a register is legal at one bel and refused at
  another, or it fails as too weak.
- A second scan oracle over the same hostile LABs compares every scan
  with the capture path bel by bel and must contain scans whose first
  legal bel comes after a control refusal. A mutation check confirmed
  it fails on the first form's early exit, which the main oracle did
  not.
- The core's placement checksum, which is what caught it.

**Outcome (2026-09-21): no gain, reverted.** The corrected form holds
the core's checksum and takes strict legalisation from 391.3 s to
391.8 s: first-walk refusals are rare among the core's scans, and the
common control refusals are the kind that must not be shortcut. The
early exit is removed. What the unit leaves behind is worth more than
it set out to save: the hostile scan oracle, the rule that a shortcut
needs a hostile property test, a mutation check, and the core's
checksums (CLAUDE.md), and the split of the scan's legality check that
16.1 reuses. The 56 s remain the cost of asking the control rules per
bel; reducing it means making `rules::evaluate` cheaper, not asking it
less.

### 16.5 One lookup per wire in the arch, a flat index in router2 (C++)

**Profile.** Inside router2's 143 s, three hash lookups per visited
wire cost 33 s: router2's own `wire_to_idx` (`dict<WireId, int>::at`,
14.5 s), the base arch's pip binding map
(`dict<PipId, NetInfo *>::do_lookup`, 12 s, under `checkPipAvailForNet`),
and the arch's wire table (`dict<WireId, WireInfo>::at`, 6.4 s, under
`is_pip_blocked`). The wire table is also the top of the device load.

**Design, first step, in `mistral/`.** The arch answers a pip's
availability from the destination wire's record alone. `WireInfo` gains
the wire's bound net and the pip that drives it, and the arch overrides
the binding API of the base arch (`bindWire`, `unbindWire`, `bindPip`,
`unbindPip`, `getBoundWireNet`, `getBoundPipNet`, the two
`getConflicting` queries, `checkWireAvail`, `checkPipAvail`,
`checkPipAvailForNet`) to read and write those fields. A bound pip is
the pip recorded on its destination wire, so `getBoundPipNet(pip)` is
one lookup of the destination wire, the same lookup `is_pip_blocked`
makes; `checkPipAvailForNet` does both with one. The base arch's two
maps are no longer written.

**Design, second step, in router2.** `wire_to_idx` becomes a flat
open-addressing table private to router2: capacity a power of two at
least twice the wire count, linear probing, the arch's own hash of the
`WireId` mixed once. It is filled once where the dict is filled now
and only read afterwards, including from router2's worker threads.

**Identity.** Both steps change where a binding or an index is stored
and not what it is. Nothing may depend on the iteration order of the
base arch's maps; the first step's first task is to find every reader
of `base_wire2net` and `base_pip2net` (the checkpoint writer, the JSON
writer, archcheck) and confirm each goes through the API or through
the nets.

**Held by.** The probe's routed identity and report hash in the gate,
the core's routing checksum `0x681553a4`, the checkpoint round-trip
tests, which restore bindings through the API, and the route reuse
tests.

**Gain.** An estimated 20 to 25 s of router2, a faster bind and unbind
in every phase, and about a second of device load.

**Cost and risk.** The widest change of the six: eleven overrides in
the arch and a table in upstream's router. Medium risk, last in the
order, and the second step is separable and can be dropped.

**Outcome (2026-09-21): the first step kept, the second dropped.** The
overrides hold routed identity on the probe and the core and take
router2 from 99 s to 91 s, measured twice side by side. The flat index
is byte-identical and gives all of it back: mixing the wire id to avoid
collisions scatters neighbouring wires across the table, and upstream's
hash, the raw id modulo a prime, keeps the fabric's locality, which the
router's neighbourhood access pattern rewards. `router2.cc` is untouched.

### 16.6 The scan's bookkeeping becomes constant time (Rust)

**Profile.** `evaluate_scan` has 38 s of self time that is not rules:
for every bel of the order it searches the patch list to decide
whether the bel is free, copies the array of trials in view to add the
candidate, and the ALM rule writes a rejection record nobody reads.

**Design.** Three changes inside the crate. The session keeps a 60-bit
occupancy mask per LAB, maintained by `apply_bel` at every commit; a
scan overlays the patches on it once, and a bel is free when its bit is
clear. The candidate travels beside the trials instead of inside a
copy of their array: `view` and `bel_legal` take the trials and one
extra patch, so the array is built once per scan. And the ALM rule
becomes generic over where a rejection goes (`check_alm<R: Reject>`):
the verdict passes the assessment, as now, and the scan passes a sink
that discards, so the refusing branch is a return.

**Identity.** The mask is a cache of `occupied` fields that `apply_bel`
alone changes; the view holds the same references in a different
container; the ALM rule's control flow is unchanged and only the write
on a refusing branch differs. The oracle asserts the resident facts
after every step, which covers the mask, and compares every scan with
the capture path.

**Held by.** The scan oracle and the property tests, unchanged, plus an
assertion in the oracle that the occupancy mask equals the facts after
every step.

**Gain.** An estimated 15 s.

**Cost.** Forty lines in the crate. No new surface.

**Outcome (2026-09-21): kept.** Identity on the probe and the core;
strict legalisation 378 s against 390 s, about 12 s.

### 16.7 Sum and sequence

| Design | Side | Now | Estimated after | Surface | Risk |
| --- | --- | ---: | ---: | --- | --- |
| 16.4 a scan ends at a first-walk control refusal | Rust | 56 s | 56 s: built, no gain, reverted | none | the first form was wrong and the core caught it |
| 16.6 constant-time scan bookkeeping | Rust | 38 s | landed: 12 s gained | none | low |
| 16.2 scan loop filters once | C++ | 100 s | landed: 7 s gained | none | low |
| 16.1 clusters through the resident session | both | 91 s | landed: 88 s gained | one function, one FFI call | medium: moves an authority |
| 16.3 equation system append and merge | C++, upstream's file | 57 s | 57 s: bit-identical, no gain, not kept | none | - |
| 16.5 one lookup per wire | C++, arch only in the end | 33 s | landed: 10 s gained by the arch step; the router step slower, dropped | none | medium: binding API overrides |

Together an estimated 200 s of the core flow's 715 s. The estimates are
for ordering the work; each unit is measured by a Time Profiler run of
the core when it lands, and one that does not pay is recorded as that
and removed.

## 17. router2's memory traffic

The miss counts of the core flow (tracker, "Cache and TLB misses of the
core flow, counted per function") put router2 at 1.66 instructions a
cycle, 21.7 L1D load misses and 0.43 L2 TLB misses per 1,000
instructions, and at most 28% of its time waiting on far memory (32 s of
118 s, counting every L2 TLB miss as a serialized trip at the measured
140 ns). The strict legaliser, by contrast, misses mostly near and
overlaps it; its lever is branches, not layout, and it is not in this
section.

The router's L2 TLB misses name where it waits. Of about 232 million in
the phase: the priority queue's own heap array 59 M, `route_arc` 39 M
(the arch's destination-wire lookup inside `checkPipAvailForNet` and the
router's per-wire state are inlined there), router2's `wire_to_idx`
dictionary 35 M, `score_wire_for_arc` 22 M (the first touch of the
neighbour's per-wire state), and the arch's wire record in
`is_pip_blocked` 15 M.

What one neighbour costs, read from the code: the expansion loop takes
the current wire's adjacency from the arch (`getPipsDownhill`, a lookup
in `dict<WireId, WireInfo>`), and for every neighbour asks the arch
whether the pip is available (a second lookup, of the destination wire,
and a third, of the source wire, for its blocked flag), looks up the
neighbour's index in `wire_to_idx`, and looks it up again inside
`score_wire_for_arc` through `wire_data()`. The arch's delay, delay
estimate, and pip location are arithmetic on the wire id and touch no
memory. A dictionary lookup is two dependent reads, the bucket in a
table of about three entries per wire and the entry itself: the arch's
table of 2.74 million wires is about 48 MB of buckets beside 320 MB of
116-byte entries; router2's is about 48 MB beside 33 MB.

The device, measured: 2,739,969 wires and 27,175,167 pips. A wire id is
an rnode, `type << 24 | x << 17 | y << 10 | z`; 31 types occur (30 of
libmistral's and nextpnr's own type 128, a third of all wires), in
85,108 groups of type and tile, and numbering the z values of each group
densely from zero takes 2,809,423 slots, 2.5% of them empty. So an rnode
reaches a dense number through two small tables instead of a hash.

An adjacency list inside router2, each neighbour's index and pip stored
per wire, would remove the router's lookups too, but costs 12 bytes per
pip in each direction, 650 MB; the slots below reach the same with
tables of a few megabytes and keep router2 generic.

### 17.1 The router's queues keep their storage, and have fewer levels (C++, upstream's file)

**Design, first step.** `route_arc` clears its two queues between arcs
and modes by swapping each with a new, empty `std::priority_queue`, so
the heap array is freed and grown again from nothing for every search:
reallocation copies, fresh pages, and their TLB misses. The queues keep
their storage instead: a thin subclass exposes `clear()` on the
underlying vector, and the swap becomes a clear.

**Design, second step.** A four-way heap in place of the binary one:
the four children of an entry are adjacent (80 bytes of 20-byte
entries, within one or two 128-byte lines), and the depth is halved, so
a pop reads half as many levels, each one or two lines. Push, pop, top,
size, empty, and clear, with the router's comparator unchanged.

**Identity.** The first step changes where the heap lives and not one
operation on it. The second pops the same entry whenever the minimum is
unique under the comparator, the total cost and then a random tag drawn
per push; entries that tie on both would pop in a different order. The
routed checksum decides; a change is not accepted, and the step is
dropped if it moves a route.

**Held by.** The gate's routed probe identity and report hash; the
core's routing checksum `0x681553a4`, 45 iterations, 767,087 wires, from
the route-prepared checkpoint and in the full flow.

**Gain.** The queue is router2's largest TLB-miss site and 33.9 s of
self time in the closing Time Profiler run; how much of that is the
fresh storage and how much the depth is what the two steps measure.

**Cost and risk.** About forty lines in `router2.cc`. Low.

**Outcome (2026-09-22): the first step kept, the second not.** Keeping
the storage is byte-identical and worth about 1 s: the queue's misses
are the heap array itself, not its reallocation. The four-way heap moves
the route (routing checksum `0x823226a7`, 54 iterations): seed entries
are pushed with random tag 0, and seeds at one location tie on score, so
which of them pops first depends on the heap's shape.

### 17.2 Wire slots: the arch reaches a wire's record by index (C++)

**Design.** At the end of the constructor, after the routing graph is
imported and before anything binds, blocks, or reserves a wire, the
arch numbers its wires into slots: a 256-entry map from rnode type to a
compact type index, and a table over compact type, x, and y (about
2 MB, resident in L2) giving each group's first slot and its count. A
wire's slot is the group's first slot plus its z; a type or tile or z
outside the tables has none. Two arrays by slot:

- the wire's routing state, 16 bytes: the bound net, the source of the
  pip that drives it, and the flags that matter to routing (`BLOCKED`,
  `RESERVED_ROUTE` with its uphill index), moved there from `WireInfo`;
- a pointer to the wire's `WireInfo` (the dictionary's entries do not
  move once the graph is built: `add_wire` and `add_pip` are called only
  from the constructor, before the slots, and assert it).

The binding API of 16.5, `is_pip_blocked`, `getPipsDownhill`,
`getPipsUphill`, and `getWireBelPins` go through the slot. A pip's
availability then reads the destination's 16-byte state and the
source's, found through two tables that stay in cache, instead of two
116-byte records found through two hash lookups. The dictionary stays
for iteration (`getWires`, `getPips`, and everything that depends on
their order) and for the lookups that are not hot.

**Identity.** Storage only: every answer is the same, and iteration
order is untouched.

**Held by.** The binding contract test of 16.5, the checkpoint
round-trip and route reuse tests, the gate, and the core's routing
checksum.

**Gain.** The arch's two lookups per neighbour lose their buckets and
their record reads; part of `route_arc`'s 39 M and most of
`is_pip_blocked`'s 15 M.

**Cost and risk.** The arch only, about 150 lines. Medium: the routing
state moves, and every user of it must follow. The binding fields are
used only by the binding API; the flags are also written by
`block_wire` and `reserve_route`, by the checkpoint restore, and by the
Stage 1 control edits (`lab_control_edits.cc`), and read by the
checkpoint writer. The fields leave `WireInfo`, so the compiler names
every user, and the callers outside the binding API go through a flags
accessor.

**Outcome (2026-09-22): kept.** Byte-identical; `WireInfo` 80 bytes
from 104. Resumed router2 on the core 78.7 s against 88.7 s, about
10 s.

### 17.3 router2 finds a wire's index through the arch's slot (C++, upstream's file)

**Design.** `Router2Cfg` gains an optional slot function and slot count;
the Mistral arch sets them from 17.2. When they are set, router2 fills a
vector from slot to its own index, in the order it fills `flat_wires`
now, and every `wire_to_idx.at()` becomes that vector read; without
them, the dictionary is used as now, so other arches are unchanged.
`score_wire_for_arc` takes the neighbour's index from `route_arc`
instead of looking it up a second time.

**Identity.** router2's indices are the same numbers in the same order;
only how a `WireId` reaches its index changes. Unit 16.5's second step
failed on locality, a mixed hash scattering neighbouring wires; slots
are grouped by type and tile, as the fabric is.

**Held by.** As 17.1.

**Gain.** `wire_to_idx`'s 35 M TLB misses and 14 s of self time.

**Cost and risk.** About sixty lines in `router2.cc`, one hook in
`router2.h`, three lines in the arch. Low; the call through
`std::function` per lookup is the cost to watch.

**Outcome (2026-09-22): kept.** Byte-identical; resumed router2 on the
core 67.6 s against 78.7 s, about 11 s. The call through
`std::function` did not show. Design 17 together: 89.8 s to 67.6 s, a
quarter of router2, against a far-memory ceiling of 32 s for the whole
phase.

### 17.4 Sequence and measurement

17.1, 17.2, 17.3, each measured on its own and kept only if it pays:
router2 resumed from the core's route-prepared checkpoint (the routing
checksum and wall time), an L2 TLB miss recording of that resumed run
before and after, and the core's full flow at the end. Together they
address the sites that hold about 60% of router2's L2 TLB misses, a
ceiling of about 19 s; the ceiling is a bound, not an estimate.

The annealer comes next (section 18), designed from its own sites once
these land.

## 18. The annealer's chain moves

The miss counts put the annealer at 1.54 instructions a cycle and at
most 46% far-memory bound, the most of any phase per cycle. Attributing
its samples to call paths (the cycles and L2 TLB recordings of the core
flow, `callers.py` beside `misses.py`) shows where that comes from, and
it is not layout.

Each pass of the annealer tries a swap for every cell outside a cluster
(`try_swap_position`) and a move for every cluster root (`try_swap_chain`).
The first goes through the swap seam of Stage 5 (1c): the architecture
assesses the swap on an overlay, the cost delta comes from the position
overlay, and bindings change only for an accepted swap. The second
binds live: it unbinds and rebinds every cell it moves, asks
`isBelLocationValid` of each, computes the cost from the live
bindings, and on a refusal binds everything back. The recipe's 15,181
ALM pairs and 5,360 packed registers put most cells into clusters, so
the live path is most of the annealer:

| Of the annealer's 231.8 G cycles, inclusive | Cycles | L2 TLB misses |
| --- | ---: | ---: |
| Single swaps through the seam, inclusive | 27.3 G (12%) | 36.8 M |
| Chain moves' legality through `isBelLocationValid` (the resident batch capture, its compare, the Rust evaluation) | 39.0 G (17%) | 51.6 M |
| Chain moves' binds and unbinds (ALM input counts 16.0 G) | 24.1 G (10%) | 33.3 M |
| Cost updates, mostly for chain moves: `compute_cost_changes` 51.1 G and `add_move_cell` 28.8 G (25.6 G of it from the chain path) | 79.9 G (34%) | 70.6 M |
| `getClusterPlacement` | 7.3 G (3%) | |
| `powf` in the timing cost | 11.4 G (5%) | 8.3 M |

The rows overlap: the cost row contains `powf`, and the seam row its own
cost updates. A single swap costs 3.9 kcycles per assessed try through
the seam. Chain moves are not counted in the log; their path is the
rest of the annealer, and every refused one undoes everything it bound.

### 18.1 Chain moves through the swap seam (C++, upstream's file and the arch)

**Design.** `try_swap_chain` plans before it binds. The plan walks the
same queue of displaced clusters in the same order and applies the same
refusals in the same order, reading a small occupancy overlay (bel to
occupant, cell to bel) laid over the live bindings instead of changing
them: `getClusterPlacement` for each cluster at its base; for each
destination bel, the occupant the live code would find there, the
availability of the old bel as the live code would see it after the
steps before, a displaced cluster's new root from the same locations,
and a displaced single cell moved to the old bel. The plan's result is
the list of moved cells in the order the live code's `moved_cells` holds
them, with old and new bels, or the point where the live code would have
failed.

From the plan: legality through the seam's assessment of exactly the
bels the live code checks, the new bel of every moved cell after the
whole move (the live code checks moved cells only, not vacated bels,
unlike a single swap, which checks both, so the chain assessment takes
the list of bels to certify); the region test; the cost delta from
`add_move_cell` with the position overlay for every moved cell, in the
same order, and `compute_cost_changes` on it; the acceptance draw under
the same condition. An accepted move binds as the live path ends: every
moved cell unbound and bound at its new bel with `STRENGTH_WEAK`. A
refused or illegal move binds nothing, but reproduces what the live
revert leaves behind: every cell the live path would have moved up to
the point of refusal is left with `STRENGTH_WEAK`, the strength the
revert binds with, since later moves read strengths.

A move the seam cannot assess (a bel outside a LAB, more edits than the
overlay holds, an MLAB) runs the live path as now. `BelOverlay::MAX`
rises from 4 to cover the moves the recipe makes, measured by counting
them; the Rust session's trial budget per LAB (`MAX_TRIALS`, 8) bounds
what it answers without the capture path. The log gains chain counters
(planned, assessed, unsupported, illegal, refused, committed) like the
single swaps'.

**Identity.** The plan reads what the live code would read at each
step, because the overlay holds exactly the bindings the live code
would have made by then; the assessment answers the question the live
code asks; the cost functions run on the same positions; the draws are
the same draws; and the bindings and strengths left behind are the
same. The seam's shadow mode, the oracle of 1c, extends to chains: it
plans and assesses, then lets the live path run, and compares the
failure point, legality, and both deltas move by move; a mismatch is an
error at the end of the phase.

**Held by.** Shadow mode on the probe under both option sets and on the
core with zero mismatches; the probe's and the core's placement
checksums (`0xa99e0f68` for the probe's recipe placement, `0xe0b15557`
for the core's) and the core's routing checksum; a gtest planning a
chain move in the fixture against its live counterpart, with a
displaced cluster, a displaced single cell, and each refusal.

**Gain.** The binds and unbinds of refused moves (most of 24 G) and the
legality capture (39 G) stop running for assessable moves; the
assessment replaces them at the seam's cost per question (11.5 G for 6.9
million single swaps today). The cost updates stay. An estimated 40 to
50 G cycles, 13 to 17 s of the annealer's 77 s, measured when it lands.

**Cost and risk.** The largest unit of the three designs since 16.1:
about 250 lines in `placer1.cc`, the assessment entry for a list of
bels in the arch. Medium to high risk: the plan must reproduce every
branch of the live walk, which the shadow mode checks move by move.

**Outcome (2026-09-22): kept.** Byte-identical on the probe and the
core; shadow compared 7,024,566 chain moves on the core and 644,483 on
the probe's recipe with no mismatch. On the core 6,978,217 moves were
assessed and 210,943 ran live (longer than the overlay's 16 bels). The
annealer took 69.0 s against 78.4 to 81.5 s before it, about 10 s, the
low end of the estimate. The gtest the design named, planning a move
against its live counterpart, was not written: the planner is private to
the annealer, and the shadow mode compares every move of real runs; the
seam test instead certifies bel subsets over an overlay, with a
generator that reaches subsets legal where the whole is not.

### 18.2 The timing weight computed once per timing update (C++, upstream's file)

**Design.** `get_timing_cost` multiplies an arc's predicted delay by
`std::pow(crit, crit_exp)` for every changed arc of every move, 11.4 G
cycles. The criticality table is written in one place, `setup_costs`,
after each timing analysis, and `crit_exp` is a constant. A second
table beside it holds `std::pow(crit, crit_exp)`, written in the same
place from the same values, and `get_timing_cost` reads it.

**Identity.** The same expression with the same arguments, evaluated
once instead of per move: bit-identical by construction.

**Held by.** The probe's and the core's placement checksums.

**Gain.** Up to 11.4 G cycles, about 4 s.

**Cost and risk.** Ten lines. Low.

**Outcome (2026-09-22): negative, not kept.** Bit-identical and no
faster: the annealer 79.8 s against 78.4 s, inside the noise of the
runs. The cycles the recording credited to `powf` were most likely skid
from the record reads just before it in `predict_arc_delay`: a counter's
interrupt lands a few instructions after the event, so a cheap leaf that
follows a stalling load inherits the load's samples.

### 18.3 Sequence

18.2 first (small and independent), then 18.1. Dense per-cell positions
for the cost functions, the layout change the miss counts first
suggested, waits for the measurement after 18.1: the cost updates stay
in both paths, and what they cost then decides it.

## 19. Beyond byte identity: quality over seeds

Every unit from Stage 4 to design 18 was held to byte identity: a change
had to leave the placement and the routing unchanged to the byte, and
its worth was time. That rule made the refactor safe (the evaluator
moved to Rust, the legaliser to batch queries, router2 to wire slots,
the annealer to a detached seam, all without one result changing) and
it is also why none of it moved the result: the core signs off at 11.39
MHz on 28,652 ALMs where Quartus reaches 25.18 MHz on 20,576. From
2026-09-23, at the user's direction, a change may move placements and
routes, and is judged by what it does to them over seeds.

**What stays exact.** Legality is not a quality knob: the evaluator's
answers keep their verify and shadow oracles and the three guards for a
shortcut (CLAUDE.md), router1's legality pass and the signoff
assertions stay. Determinism stays: the same inputs, seed, and thread
count give the same bytes run to run, and a resumed checkpoint equals
the uninterrupted run; without it no comparison means anything. And
every capability arrives opt-in and becomes a default only by a decision
row with the evidence below.

**The acceptance rule.** Five seeds of the core recipe's full flow, and
three of the probe as a smoke test. Every seed routes to completion, and
no seed's Fmax falls below the worst of the baseline's seeds. A quality
change must lift the median Fmax above the baseline's median by more
than the baseline's own spread (its maximum minus its minimum); a speed
change must keep the median Fmax inside the baseline's range and lower
the median wall time. Routed or not, iterations, fabric wires, ALMs and
LABs used, and design 9.5's shape of the placement (rows a net touches,
LABs its sinks sit in other than the driver's) are reported with every
set. Seed statistics come from parallel runs; a wall-time claim needs
serial runs on a quiet machine.

**The harness.** `mistral/tests/quality.py` runs a binary over seeds of
a named configuration (`core`, the recipe that routes the full core;
`probe`, the gate's default path), several at a time, collects the
report's Fmax, the log's iterations and wires, the telemetry's phase
times, and the placed shape from the routed JSON, and compares two sets
under the rule (`compare --kind quality|speed`). `--determinism` repeats
the first seed and requires identical bytes; `--drop` removes a flag of
the configuration, for references such as the recipe without the unit
wire cost. Outputs stay under `build/quality/`.

**The units.** Each is designed here before it is built, and recorded
kept or dropped in the tracker by the rule above.

- 19.1, speed: router2's four-way heap, built and dropped under 17.1
  because it moves the route.
- 19.2, quality: the placement cost model that 9.5 named as the
  remaining gap, rows and LAB entries per net priced where the annealer
  decides.
- 19.3, quality: criticality-aware routing cost, the unit cost for
  arcs that are not critical and the delay cost for those that are.
- 19.4, quality: registers packed with their LUTs beyond 6g's 56%.
- 19.5, speed: parallel annealing that includes chain moves, which
  18.1 detached from the live design.

**Routing-only changes (2026-09-24).** A change that leaves every
placement alone, measured from the base's route-prepared checkpoints, is
judged seed by seed instead of against the spread: every seed the base
routes must still route and gain Fmax, on a placement that must match.
The spread measures how placements vary between seeds, and such a change
does not vary them (`quality.py compare --kind routing`).

### 19.1 router2's four-way heap (C++, upstream's file)

**Why.** Router2's priority queue was the largest TLB-miss site of the
whole flow (59 million L2 TLB misses) and 33.9 s of self time before
design 17. Under 17.1 a four-way heap routed seed 1 at 1.72 s per
iteration against 1.97 s, and was dropped only because it moved the
route: 54 iterations instead of 45, because the router's seed entries
are all pushed with random tag 0 and those that tie on score pop in an
order that depends on the heap's shape. Under the rule of this section
that is no longer disqualifying; whether it pays over seeds is the
question.

**Design.** `WireQueue` becomes a heap of selectable arity. With arity
2 it is exactly `std::priority_queue`: `push_back` then `std::push_heap`,
`std::pop_heap` then `pop_back`, with the router's comparator, so the
default path is unchanged to the byte. With arity 4 the four children
of an entry are adjacent (80 bytes of 20-byte entries, within one or two
128-byte lines) and a pop reads half as many levels. The storage is kept
between arcs in both. `Router2Cfg::queue_arity` selects it; the Mistral
arch sets it from `--router2-quad-heap` in `ArchArgs` (not a settings
key, which would shift the IdString table).

**Measurement.** The heap changes routing only, so placement is shared:
the harness gains `prepare`, which writes a route-prepared checkpoint for
each seed, and `run --resume-dir`, which routes from them. A resumed run
equals the uninterrupted one to the byte (Stage 5 2a/2b), so the quality
of resumed runs is the full flow's, and routing them one after another
gives serial router times. The speed rule applies: every seed routes,
the median Fmax stays inside the baseline's range, no seed falls below
its worst, and the median router2 time is lower.

**Cost and risk.** Fifty lines in `router2.cc`, one field in
`router2.h`, an option in the arch. Low: the default is unchanged, and
routing legality is checked by router1 on every run.

**Outcome (2026-09-23): negative, removed.** Resumed from the five
route-prepared checkpoints, one run at a time: the heap routes three
seeds of five where the binary heap routes four (seed 2 now ends at its
cap of 100), the median Fmax of the routed seeds falls from 11.17 to
10.77 MHz, and the worst seed (10.73) falls below the baseline's worst
(10.92). It is about 12% faster per iteration and needs more of them.
Which tied seed entry pops first matters to how the negotiation
converges, and here it did not help. The option and the arity switch
are reverted; the queues keep their storage (17.1).

### 19.2 Rows and LAB entries priced where the annealer decides (C++, upstream's file and the arch)

**Why.** Section 9.5 measured where the fabric wires go: a LAB is
entered through row wires, so every extra row a net touches costs a
stair of two or three wires, and every LAB its sinks enter costs about
one more. Quartus's placement touches 1.52 rows per net and enters a LAB
with one fabric wire. On the full core, measured the same way (the
harness's definitions, nets of up to 64 sinks, route-through buffers
left out), Quartus's fit touches 1.44 rows per net and enters 0.84 LABs
per net besides the driver's; nextpnr's baseline touches 1.91 and enters
1.61, and uses 2.3 times Quartus's fabric wires and a fifth of its local
lines (tracker, "The core against Quartus, measured the same way"). The placer's
objective does not see either. HPWL prices a net's bounding box: two
sinks five rows apart cost the same as sinks in all six rows between,
and a net whose ten sinks sit in ten LABs of one row costs the same as
one whose sinks share two. `--row-cost` (6h) made a vertical tile dearer
in HeAP, and the annealer inherits it, truncated: `Placer1Cfg` keeps its
scales as integers, so 2.5 arrives as 2.

**Design.** The annealer's per-net cost gains two terms beside the
bounding box:

    cost(net) = sx * (x1 - x0) + sy * (y1 - y0)
              + wr * (rows(net) - 1) + we * entries(net)

where `rows` counts the distinct rows of the driver and sinks and
`entries` the distinct tiles of sinks other than the driver's. Both are
generic geometry (a row is a y, a LAB a tile), so the terms live in
placer1 behind `Placer1Cfg` weights, and Mistral sets them from
`--sa-row-weight` and `--sa-entry-weight` (`ArchArgs`), off by default;
the scales become floats so the row cost arrives whole.

Each net keeps two small histograms, rows and sink tiles with their pin
counts, for nets of up to 64 sinks (larger nets, clocks and enables,
keep the bounding box alone; a histogram of thousands of pins would be
slow to update and the router spreads them on global and dedicated
lines anyway). A move records, per net it touches, the pins that move
and where; `compute_cost_changes` applies them to a copy of the
histograms and prices the change in distinct rows and tiles;
`commit_cost_changes` keeps it. The seam's detached path and the live
path go through the same code with their own positions, so shadow mode
still compares them.

**What it cannot do.** The terms act in refinement, which starts at a
temperature of 1e-7 and so descends greedily from HeAP's placement; they
do not reach HeAP's analytic solver, which needs a convex objective.
Whether a greedy descent finds enough is the measurement's question; a
refinement at a positive temperature, affordable since 18.1, is the next
lever if it does not.

**Measurement.** A screening grid on seeds 1 and 2 (row weight 0, 2, 4,
8 against entry weight 0, 1, 2, 4, in bounding-box units, where one
horizontal tile is 1), then the best on five seeds against the baseline
under the quality rule. Rows and LAB entries per net must fall; fabric
wires, router iterations, and Fmax say whether it paid. The float scale
alone is a separate arm.

**Cost and risk.** About 200 lines in `placer1.cc`, options in the arch.
Medium: incremental histograms must stay exact, which a debug recompute
of every net's cost after each temperature step checks, as the bounding
boxes already are in debug mode. The default path is unchanged.

**Outcome (2026-09-23): kept opt-in, not promoted.** Screened on seeds 1
and 2, then row weight 4, and row weight 4 with entry weight 4, on five
seeds. Both route all five seeds, where the baseline does not route seed
4; both move every gap measure the right way (with both weights: LABs
entered per net 1.61 to 1.48, rows 1.91 to 1.82, wires 2.5% fewer,
column wires 5% fewer, local lines 17% more); neither lifts the median
Fmax by more than the baseline's spread (+0.58 and +0.27 MHz against
0.95), and row weight 4 alone puts a seed below the baseline's worst.
Cells per LAB do not move (13.16 in every set): the annealer moves cells
between LABs but cannot change how full HeAP left them, which is what
the next levers are for. The options stay, off, to compose with them.

### 19.6 LAB affinity in the strict legaliser (C++, upstream's file and the arch)

**Why.** Units 19.2 showed where the LAB structure of a placement is
decided: the annealer moved LAB entries per net from 1.61 to 1.48 and
rows from 1.91 to 1.82, but cells per LAB stayed at 13.2 in every set
against Quartus's 15.9, and LABs used at 4,186 of 4,191 against 3,219.
The strict legaliser is where cells get their LABs, and it does not look
at connectivity. A cell's search starts at radius 0, where the number of
candidates to compare (`need_to_explore = 2 * radius`) is zero: in the
common, uncrowded case the cell takes the first legal bel of the tile
the analytic solver rounded it to. Only when that tile is full does the
radius grow, and then candidates are compared by the distance from the
cell's input drivers alone, over randomly drawn tiles. Connected cells
that the solver put a tile apart stay a tile apart: in adjacent LABs,
each an entry of the net, where Quartus puts them in one LAB and joins
them with its local lines.

**Design.** When the legaliser places a cell (or a cluster's root), it
considers, beside the tile the random search draws, the tiles of the
cell's placed neighbours: the driver of each input net and the placed
sinks of each output net, for nets of up to 64 sinks, within a reach of
`R` tiles of the solver's position (anisotropic, `R / hpwl_scale_y`
rows). For each candidate tile it takes the first legal bel through the
same scan as now (the tile scan of design 14 answers it in one call),
and scores the legal ones:

    score(tile) = sx * |x - x_solver| + sy * |y - y_solver|
                + W * entries_added(cell, tile)

where `entries_added` counts the cell's nets that have no other pin in
that tile yet (each is a LAB the net must now enter). The lowest score
wins; ties keep the order the candidates were enumerated in (the solver's
tile first, then neighbours in net and pin order), so the result is
deterministic. The distance term keeps the solver's intent; the entry
term pulls a cell into a LAB its nets already use. With `W = 0` the
search is today's.

`PlacerHeapCfg::lab_affinity_weight` and `lab_affinity_reach` (generic:
a tile is a location, as it is to the spreader); the Mistral arch sets
them from `--heap-lab-affinity W` and `--heap-lab-reach R` in `ArchArgs`,
off by default.

**What it changes.** Placements, and the RNG sequence (fewer random
draws are needed when a neighbour's tile is legal). Legality is the
same check as now: every bel the legaliser binds is certified by
`isBelLocationValid`, so crowding a LAB can only make candidates
illegal, never admit an illegal one. Fuller LABs meet the LAB input limit
and the control sets sooner; the measure of that is legalisation time
and the stall exit (6a), which must not trigger.

**Measurement.** On top of 19.2 (rows 4, entries 4) and the best density
setting of the sweep: `W` of 1, 2, 4 in distance units and `R` of 1 and
2, screened on seeds 1 and 2, the best on five seeds under the quality
rule. Cells per LAB and LABs used must rise toward Quartus's, LAB entries
per net and local lines must move the same way, and legalisation must
not stall.

**Cost and risk.** About 120 lines in `placer_heap.cc` (the candidate
enumeration and scoring beside the existing search), options in the
arch. Medium: it changes where most cells land, so the routed result
can move a lot either way; the harness decides.

**Outcome (2026-09-23): kept opt-in, not promoted.** Screened with
reaches of 2.5 and 5 weighted tiles: the design's 1 and 2 admit no other
row at the row weight of 4. Weight 2, reach 5 on five seeds routes every
seed, with the median at 11.75 MHz against 11.17 and the worst seed at
11.52 against 10.92. It is a quality REJECT, because the gain is inside
the spread. On the speed rule's criteria it passes, but only with
parallel timings. LAB entries per net fall to 1.40 and router iterations
to 27 to 52.

The design's premise, that pulling cells to their neighbours' LABs
fills them, did not hold: cells per LAB stay at 13.2. The legaliser's
choice of tile moves nets together, but how many cells a LAB holds is
set by the LAB's limits, which the next unit (19.4) relaxes. A serial
timing of this set against the baseline is the step before a speed
promotion.

### 19.4 The second register of an ALM half (arch, then packing; silicon first)

**Why.** 6g packs 5,360 of the core's 9,632 LUT-driven registers beside
their LUT. Of the rest, 3,980 are refused because their LUT's register
slot is taken (292 more are control-set conflicts): the LUT drives two or
more registers, and the ALM rule admits one register per half, because
it refuses the second register bel of every half outright (`lab.cc`,
"TODO: why are these FFs broken?"). That refusal also takes 20 of a
LAB's 40 register bels out of play. It has no evidence behind it:
`MISTRAL_GAPS.md` has no finding on it, and the model wires both
registers of a half identically, each able to take the half's LUT output
(`PKREG`) or the E/F input, each with an output mux (the second also has
the `L` local output). Quartus puts 95% of LUT-driven registers in their
LUT's ALM (exec probe); a LUT that drives two registers keeps both. And
Quartus's own fit of the core uses both register slots of a half 1,314
times (2,628 registers, 18% of its 14,590; `build/quality/quartus-core`,
slots N+1 and N+2 of each LUT slot N): the hardware has the capacity the
rule withholds. Whether nextpnr's bitstream encodes it correctly is what
the steps below settle.

**Design, in three steps, each gating the next.**

1. *Ground truth from Quartus, no board.* A small WYSIWYG design on the
   NAS in which LUTs drive two registers, fixed by location assignments
   to both register bels of one half, fitted by Quartus 17.0.2; its
   bitstream decoded with libmistral and compared, bit by bit in the
   ALM's configuration, with the same placement written by nextpnr with
   the refusal lifted (an environment override, test only). The IO
   registers were completed the same way (`MISTRAL_GAPS.md`, the
   qrbase/qrout differentials). Either the bits agree, or the difference
   is the missing piece of the model.
2. *Silicon.* A golden-checksum design on the DE10-Nano: a network of
   LUTs each driving two registers, both in its half, whose state after
   a fixed number of cycles is checked against a simulated golden value
   (the method that verified the DSP block, `MISTRAL_GAPS.md` G7). Built
   by nextpnr with the refusal lifted. This needs the board.
3. *Only if both pass:* the refusal becomes an option
   (`--alm-both-registers`, `ArchArgs`, the Rust and C++ rules alike,
   with the verify harness and the property tests updated), register
   packing places a LUT's second register on its half's second bel, and
   the unit is measured on five seeds.

**What it could change.** Up to 3,980 more registers beside their LUT
(from 56% to about 97% of LUT-driven registers), a LAB's register
capacity from 20 to 40, one LAB entry fewer for each such register, and
the density lever that 19.2 and the sweep could not reach.

**Risk.** The refusal may exist for a reason no one wrote down; steps 1
and 2 are there to find it before anything depends on the answer. A
silicon claim is recorded only from a test that can fail (the lesson of
the voided loopback claims in `MISTRAL_GAPS.md`).

### 19.3 Criticality-aware routing cost (the arch's cost function for router2)

**Why.** The recipe routes the core under 6f's unit cost: every wire
costs one unit, whatever its delay and whatever the arc's criticality.
The unit cost is what makes the core converge. The delay cost with the
same recipe needed 125 iterations and 842,300 wires, and signed off at
9.76 MHz, below every unit-cost run (tracker, 2026-09-18). But the
unit cost's routes are delay-blind.

On the baseline's seed 1 (11.39 MHz) the critical path is 87.8 ns:
- 74.8 ns routing, over 53 arcs;
- 12.5 ns logic, 0.7 ns clock-to-output.

Fifteen of those arcs stay inside their tile and cost nothing; the
median arc costs 0.9 ns. Some arcs are detours no delay-aware search
would take: 2.92 ns for a one-row hop, 7.85 ns for five columns and
three rows. The routing that decides Fmax is chosen by wire count.

router2 already knows each arc's criticality. It computes a
`crit_weight` of `max(floor, 1 - crit²)` per arc from the timing
analysis it re-runs every iteration. It uses that weight only to scale
the congestion terms, so under the unit cost a critical arc still counts
wires.

**Design.** Blend the two costs per arc through the weight router2
already passes to `Router2Cfg::get_base_cost`:

    base(wire, pip, w) = w * U + (1 - w) * delay_ns(wire, pip)

- `w` is the arc's `crit_weight`: 1 for an arc with no criticality,
  down to the floor (0.05) for the most critical.
- `delay_ns` is today's delay cost, `default_base_cost`.
- `U` is the mean of `delay_ns` over the device's wires, computed once
  when the router is configured.

With `U` in nanoseconds, the non-critical arcs keep the unit cost's
behaviour at the scale of the to-go estimate, which is already in
nanoseconds (`get_togo_cost`). Today the unit cost's wire term and the
estimate are in different units; how that has weighed the search is
not measured. Only arcs
whose criticality is high move toward the delay cost: at a criticality
of 0.9, `w` is 0.19. The congestion terms are unchanged, and the delay
term only reorders the candidates a critical arc sees.

The arch sets it behind `--router2-crit-cost` (`ArchArgs`, like the
unit cost; it implies the unit cost for non-critical arcs and replaces
`--router2-unit-cost` when both are given). router2 itself does not
change.

**Measurement.** Two numbers first, from one core seed:
- the share of arcs with a criticality above 0.5, 0.8, and 0.9 at the
  first and the last iteration (a counter in the arch's lambda: the
  weights it is called with);
- the critical path's routing delay against its tile distance, per arc,
  as above.

Then five seeds under the quality rule, on the recipe and on the best
placement set of 19.2 and 19.6. It must route every seed, which is the
risk the delay cost did not meet. Expected: Fmax up by the detours'
share of the critical path, and a small rise in wires.

**What it changes.** Routes and signoff, not placement. Determinism is
kept: the weights come from router2's own timing analysis, which is
deterministic.

**Cost and risk.** About 30 lines in `mistral/arch.cc` and the option
plumbing. Risk: critical arcs take the same short wires and congest them
(what sank the delay cost). The floor of the present-congestion term
(`present_cong_floor`) keeps them paying for overuse. If convergence
suffers, a steeper blend (`w` squared) or a criticality threshold is the
next setting, not a new mechanism.

**Outcome (2026-09-23): negative in its first form.** No core seed
routed within the cap of 100. Wires rose 13 to 16%, and 46 to 1,244
wires were still overused at the cap. The blend moves too many arcs off
the unit cost. The threshold form is the next setting to screen.

### 19.7 The annealer's timing constants (upstream's file and the arch)

**Why.** The Fmax set's largest single gain came from a timing constant
nobody had tuned for the core: HeAP's weight, raised from 10 to 100. The
annealer has the same kind of constants, and they are hard-coded in
`placer1.cc`:
- `lambda = 0.5` splits a move's cost between timing and wirelength;
- `crit_exp = 8` sharpens criticality, so an arc's timing cost is
  `delay * crit^8`.

**Design.** Make them `Placer1Cfg` fields `timing_lambda` and
`timing_crit_exp`, with today's defaults, so nothing changes unless an
arch sets them. HeAP copies them into its `placer1_cfg` as it copies the
row and entry weights. The arch sets them from `--sa-timing-lambda` and
`--sa-crit-exp` (`ArchArgs`).

**Measurement.** On the Fmax set, seeds 1 and 2: lambda 0.7 and 0.9, and
exponent 4. The best goes to five seeds under the quality rule against
`f-w100-crit-5s`.

**Cost and risk.** About 20 lines. The defaults reproduce today's runs
exactly, which is the check.

### 19.8 The placement delay model, calibrated by span (the arch)

**Why.** Before routing, every arc's delay is `Arch::predictDelay`,
which is `75 * dx + 200 * dy` ps. That value decides which arcs are
critical, for HeAP's timing weights and for the annealer's timing cost.
The arc dump of the routed baseline (tracker, "Where the critical path's
time goes") shows how wrong it is:

| Span (columns, rows) | Median routed delay | `predictDelay` |
| --- | ---: | ---: |
| (1, 0) | 0.55 ns | 0.08 ns |
| (0, 1) | 0.90 | 0.20 |
| (0, 3) | 1.24 | 0.60 |
| (10, 0) | 1.15 | 0.75 |
| (0, 10) | 2.11 | 2.00 |
| (0, 20) | 2.78 | 4.00 |
| (40, 0) | 3.00 | 3.00 |

Leaving a LAB costs half a nanosecond or more, and the model says almost
nothing. Long vertical arcs cost far less than the model says. So a
critical arc gains little in the placers' eyes from staying inside a LAB,
which is exactly where Quartus wins (its local lines). And criticality
itself is computed from the wrong delays until the router runs.

**Design.** `predictDelay` reads a table of median routed delay by span
(dx up to 91, dy up to 81, in picoseconds; the device grid is 89 by 81), made from the arc dumps and
made non-decreasing in both directions so that moving away never looks
cheaper. Beyond the sampled range, the last value grows linearly. It is
checked in as a generated header, `mistral/span_delay.h`, with the
script that makes it (`mistral/tests/span_delay.py`) and the source
dumps named in its header. `estimateDelay`, router2's to-go estimate,
is unchanged: the A* search depends on its current scale, and 19.3's
first form showed how sensitive the router is to that balance.
`--placement-delay table|linear` selects it; the default stays `linear`.

**Measurement.** On the Fmax set, seeds 1 and 2, then five seeds. The
table is fitted on seeds 1, 2, 3 and 5 of the baseline; an Fmax gain on
seed 4, which none of the dumps saw, is the check that it is not fitted
to its own sample.

**Cost and risk.** About 40 lines and the table. Risk: the median
includes congestion detours, so the table is the delay this router
achieves rather than the fabric's best. That is the right target for
predicting a routed delay, but it changes with the router. The script
regenerates the table.

### 19.9 Timing repair after convergence (router2, upstream's file)

**Why.** Before 19.3b, detours were 44% of the core's critical paths:
about 24 ns per path above the median delay for each arc's span
(tracker, "Where the critical path's time goes"). 19.3b lets critical
arcs see their delay while the design negotiates, and it gained 0.48 MHz.
But during negotiation a critical arc still competes for every wire, and
the congestion terms push it off the fast ones. Once the design has
converged, no arc has to share a wire any more. A critical arc re-routed
then, over the wires nobody uses, can take its fastest free path without
disturbing anything.

**Design.** A phase at the end of router2, after the loop exits with no
overuse and before router1's check. `Router2Cfg::repair_rounds` sets how
many rounds run (0 = off), and `repair_crit` sets the criticality
threshold. Each round:

1. Run the timing analysis. Collect the arcs with criticality of at
   least `repair_crit` (default 0.9), sorted by criticality, then net
   and arc index, so the order is deterministic.
2. For each arc, in that order:
   - record its route, the list of (wire, pip) from sink to source that
     `ripup_arc` walks, and its routed delay;
   - rip it up, and route it again with the delay cost alone (crit
     weight at the floor). A wire used by another net is unusable, and
     the wires of its own net are free to share.
   - Keep the new route if it was found and its delay is lower. Otherwise
     rip the new route up and bind the recorded pips back with
     `bind_pip_internal`, which restores the arc exactly.
3. Bind the result to the arch (`bind_and_check_all`).

Only unused wires are ever taken, so no overuse can appear, and router1's
check still certifies the result. A kept route is strictly faster for
its arc, but the path it is on may still not improve: the next round
re-times the design and moves on to the new worst arcs.

**Measurement.** First, measure the detour that 19.3b leaves: the arc
dump (`MISTRAL_DUMP_ARC_DELAYS`) of the Fmax set's routed seeds, split as
before. If that detour is small, the unit is not built. If it is built:
the Fmax set with 1, 2 and 4 rounds, from route-prepared checkpoints of
the Fmax set, then five seeds under the quality rule.

**Cost and risk.** About 150 lines in router2. The risk is in the
restore: a new route can share wires with the net's other arcs, so the
rip-up must leave those arcs intact. A debug assertion compares the
net's wire counts before a rejected repair and after its restore.

**Outcome (2026-09-24): kept opt-in; every seed gains.** No debug
assertion was built: the restore rebinds the recorded pips, and router1's
check certified every run.

- **Threshold.** At 0.9 only 3 core arcs qualify, because router2's
  criticality puts the core's critical paths between 0.5 and 0.9. At
  0.5, two rounds raise every seed of the Fmax set, on the same
  placements, by 0.20 to 1.35 MHz (median 12.59 to 13.28), for 4 to 7 s
  of routing.
- **A bug found on the probe.** The first form seeded a repair from a
  routing context that still held the previous net's wires; route_arc
  then traced a path back to another net's wire and asserted. A repair
  now seeds from the source alone.

### 19.10 The LAB input count by distinct nets (the LAB rules, C++ then Rust)

**Why.** Today's Fabi386 core does not place (tracker, 2026-09-24): every
LAB is in use, and the binding limit is the LAB input count of 42, not
ALMs (60% in use). The count sums each ALM's unique inputs. A net that
enters several ALMs of one LAB counts once per ALM, though the hardware
carries it on one input line (TD), and nets driven inside the LAB count
too. Measured on the routed Fmax set of the 2026-09-17 core, seeds 1
and 2 (`build/quality/lab_lines.py`):

| Per LAB (mean) | Seed 1 |
| --- | ---: |
| The count (sum of each ALM's unique data inputs) | 36.3 |
| Distinct data nets from outside the LAB | 23.4 |
| Data nets driven inside the LAB, all by registers | 7.3 |
| Input lines the route uses (TD) | 29.0 |
| Local lines the route uses (LD) | 1.1 |

The distinct-net demand is every distinct net on a data pin (LUT inputs,
register data and synchronous data) that is not driven by a LUT of the
same LAB. It predicts the input lines the route uses:

| Demand | LABs (seed 1) | Input lines used: median, 95th percentile, max |
| ---: | ---: | --- |
| 24 to 27 | 559 | 27, 32, 35 |
| 32 to 35 | 729 | 33, 37, 40 |
| 36 to 39 | 631 | 35, 40, 43 |
| 40 to 43 | 729 | 38, 42, 44 |

Seed 2 is the same within one line. The demand averages 30.7 where the
count averages 36.3, so a limit on the demand admits about 15% more logic
per LAB. The router already delivers the lines that demand implies: it
never used more than 44 of the 46. A LUT-driven net inside the LAB
reaches its sinks by local lines. A register-driven one does not: the
first register of a half has no local output, and the second one's (`L`)
is closed by the rule 19.4 would lift. This is why registers are the
cells left over on the current core.

A per-class count (A/C 25 lines, B/D 21, E 22, F 24; section 9.2) was
tried on the same data. Its bound reaches 61 lines where the route never
used more than 44, because the netlist's LUT pin names are not the
physical pins. It is not the model.

**Design.**
- **The model.** `--lab-input-model count|nets` selects between today's
  per-ALM sum (`count`, the default) and the distinct-net demand
  (`nets`). The limit stays `resolved_lab_input_limit()` (42, or
  `MISTRAL_LAB_INPUT_LIMIT`). The ALM rule (eight inputs per ALM, the
  E/F rules) is unchanged: it is the per-ALM check.
- **Upkeep.** The demand is kept incrementally per LAB: a map from net
  to its data-pin uses in the LAB, and the set of nets whose driver is a
  LUT of the LAB. It is updated where the ALM input counts are updated
  today (`update_alm_input_count`, the LAB version hooks). The check
  reads a counter. The overlay form (`check_lab_input_count_overlay`)
  adjusts that counter by the overlay's bels only.
- **Phase A, C++ only.** Under `--lab-legality legacy`, with the Rust
  authority refusing `nets` until phase B. Measure on the 2026-09-17
  core (cells per LAB, LABs used, routing, Fmax: the quality rule over
  five seeds), then on today's core (does it place and route).
- **Phase B, Rust, only if phase A pays.** The same model in
  `rules.rs` and the resident session, whose facts already carry the
  nets. That means the verify harness, a property test whose generator
  finds LABs where the two models disagree, a mutation check, and the
  core's checksums in verify mode, the three guards CLAUDE.md sets for
  a rule change. It reopens the concluded crate for a rules revision,
  which needs its own decision row.

**Risk.** Denser LABs raise the fabric demand around them, and the
measurement above comes from placements the old count made. Whether
router2 delivers at the new density is exactly what phase A measures.
If it does not, the limit on the demand (below 42) is the setting to
screen, not a new mechanism. Faster checks are not expected. Placement
changes and the RNG sequence with it.

**Outcome (2026-09-24): phase A negative; kept opt-in, phase B not built.**
- **At limit 42:** the probe no longer routes.
- **At limit 40:** the probe routes (+4%), but the 2026-09-17 core's
  router2 stalls at 440 to 500 overused wires.
- **At 38 and below:** the divider's carry chain, whose segment needs 40
  distinct nets, cannot be placed. No limit both routes and places.

The measurement predicted the lines used only for LABs the per-ALM count
had shaped. Packed to the new limit, a LAB's nets reach more pin classes
than its lines can serve (section 9.2). The density the core needs has
to come from what the lines must carry (19.4's local register outputs),
not from a looser count.

## 20. Closing the algorithmic gap to Quartus

After design 19 the 2026-09-17 core reaches a median of 13.28 MHz over five
seeds, against Quartus 17.0.2's 25.18 MHz on the identical netlist. The
2026-09-24 core (12.4% more logic) places with `--lab-global-clocks` but
does not route. The device map is not the gap: libmistral's routing graph
matches Quartus's resource counts, and every bitstream difference decoded
side by side (19.4, the three-enable LAB) was a choice, not a missing
wire. The gap is in the algorithms:

| Old core | nextpnr (recipe) | Quartus |
| --- | --- | --- |
| LABs used | about 4,150 | 3,219 |
| Cells per LAB | 13.2 | 15.9 |
| LAB entries per net | 1.41 | 0.84 |
| Fabric wires | about 2x | 1x |
| Local-line use | about 0.26x | 1x |
| LUT-register pairs in one ALM | 56% | 95% |

On the exec probe the 1.8x Fmax gap splits into synthesis 1.12x, the
timing model 1.22x, and placement and routing 1.32x. The aim is that
today's core routes on the 5CSEBA6 and the core's median Fmax rises well
beyond 13.3 MHz. Quartus parity is not the aim.

The units, each with its own subsection before code:

- **20.0, the oracle.** Route Quartus's LAB membership with nextpnr,
  which splits the gap into clustering and routing and orders what
  follows. It needs:
  - a name-preserving hand-over (`mistral/tests/quartus_handover.py`: each
    cell is renamed to a short public name with a map back, and only the
    wires are enumerated);
  - a Quartus fit of the netlist as given (physical synthesis off, no
    merging or removal of cells);
  - an opt-in `--lab-hint file` that gives HeAP a LAB per cell, reusing
    19.6's candidate-tile search.

  It also measures why local lines go unused.
- **20.1, LAB clustering.** Decide which cells share a LAB before
  placement, grown from critical or connected seeds by an attraction
  score, and admit each cell through the LAB legality evaluator on a
  virtual LAB (the V2 capture and evaluate path). Placement follows
  through the 20.0 hints, and whole-LAB moves if hints do not suffice.
  Exit: LABs ≤ 3,600, cells per LAB ≥ 15, entries per net ≤ 1.1 on the
  old core over five seeds, and today's core routes.
- **20.2, LUT input permutation in the router.** Pseudo-pips from an
  ALM's physical inputs to a LUT's logical pins, restricted to legal
  permutations, with the bitstream writer rewriting each mask. It lifts
  the input-line group wall (section 9.2) that sank 19.10.
- **20.3, complete register packing.** 19.4 with its silicon test, then
  register packing for carry-chain outputs.
- **20.4, timing.** Calibrate the delay model to silicon (30 to 60%
  pessimistic today, worse with size) and to the oracle fit, and make
  20.1's attraction timing-driven.
- **20.5, physical synthesis.** Retiming and duplication, mostly in
  Yosys. Later, and optional.

20.0 comes first. 20.3 follows whenever the board is online. 20.1 and
20.2 run in the order 20.0 decides, and 20.4 after 20.1's clusterer
exists. Every unit keeps section 19's acceptance rule and the legality
guards, and is measured on the old core and then confirmed on today's.

**20.0 outcome (2026-09-25).** The oracle fit reaches 16.71 MHz with the
netlist as given, against 25.18 in production, so netlist optimisation
is a larger share of the gap than assumed. That raises 20.5's value.
nextpnr's rules admit 346 of the oracle's 3,219 LABs, and hints alone do
not route. Two rules block 86% of the LABs:
- **the second register (20.3):** evidence-free; the Quartus
  differential agrees;
- **the LAB input count (20.2):** Quartus routes LABs of count 50 and
  more because it permutes LUT inputs and uses local lines.

Clustering (20.1) follows both. The local-line measurement points at the
same two units: half of intra-LAB LUT nets detour for want of a
permuted pin, and first-register outputs have no local line.

### 20.2 LUT input permutation in the router (the arch; router2 unchanged)

**Why.** `reassign_alm_inputs` (`lab.cc`) fixes each LUT's logical inputs
to physical ALM pins before routing. The router then has to deliver each
net to that one pin. That creates two measured walls:
- **The input-line groups.** Each ALM pin is fed by 21 to 25 of the 46
  input lines, so a pin fixed in advance forces a line (section 9.2).
- **The local-line detour.** A local line reaches 42 of the ALM inputs,
  so half of the intra-LAB LUT nets leave and re-enter (20.0, step 0.4).

Quartus routes LABs with a nextpnr input count of 50 and more because it
permutes. The code already anticipates this: the TODO at the end of
`reassign_alm_inputs` proposes pseudo-pips in front of the ALM. The mask
writer already maps physical pins to logical inputs through each logical
pin's `bel_pins[0]` (`compute_lut_mask`, "Depermute physical pin").

**The hardware.** In L5 mode an ALM holds two 5-input LUTs. A and B are
shared by both halves; C, E0 and F0 are the top half's; D, E1 and F1 are
the bottom half's. A half's LUT reads exactly its five pins, and any
assignment of its logical inputs to those five is expressible, because
the mask is rebuilt for the assignment. Excluded:
- L6 (one 6-input LUT across the ALM);
- arithmetic cells, whose D0 and D1 are tied to E and F by the carry
  structure;
- MLAB LUTRAM;
- route-through buffers.

These keep today's fixed assignment.

**Design.**
1. **Pseudo-wires, created with the device** (`--lut-permutation`, in
   `ArchArgs`, so before the wire numbering of design 17). For every ALM
   half:
   - five logical-input wires `LPERM.x.y.alm.half.k`, added as bel pins
     `P0` to `P4` of the half's COMB bel;
   - 25 pseudo-pips, one from each of the half's physical pin wires (A,
     B, C or D, E_h, F_h: the GOUT wires) to each `LPERM` wire, with no
     delay.

   That is about 420,000 wires and 2.1 million pips on the 5CSEBA6, only
   when the option is on.
2. **Pre-route.** `reassign_alm_inputs` keeps its logic for everything it
   excludes above. For a plain L5 half it maps logical input k to bel pin
   `Pk` instead of a physical pin. FF route-through and the E/F data
   paths keep their physical pins, and those wires' occupancy keeps the
   router from giving them to a LUT input of another net. A net that
   feeds both halves may use A or B (whose pips reach both halves' `LPERM`
   wires) or two separate pins.
3. **Post-route (`Arch::route`, after router2 and router1's check).** For
   each permuted logical pin:
   - read the pip that drives its `LPERM` wire; its source is the
     physical pin;
   - set `pin_data[pin].bel_pins = {physical}`;
   - unbind the pseudo-pip and the `LPERM` wire, so the net ends on the
     physical pin.

   `compute_lut_mask` then rebuilds the mask unchanged, and signoff and
   the bitstream see only real routing. A post-route check requires that:
   - each permuted pin is on a pin of its half;
   - no two logical inputs of a half share a pin;
   - the net on the physical wire is the logical pin's net.

   Any failure is an error.
4. **Unchanged:**
   - the placer: `comb_pinmap` estimates;
   - the timing model: `getCellDelay` keys on logical ports, as today;
   - checkpoints: `bel_pins` are serialised; a route-prepared checkpoint
     carries the `Pk` mapping and resumes under the same option;
   - router2: it routes to the `LPERM` sink wires.

**Guards.**
- **Mask test (gtest).** For random LUT functions of 1 to 5 inputs and
  all assignments to a half's five pins, the rewritten mask evaluated on
  physical pin values equals the LUT. The hardware index is written
  independently of `get_phys_pin_val`, from libmistral's LUT model.
- **Decode check** (`mistral/tests/lut_perm_check.py`). From the routed
  JSON and the RBF decoded by `mistral-cv`:
  - recover each LUT's physical pins from the routes and its mask from
    `LUT_MASK`;
  - recompute its function;
  - compare it with the cell's `LUT` parameter.

  This is independent of `compute_lut_mask`. Run on the probe and the
  core.
- **Silicon** when the board is back: a golden-checksum design routed
  with permutation, like 19.4's.

**Measurement.** A routing-only change measured from checkpoints: the
probe first, then the core's Fmax set, seed by seed (`--kind routing`).
Expected:
- fewer TD lines and more LD per intra-LAB net;
- fewer fabric wires and iterations.

Then the payoff that 20.0 identified, the input count relaxed under
permutation:
- 19.10's `--lab-input-model nets` at 42;
- the per-ALM count's limit raised toward Quartus's median of 50;
- both on the old core and on today's core.

**Risk.**
- Memory and graph-import time: measure the wire and pip counts, and the
  import time, with the option on.
- `reassign_alm_inputs`'s assumptions about which pins are free (the
  ALM rule's E/F availability) remain placement-time rules. The router
  can always fall back to the fixed assignment, so no LAB the rules admit
  today becomes unroutable in principle.
