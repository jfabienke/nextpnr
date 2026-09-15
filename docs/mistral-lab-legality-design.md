# Mistral LAB Legality Evaluator Design

Status: staged design. [Implementation milestones A–C](mistral-lab-legality-plan.md) provide C++ capture/reference verification, the pure Rust evaluator, native FFI, and serial dispatch/shadow modes. C++ remains the default and owns preparation mutations. Consuming Rust preparation plans, full-LAB expansion, and transactions remain proposed. The implementation plan records completed validation and remaining gates; budget targets are not measured performance claims.

The [four-stage follow-on design](mistral-lab-next-stages-design.md) specifies
preparation-plan consumption, measured boundary optimization, full-LAB evaluation,
and versioned transactions with parallel and incremental reuse. It refines the
remaining D–F interfaces, failure contracts, and acceptance gates after profiling.

This refines the LAB work in [the parallel and incremental build design](parallel-incremental-design.md). A LAB is a logic array block containing ten ALMs; each ALM exposes two LUT and four FF BELs in the current backend model.

## 1. Decision and first deliverable

Start with a pure Rust implementation of **FF control-set evaluation for one LAB**, matching `LabCtrlSetWorker` in [mistral/lab.cc](../mistral/lab.cc). C++ captures a bounded value snapshot, Rust validates and evaluates it, and C++ retains ownership of placement, routing preparation, and all mutations.

The first integrated Rust deliverable is a serial shadow evaluator with a replay corpus and structured disagreement reports. Its C++ reference, pure Rust core, synchronous bridge, and four rollout modes are implemented. This precedes the wider placement transaction and checkpoint projects. After broader parity/preparation validation, promote the control subcheck, then extend the same boundary to ALM legality, LAB input accounting, and MLAB compatibility.

Success means an auditable ownership boundary, explicit states, deterministic compatibility, bounded memory, and acceptable measured overhead. Rust need not outperform equivalent C++. This small component is unlikely to reduce whole-process memory materially by itself: the current control worker already uses fixed arrays. Larger memory gains depend on avoiding duplicated graphs and speculative state elsewhere.

The first delivery excludes candidate search, scoring, timing, packing, route reservation, pin permutation, global/PLL legality, and hardware-rule improvements. A successful control assessment is **not** proof that an entire LAB or placement move is legal.

## 2. Existing behavior that defines compatibility

| Code | Current responsibility | Design consequence |
| --- | --- | --- |
| `get_ctrlsig`, `assign_ff_info` in [lab.cc](../mistral/lab.cc) | Normalize ports, constants, and inversion into `ffInfo` | Capture normalized facts; do not independently interpret raw ports in Rust |
| `LabCtrlSetWorker::run` in [lab.cc](../mistral/lab.cc) | Collect control sets and greedily allocate four shared DATAIN signals | Preserve physical iteration order and exact allocations |
| `isBelLocationValid` in [arch.cc](../mistral/arch.cc) | Compose different subchecks for FF and combinational BELs | Replace only the control subcheck initially |
| `assign_control_sets` in [lab.cc](../mistral/lab.cc) | Rerun the worker, reserve clock/enable/reset routes, write selection indices | Compare allocation plans before using Rust results in preparation |
| `update_bel` in [arch.cc](../mistral/arch.cc) | Refresh cached ALM input counts after binding changes | Later detached evaluation must recompute the equivalent counts |
| `StrictLegaliser::try_place_cluster` in [placer_heap.cc](../common/place/placer_heap.cc) | Bind speculative targets, check legality, revert failures | Shadow evaluation still observes serial speculative state; removing these mutations is a later transaction change |

The existing query order matters:

```text
combinational BEL: queried ALM -> whole-LAB input count -> MLAB groups
FF BEL:            queried ALM -> whole-LAB input count -> FF controls -> MLAB groups
```

These are short-circuit queries. Checking every ALM or adding FF controls to a combinational query can change results when unrelated occupants are already invalid. Provide distinct APIs for the legacy query and, later, a complete LAB assessment.

Call the initial semantics `LegacyControlRulesV1`. They are the backend's current conservative rules, not a complete description of Cyclone V capabilities:

- One distinct clock and at most three distinct enables. The source comments describe a less restrictive clock/pair model that is not implemented here.
- Up to two asynchronous clears, one synchronous clear, and one synchronous load.
- Only a global **clock** avoids a DATAIN allocation. The worker gives global resets/enables no corresponding exemption.
- The control worker scans all forty FF slots. `is_alm_legal` separately rejects occupied FF slots 1 and 3 in each ALM.

