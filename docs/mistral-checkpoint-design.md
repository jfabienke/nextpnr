# Mistral Checkpoint Design (Stage 5, units 2a/2b)

Status: designed 2026-09-16; all four phases implemented the same day in
`mistral/checkpoint.cc` (2a: packed, placed; 2b: route-prepared, routed),
with the generic hooks in `common/kernel/basectx.h`, `json/jsonwrite.cc`,
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

An eighth was found by the bitstream gate of 2b: routing preparation
leaves constant and unused LUT inputs with an *empty* bel-pin list, and
`compute_lut_mask` keys on that emptiness. The restore recorded those
entries (they are non-default) but applied them before `assignArchInfo()`,
whose default pass refilled every empty list, so 4,569 pins and every
affected LUT mask differed while the JSON, report, and checksum, none of
which see pin maps, were identical. Applying the recorded entries only
after the default pass is wrong too: `assign_ff_info` reads the `PIN_INV`
states to set the control-set inversions the bitstream programs, so the
states must be in place before it. Recorded entries are therefore applied
before `assignArchInfo()` and their bel-pin lists re-applied after it. The
order of restore steps is itself state, and only the bitstream sees these
two mistakes.

A ninth was found the same way, once the pin maps matched and the
bitstreams still differed: a per-write log of the bitstream generator in
both flows showed the resumed run taking the LUTRAM path for whole LABs.
That path keys on `combInfo.mlab_group`, and the route-through buffers
(`MISTRAL_BUF`) that `lab_pre_route` creates had never been through
`assign_comb_info` in the restored context: the clean flow calls it on
each buffer when it creates it, but `assignArchInfo()` only covered the
`is_comb_cell` types and MLABs, so a restored buffer kept a zero-filled
union in which the MLAB group reads as 0. `assignArchInfo()` now covers
buffers too, which also repairs the plain `--json` reload of a routed
nextpnr output, where the same buffers exist.

An eleventh, from the single-PLL fixture: placement ends by blocking the
PLL reference-clock spine (`WireInfo::BLOCKED`, `fixup_pllclk_placement`),
and the checkpoint recorded wire flags only for `RESERVED_ROUTE` wires and
only from route-prepared on. A placed or route-prepared resume therefore
let the router take the reference clock through the spine, which the raw
CRAM writes at bitstream time then clobber: the G2/G4 silicon failure the
flag exists to prevent, visible here as a different route and bitstream
with an identical report. Every non-zero flags word is now recorded at
every phase.

