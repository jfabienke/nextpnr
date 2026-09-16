# Mistral Checkpoint Design (Stage 5, units 2a/2b)

Status: proposed design, 2026-09-16. Refines Stage 2 of
[`parallel-incremental-design.md`](parallel-incremental-design.md) for the
Mistral backend with a field audit of the current code. Nothing here exists
yet. The tracker records implementation state; this document owns rationale.

## 1. Decision and scope

A checkpoint is a completed phase of one design, written by nextpnr and
restorable by nextpnr into a context equivalent to the one that wrote it, so
that the next phase runs exactly as it would have in the writing process.
"Equivalent" is defined by the acceptance gate in section 7: resuming from a
checkpoint must produce the same placement and routing as the uninterrupted
run, compared by name, plus an identical report.

Phases and what a checkpoint of each contains:

```text
packed  ->  placed  ->  route-prepared  ->  routed
```

| Phase | Adds to the previous | Restorable today? |
| --- | --- | --- |
| packed | post-pack netlist, cluster geometry, pin maps and pin states, IO attributes from the QSF, settings, RNG state | No. The JSON writer persists attributes and parameters only; clusters and pin data are lost |
| placed | BEL bindings and strengths, PLL counter selections, placement-dependent PLL fixups | Bindings only (`NEXTPNR_BEL`); the rest is lost |
| route-prepared | LAB control allocations and their route reservations, route-through cells and rewired FF inputs, global routing | No |
| routed | route trees and strengths | Routes only (`ROUTING`) |

Unit 2a delivers packed and placed checkpoints. Unit 2b delivers
route-prepared and routed. Mid-search resumption (solver queues, congestion
history) is out of scope, as the parent design says.

Three things this design does not do. It does not change the Yosys JSON
input path. It does not invent a binary format: the payload is the existing
nextpnr JSON with one additional top-level object, so every tool that reads
nextpnr's output today keeps working. It does not persist pointer-based
derived facts (`combInfo`, `ffInfo`, `unique_input_count`); those are
reconstructed by `assignArchInfo()` and the bind path, which is how they are
built in a normal run.

## 2. Field audit

What the design owns per phase, from the code as of commit `68fe1124`.

### 2.1 Persisted today by `jsonwrite.cc` and `archInfoToAttributes()`

- Cells: type, `params`, `attrs`, `connections`; `NEXTPNR_BEL` and
  `BEL_STRENGTH` attributes when placed.
- Nets: `attrs`; `ROUTING` attribute as a `wire;pip;strength;` list when routed.
- Top-level ports and `settings` (as module attributes).

### 2.2 Written by `Arch::pack()` and lost on write

| Field | Where written | Restore by |
| --- | --- | --- |
| `cluster`, `constr_x/y/z`, `constr_abs_z`, `constr_children` | `pack.cc` carry chains (`constr_z = ((i/2)%10)*6 + i%2`, `constr_y = -(i/20)`), MLAB groups, DSP, M10K, IO-register packing | persist per cell; rebuild `constr_children` from members |
| `pin_data[port].bel_pins`, `pin_data[port].state` (`PIN_INV`, mux requests) | `pack.cc` (lines 100–130, 400–430), `assign_default_pinmap` | persist per cell per port |
| `io_attr` | `qsf.cc` from `set_location_assignment` / `set_instance_assignment` | persist as a map; the QSF is not re-read on resume |
| `combInfo`, `ffInfo` | `assignArchInfo()` at the end of pack | reconstruct, never persist (pointer-based) |
| generated cells created by packing (`MISTRAL_CLKENA`, PLL clock cells) | `pack.cc`, `pll.cc` | ordinary cells in the netlist; persisted like any other |

### 2.3 Written by `Arch::place()` and lost on write

| Field | Where written | Restore by |
| --- | --- | --- |
| `pllclk_sel_map` | `pll.cc` (`fixup_pllclk_placement` after placement) | persist as a list of (key, selection) |
| bindings | placer | already persisted (`NEXTPNR_BEL`, `BEL_STRENGTH`) |
| `unique_input_count` per ALM | `update_bel` on every bind | reconstruct by binding |

### 2.4 Written by `Arch::route()` before the router and lost on write