An adjacent defect deserves a separate fix: `ControlSig::operator!=` in [archdefs.h](../mistral/archdefs.h) currently duplicates `operator==`. The control worker uses equality; `FFControlSet::operator!=` negates its own equality correctly. Do not fold unrelated fixes into this port or accidentally reproduce the defective inequality in the Rust type.

## 3. Ownership and execution boundary

```text
                   C++ placement / preparation owner
                  authoritative cells, nets, bindings
                                 |
                    capture while state is stable
                                 v
               fixed C ABI snapshot + host provenance
                                 |
                  synchronous, callback-free FFI
                                 v
                    Rust boundary validation
                                 |
                  owned, typed ControlLabSnapshot
                                 |
                   pure evaluation + fixed scratch
                                 v
                 ControlLegal(plan) / ControlIllegal(reason)
                                 |
                    fixed C ABI result, same request ID
                                 v
                 C++ compare / compose / freshness check
                                 |
                       existing C++ mutation path
```

The pure crate receives no `Context`, `Arch`, `CellInfo`, `NetInfo`, BEL handle, or function pointer. It does not call the existing Rust `nextpnr` wrapper. Net identity becomes a snapshot-local integer; physical slot identity is an array position. No worker interns names, reads the environment, logs through C++, or accesses lazy architecture caches.

C++ owns the transport buffers for the duration of the synchronous call. Rust owns each decoded value and its scratch until the record completes; it retains no C++ references afterward. The bounded copy is intentional. Avoid creating a second design-wide graph or a heap object per FF.

There is no embedded target requirement. Allow `std` and select crates on correctness, maintainability, and measured cost. Heap allocation is appropriate for design data, batches, worker storage, and caches. The small control evaluator currently needs only fixed arrays; allocation-free calls are a local property, not a project-wide prohibition on heap use. Fixed arrays may themselves reside in host-owned heap buffers.

Initially one serial caller captures and immediately evaluates. Later callers may execute independent frozen batches concurrently. Use one scheduling layer: the C++ coordinator schedules batches, while the Rust entry point processes its batch serially. Do not create threads per LAB or add a nested global pool.

## 4. Snapshot contract and normalization

Each control snapshot contains exactly forty FF records, indexed as `4 * alm + ff`, in ascending physical order. An unoccupied slot is distinct from an occupied FF whose signals are disconnected.

An occupied record contains `CLK`, `SLOAD`, `SCLR`, `ACLR`, and `ENA`, in that wire-format order. Copy these from `ffInfo.ctrlset`. Specifically:

- A missing ENA currently becomes `$PACKER_VCC_NET` through `get_ctrlsig(..., true)`. The implementation uses a local `PIN_1` value here; do not infer different constant behavior from the surrounding comment.
- An active SCLR with no SLOAD introduces non-inverted `$PACKER_GND_NET` as SLOAD in `assign_ff_info`.
- Polarity comes from the normalized control signal. Capture each connected net's current `is_global` value.
- Net equality follows `NetInfo` identity within this capture, plus polarity for control-signal equality. Equal names or equal constant values do not establish identity.
- Preserve polarity even for a disconnected signal. The worker ignores disconnected inputs when allocating, but downstream equality comparisons in preparation can observe their polarity.

Capture requires current packed FF annotations. Calls before annotation, or after connectivity changes without refreshing those facts, are host integration errors. The Rust decoder cannot discover that a well-formed snapshot omitted a live-state change.

Assign local net IDs in first-encounter order using the FF/control order above. Zero denotes no net; connected nets use IDs 1 through `net_count`, with at most 200 distinct nets. A fixed C++ side table maps these IDs back to live nets only while the capture is valid. Never export pointer bits as IDs or reuse this map across design mutation.

Names and physical LAB coordinates belong in an optional host diagnostic record. They are unnecessary for evaluation. Snapshot-local IDs are not persistent identity; replay records include explicit signal metadata, and any cross-build reuse must satisfy the wider design's matching contract.

## 5. Concrete C ABI, version 1

Use an independently includable C header and Rust `#[repr(C)]` transport structs. Wrap function declarations in `extern "C"` when included from C++. Do not expose Rust enums, `Option`, references, slices, `Vec`, C++ containers, or native `bool` across this boundary. The following defines the proposed input layout; constants are part of the header.

