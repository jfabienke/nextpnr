# Mistral Checkpoint Design (Stage 5, units 2a/2b)

Status: designed 2026-09-16; units 2a-1 and 2a-2 (packed and placed
checkpoints) implemented the same day in `mistral/checkpoint.cc`, with the
generic hooks in `common/kernel/basectx.h`, `json/jsonwrite.cc`,
`frontend/json_frontend.cc` and `common/kernel/command.cc`. Refines Stage 2
of [`parallel-incremental-design.md`](parallel-incremental-design.md) for
the Mistral backend with a field audit of the current code. Section 2.6 and
the format in section 3 record what the implementation found that the
proposal had missed. The tracker records implementation state; this
document owns rationale.

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

### 2.6 What a reload of nextpnr's own JSON loses (found by implementation)

The proposal assumed that reading the output JSON back rebuilds the logical
netlist as the writing process had it, so that only the arch-owned fields
above needed persisting. It does not, in four ways, and each one changes
the placement or routing that follows:

1. **Iteration order.** nextpnr's `dict` iterates newest entry first, and
   packing's deletions move the last entry into the hole, so the post-pack
   order of `ctx->cells`, `ctx->nets`, every cell's `ports`, `attrs` and
   `params`, every net's `attrs`, and every net's `users` store is a
   function of history. json11 objects are sorted maps, so a reload
   produces name order instead. HeAP walks cells and users to build its
   equations, the annealer and router draw from the RNG in cell and net
   order, and floating-point sums depend on term order: a different order
   is a different trajectory. The checkpoint therefore records every one of
   these orders and the restore rebuilds each container in place (values
   are moved, so every pointer stays valid; users are re-added and each
   `user_idx` reassigned).
2. **IdString indices.** Interning order differs between the writing and
   the reading process, and indices are what `IdString` comparison and the
   writer's net numbering use. The checkpoint records the whole table in
   index order; the restore verifies that everything the process has
   already interned is a prefix of it (same build, same options) and
   interns the rest before the netlist is imported, so the frontend then
   finds every name at the recorded index. Net numbering in the resumed
   run's output is thereby identical to the clean run's, which turns the
   section 7 gate from name comparison into byte identity.
3. **Top-level ports.** A nextpnr-written file carries the `synth` setting,
   and the frontend then skips IO buffer creation and with it `ctx->ports`.
   The packer reads that table (`prepare_io`) and the writer emits it. It
   is persisted with its net and direction.
4. **Disconnected cell ports.** The frontend creates one port per
   connection bit, so a port with an empty connection (an FF's unused
   `SCLR`, say) does not exist after reload. Code that asks
   `ports.count(x)` would then answer differently. Each cell's port list in
   the checkpoint carries the direction, and the restore recreates missing
   ports before it reorders.

A fifth finding is in the writer rather than the reader: `write_module`
interned the `"module"` attribute key on every write. A run that writes an
intermediate file therefore interned that key before the route-through
nets, and a run that did not interned it after them, which shifted every
route-through net's number by one. The lookup is now non-interning; a
write no longer changes the table.

Bindings are a sixth: the packer binds QSF-located IO cells and PLL cells
(`STRENGTH_LOCKED`) before placement, so a packed checkpoint carries
bindings too. They are persisted explicitly at every phase, by BEL name,
and resolved through the BEL list rather than `getBelByNameStr`, whose
name parser interns every component.

A seventh was found by the log checksum, the last comparison that still
differed after the outputs were byte-identical: an undriven net keeps the
port name of a driver the packer removed (`disconnectPort` clears the cell
pointer, not the port), and `Context::checksum()` hashes that name. On
Fabi386 it is one net, the top-level clock, with `O` left behind by the
removed nextpnr input buffer. It changes no behaviour, but the resumed
context is meant to be the writer's context, so the checkpoint carries it
(`netlist.stale_driver_ports`) and the checksums then agree across process
kinds as well.

## 3. Format

One file, the ordinary nextpnr output JSON, plus a top-level object:

