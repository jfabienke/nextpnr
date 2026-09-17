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
5. Default behavior stays legacy during rollout. Unsupported capabilities select
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
pairs in one ALM against 4%, and so it enters a LAB with about one
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