```c
typedef struct {
    uint32_t net_id;       /* 0 = disconnected; otherwise 1..net_count */
    uint32_t flags;        /* bit 0: inverted; bit 1: global net */
} NpnrControlSignalV1;

typedef struct {
    uint32_t occupied;    /* exactly 0 or 1 */
    uint32_t reserved;    /* zero */
    NpnrControlSignalV1 control[5]; /* CLK, SLOAD, SCLR, ACLR, ENA */
} NpnrLabFfV1;

typedef struct {
    uint32_t abi_version;   /* 1 */
    uint32_t struct_size;
    uint32_t rules_version; /* LegacyControlRulesV1 = 1 */
    uint32_t net_count;
    uint64_t request_id;
    uint64_t snapshot_epoch;
    NpnrLabFfV1 ff[40];
} NpnrLabControlsV1;
```

The expected input size on supported targets is 1,952 bytes. Assert sizes, alignment, and offsets in both language builds; do not use packed structs or infer ABI equivalence from size alone. This is a native in-process ABI, not a file format. Serialize replay fixtures by named fields with an explicit schema version, never by dumping native struct bytes.

Decode requirements:

1. Validate ABI, structure size, rule version, count bounds, occupancy values, reserved fields, and unknown flag bits before constructing typed values.
2. Require unoccupied records to be zero-filled. Disconnected signals may retain inversion but must not have the global bit set.
3. Require IDs in range, a dense used ID set, and consistent global classification for every occurrence of a connected net. Inversion may differ between occurrences.
4. Echo `request_id` and `snapshot_epoch`. These identify the host request; Rust cannot certify that an epoch is current.

Results have a fixed header, the same request/epoch fields, a numeric status, and fixed payloads. Reserve at most 256 bytes per result:

| Payload | Contract |
| --- | --- |
| Status | `ControlLegal`, `ControlIllegal`, `BadSnapshot`, `UnsupportedRules`, or `InternalError`; numeric values decoded before constructing language enums |
| Allocation | Twelve signals: `clk`, `sload`, `sclr`, `aclr[2]`, `ena[3]`, `datain[4]`; usable only for `ControlLegal` |
| Conflict | Reason, control kind, incoming signal, originating FF slot, resource mask interpreted in rule order, and up to four blocker signals with their originating FF slots |
| Unused fields | Initialized deterministically; absent FF origins use `UINT32_MAX` |

Illegal/error results expose no reusable plan. A conflict identifies the first failing legacy operation, not a mathematically minimal unsatisfiable subset. Host formatting supplies names when diagnostics are requested.

```c
uint32_t npnr_mistral_eval_controls_v1(
    const NpnrLabControlsV1 *inputs,
    uint32_t count,
    NpnrLabControlResultV1 *outputs,
    uint32_t output_capacity);
```

Start with a maximum of 64 records per call. The return value reports call-level success or failure; each output carries its assessment/error status. A zero-count call accepts null pointers. Nonzero calls require correctly aligned, live, nonoverlapping buffers, sufficient capacity, and no concurrent input mutation or output access. Pointer validity remains an audited caller obligation; a version field cannot make an invalid pointer safe.

Initialize outputs before evaluation. On a call-level failure, the caller ignores the whole batch. No exception or Rust panic may unwind across the ABI. Use an outer panic boundary with an unwind-capable Rust profile; a caught panic invalidates the batch. Allocation failure or process abort is not a recoverable legality result. Keep routine validation and evaluation free of panicking indexing and input-dependent `unwrap` calls.

There is no ownership-transfer/free API, callback, or per-signal FFI call. Confine unsafe slice construction and transport access to the FFI crate; use `#![forbid(unsafe_code)]` in the pure evaluator crate.

The V1 bridge returns call codes 0–7 for success, bad count, insufficient capacity, null pointer, misalignment, address overflow, overlap, and caught panic. Envelope errors leave outputs untouched. A caught panic resets every active output to `InternalError`; the caller discards the complete batch. Only `count` output slots participate, even if capacity is larger. See the [bridge contract](../rust/npnr_mistral_lab_ffi/README.md) for pointer obligations and panic limits.

## 6. Rust types and state transitions

Decode numeric transport fields into private, validated types:

```rust
struct NetId(NonZeroU16);       // local to one validated snapshot
struct FfSlot(u8);             // validated range 0..40
enum Polarity { Normal, Inverted }

struct ControlSignal {
    net: Option<NetId>,
    polarity: Polarity,
}

pub enum ControlAssessment {
    Legal(ControlAllocation),
    Illegal(ControlConflict),
}

pub fn evaluate(snapshot: &ControlLabSnapshot) -> ControlAssessment;
```

These are internal type sketches, not ABI declarations. Store global classification in a fixed net-facts table. Signal equality compares only net ID and polarity, matching C++; global classification affects allocation eligibility, not identity. Constructors and allocation fields are private. A plan can only be produced by completing evaluation.