| Field | Where written | Restore by |
| --- | --- | --- |
| LAB control allocation: `clk_ena_idx`, `aclr_idx` per ALM, `aclr_used` per LAB | `assign_control_sets` (Stage 1 prepared edits) | persist per LAB |
| wire route reservations: `WireInfo::flags & RESERVED_ROUTE` and the uphill index | `assign_control_sets` | persist as (wire, uphill index) |
| route-through cells `<ff>$ROUTETHRU` (`MISTRAL_BUF`) and rewired FF inputs | `reassign_alm_inputs` (`lab.cc` ~913) | ordinary cells and connections in the netlist; persisted; must be marked generated so an edit can remove them |
| global clock routing | `route_globals` | routes, persisted with `ROUTING` |

### 2.5 Context-level

- `settings`, the seed, and `ctx->rngstate` at the end of the phase.
- `placement_revision` session and revision: not persisted; a restored
  context starts a new session. Frozen work never crosses processes.
- Device, package, speed grade (`ArchArgs::device`), libmistral fingerprint,
  nextpnr build version.

Counts on Fabi386, for the acceptance gate: 1,226 clustered cells, 7,813
non-default pin-state entries (from the parent design's audit), 549
route-through cells, 4,191 LABs, 2 IO attributes.

## 3. Format

One file, the ordinary nextpnr output JSON, plus a top-level object:

```json
"nextpnr_checkpoint": {
  "manifest": {
    "schema": 1,
    "backend": "mistral",
    "backend_state_version": 1,
    "phase": "packed" | "placed" | "route-prepared" | "routed",
    "device": "5CSEBA6U23I7",
    "nextpnr": "<build version>",
    "libmistral": "<fingerprint>",
    "seed": 1,
    "rng_state": "<uint64 as decimal string>",
    "settings": { ... },
    "input_sha256": "<of the Yosys JSON>",
    "parent_sha256": "<of the checkpoint this was resumed from, if any>"
  },
  "packing": {
    "clusters": { "<root cell>": [ { "cell": "...", "x": 0, "y": -1, "z": 6, "abs_z": true }, ... ] },
    "pins": { "<cell>": { "<port>": { "bel_pins": ["..."], "state": "inv" } } },
    "io_attr": { "<port>": { "<name>": "<value>" } },
    "generated": ["<cell>", ...]
  },
  "physical": {
    "pllclk_sel": [ { "key": "<uint64 decimal>", "sel": 3 } ],
    "labs": { "<lab index>": { "clk_ena_idx": [0, 1], "aclr_idx": [0, 1], "aclr_used": [true, false] } },
    "reserved_routes": [ { "wire": "<wire name>", "uphill": 2 } ],
    "generated_routethru": ["<cell>", ...]
  }
}
```

Rules:

- The manifest is written first and validated before anything is adopted. A
  missing or older `backend_state_version` is an error, not a partial load.
- Only sections at or below `phase` are present. A `placed` checkpoint has
  `packing` and `physical.pllclk_sel`; `physical.labs` and
  `reserved_routes` appear from `route-prepared`.
- Cells and wires are named by their nextpnr names, never by index. Indices
  depend on `IdString` interning order, which differs between the writing and
  the restoring process.
- Property values are written with `Property::to_string()`, the same
  encoding the frontend already parses.
- The file is written to a temporary name and renamed after the payload is
  complete, so a crash leaves no half checkpoint.

A checkpoint is not the plain output JSON of today. The `placement_reuse`
adapter (Stage 4E) keeps parsing plain output JSON for the name-and-signature
case and does not depend on this format; once checkpoints exist, its loader
becomes the second consumer of the same parser.

## 4. Writing

`--checkpoint <file>` writes the checkpoint of the last completed phase at the
point the flow stops (`--pack-only`, `--no-place`, `--no-route`, or the end).
`Arch::write_checkpoint(phase, path)` collects sections 2.2 to 2.5 from the
live context, then calls the existing `write_json` with the extra object.
Nothing in the flow changes when the option is absent.

## 5. Restoring

`--resume <file>` replaces `--json`. Order, from the parent design's restore
ordering, adapted to the code:

1. Parse the file; validate the manifest against this build, this device,
   and the requested operation. Options that conflict with the manifest's
   settings are rejected; the checkpoint's settings win otherwise.
2. Load the logical design through the existing frontend in a deferred mode:
   `import_toplevel_ports` runs, `attributesToArchInfo()` does not. This is
   one flag on `GenericFrontend`; the default path is unchanged.
3. Restore packing: `io_attr`; then for each cluster, set `cluster`,
   `constr_*` on every member and rebuild the root's `constr_children` in
   the recorded order; then `pin_data` per cell per port.
4. `assignArchInfo()`: rebuild `combInfo` and `ffInfo` from the restored
   netlist, pin states, and clusters. This is the same call `pack()` ends
   with.
5. If `phase >= placed`: bind every cell with `NEXTPNR_BEL` at its
   `BEL_STRENGTH` through `bindBel`, which rebuilds `unique_input_count`;
   restore `pllclk_sel_map`. The existing `attributesToArchInfo()` does the
   binding part; it is reused for that and only that.
6. If `phase >= route-prepared`: restore LAB control indices and
   `aclr_used`; set `RESERVED_ROUTE` flags and uphill indices on the named
   wires; the route-through cells are already in the netlist.
7. If `phase == routed`: bind wires and pips from `ROUTING`, as today.
8. Restore `ctx->rngstate`; mark `ctx->settings["step"]`; run `ctx->check()`
   and the backend's LAB legality sweep over every bound LAB BEL. Only then
   publish the context.

The restore builds into a fresh context. A failed restore is an error with
the offending section named; it never leaves a partially restored design.
The flow then skips completed phases: a `packed` resume runs place and route;
a `placed` resume runs route (which begins with `lab_pre_route`); a
`route-prepared` resume runs the router only, which requires splitting
`Arch::route()` into `prepare_route()` and the router call; a `routed`
resume runs signoff and bitstream generation.

## 6. Increments

| Unit | Deliverable | Gate |
| --- | --- | --- |
| 2a-1 | `packing` section written and restored; deferred frontend mode; resume from `packed` | Every clustered-cell record and pin-state entry equal after round trip (1,226 and 7,813 on Fabi386); resume-from-packed then place and route equals the clean run by the section 7 comparison |
| 2a-2 | `pllclk_sel` and placed resume | Resume-from-placed then route equals the clean run's routing and report |
| 2b-1 | `Arch::route()` split; LAB control state and reservations persisted; route-prepared resume | Resume-from-prepared then router equals the clean run |
| 2b-2 | routed resume and signoff-only flow; manifest lineage (`parent_sha256`) | Bitstream bits identical to the clean run |

## 7. Acceptance and comparison rules

Byte identity of routed JSON is the wrong gate here, and the reason is
recorded in the tracker: the writer numbers nets by `IdString` index, and a
restored process interns strings in load order. Comparison is therefore by
name: for every cell, the same BEL and strength; for every net, the same
set of (wire, pip) pairs; and the report (`--report`) byte-identical, since
it carries no indices. Log checksums are compared only between runs of the
same process kind.

Identity holds only if the RNG state is restored. Packing may consume RNG
draws (`ctx->shuffle` in the annealer's legaliser, HeAP's initial placement);
the checkpoint records `rngstate` at the phase boundary so the next phase
draws exactly what the uninterrupted run drew. If a phase turns out to depend
on state this design does not persist, the comparison fails and names the
first differing object; that is the audit finishing itself.

Additional fixtures beyond Fabi386, because the execution-stage probe does
not exercise them: the feature fixture (CLKBUF, MLAB), an M10K design, a PLL
design, and an IO-register design from the silicon work.

## 8. What this unlocks and what it costs

Phase reuse for experiments: pack once, place many times; place once, route
many times. The gaps document's PLL, IO-register, and router-deadlock
investigations each re-ran pack and place; those become a resume. Stage 4E's
placement reuse gets a typed source instead of parsing output JSON, and
route reuse (3c) gets the provenance it needs: generated cells, rewired
inputs, and reservations are named in the checkpoint.

Costs: two new sections in the writer and a loader of comparable size to
`placement_reuse.cc`, the frontend flag, the `Arch::route()` split, and the
fixtures. The checkpoint of Fabi386 is the output JSON (33 MB) plus a few
hundred kilobytes.