```json
"nextpnr_checkpoint": {
  "manifest": {
    "schema": 1, "backend": "mistral", "backend_state_version": 1,
    "phase": "packed" | "placed" | "route-prepared" | "routed",
    "device": "5CSEBA6U23I7", "nextpnr": "<build version>",
    "seed": "<uint64 decimal>", "rng_state": "<uint64 decimal>", "idstrings": 55320
  },
  "idstrings": ["", "...", ...],
  "order": {
    "cells": ["<cell>", ...], "nets": ["<net>", ...],
    "settings": ["<key>", ...], "attrs": ["<key>", ...],
    "cell_ports": [[[<port idstring index>, <direction>], ...], ...],
    "cell_attrs": [[<idstring index>, ...], ...], "cell_params": [[...], ...],
    "net_attrs": [[...], ...],
    "users": [[[<cell position>, <port idstring index>], ...], ...]
  },
  "netlist": { "top_ports": [ { "name": "clk", "net": "clk", "type": 0 } ], "stale_driver_ports": [["clk", "O"]] },
  "packing": {
    "cluster_cells": [ { "cell": "...", "cluster": "<root>", "x": 0, "y": -1, "z": 6, "abs_z": true, "children": ["..."] } ],
    "pins": [ { "cell": "...", "ports": [ { "port": "...", "state": 3, "bel_pins": ["..."] } ] } ],
    "io_attr": [ { "port": "...", "attrs": [ { "name": "...", "value": { "str": "..." } | { "bits": "..." } } ] } ]
  },
  "physical": {
    "bindings": [ { "cell": "...", "bel": "<bel name>", "strength": 3 } ],
    "pllclk_sel": [ { "key": "<uint64 decimal>", "sel": 3 } ]
  }
}
```

Rules:

- The manifest is written first and validated before anything is adopted. A
  missing or older `backend_state_version` is an error, not a partial load;
  so is another device or a phase this build cannot restore. A different
  nextpnr version is a warning: identity with the writing run is then not
  promised.
- `idstrings` is the complete table in index order (section 2.6, item 2).
  The `order` section's per-object lists are aligned with `order.cells`
  and `order.nets` and hold IdString indices, which the replayed table
  makes exact; `users` names cells by their position in `order.cells`. All
  other sections name objects by their nextpnr names.
- `packing.pins` omits every entry that `assign_default_pinmap()` would
  recreate (state `PIN_SIG`, one bel pin equal to the port or its
  `comb_pinmap` image, port present). On Fabi386 that leaves the 7,813
  non-default entries of section 2.5 out of 63,103. The one order the
  restore does not reproduce is `pin_data`'s own: recorded entries come
  first, regenerated defaults follow in port order. Nothing but the ALM
  debug dump in `lab.cc` iterates that map; every consumer looks pins up by
  name.
- Property values are written as `{"str": s}` or `{"bits": s}` with the
  `Property::to_string()` bit encoding, so a string that happens to look
  like a bit vector cannot be misread.
- `physical.bindings` and `physical.pllclk_sel` are present at every phase.
  Later phases add `physical.labs`, `reserved_routes` and the routed
  section (2b).
- The file is written to a temporary name and renamed after the payload is
  complete, so a crash leaves no half checkpoint.

Size on Fabi386: the packed checkpoint is about 73 MB against a 25 MB
output JSON. The IdString table is 28 MB (the netlist names once more),
the orders about 30 MB, packing about 12 MB. The proposal's estimate of a
few hundred kilobytes assumed the netlist reload was order-preserving; it
is not, and the orders are the price of identity.

A checkpoint is not the plain output JSON of today. The `placement_reuse`
adapter (Stage 4E) keeps parsing plain output JSON for the name-and-signature
case and does not depend on this format.

## 4. Writing

`--checkpoint <file>` writes the checkpoint of the last completed phase at the
point the flow stops (`--pack-only`, `--no-place`, `--no-route`, or the end).
`write_json_file` takes an optional phase; with one it emits the modules as
today and then asks `BaseCtx::writeCheckpoint(stream, phase)` for the
object, which `Arch` implements in `mistral/checkpoint.cc`. Other backends
report no support and the option is refused. The writer interns nothing
(section 2.6), so the table it records is the table the run would have had
without the option. Nothing in the flow changes when the option is absent.