Keep the following states distinct:

```text
raw transport -> decode -> ValidatedControlSnapshot
                   |               |
              BoundaryError     evaluate
                               /        \
                   ControlIllegal      ControlLegal(plan)
                                             |
                               C++ composes remaining checks
                                             |
                              CompleteMoveAssessment [later]
                                             |
                              freshness + commit preconditions
                                      /              \
                                   Stale           Committed
```

`BadSnapshot` is an integration/data error, not an illegal FPGA placement. `UnsupportedRules` requests explicit dispatch/fallback. Neither becomes `false` in a Boolean legality wrapper without handling the distinction. A control plan cannot be used as a full move certificate.

Rust prevents accidental mutation through shared snapshot references and invalid construction through these APIs. Runtime checks still enforce hardware rules, snapshot provenance, and freshness. A `NetId` newtype distinguishes it from other ID domains but does not, by itself, prevent mixing two snapshots; keep IDs and plans scoped to their owning assessment and check request identity when crossing the ABI.

## 7. Exact control evaluation algorithm

The kernel runs bounded greedy assignment, with no backtracking, heap allocation, sorting, hash-based iteration, or architecture access. Scratch begins with disconnected, non-inverted signals in all twelve allocation entries.

### Signal assignment primitive

For a candidate signal and an ordered list of resource slots:

```text
if candidate is disconnected: succeed without modifying any slot
for slot in the specified order:
    if slot equals candidate: succeed
    if slot is disconnected: assign candidate to slot; succeed
fail
```

The first empty slot wins, even if a later slot already contains an equal signal. Preserve this behavior for both control pools and DATAIN resources. Retain the first assigning FF as provenance; provenance does not participate in equality or placement decisions.

### Pass A: gather LAB-wide control sets

Visit ALMs 0 through 9, FF slots 0 through 3, skipping unoccupied slots. For each FF, apply the assignment primitive in this order:

| Incoming control | Ordered pool | Failure reason |
| --- | --- | --- |
| CLK | `clk` | `ClockConflict` |
| SLOAD | `sload` | `SloadConflict` |
| SCLR | `sclr` | `SclrConflict` |
| ACLR | `aclr[0], aclr[1]` | `AclrCapacity` |
| ENA | `ena[0], ena[1], ena[2]` | `EnaCapacity` |

Return immediately on the first failure. A signal and its inverted form consume different choices. An absent signal consumes no choice, including a disconnected signal whose recorded polarity is inverted.

### Pass B: assign the shared DATAIN resources

Starting with four empty entries, execute these operations in order:

| Signal | Candidate DATAIN entries | Condition |
| --- | --- | --- |
| `clk` | 0 | Connected and not global |
| `sload` | 1 | Assignment primitive handles absence |
| `sclr` | 3 | Assignment primitive handles absence |
| Each `aclr`, in gathered order | 3, then 2 | No global exemption |
| Each `ena`, in gathered order | 2, then 3, then 0 | No constant/global exemption |

Return the allocation on success. On failure, return `DatainConflict` with the control kind, incoming signal/origin, candidate mask, and occupants/origins of the candidate entries. Control-pool failures similarly report the relevant occupied pool. Unused blocker positions are empty; reason/control kind determines how to interpret mask bits.

The gathering pass performs at most 320 slot probes: forty FFs times the capacities `1 + 1 + 1 + 2 + 3`. The DATAIN pass performs at most sixteen more. Snapshot capture and boundary validation may therefore cost more than evaluation; measure them separately.

### Order-sensitive compatibility example

Consider three FFs with one global clock, SCLR signal A, synthetic GND SLOAD, no ACLR, and enable signals encountered as A, B, C. Pass B first puts GND in DATAIN[1] and A in DATAIN[3]. It then assigns:

```text
ENA A -> DATAIN[2] is empty, so assign A there too
ENA B -> DATAIN[2] and [3] conflict; assign B to [0]
ENA C -> [2], [3], [0] all conflict; reject
```

Encountering enables as B, C, A instead permits B on [2], C on [0], and A to reuse [3]. Thus permuting FFs can change the legacy verdict. An optimal matching algorithm or a preference for sharing before using free slots would change semantics. Either improvement requires a separate rule version, preparation validation, and physical routing evidence.

## 8. C++ integration and preparation plans

Introduce `capture_lab_controls` and an evaluator dispatcher adjacent to the existing LAB code. Capture and evaluate the same stable state that the legacy worker reads. During shadow rollout, run the unmodified live worker and the snapshot evaluator; compare verdicts and, for legal cases, all twelve allocated signals after translating local IDs back through the host map.