A tenth came from the fixtures beyond Fabi386: the frontend materialises
a net only when a cell connection or a port refers to it, so a net with
neither driver nor users never comes back on reload. Packing leaves such
nets behind (a PLL output it disconnected, the `$iobuf_i` halves of the
bidirectional IO buffers it removed, 32 of them on the SDRAM IO-register
design), and the order lists then name nets the design does not have.
The checkpoint records them with their attributes (`netlist.orphan_nets`)
and recreates them before the orders are restored.

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
  "netlist": { "top_ports": [ { "name": "clk", "net": "clk", "type": 0 } ], "stale_driver_ports": [["clk", "O"]],
               "orphan_nets": [ { "name": "c0", "attrs": [ { "name": "src", "value": { "str": "..." } } ] } ] },
  "packing": {
    "cluster_cells": [ { "cell": "...", "cluster": "<root>", "x": 0, "y": -1, "z": 6, "abs_z": true, "children": ["..."] } ],
    "pins": [ { "cell": "...", "ports": [ { "port": "...", "state": 3, "bel_pins": ["..."] } ] } ],
    "io_attr": [ { "port": "...", "attrs": [ { "name": "...", "value": { "str": "..." } | { "bits": "..." } } ] } ]
  },
  "physical": {
    "bindings": [ { "cell": "...", "bel": "<bel name>", "strength": 3 } ],
    "pllclk_sel": [ { "key": "<uint64 decimal>", "sel": 3 } ],
    "labs": [ [ [ [<clk_ena_idx 0>, <1>, <aclr_idx 0>, <1>, <l6_mode>, <carry_mode>], ... 10 ALMs ], [<aclr_used 0>, <1>] ], ... ],
    "wire_flags": [ ["<wire name>", "<flags decimal>"], ... ],
    "routes": [ ["<net>", [ ["<wire name>", "<pip name or empty>", <strength>], ... ] ], ... ]
  }
}
```

The manifest also carries lineage: `input` (`path`, `sha256` of the Yosys
JSON this design came from, propagated through every checkpoint) and
`parent` (`path`, `phase`, `sha256` of the checkpoint this run resumed
from). The hash is a self-contained SHA-256 in `checkpoint.cc`, checked
against the FIPS known answers by the unit test.

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
  `physical.wire_flags` (the complete `flags` word of every wire whose
  word is non-zero: `BLOCKED` from the device table and from the PLL
  reference-clock spine that placement reserves, `RESERVED_ROUTE` from
  routing preparation) is present at every phase. From `route-prepared`
  on, `physical.labs` (every LAB, every ALM: the control allocation and
  the LUT6/carry modes `reassign_alm_inputs` sets) and `physical.routes`
  (every net with bound wires, in its `wires` map order: the globals at
  route-prepared, everything at routed) are present too. Wires and pips are named; Mistral builds those
  names from tables interned at startup, so naming and parsing intern
  nothing, and the restore verifies that the table did not grow.
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
   cell, `constr_children` in recorded order, `io_attr`); then
   `assignArchInfo()`, the same call `pack()` ends with, which rebuilds
   `combInfo`, `ffInfo` and the default pin maps. The recorded non-default
   `pin_data` entries are applied before that call (their `PIN_INV` states
   feed `assign_ff_info`) and their bel-pin lists again after it, so that a
   recorded empty list stays empty. Settings the command line added that
   the checkpoint never had (such as `uncompressed_rbf`) are kept and
   iterate after the recorded ones; every other order list must match the
   design exactly.
4. Bind every recorded binding through `bindBel` (which rebuilds
   `unique_input_count`), restore `pllclk_sel_map`, and run the live
   legality check over every bound BEL: a checkpoint that certifies an
   illegal placement is refused, not repaired.
5. If `phase >= route-prepared`: restore the LAB control allocation and
   ALM modes, the reserved-wire flags, and every route (bound in reverse
   of the recorded order, so the `wires` map iterates as the writer's
   did). A routed restore also marks routing complete for the LAB state
   report.
6. Restore `ctx->rngstate`. The flow then runs `ctx->check()` as it does
   after packing, and skips the completed phases: a `packed` resume runs
   place and route; a `placed` resume runs route; a `route-prepared`
   resume runs the router only (`Arch::route()` is split into
   `prepare_route()`, which `lab_pre_route` and `route_globals` make up,
   and the router call, and skips the first half when the restored phase
   says it already ran); a `routed` resume runs nothing and goes straight
   to `--write`, `--report`, and the bitstream. `--route-prepare-only`
   stops a normal run after `prepare_route()` so that a route-prepared
   checkpoint can be written; `BaseCtx::checkpointPhaseAfterRoute()`
   tells the flow which phase a completed `route()` call represents.

The file's `settings` are imported by the frontend as for any
nextpnr-written JSON and therefore win over command-line options. The
proposal said conflicting options would be rejected; the implementation
cannot tell an option the user typed from a default the flow filled in,
and rejecting every difference would refuse any resume of a checkpoint
written with a non-default option. So: a differing `seed` is an error (the
manifest's RNG state is what runs, and a different seed would be silently
ignored), and every other difference is a warning that names the key and
both values. A failed restore is an error that names the offending section
and object; it never leaves a partially restored design in use.

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

## 9. Route reuse (Stage 5, 3c)

The parent design's Stage 3c asks for route reuse with region expansion,
gated on preserved routes remaining valid and a demonstrated fallback. The
checkpoint supplies the provenance it was blocked on: every route is named
by wire and pip, route-through cells and their rewired inputs are ordinary
named cells, and the reservations are recorded flags. `--reuse-routes
<file>` takes a routed checkpoint (its `physical.routes`) or any routed
nextpnr output (its `ROUTING` attributes) and runs after routing
preparation, before the router (`mistral/route_reuse.cc`).

Conservative by construction. A previous route is matched to a current net
by name and preserved only when every check passes under the current
design: the current source wire is the route's source; every current sink
wire is on the route and reaches the source through the route's own pips;
no leaf of the route is anything but a current sink (a stale branch would
drive a pin that no longer belongs to the net); every wire and pip resolves
on this device; every wire is free and every pip passes `checkPipAvail`
under the current reservations and blocked wires. Nets the global router
already bound are left to it. The check for pip availability is not
optional: router2 trusts pre-routed pips even where its own availability
test fails, on the assumption that whoever pre-routed them knew better.

Preserved routes are bound at `STRENGTH_STRONG`. Router2's setup registers
every bound wire in its congestion model and marks each complete
pre-routed arc as routed (`record_prerouted_net`), so its first iteration
starts from the preserved routes rather than from nothing. A pre-routed
arc is not exempt from rip-up: once one of its wires is overused,
`route_net` rips it up like any other arc, and the closing measurement
found about 18% of the applied routes on the controlled edits re-routed
by the end. The reuse report therefore counts applied routes, and
survival is a separate number. What the strength changed is empirical:
bound weak (the first attempt) the provenance experiment crawled to the
iteration cap with one overused wire that only the final bind resolved;
bound strong the same experiment fails as a real router failure and the
fallback takes over. Router2's final pass rebinds every wire up to
`STRENGTH_STRONG` at `STRENGTH_WEAK`, so the output carries the strengths
an uninterrupted run writes and an unchanged design stays byte-identical.
Region expansion is therefore not rip-up of preserved routes; it is the
dirty nets' freedom to route around them, and beyond that the fallback
below.

Bind order (3c-2). Router2's final pass, `bind_and_check_all`, used to rip
up and rebind one net at a time. With preserved routes that is wrong in a
way the clean flow never shows: a preserved arc that router2 re-routed in
its model keeps its strong context binding until its own turn in that
pass, so any earlier net whose new route crosses those wires fails the
bind (`checkWireAvail` false, wire bound to another net), is queued
again, and costs a whole iteration. On the INIT edit all 4,066 failures
were of this kind, between reused nets only. The pass now rips up every
net's weak and strong wires first and binds afterwards; the clean flow
has nothing bound before the first bind pass and is byte-identical
(Fabi386 and every checkpoint fixture), and the edits lose their extra
iterations. The refinement the first version of this section left open,
releasing only the preserved routes a failing net collides with, is
subsumed: nothing collides at bind time any more, and what remains is the
re-routing that negotiated congestion itself decides on.

Fallback. Router2 does not fail at its iteration cap: it gives up, and this
fork then runs router1 to legalise whatever is left, which with preserved
routes in the way is a different routing, not the uninterrupted one (the
provenance experiment: a seed-2 placement given the seed-1 routes kept 11
of them, router2 crawled to 100 iterations with one overused wire, and
router1 re-routed 2,000 arcs). Router2 therefore sets `router_gave_up` on
the context when it stops at the cap, and the reuse path treats that, like
a router exception, as failure: every preserved route is dropped, every
other wire bound below `STRENGTH_LOCKED` is unbound as well (router1 has
by then bound a legalised routing on every net; only the global router's
work stays), and the router runs again from the RNG state it started with,
so the fallback result is the uninterrupted run's routing rather than a
third trajectory. The first version of this fallback dropped only the
preserved routes and re-routed those eleven nets on top of router1's
result in under a second, which is a routing but not the clean one; the
tracker records both runs.
`MISTRAL_ROUTE_REUSE_FORCE_FALLBACK` exercises the path without a failure.

What is not reused: timing. Reused routes change no timing result; the
router's timing analysis runs over the whole design as before, and the
report is recomputed. Route reuse composes with placement reuse
(`--reuse-placement`): matched cells keep their BELs, unchanged nets between
them keep their routes, and the dirty set is what the edit touched plus
whatever the placer moved.

## 10. Reuse plan, placement region expansion, typed build states (3a, 3b)

The parent design's Stage 3 asked for three things beyond what Stage 4E and
unit 3c built: a reuse plan emitted before anything is applied, with a
reason per decision; region expansion when a local placement repair fails;
and a typed build state machine whose invalid transitions do not compile.
All three are in C++ (the Rust crates are concluded).

The plan (`mistral/reuse_plan.*`). Placement reuse and route reuse both
compute a `ReusePlan` first: one decision per cell (reuse, changed, added,
user-constrained, missing BEL, released) and per net (reuse, already
routed, added, endpoint mismatch, unresolved, unavailable), each with a
one-line reason such as `parameter LUT differs` or `sink x.A is not on the
route`. `--reuse-plan-out` writes it as JSON; `--reuse-dry-run` writes it
and applies nothing, which is how a plan is compared against a controlled
edit. Applying a plan validates each decision again against the live
design (a route is checked a second time before it is bound); the plan is
a record of what will be done and why, not an authority.

Region expansion (3b). Stage 4E transplants matched cells as hard BEL
constraints and lets the placer place the rest. If the placer fails, the
transplants within a growing Manhattan radius (2, then 5, then 12 tiles)
of the dirty cells are released, that is their BEL attributes cleared and
their plan decisions turned to `released`, everything the placer or the
constraint placer bound is unbound (the packer's locked pins stay), the
pre-placement RNG state is restored, and the placer runs again; the last
rung releases every transplant, which is the clean placement. Anchors are
the previous BELs of changed cells and, for added cells, the previous BELs
of the cells on their nets. The ladder is exercised with
`MISTRAL_PLACEMENT_REUSE_FORCE_FALLBACK=<n>`, which fails the first n
attempts; a real placer failure has not been provoked on Fabi386, whose
utilisation leaves the strict legaliser room to spread.

Typed build states (3a, `mistral/build_state.*`). `Build<Phase>` is a
move-only handle that exists only while the context is in that phase;
`place_build`, `prepare_build`, `route_build`, and `validate_build` consume
their input and return the next phase, so a transition that does not exist
does not compile and a caller cannot keep an obsolete handle. The legacy
entry points adopt the context into the phase they need, which checks at
run time what the types cannot see across that boundary: `Arch::place()`
adopts `Packed`, `Arch::route()` adopts `Placed` (or `RoutePrepared` after a
route-prepared restore), and the bitstream writer adopts `Routed` and runs
`validate_build`, which checks the context and that every arc of every
driven net reaches a sink over the net's own pips. A bitstream request on
a packed or placed design is therefore refused where it used to write a
meaningless file. `pack()` and the checkpoint restore set the phase; only
the checked loader constructs a restored one. The dirty state the parent
design describes is the reuse plan itself: the invalidation set of an edit
against a previous checkpoint, carried by the packed build into placement.