## 5. Restoring

`--resume <file>` replaces `--json`. The order, as implemented:

1. `parse_json` parses the file and hands the `nextpnr_checkpoint` object
   to `BaseCtx::checkpointPreload` before any import: validate the
   manifest against this build, this device and the restorable phases;
   replay the IdString table (prefix verified, remainder interned in
   order). The parsed object is kept for step 3.
2. Load the logical design through the existing frontend in a deferred
   mode: `import_toplevel_ports` runs, `attributesToArchInfo()` does not.
   This is one flag on `GenericFrontend`; the default path is unchanged.
   The import must intern nothing beyond the recorded table, or the
   checkpoint does not belong to this netlist and the restore stops.
3. `BaseCtx::checkpointRestore`: rebuild every recorded iteration order
   (cells, nets, settings, attrs, per-cell ports with missing ports
   recreated, per-cell attrs and params, per-net users and attrs); restore
   the top-level port table; restore packing (`cluster` and `constr_*` per
   cell, `constr_children` in recorded order, the non-default `pin_data`
   entries, `io_attr`); then `assignArchInfo()`, the same call `pack()`
   ends with, which rebuilds `combInfo`, `ffInfo` and the default pin maps.
4. Bind every recorded binding through `bindBel` (which rebuilds
   `unique_input_count`), restore `pllclk_sel_map`, and run the live
   legality check over every bound BEL: a checkpoint that certifies an
   illegal placement is refused, not repaired.
5. Restore `ctx->rngstate`. The flow then runs `ctx->check()` as it does
   after packing, and skips the completed phases: a `packed` resume runs
   place and route; a `placed` resume runs route (which begins with
   `lab_pre_route`). Route-prepared and routed resumes are unit 2b, which
   requires splitting `Arch::route()` into `prepare_route()` and the
   router call.

The file's `settings` are imported by the frontend as for any
nextpnr-written JSON and therefore win over conflicting command-line
options, including `threads`; validating and reporting such conflicts is
part of 2b-2 with the manifest lineage. A failed restore is an error that
names the offending section and object; it never leaves a partially
restored design in use.

## 6. Increments

| Unit | Deliverable | Gate |
| --- | --- | --- |
| 2a-1 | `packing` section written and restored; deferred frontend mode; resume from `packed` | Every clustered-cell record and pin-state entry equal after round trip (1,226 and 7,813 on Fabi386); resume-from-packed then place and route equals the clean run by the section 7 comparison |
| 2a-2 | `pllclk_sel` and placed resume | Resume-from-placed then route equals the clean run's routing and report |
| 2b-1 | `Arch::route()` split; LAB control state and reservations persisted; route-prepared resume | Resume-from-prepared then router equals the clean run |
| 2b-2 | routed resume and signoff-only flow; manifest lineage (`parent_sha256`) | Bitstream bits identical to the clean run |

## 7. Acceptance and comparison rules

The proposal argued that byte identity of routed JSON was the wrong gate,
because the writer numbers nets by `IdString` index and a restored process
interns strings in load order. Replaying the table (section 2.6, item 2)
removes that argument: a resumed run's output JSON is byte-identical to the
clean run's once the `creator` line is ignored, and that is the gate. The
name comparison (for every cell the same BEL and strength, for every net
the same routing) is kept as the diagnostic that names the first differing
object when the byte gate fails, and the report (`--report`) must be
byte-identical as well. The comparison script is
`build/stage5-validation/checkpoint/compare.py` (kept outside git with the
other validation drivers).

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

Costs: the writer and loader in `mistral/checkpoint.cc` (about 600 lines),
four small generic hooks, the frontend flag, the `Arch::route()` split
(2b), and the fixtures. The checkpoint of Fabi386 is about three times the
output JSON (section 3), almost all of it the IdString table and the
iteration orders that identity requires.