Keep the original worker as an independent reference. A detached C++ evaluator using the same snapshot schema is useful for replay and cost comparison, but agreement between two ports is insufficient if their common capture adapter dropped information. Validate live-to-snapshot equivalence as well.

At `is_lab_ctrlset_legal`, only the returned control verdict is replaced after promotion. Earlier/later checks in `isBelLocationValid` remain in their existing order. The initial adapter captures on demand; no caching, parallel capture of live state, or mutation is introduced.

At `assign_control_sets`, initially retain the legacy worker's output and compare a shadow plan. A later change can supply the verified Rust plan to the existing C++ application loops. Preserve their lookup behavior:

- Enable choice scans DATAIN entries 2, 3, 0; its position selects ENA wire/index 0, 1, 2. The clock reservation currently uses clock wire 0.
- Reset choice scans DATAIN entries 3, 2; its position selects ACLR wire/index 0, 1.
- Existing disconnected-signal equality behavior, including polarity, remains intact. Do not invent a selected resource for a control that would not match.
- LUTRAM clock/enable reservations, `aclr_used`, per-half `clk_ena_idx`/`aclr_idx`, and all `reserve_route` calls remain C++ responsibilities.

Recompute immediately before preparation or verify the complete read set before applying a retained plan. A placement-time plan can become stale when global-net classification or occupancy changes. The plan contains no wire IDs and grants no permission to reuse routing reservations independently.

`lab_pre_route` also invokes `reassign_alm_inputs`, which changes pin mappings and can insert route-through cells. Neither operation belongs in this pure evaluator. Evaluator adoption alone does not make route preparation incremental or transactional.

## 9. Expansion to complete LAB legality

Introduce a separate `LabSnapshotV2` after control parity. Keep the small control ABI available rather than enlarging every hot control query immediately. V2 contains twenty optional LUT fact records, forty optional FF fact records, the physical LAB/MLAB kind, resolved input-limit policy, and request/provenance fields. LUT and FF net IDs share one snapshot domain.

| Subcheck | Additional facts | Behavior to preserve |
| --- | --- | --- |
| ALM storage/input feasibility | LUT input identities/counts, bit count | At most 64 LUT bits; existing nested shared-input matching when total inputs exceed eight |
| Carry compatibility | LUT carry mode | Reject mixed carry/non-carry LUTs in an ALM |
| FF data accessibility | LUT output, used-input count, FF SDATA/DATAIN | Current route-through availability, mirrored E/F availability, and consumption order |
| FF slot restrictions | Physical FF slot | Reject occupied odd slots before any future dual-FF support |
| LAB input accounting | Used/chain-shared counts, LUT inputs/outputs, FF data, MLAB group, `constr_z` | Recompute all ten contributions before comparing their sum with the policy limit |
| MLAB compatibility | Physical `is_mlab`, LUT group IDs, FF occupancy | On MLAB tiles require one common LUT group; a nonnegative group excludes all FFs |

Some superficially similar rules are deliberately different today:

- `is_alm_legal` uses nominal LUT input counts and its nested sharing loop can match null entries. `update_alm_input_count` uses used-input counts, skips null shared signals, and caps ordinary LUT sharing at two. Do not merge these calculations during extraction.
- Mirrored E/F availability uses the opposite LUT's `used_lut_input_count <= 2`, regardless of the nearby comment's wording. External DATAIN first consumes an available route-through LUT, then E/F; SDATA consumes E/F first.
- The MLAB input-count shortcut returns zero for the whole ALM when a qualifying LUT has `mlab_group != -1` and `constr_z > 2`. Preserve and test that branch before reconsidering it.
- Input accounting counts uses conservatively, not globally distinct LAB net IDs. It is not a routing proof. Resolve `MISTRAL_LAB_INPUT_LIMIT` in C++ and pass the actual signed value; its current default is 42. Any change to environment parsing or allowed ranges is separate policy work.

Prefer recomputation from bounded facts to trusting cached `unique_input_count`. During shadowing, compare recomputed counts against the current cache as well as the legacy verdict; a disagreement may identify stale host state instead of a Rust algorithm defect. Cell-fact derivation remains in C++ and must be refreshed after relevant packing or connectivity changes.

Provide two explicit entry points:

- `evaluate_for_bel(snapshot, query)` preserves the original combinational/FF scope and short-circuit order.
- `evaluate_whole_lab(snapshot)` checks all ten ALMs, input count, FF controls, and MLAB groups. Its result certifies only the represented LAB rules, not region constraints, cluster geometry, BEL ownership, timing, or routeability.

Placement transactions compose complete LAB assessments with those external constraints. Do not use the whole-LAB verdict as a drop-in replacement for a scoped query without measuring and reviewing the behavior change.

## 10. Parallel and incremental integration

### Detached candidates and commits

Parallel execution starts after the serial snapshot path works. Capture a frozen base, apply candidate occupancy changes to bounded overlays, and evaluate every source/destination LAB affected by a move or displaced cluster. A carry cluster can span several LABs. A control-only assessment remains one component of this process.

Initially use a conservative host design epoch covering every relevant mutation. A retained assessment is usable only if its epoch and proposal identity still match. An epoch must advance for provisional mutation and restoration too if assessments can survive those operations; restoring occupancy must not accidentally make an old certificate current again.

Later replace broad invalidation with explicit read sets:

| Dependency | Changes that invalidate an assessment |
| --- | --- |
| LAB occupancy revision | Binding, unbinding, or moving any represented occupant |
| Packed cell-facts revision | Control connection/polarity, LUT facts, FF data, MLAB group, or relevant cluster facts |
| Referenced net-properties revision | In particular, a clock becoming global/non-global |
| Rules/configuration revision | Semantics version, supported device model, input-limit policy for full LAB checks |
| External move constraints | Expected BEL ownership/strength, cluster placement, regions, and other transaction checks |

Control-net changes can invalidate physically distant LABs; use an incidence map from nets to dependent LABs. An input-limit change affects full LAB assessments but need not invalidate a control-only cache. The dependency set should describe what the particular evaluator actually reads.

The commit coordinator serializes publication, checks all read versions and expected bindings, and commits in a stable proposal order. A preceding commit that touches a read dependency makes a later assessment stale; reevaluate it instead of accepting its earlier legal verdict. Preflight allocation and apply failures as described in the parent transaction design. Rejected or stale detached proposals must leave committed state unchanged.

Every legacy mutation path active during an epoch must participate in versioning or be excluded until the barrier ends. Adding counters only to a new adapter is insufficient. Do not interpret Rust `Send`/`Sync` on owned values as permission to read a concurrently mutating C++ design.

### Incremental reuse

Do not add caching in the first implementation. Later, begin with at most one accepted assessment per LAB and a byte-bounded replay/cache budget. Dirty LABs are recomputed from current facts; unchanged LABs may reuse an assessment only with matching dependencies and rules.

Keep separate states for `Dirty`, `Evaluated`, and `Prepared`. A new legal assessment invalidates affected preparation artifacts; it does not itself install reservations or pin maps. Incremental preparation needs its own dependency/provenance and removal/rebuild logic, especially for inserted route-through cells. Until that exists, rerun the established full preparation path.

Cache legality and reusable abstract allocations only. Do not retain C++ pointers, old ID maps, or live snapshot references. Any content-addressed cache must compare complete normalized fields after hashing and instantiate its plan against the current snapshot mapping. A hash alone is not a validity or freshness proof. Cross-build reuse additionally requires faithful packed-state reconstruction and entity matching from the parent design.

## 11. Memory and cost budgets

Use explicit initial engineering budgets, then report measurements rather than assuming language changes improve them:

| Item | Initial budget/policy |
| --- | --- |
| C ABI input | 1,952 bytes per LAB |
| Result | At most 256 bytes per LAB |
| Batch transport | At most 138 KiB for 64 inputs/results at those limits |
| Rust decode/evaluation scratch | At most 8 KiB per active worker; verify actual stack/layout usage |
| C++ capture mapping | Fixed capacity for 200 net pointers; retained only during capture/comparison/application |
| Hot kernel allocations | Zero; no strings, per-FF heap nodes, or reference-counted snapshots |
| Retained history | None by default; explicit bounded diagnostic recording and later cache budgets |

Transport, host mapping, and decoded scratch coexist briefly; account for all three. Reuse batch buffers without retaining every observed candidate. Measure peak process footprint, live/retained allocation bytes, capacities, and growth with workers separately. Releasing owned memory does not necessarily reduce RSS immediately.

Instrument captures, evaluations, outcomes by reason, shadow mismatches, batch sizes, cache hits/invalidations when added, and stale retries. Measure capture, decode/FFI, kernel, comparison, and commit time separately. Prefer sampled or batch timing to a clock read on every tiny operation; quantify instrumentation overhead.

Compare four configurations on identical inputs: current live C++, detached C++ over the snapshot, serial Rust over that snapshot, and parallel detached evaluation. This separates the benefit of changing state access from the choice of language. Include end-to-end placement/P&R results; a faster kernel is not evidence of faster overall builds.

## 12. Validation and acceptance gates

### Rule and differential tests

Use Rust unit tests for pure rules, fixed C++ fixtures for capture/live-reference parity, and a language-neutral replay corpus. Store schema/rules versions, code revision plus working-tree patch identity, device/configuration, normalized fields, expected verdict, and expected legal allocation. Recording is opt-in and capped; private Fabi386 net/cell names need not enter repository fixtures.

Required cases include:

- Empty LAB; occupied FF with disconnected controls; repeated identical controls; both polarities of the same net; disconnected inverted signals.
- Every control-pool capacity boundary and each DATAIN assignment/failure path, including shared resources, global versus local clock, constants, and the order-sensitive example above.
- Host normalization for absent ENA and synthetic SLOAD, checked from actual packed C++ cell facts.
- All forty physical FF positions. Control-only evaluation may accept odd slots while the composed ALM check rejects them.
- Malformed versions, lengths, IDs, occupancy, reserved bits, null/global combinations, and inconsistent per-net facts; these are boundary failures, not placement rejections.

Exhaustively enumerate reduced domains of a few slots/net identities, then generate bounded randomized forty-slot cases. Compare each case with the independent legacy worker and both detached implementations. Shrink mismatches by removing FFs/signals while preserving the failing comparison. Cover every rejection branch and legal allocation choice; a high line-coverage percentage alone is inadequate.

Useful properties are determinism, input immutability, invariance under bijective net-ID renaming with corresponding plan translation, and equivalence of single-record versus batched execution. FF-order permutation is **not** an invariance property: compare each permutation separately against the reference.

For later full LAB tests add LUT bit/input boundaries, duplicate/null inputs, carry mixtures, mirrored E/F cases, MLAB groups/shortcuts, the signed input-limit boundary, and stale cached input counts. Test legacy scoped queries separately from complete LAB assessment.

### Boundary, state, and concurrency tests

Assert ABI offsets/alignment/sizes on every supported target. Exercise zero/max/oversized batches, insufficient output capacity, deterministic error initialization, and legal-plan ID validation. Fuzz owned transport values; pointer validity is established by the harness rather than fuzzing arbitrary process addresses.

Use compile-fail tests for invalid private-type construction and APIs that try to promote a control assessment directly to a full move certificate. The pure crate must have no dependency on the live backend wrapper and no unsafe code. Confirm its validated snapshot/scratch ownership supports the intended worker use without manual unsafe `Send`/`Sync` implementations.

Run Rust interpreter/sanitizer checks where supported, and C++/Rust boundary sanitizers on a supported CI platform. A sanitizer that fails to start is unavailable evidence, not a passing test. Exercise repeated mixed valid/error batches to detect retained memory. After transactions exist, test stale epochs, remote net-property changes, competing proposals, and exact state preservation on rejected/stale commits.

### Fabi386 and end-to-end evidence

Use the Fabi386 execution-stage ALUT-only slice already used for repository investigation as the first real candidate source. Capture both accepted and rejected candidates during placement, not just final legal LABs. Preserve tool/input hashes, device, seed, options, and environment policy in the run manifest; leave the Fabi386 source checkout unchanged.

That slice does not cover every Mistral feature. Add purpose-built FF-control and LUTRAM/MLAB designs, mixed carry/LUT cases, and global-clock classification cases. Retain the previously difficult large carry-chain case with an explicit time/search budget and recorded failure diagnostics; evaluator parity does not imply it becomes placeable.

In serial parity modes require matching control verdicts and legal plans, unchanged candidate/RNG ordering, placement success/failure, and existing final checks. Compare placement/preparation artifacts and routed output where runs are deterministic. Require full P&R and timing/resource checks before authoritative preparation consumes Rust plans. Physical hardware-rule changes require additional evidence beyond software parity.

For parallel promotion, evaluate 1/2/4/8 workers where supported. Require reproducible ordered commits, zero false accepts or false rejects against the frozen reference corpus, bounded memory/retries, and valid end-to-end results. Report runtime and memory distributions over fixed repeated runs. No speedup is required; set an acceptable overhead budget from the measured baseline before enabling the new default.

## 13. Rollout modes and failure policy

Expose a proposed Mistral-only setting through the normal argument/configuration path, resolved once before evaluation:

| Mode | Behavior |
| --- | --- |
| `legacy` (initial default) | Existing worker only; no snapshot overhead |
| `shadow` | Legacy remains authoritative; compare Rust results and record bounded mismatch/error diagnostics |
| `verify` | Compare both; any mismatch or unexpected boundary error fails the test/run |
| `rust` | Rust decides the control subcheck; C++ continues other checks and mutations |

Selecting a Rust-dependent mode in a `BUILD_RUST=OFF` binary is a configuration error, not a silent selection of another mode. In authoritative mode, explicitly unsupported rules may dispatch to the declared legacy path and increment a fallback counter. Invalid snapshots, impossible result IDs, or internal errors are implementation failures; do not silently interpret them as legal/illegal or continue with a suspect plan.

Shadow mismatch records contain the first differing verdict/allocation, normalized snapshot, provenance, and evaluator versions. Keep only a bounded number of complete records while retaining aggregate counts. A mismatch blocks promotion; retain the legacy default while diagnosing it. CI uses full verification, while optional sampling may reduce development profiling overhead.

## 14. Implementation units and file layout

| Unit | Changes | Exit criterion |
| --- | --- | --- |
| A: capture/reference | Snapshot schema, capture adapter, independent legacy-plan export, replay writer, detached C++ reference | Live/captured verdict and legal-plan parity; normalization fixtures |
| B: pure Rust | Validated types, exact two-pass algorithm, reasons, differential/property tests | Corpus parity; bounded layout; pure crate contains no unsafe code |
| C: shadow bridge | Synchronous ABI, CMake/Cargo wiring, dispatcher, counters/modes | Actual Mistral `BUILD_RUST=ON` link/run; ABI and Fabi386 shadow checks; Rust-off build preserved |
| D: serial promotion | Rust-authoritative control subcheck, then separately preparation-plan consumption | Zero verification mismatches; full preparation/P&R validation; documented resource costs |
| E: full LAB evaluator | V2 LUT/FF facts, ALM rules, input accounting, MLAB checks | Per-rule and scoped-query parity; whole-LAB semantics explicitly tested |
| F: transactions/reuse | Frozen overlays, read sets, ordered commits, dirty-LAB tracking; later bounded cache | Rejection/staleness leaves committed state unchanged; parallel and incremental regression gates pass |

Source organization (A and the pure Rust crate from B are implemented; the FFI and later expansion remain planned):

```text
mistral/lab_snapshot.h               C++ snapshot/capture declarations
mistral/lab_control_abi.h            shared C-compatible value layout
mistral/lab_model.h                  detached evaluator interface
mistral/lab_model.cc                 detached C++ reference rules
mistral/lab_legality.cc              capture, dispatch, comparison, integration
mistral/lab_replay.h/.cc             named-field JSON replay reader/writer
mistral/tests/lab_legality.cc        host capture and differential tests
mistral/tests/fixtures/             backend-local replay fixtures
mistral/tests/compare_lab_controls.py  C++/Rust differential runner
mistral/tests/lab_control_oracle.cc  standalone C++ test driver
rust/npnr_mistral_lab/               pure model, validation, rules, unit tests
rust/npnr_mistral_lab_ffi/           static library; small audited ABI boundary
```

Both Rust crates are in [rust/Cargo.toml](../rust/Cargo.toml); neither needs the existing `nextpnr` wrapper. The root [CMakeLists.txt](../CMakeLists.txt) imports the existing shared Rust example separately from the Mistral bridge, which is imported/linked only for Mistral. [mistral/CMakeLists.txt](../mistral/CMakeLists.txt) registers native tests and a standalone FFI replay driver. Rust-off Mistral, Rust-on Mistral, and Rust-on generic have been built and exercised on the development host.

Planned validation commands after implementation, using an existing valid Mistral dependency configuration:

```sh
cargo test --manifest-path rust/Cargo.toml -p npnr_mistral_lab -p npnr_mistral_lab_ffi
cmake -S . -B build-lab -DARCH=mistral -DBUILD_RUST=ON -DBUILD_TESTS=ON -DMISTRAL_ROOT=/path/to/mistral
cmake --build build-lab --parallel
ctest --test-dir build-lab --output-on-failure
```

Units A and B establish capture/reference semantics and the pure Rust implementation. Unit C integrates the bounded Rust experiment without depending on checkpoints, a new placer, or a routing rewrite. See the [implementation plan](mistral-lab-legality-plan.md) for the current completion record.

## Design validation performed

Initial design checks resolved the local document links and Markdown whitespace/fences. An isolated C++ probe confirmed the order-sensitive example. Milestone A passed live C++ differential tests and paired Fabi386 placement/routing runs. Milestone B passed Rust unit/compile-fail tests, native layout comparison, and 15,137 complete-result comparisons against C++. Detailed evidence and the remaining FFI/cross-platform gates are in the implementation plan.
