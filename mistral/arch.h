/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  Lofty <dan.ravensloft@gmail.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifndef MISTRAL_ARCH_H
#define MISTRAL_ARCH_H

#include <atomic>
#include <set>
#include <sstream>

#include "base_arch.h"
#include "build_state.h"
#include "lab_dispatch.h"
#include "lab_profile.h"
#include "lab_reuse.h"
#include "lab_v2.h"
#include "nextpnr_types.h"
#include "placement_revision.h"
#include "relptr.h"

#include "cyclonev.h"
#include "monitor_abi.h"

NEXTPNR_NAMESPACE_BEGIN

enum class SwapSeamMode
{
    Off,    // annealer uses its live bind/check/revert path (default)
    Shadow, // detached assessment computed and compared; live path decides
    On      // detached assessment decides; bindings change only on accepted swaps
};

struct ArchArgs
{
    std::string device;
    bool verify_lab_controls = false;
    LabControlMode lab_controls = LabControlMode::Legacy; // the complete evaluator owns the control check
    std::string lab_control_profile_path;
    // The complete LAB evaluator defaults to the Rust authority where it is built (promoted
    // 2026-09-19) and to the legacy rules otherwise; main.cc's command-line default is the same.
#ifdef NO_RUST
    LabLegalityMode lab_legality = LabLegalityMode::Legacy;
#else
    LabLegalityMode lab_legality = LabLegalityMode::Rust;
#endif
    int placer_lookahead = 0;                   // Stage 4D: candidates speculated per HeAP cluster batch (0 = serial)
    LabReuseMode lab_reuse = LabReuseMode::Off; // Stage 4E: same-session LAB assessment reuse
    std::string reuse_placement_path;           // Stage 4E-2: previous output JSON to transplant BELs from
    std::string reuse_routes_path;     // Stage 5 (3c): previous routed output or checkpoint to reuse routes from
    std::string reuse_plan_path;       // Stage 5 (3a): write the reuse plan (cells and nets, with reasons) here
    bool reuse_dry_run = false;        // Stage 5 (3a): plan reuse but apply nothing
    float reuse_routes_history = 1.0f; // Stage 5 (3c-4): router2 history seeded on preserved wires; 1.0 = off
    int alm_pairing = 0;               // Stage 6 (6b): pair plain LUTs into ALMs before placement; 0 = off
    int spread_demand = 0;             // Stage 6 (6c): HeAP spreads comb cells by unique inputs; 0 = off
    bool spread_congestion = false;    // Stage 6 (6e): HeAP spreads by a wire-density estimate of the current placement
    int router2_reroute = 0; // Stage 6 (6f): router2 rips up and re-routes every arc every N iterations; 0 = off
    bool router2_reroute_contested = false; // Stage 6 (6f): that re-route queues only nets on wires with history
    bool router2_unit_cost = false;         // Stage 6 (6f): router2 costs every wire one unit instead of its delay
    bool register_packing = false;          // Stage 6 (6g): pack a register with the LUT that drives it into one ALM
    std::string telemetry_path;             // --telemetry: the run's counters and phase times, as JSON
    // Designs 14 and 16.1: the Rust session's batch forms in the Rust legality modes, the strict
    // legaliser's tile scan and a cluster candidate's edits as one call. Off: one question per bel,
    // and frozen whole-LAB records per cluster candidate, as before them. Results are identical.
    bool lab_tile_scan = true;
    float row_cost = 0.0f; // Stage 6 (6h): a vertical tile's placement cost in horizontal tiles; 0 = off
    SwapSeamMode sa_seam = SwapSeamMode::On; // Stage 5 (1c): annealer swap seam; on by default since 2026-09-19
    int sa_batch = 0;                        // Stage 5 (1c-B): candidates per refinement batch (0 = serial)
    bool route_prepare_only = false;         // Stage 5 (2b): stop route() after preparation, before the router
};

// These structures are used for fast ALM validity checking
struct ALMInfo
{
    // Wires, so bitstream gen can determine connectivity
    std::array<WireId, 2> comb_out;
    std::array<WireId, 2> sel_clk, sel_ena, sel_aclr, sel_ef;
    std::array<WireId, 4> ff_in, ff_out;
    // Pointers to bels
    std::array<BelId, 2> lut_bels;
    std::array<BelId, 4> ff_bels;

    bool l6_mode = false;
    bool carry_mode = false;

    // Which CLK/ENA and ACLR is chosen for each half
    std::array<int, 2> clk_ena_idx, aclr_idx;

    // For keeping track of how many inputs are currently being used, for the LAB routeability check
    int unique_input_count = 0;
};

struct LABInfo
{
    // LAB or MLAB?
    bool is_mlab;
    std::array<ALMInfo, 10> alms;
    //  Control set wires
    std::array<WireId, 3> clk_wires, ena_wires;
    std::array<WireId, 2> aclr_wires;
    WireId sclr_wire, sload_wire;
    // TODO: LAB configuration (control set etc)
    std::array<bool, 2> aclr_used;
};

// A tiny occupancy overlay: up to MAX BELs whose occupant differs from the
// live binding (nullptr = empty). Used to evaluate the live LAB rules for a
// proposed move without binding. Linear lookup; MAX is small on purpose.
struct BelOverlay
{
    static constexpr unsigned MAX = 4;
    std::array<BelId, MAX> bels{};
    std::array<const CellInfo *, MAX> cells{};
    unsigned count = 0;
    void add(BelId bel, const CellInfo *cell)
    {
        NPNR_ASSERT(count < MAX);
        bels[count] = bel;
        cells[count] = cell;
        ++count;
    }
    const CellInfo *lookup(BelId bel, const CellInfo *live) const
    {
        for (unsigned i = 0; i < count; ++i)
            if (bels[i] == bel)
                return cells[i];
        return live;
    }
    bool touches_alm(const ALMInfo &alm) const
    {
        for (unsigned i = 0; i < count; ++i)
            for (const BelId &b : alm.lut_bels)
                if (b == bels[i])
                    return true;
        for (unsigned i = 0; i < count; ++i)
            for (const BelId &b : alm.ff_bels)
                if (b == bels[i])
                    return true;
        return false;
    }
};

struct PinInfo
{
    WireId wire;
    PortType dir;
};

struct BelInfo
{
    IdString name;
    IdString type;
    IdString bucket;

    CellInfo *bound = nullptr;

    // For cases where we need to determine an original block index, due to multiple bels at the same tile this
    // might not be the same as the nextpnr z-coordinate
    int block_index;
    dict<IdString, PinInfo> pins;
    // Info for different kinds of bels
    union
    {
        // This enables fast lookup of the associated ALM, etc
        struct
        {
            uint32_t lab; // index into the list of LABs
            uint8_t alm;  // ALM index inside LAB
            uint8_t idx;  // LUT or FF index inside ALM
        } lab_data;
    };
};

// We maintain our own wire data based on mistral's. This gets us the bidirectional linking that nextpnr needs,
// and also makes it easy to add wires and pips for our own purposes like LAB internal routing, global clock
// sources, etc.
struct WireInfo
{
    // name_override is only used for nextpnr-created wires
    // otherwise; this is empty and a name is created according to mistral rules
    IdString name_override;

    // these are transformed on-the-fly to PipId by the iterator, to save space (WireId is half the size of PipId)
    std::vector<WireId> wires_downhill;
    std::vector<WireId> wires_uphill;

    std::vector<BelPin> bel_pins;

    // Flags of a wire, kept in its WireRoute (Arch::wire_flags). If the RESERVED_ROUTE mask is set,
    // then only wires_uphill[flags&0xFF] may drive this wire - used for control set preallocations
    static const uint64_t RESERVED_ROUTE = 0x100;
    // No pip may target this wire at all. Used for the PLL reference-clock spine: those routing
    // muxes are programmed by raw CRAM writes at bitstream time, so any fabric net routed through
    // them is silently clobbered -- measured as a wrong answer on silicon (MISTRAL_GAPS G2/G4).
    static const uint64_t BLOCKED = 0x200;
};

// The routing state of a wire, 16 bytes in an array by wire slot (design sections 16.5 and 17.2):
// the router asks about a pip for every wire it visits, and the answer needs only this. A wire is
// bound to at most one net, through at most one pip, whose destination is the wire: that pip is
// recorded by its source, and a wire bound directly has none. The flags are WireInfo::BLOCKED and
// WireInfo::RESERVED_ROUTE with the reserved uphill index in the low byte.
struct WireRoute
{
    NetInfo *bound_net = nullptr;
    CycloneV::rnode_t bound_src = invalid_rnode;
    uint32_t flags = 0;
};

// This transforms a WireIds, and adds the mising half of the pair to create a PipId
using WireVecIterator = std::vector<WireId>::const_iterator;
struct UpDownhillPipIterator
{
    WireVecIterator base;
    WireId other_wire;
    bool is_uphill;

    UpDownhillPipIterator(WireVecIterator base, WireId other_wire, bool is_uphill)
            : base(base), other_wire(other_wire), is_uphill(is_uphill) {};

    bool operator!=(const UpDownhillPipIterator &other) { return base != other.base; }
    UpDownhillPipIterator operator++()
    {
        ++base;
        return *this;
    }
    UpDownhillPipIterator operator++(int)
    {
        UpDownhillPipIterator prior(*this);
        ++(*this);
        return prior;
    }
    PipId operator*() { return is_uphill ? PipId(base->node, other_wire.node) : PipId(other_wire.node, base->node); }
};

struct UpDownhillPipRange
{
    UpDownhillPipIterator b, e;

    UpDownhillPipRange(const std::vector<WireId> &v, WireId other_wire, bool is_uphill)
            : b(v.begin(), other_wire, is_uphill), e(v.end(), other_wire, is_uphill) {};

    UpDownhillPipIterator begin() const { return b; }
    UpDownhillPipIterator end() const { return e; }
};

// This iterates over the list of wires, and for each wire yields its uphill pips, as an efficient way of going over
// all the pips in the device
using WireMapIterator = dict<WireId, WireInfo>::const_iterator;
struct AllPipIterator
{
    WireMapIterator base, end;
    int uphill_idx;

    AllPipIterator(WireMapIterator base, WireMapIterator end, int uphill_idx)
            : base(base), end(end), uphill_idx(uphill_idx) {};

    bool operator!=(const AllPipIterator &other) { return base != other.base || uphill_idx != other.uphill_idx; }
    AllPipIterator operator++()
    {
        // Increment uphill list index by one
        ++uphill_idx;
        // We've reached the end of the current wire. Keep incrementing the wire of interest until we find one with
        // uphill pips, or we reach the end of the list of wires
        while (base != end && uphill_idx >= int(base->second.wires_uphill.size())) {
            uphill_idx = 0;
            ++base;
        }
        return *this;
    }
    AllPipIterator operator++(int)
    {
        AllPipIterator prior(*this);
        ++(*this);
        return prior;
    }
    PipId operator*() { return PipId(base->second.wires_uphill.at(uphill_idx).node, base->first.node); }
};

struct AllPipRange
{
    AllPipIterator b, e;

    AllPipRange(const dict<WireId, WireInfo> &wires) : b(wires.begin(), wires.end(), -1), e(wires.end(), wires.end(), 0)
    {
        // Starting the begin iterator at index -1 and incrementing it ensures we skip over the first wire if it has no
        // uphill pips
        ++b;
    };

    AllPipIterator begin() const { return b; }
    AllPipIterator end() const { return e; }
};

// This transforms a map to a range of keys, used as the wire iterator
template <typename T> struct key_range
{
    key_range(const T &t) : b(t.begin()), e(t.end()) {};
    typename T::const_iterator b, e;

    struct xformed_iterator : public T::const_iterator
    {
        explicit xformed_iterator(typename T::const_iterator base) : T::const_iterator(base) {};
        typename T::key_type operator*() { return this->T::const_iterator::operator*().first; }
    };

    xformed_iterator begin() const { return xformed_iterator(b); }
    xformed_iterator end() const { return xformed_iterator(e); }
};

using AllWireRange = key_range<dict<WireId, WireInfo>>;

struct ArchRanges : BaseArchRanges
{
    using ArchArgsT = ArchArgs;
    // Bels
    using AllBelsRangeT = const std::vector<BelId> &;
    using TileBelsRangeT = std::vector<BelId>;
    using BelPinsRangeT = std::vector<IdString>;
    using CellBelPinRangeT = const std::vector<IdString> &;
    // Wires
    using AllWiresRangeT = AllWireRange;
    using DownhillPipRangeT = UpDownhillPipRange;
    using UphillPipRangeT = UpDownhillPipRange;
    using WireBelPinRangeT = const std::vector<BelPin> &;
    // Pips
    using AllPipsRangeT = AllPipRange;
};

// This enum captures different 'styles' of cell pins
// This is a combination of the modes available for a pin (tied high, low or inverted)
// and the default value to set it to not connected
enum CellPinStyle
{
    PINOPT_NONE = 0x0, // no options, just signal as-is
    PINOPT_LO = 0x1,   // can be tied low
    PINOPT_HI = 0x2,   // can be tied high
    PINOPT_INV = 0x4,  // can be inverted

    PINOPT_LOHI = 0x3,    // can be tied low or high
    PINOPT_LOHIINV = 0x7, // can be tied low or high; or inverted

    PINOPT_MASK = 0x7,

    PINDEF_NONE = 0x00, // leave disconnected
    PINDEF_0 = 0x10,    // connect to 0 if not used
    PINDEF_1 = 0x20,    // connect to 1 if not used

    PINDEF_MASK = 0x30,

    PINGLB_CLK = 0x100, // pin is a 'clock' for global purposes

    PINGLB_MASK = 0x100,

    PINSTYLE_NONE = 0x000, // default

    PINSTYLE_COMB = 0x017, // combinational signal, defaults low, can be inverted and tied
    PINSTYLE_CLK = 0x107,  // CLK type signal, invertible and defaults to disconnected

    PINSTYLE_CE = 0x027,   // CE type signal, invertible and defaults to enabled
    PINSTYLE_RST = 0x017,  // RST type signal, invertible and defaults to not reset
    PINSTYLE_DEDI = 0x000, // dedicated signals, leave alone
    PINSTYLE_INP = 0x001,  // general inputs, no inversion/tieing but defaults low
    PINSTYLE_PU = 0x022,   // signals that float high and default high

    PINSTYLE_CARRY = 0x001, // carry chains can be floating or 0?

};

struct Arch : BaseArch<ArchRanges>
{
    ArchArgs args;
    mistral::CycloneV *cyclonev;

    // Mistral needs the bitstream configuring before it can use the simulator.
    bool bitstream_configured = false;

    Arch(ArchArgs args);
    ArchArgs archArgs() const override { return args; }
    void notifyCellMutation(CellInfo *cell, ContextMutationKind kind) override
    {
        note_lab_cell_mutation(cell);
        note_placement_mutation(kind);
    }
    void notifyNetMutation(NetInfo *net, ContextMutationKind kind) override
    {
        note_lab_net_mutation(net);
        note_placement_mutation(kind);
    }
    // Stage 5 units 2a/2b: versioned checkpoints (mistral/checkpoint.cc).
    bool writeCheckpoint(std::ostream &out, const std::string &phase) const override;
    bool checkpointPreload(const std::string &checkpoint_json) override;
    bool checkpointRestore() override;
    std::string checkpointPhase() const override;
    std::string checkpointPhaseAfterRoute() const override;
    std::shared_ptr<struct MistralCheckpoint> pending_checkpoint;
    std::string checkpoint_phase_;
    std::string restored_input_json_;              // the resumed checkpoint's manifest "input", propagated as lineage
    std::shared_ptr<struct ReusePlan> reuse_plan_; // Stage 5 (3a): the plan of this run's placement and route reuse

    void notifyContextMutation(ContextMutationKind kind) override
    {
        // A newly created cell or net is nobody's dependency until it is bound
        // or connected, and both of those paths invalidate precisely. Every
        // other object-less mutation (constraints today) invalidates every LAB.
        if (kind != ContextMutationKind::GeneratedObjects)
            note_lab_facts_mutation();
        note_placement_mutation(kind);
    }
    void note_placement_mutation(ContextMutationKind kind)
    {
        switch (kind) {
        case ContextMutationKind::Connectivity:
            placement_revision.note_mutation(PlacementMutation::Connectivity);
            break;
        case ContextMutationKind::CellFacts:
            placement_revision.note_mutation(PlacementMutation::CellFacts);
            break;
        case ContextMutationKind::NetFacts:
            placement_revision.note_mutation(PlacementMutation::NetFacts);
            break;
        case ContextMutationKind::Constraints:
            placement_revision.note_mutation(PlacementMutation::Constraints);
            break;
        case ContextMutationKind::GeneratedObjects:
            placement_revision.note_mutation(PlacementMutation::GeneratedObjects);
            break;
        }
    }

    std::string getChipName() const override { return args.device; }
    // -------------------------------------------------

    int getGridDimX() const override { return cyclonev->get_tile_sx(); }
    int getGridDimY() const override { return cyclonev->get_tile_sy(); }
    int getTileBelDimZ(int x, int y) const override; // arch.cc
    char getNameDelimiter() const override { return '.'; }

    // -------------------------------------------------

    BelId getBelByName(IdStringList name) const override; // arch.cc
    IdStringList getBelName(BelId bel) const override;    // arch.cc
    const std::vector<BelId> &getBels() const override { return all_bels; }
    std::vector<BelId> getBelsByTile(int x, int y) const override;
    Loc getBelLocation(BelId bel) const override
    {
        return Loc(CycloneV::pos2x(bel.pos), CycloneV::pos2y(bel.pos), bel.z);
    }
    BelId getBelByLocation(Loc loc) const override
    {
        if (loc.x < 0 || loc.x >= cyclonev->get_tile_sx())
            return BelId();
        if (loc.y < 0 || loc.y >= cyclonev->get_tile_sy())
            return BelId();
        auto &bels = bels_by_tile.at(pos2idx(loc.x, loc.y));
        if (loc.z < 0 || loc.z >= int(bels.size()))
            return BelId();
        return BelId(CycloneV::xy2pos(loc.x, loc.y), loc.z);
    }
    IdString getBelType(BelId bel) const override; // arch.cc
    WireId getBelPinWire(BelId bel, IdString pin) const override
    {
        auto &pins = bel_data(bel).pins;
        auto found = pins.find(pin);
        if (found == pins.end())
            return WireId();
        else
            return found->second.wire;
    }
    PortType getBelPinType(BelId bel, IdString pin) const override { return bel_data(bel).pins.at(pin).dir; }
    std::vector<IdString> getBelPins(BelId bel) const override;

    bool isBelLocationValid(BelId bel, bool explain_invalid = false) const override;
    // CLKBUF and PLLCLK bels alias the same physical CMUX*G CLKOUT; at most one may be used.
    bool global_sibling_occupied(BelId bel, IdString other_type) const;
    // static sibling relation, built lazily -- the naive per-query tile scan allocated on
    // every placer validity check and dominated placement at ~24k cells (profiled ~60%)
    mutable dict<BelId, std::vector<BelId>> clk_sibling_cache;
    mutable bool clk_sibling_cache_built = false;

    void bindBel(BelId bel, CellInfo *cell, PlaceStrength strength) override
    {
        auto &data = bel_data(bel);
        NPNR_ASSERT(data.bound == nullptr);
        data.bound = cell;
        cell->bel = bel;
        cell->belStrength = strength;
        // getenv() is a locked linear environ scan; in bindBel it sat on the HeAP legaliser's
        // innermost path and turned a 39k-ALUT placement into hours (found by sampling: half of
        // all cycles in __findenv_locked). Cache once; cheap type check first.
        static const bool vup_trace_pll = getenv("VUP_TRACE_PLL") != nullptr;
        if (cell->type == id_altera_pll && vup_trace_pll)
            fprintf(stderr, "  [trace] bind   '%s' -> FPLL(%d,%d) strength=%d\n", cell->name.c_str(this),
                    CycloneV::pos2x(CycloneV::pos_t(bel.pos)), CycloneV::pos2y(CycloneV::pos_t(bel.pos)),
                    int(strength));
        update_bel(bel);
        note_lab_binding(data);
        placement_revision.note_mutation(PlacementMutation::BelBinding);
    }
    void unbindBel(BelId bel) override
    {
        auto &data = bel_data(bel);
        NPNR_ASSERT(data.bound != nullptr);
        static const bool vup_trace_pll = getenv("VUP_TRACE_PLL") != nullptr;
        if (data.bound->type == id_altera_pll && vup_trace_pll)
            fprintf(stderr, "  [trace] unbind '%s' from FPLL(%d,%d)\n", data.bound->name.c_str(this),
                    CycloneV::pos2x(CycloneV::pos_t(bel.pos)), CycloneV::pos2y(CycloneV::pos_t(bel.pos)));
        data.bound->bel = BelId();
        data.bound->belStrength = STRENGTH_NONE;
        data.bound = nullptr;
        update_bel(bel);
        note_lab_binding(data);
        placement_revision.note_mutation(PlacementMutation::BelBinding);
    }
    bool checkBelAvail(BelId bel) const override { return bel_data(bel).bound == nullptr; }
    CellInfo *getBoundBelCell(BelId bel) const override { return bel_data(bel).bound; }
    CellInfo *getConflictingBelCell(BelId bel) const override { return bel_data(bel).bound; }

    void update_bel(BelId bel);
    BelId bel_by_block_idx(int x, int y, IdString type, int block_index) const;

    // -------------------------------------------------

    WireId getWireByName(IdStringList name) const override;
    IdStringList getWireName(WireId wire) const override;
    DelayQuad getWireDelay(WireId wire) const override { return DelayQuad(0); }
    const std::vector<BelPin> &getWireBelPins(WireId wire) const override { return wire_info(wire).bel_pins; }
    AllWireRange getWires() const override { return AllWireRange(wires); }
    // The binding API over the wires' routing state (design sections 16.5 and 17.2), each function
    // the base arch's line for line with its two maps replaced by WireRoute::bound_net and bound_src.
    // The base maps are not written.
    void bindWire(WireId wire, NetInfo *net, PlaceStrength strength) override
    {
        NPNR_ASSERT(wire != WireId());
        WireRoute &data = wire_route(wire);
        NPNR_ASSERT(data.bound_net == nullptr);
        net->wires[wire].pip = PipId();
        net->wires[wire].strength = strength;
        data.bound_net = net;
        data.bound_src = invalid_rnode;
        refreshUiWire(wire);
        ++lab_routing_epoch;
        placement_revision.note_mutation(PlacementMutation::Routing);
    }
    void unbindWire(WireId wire) override
    {
        NPNR_ASSERT(wire != WireId());
        WireRoute &data = wire_route(wire);
        NPNR_ASSERT(data.bound_net != nullptr);
        auto &net_wires = data.bound_net->wires;
        auto it = net_wires.find(wire);
        NPNR_ASSERT(it != net_wires.end());
        net_wires.erase(it);
        data.bound_net = nullptr;
        data.bound_src = invalid_rnode; // a pip that drove the wire is unbound with it
        refreshUiWire(wire);
        ++lab_routing_epoch;
        placement_revision.note_mutation(PlacementMutation::Routing);
    }
    void bindPip(PipId pip, NetInfo *net, PlaceStrength strength) override
    {
        NPNR_ASSERT(pip != PipId());
        const WireId dst(pip.dst);
        WireRoute &data = wire_route(dst);
        NPNR_ASSERT(data.bound_net == nullptr); // neither this pip nor another drives the wire
        data.bound_net = net;
        data.bound_src = pip.src;
        net->wires[dst].pip = pip;
        net->wires[dst].strength = strength;
        ++lab_routing_epoch;
        placement_revision.note_mutation(PlacementMutation::Routing);
    }
    void unbindPip(PipId pip) override
    {
        ++lab_routing_epoch;
        NPNR_ASSERT(pip != PipId());
        const WireId dst(pip.dst);
        WireRoute &data = wire_route(dst);
        NPNR_ASSERT(data.bound_net != nullptr && data.bound_src == pip.src);
        data.bound_net->wires.erase(dst);
        data.bound_net = nullptr;
        data.bound_src = invalid_rnode;
        placement_revision.note_mutation(PlacementMutation::Routing);
    }
    bool checkWireAvail(WireId wire) const override { return getBoundWireNet(wire) == nullptr; }
    NetInfo *getBoundWireNet(WireId wire) const override
    {
        const WireRoute *route = find_wire_route(wire);
        return route == nullptr ? nullptr : route->bound_net;
    }
    NetInfo *getConflictingWireNet(WireId wire) const override { return getBoundWireNet(wire); }
    NetInfo *getBoundPipNet(PipId pip) const override
    {
        const WireRoute *route = find_wire_route(WireId(pip.dst));
        return route == nullptr ? nullptr : pip_net(pip, *route);
    }
    NetInfo *getConflictingPipNet(PipId pip) const override { return getBoundPipNet(pip); }
    // The net bound through `pip`, given its destination wire's routing state: the wire's net when
    // this pip is the one that drives it.
    static NetInfo *pip_net(PipId pip, const WireRoute &dst_data)
    {
        return dst_data.bound_src == pip.src ? dst_data.bound_net : nullptr;
    }

    bool wires_connected(WireId src, WireId dst) const;
    // Only allow src, and not any other wire, to drive dst
    void reserve_route(WireId src, WireId dst);
    void create_dsp(int x, int y); // dsp.cc (G7)
    void block_wire(WireId w);     // no pip may use this wire (see WireInfo::BLOCKED)

    // -------------------------------------------------

    PipId getPipByName(IdStringList name) const override;
    AllPipRange getPips() const override { return AllPipRange(wires); }
    Loc getPipLocation(PipId pip) const override { return Loc(CycloneV::rn2x(pip.dst), CycloneV::rn2y(pip.dst), 0); }
    IdStringList getPipName(PipId pip) const override;
    WireId getPipSrcWire(PipId pip) const override { return WireId(pip.src); };
    WireId getPipDstWire(PipId pip) const override { return WireId(pip.dst); };
    UpDownhillPipRange getPipsDownhill(WireId wire) const override
    {
        return UpDownhillPipRange(wire_info(wire).wires_downhill, wire, false);
    }
    UpDownhillPipRange getPipsUphill(WireId wire) const override
    {
        return UpDownhillPipRange(wire_info(wire).wires_uphill, wire, true);
    }

    bool is_pip_blocked(PipId pip) const { return is_pip_blocked(pip, wire_route(WireId(pip.dst))); }
    // The same with the destination wire's routing state in hand, for a caller that needs it anyway.
    bool is_pip_blocked(PipId pip, const WireRoute &dst_data) const
    {
        if ((dst_data.flags & WireInfo::BLOCKED) != 0)
            return true;
        {
            const WireRoute *src_data = find_wire_route(WireId(pip.src));
            if (src_data != nullptr && (src_data->flags & WireInfo::BLOCKED) != 0)
                return true;
        }
        if ((dst_data.flags & WireInfo::RESERVED_ROUTE) != 0) {
            if (WireId(pip.src) != wire_info(WireId(pip.dst)).wires_uphill.at(dst_data.flags & 0xFF))
                return true;
        }
        return false;
    }

    // One lookup of the destination wire answers both halves: reserved and blocked routes, and
    // whether the pip is bound (design section 16.5).
    bool checkPipAvail(PipId pip) const override
    {
        const WireRoute &dst_data = wire_route(WireId(pip.dst));
        return !is_pip_blocked(pip, dst_data) && pip_net(pip, dst_data) == nullptr;
    }

    bool checkPipAvailForNet(PipId pip, const NetInfo *net) const override
    {
        const WireRoute &dst_data = wire_route(WireId(pip.dst));
        if (is_pip_blocked(pip, dst_data))
            return false;
        const NetInfo *bound = pip_net(pip, dst_data);
        return bound == nullptr || bound == net;
    }

    // -------------------------------------------------

    delay_t estimateDelay(WireId src, WireId dst) const override;
    delay_t predictDelay(BelId src_bel, IdString src_pin, BelId dst_bel, IdString dst_pin) const override;
    delay_t getDelayEpsilon() const override { return 10; };
    delay_t getRipupDelayPenalty() const override { return 100; };
    float getDelayNS(delay_t v) const override { return float(v) / 1000.0f; };
    delay_t getDelayFromNS(float ns) const override { return delay_t(ns * 1000.0f); };
    uint32_t getDelayChecksum(delay_t v) const override { return v; };

    BoundingBox getRouteBoundingBox(WireId src, WireId dst) const override;

    TimingPortClass getPortTimingClass(const CellInfo *cell, IdString port,
                                       int &clockInfoCount) const override;                                // delay.cc
    TimingClockingInfo getPortClockingInfo(const CellInfo *cell, IdString port, int index) const override; // delay.cc
    bool getCellDelay(const CellInfo *cell, IdString fromPort, IdString toPort,
                      DelayQuad &delay) const override;                                                      // delay.cc
    DelayQuad getPipDelay(PipId pip) const override;                                                         // delay.cc
    bool getArcDelayOverride(const NetInfo *net_info, const PortRef &sink, DelayQuad &delay) const override; // delay.cc

    // -------------------------------------------------

    const std::vector<IdString> &getBelPinsForCellPin(const CellInfo *cell_info, IdString pin) const override
    {
        return cell_info->pin_data.at(pin).bel_pins;
    }

    bool isValidBelForCellType(IdString cell_type, BelId bel) const override;
    BelBucketId getBelBucketForCellType(IdString cell_type) const override;
    BelBucketId getBelBucketForBel(BelId bel) const override;

    // -------------------------------------------------
    // Expanding bounding box seems to make thing worse for CycloneV
    // as it slows down the resolution of TD congestion, disabling it
    void expandBoundingBox(BoundingBox &bb) const override {};
    // -------------------------------------------------

    void assignArchInfo() override;
    bool pack() override;
    bool place() override;
    bool route() override;
    // Stage 5 (2b): the half of route() before the router (LAB preparation, globals).
    void prepare_route();
    // Stage 5 (3a): the phase bodies the typed transitions run (build_state.h);
    // place() and route() adopt the context and go through them.
    bool run_placement();
    void report_legalisation_stall(const std::vector<CellInfo *> &stuck) const; // Stage 6 (6a)
    // Stage 6 (6e): per-tile inflation from a bounding-box wire-density estimate (RUDY) of the
    // current placement, rebuilt at the start of every spreading pass.
    std::vector<float> spread_inflation;
    int spread_inflation_w = 0, spread_inflation_h = 0;
    void rebuild_spread_inflation(const std::function<Loc(const CellInfo *)> &loc_of);
    float spread_inflation_at(int x, int y) const;
    // Stage 6 (6b): an ALM pair cluster lands on the two LUT halves of the ALM its root bel is in.
    bool getClusterPlacement(ClusterId cluster, BelId root_bel,
                             std::vector<std::pair<CellInfo *, BelId>> &placement) const override;
    bool run_router_phase();
    BuildPhase build_phase = BuildPhase::Loaded;

    // -------------------------------------------------
    // Functions for device setup

    BelId add_bel(int x, int y, IdString name, IdString type);
    WireId add_wire(int x, int y, IdString name, uint64_t flags = 0);
    PipId add_pip(WireId src, WireId dst);

    void add_bel_pin(BelId bel, IdString pin, PortType dir, WireId wire);

    CycloneV::rnode_t find_rnode(CycloneV::block_type_t bt, int x, int y, CycloneV::port_type_t port, int bi = -1,
                                 int pi = -1) const;
    WireId get_port(CycloneV::block_type_t bt, int x, int y, int bi, CycloneV::port_type_t port, int pi = -1) const;
    bool has_port(CycloneV::block_type_t bt, int x, int y, int bi, CycloneV::port_type_t port, int pi = -1) const;

    void create_lab(int x, int y, bool is_mlab);       // lab.cc
    void create_m10k(int x, int y);                    // m10k.cc
    void create_gpio(int x, int y);                    // io.cc
    void create_clkbuf(int x, int y);                  // globals.cc
    void create_control(int x, int y);                 // globals.cc
    void create_hps_mpu_general_purpose(int x, int y); // globals.cc
    void create_hps_lwh2f(int x, int y);               // globals.cc (G3)
    void create_hps_f2sdram(int x, int y);             // globals.cc (G3)
    void add_hps_pin(BelId bel, IdString pin, CycloneV::block_type_t bt, int x, int y, CycloneV::port_type_t pt, int bi,
                     int pi);       // globals.cc (G3, auto-direction)
    void create_fpll(int x, int y); // pll.cc

    // G4: PLL outclk -> global clock network (see PLL_OUTCLK_DESIGN.md). The map is composed at
    // init from libmistral's p2p tables (FPLL PLLCOUT[c] -> CMUX* PLLIN[k], dedicated wiring) and
    // the compiled cmux link tables (PLLIN line k -> INPUT_SEL entry e per gclk instance):
    //   key(fpll_pos, counter, cmux_pos, gclk_instance) -> the INPUT_SEL value selecting that
    //   PLL counter at that gclk instance.
    void create_pllclk(int x, int y, bool vertical);                                      // pll.cc
    void build_pllclk_map();                                                              // pll.cc
    int pllclk_lookup(uint32_t fpll_pos, int counter, uint32_t cmux_pos, int inst) const; // -1 if absent
    bool pllclk_pos_is_vertical(uint32_t cmux_pos) const;                                 // CMUXVG vs CMUXHG position
    // Deterministic co-assignment for pack: choose an FPLL position plus, per logical output clock,
    // a (physical counter, cmux pos, gclk instance, INPUT_SEL) tuple. Physical-counter freedom is
    // required: only C4..C8 have dedicated wiring to the global cmuxes (p2p-verified).
    struct PllClkChoice
    {
        uint32_t cmux_pos;
        int inst;
        int phys_counter;
        int sel;
    };
    // A design may instantiate several PLLs (a video core wants a pixel clock and a memory clock),
    // so the caller threads through what earlier PLLs already claimed: without it every PLL picks
    // the same FPLL position and the second bindBel trips an assertion. pll.cc
    bool pllclk_choose(int nclk, uint32_t &fpll_pos, std::vector<PllClkChoice> &out,
                       const std::set<uint32_t> &taken_fpll = {},
                       const std::set<std::pair<uint32_t, int>> &taken_gclk = {}) const;
    void fixup_pllclk_placement(); // pll.cc
    std::map<uint64_t, uint8_t> pllclk_sel_map;
    static uint64_t pllclk_key(uint32_t fpll_pos, int counter, uint32_t cmux_pos, int inst)
    {
        return (uint64_t(fpll_pos) << 40) | (uint64_t(counter & 0xff) << 32) | (uint64_t(cmux_pos) << 8) |
               uint64_t(inst & 0xff);
    }

    // -------------------------------------------------

    bool is_comb_cell(IdString cell_type) const;        // lab.cc
    bool is_alm_legal(uint32_t lab, uint8_t alm) const; // lab.cc
    bool is_lab_ctrlset_legal(uint32_t lab) const;      // lab.cc
    // Overlay forms of the live rules (lab.cc): the same checks with up to
    // BelOverlay::MAX occupants replaced, no binding, no serialisation.
    bool is_alm_legal_overlay(uint32_t lab, uint8_t alm, const BelOverlay &overlay) const;
    int alm_input_count_overlay(uint32_t lab, uint8_t alm, const BelOverlay &overlay) const;
    bool check_lab_input_count_overlay(uint32_t lab, const BelOverlay &overlay) const;
    bool is_lab_ctrlset_legal_overlay(uint32_t lab, const BelOverlay &overlay) const;
    bool check_mlab_groups_overlay(uint32_t lab, const BelOverlay &overlay) const;
    bool overlay_bels_legal(const BelOverlay &overlay) const;
    template <typename Bound> bool alm_legal_with(const ALMInfo &alm, Bound bound) const;
    template <typename Bound> int alm_input_count_with(const ALMInfo &alm, Bound bound) const;
    template <typename Bound> bool mlab_groups_with(uint32_t lab, Bound bound) const;
    mutable LabControlStats lab_control_stats;
    mutable LabControlProfile lab_control_profile;
    mutable LabLegalityStats lab_legality_stats;
    // Stage 6 (parity): the resident LAB snapshot session of the Rust legality modes, created on
    // the first non-legacy dispatch, and per LAB the bels changed since it last saw them. Empty until a session exists,
    // so the hooks below cost one test on the legacy path.
    mutable std::shared_ptr<struct ResidentLabLegality> lab_resident;
    mutable std::vector<uint64_t> lab_bel_dirty;   // per LAB, a bit per bel (alm * 6 + slot); bit 60: reset
    mutable std::vector<uint64_t> lab_bel_refacts; // as above, for a facts change of a bound cell
    // For `--telemetry`: wall time of the placement and routing phases, and the design checksum
    // taken where the placer and router log theirs (before the arch attributes are written).
    mutable double telemetry_placement_seconds = 0.0, telemetry_routing_seconds = 0.0;
    mutable std::atomic<uint32_t> telemetry_checksum{0};
    // `--monitor`: the live dashboard session, started by the command handler after the netlist
    // is loaded; null otherwise. The phase hooks cost one test without it.
    mutable std::shared_ptr<struct MonitorSession> monitor;
    void monitor_phase(uint32_t phase) const;
    static uint64_t lab_bel_bit(const BelInfo &data)
    {
        return uint64_t(1) << (data.lab_data.alm * 6 + (data.type == id_MISTRAL_FF ? 2 : 0) + data.lab_data.idx);
    }
    mutable PlacementRevisionState placement_revision;

    // Stage 4E-1 reuse state (lab_reuse.cc). Active only inside place().
    mutable LabReuseMode lab_reuse_effective = LabReuseMode::Off;
    mutable bool lab_reuse_active = false;
    mutable std::vector<uint64_t> lab_versions;
    mutable uint64_t lab_facts_epoch = 1;
    mutable std::vector<LabAssessmentEntry> lab_assessments;
    mutable LabReuseStats lab_reuse_stats;
    void note_lab_binding(const BelInfo &data) const
    {
        if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF) && !lab_bel_dirty.empty())
            lab_bel_dirty[data.lab_data.lab] |= lab_bel_bit(data);
        if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF) && data.lab_data.lab < lab_versions.size()) {
            ++lab_versions[data.lab_data.lab];
            if (lab_reuse_active)
                ++lab_reuse_stats.lab_invalidations;
        }
    }
    void note_lab_facts_mutation() const
    {
        ++lab_facts_epoch;
        if (lab_reuse_active)
            ++lab_reuse_stats.facts_invalidations;
    }
    // Precise reverse incidence: only LABs that read the mutated object are
    // invalidated. LAB queries read only bound cells, so an unbound cell's
    // facts are nobody's dependency until it is bound (which bumps its LAB).
    void note_lab_cell_mutation(const CellInfo *cell) const
    {
        if (cell == nullptr || cell->bel == BelId()) {
            if (lab_reuse_active)
                ++lab_reuse_stats.unbound_cell_mutations;
            return;
        }
        const auto &data = bel_data(cell->bel);
        if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF) && !lab_bel_dirty.empty())
            lab_bel_dirty[data.lab_data.lab] |= lab_bel_bit(data);
        if (!lab_bel_refacts.empty())
            lab_bel_refacts[data.lab_data.lab] |= lab_bel_bit(data);
        if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF) && data.lab_data.lab < lab_versions.size()) {
            ++lab_versions[data.lab_data.lab];
            if (lab_reuse_active)
                ++lab_reuse_stats.precise_cell_invalidations;
        }
    }
    void note_lab_net_mutation(const NetInfo *net) const
    {
        if (net == nullptr)
            return;
        bool any = false;
        auto touch = [&](const CellInfo *cell) {
            if (cell == nullptr || cell->bel == BelId())
                return;
            const auto &data = bel_data(cell->bel);
            if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF) && !lab_bel_dirty.empty())
                lab_bel_dirty[data.lab_data.lab] |= lab_bel_bit(data);
            if (!lab_bel_refacts.empty())
                lab_bel_refacts[data.lab_data.lab] |= lab_bel_bit(data);
            if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF) &&
                data.lab_data.lab < lab_versions.size()) {
                ++lab_versions[data.lab_data.lab];
                any = true;
            }
        };
        touch(net->driver.cell);
        for (const auto &user : net->users)
            touch(user.cell);
        if (any && lab_reuse_active)
            ++lab_reuse_stats.precise_net_invalidations;
    }
    // Stage 4E state stamps (lab_reuse.cc): recorded by control preparation and
    // by the end of routing; a later mutation makes them stale automatically.
    mutable std::vector<LabStamp> lab_prepared;
    mutable uint64_t lab_routing_epoch = 1;
    mutable uint64_t lab_routed_epoch = 0; // routing epoch when routing last completed; 0 = never
    mutable std::vector<LabContentEntry> lab_content_cache;
    LabStamp lab_stamp(uint32_t lab) const
    {
        return lab < lab_versions.size() ? LabStamp{lab_versions[lab], lab_facts_epoch, true} : LabStamp{};
    }
    bool lab_stamp_current(uint32_t lab, const LabStamp &stamp) const
    {
        return stamp.valid && lab < lab_versions.size() && stamp.lab_version == lab_versions[lab] &&
               stamp.facts_epoch == lab_facts_epoch;
    }
    void note_lab_prepared(uint32_t lab) const
    {
        if (lab < lab_prepared.size())
            lab_prepared[lab] = lab_stamp(lab);
    }
    void note_routing_complete() const { lab_routed_epoch = lab_routing_epoch; }
    void report_lab_states() const;
    // Sizes the per-LAB stamps, applies the mode gate, and clears statistics.
    void lab_reuse_begin();
    void lab_reuse_end();
    bool check_lab_input_count(uint32_t lab) const; // lab.cc
    bool check_mlab_groups(uint32_t lab) const;     // lab.cc

    void assign_comb_info(CellInfo *cell) const; // lab.cc
    void assign_ff_info(CellInfo *cell) const;   // lab.cc

    void lab_pre_route();                                   // lab.cc
    void assign_control_sets(uint32_t lab);                 // lab.cc
    void reassign_alm_inputs(uint32_t lab, uint8_t alm);    // lab.cc
    void update_alm_input_count(uint32_t lab, uint8_t alm); // lab.cc

    uint64_t compute_lut_mask(uint32_t lab, uint8_t alm); // lab.cc

    // Keeping track of unique MLAB write ports to assign them indices
    dict<IdString, IdString> get_mlab_key(const CellInfo *cell, bool include_raddr = false) const; // lab.cc
    mutable idict<dict<IdString, IdString>> mlab_groups;

    // -------------------------------------------------

    bool is_io_cell(IdString cell_type) const;                   // io.cc
    BelId get_io_pin_bel(const CycloneV::pin_info_t *pin) const; // io.cc

    // -------------------------------------------------

    bool is_clkbuf_cell(IdString cell_type) const; // globals.cc
    void route_globals();                          // globals.cc

    // -------------------------------------------------

    bool is_pll_cell(IdString cell_type) const; // pll.cc

    // -------------------------------------------------

    static const std::string defaultPlacer;
    static const std::vector<std::string> availablePlacers;
    static const std::string defaultRouter;
    static const std::vector<std::string> availableRouters;

    dict<WireId, WireInfo> wires;
    // Wire slots (design section 17.2): a dense number for every wire, reached from its rnode
    // through two small tables instead of a hash. The z values of each group of one rnode type in
    // one tile are numbered densely from the group's first slot. Built by build_wire_slots() once
    // the routing graph is imported; no wire or pip is added after that, so the WireInfo pointers
    // into `wires` stay valid. A slot with no wire (2.5% of them) has a null WireInfo and an
    // unbound, unflagged route.
    std::array<uint8_t, 256> wire_slot_type{}; // rnode type -> compact type + 1; 0 when absent
    int wire_slot_dim_x = 0, wire_slot_dim_y = 0;
    std::vector<std::pair<uint32_t, uint32_t>> wire_slot_groups; // (compact type, x, y) -> first slot, count
    std::vector<WireRoute> wire_routes;                          // by slot
    std::vector<WireInfo *> wire_infos;                          // by slot
    std::vector<std::pair<WireId, uint64_t>> pending_wire_flags; // flags given to add_wire before the slots
    void build_wire_slots();
    // The slot of `wire`'s number, or -1 when its type, tile, or z lies outside the tables. A slot
    // inside them may be empty.
    int wire_slot(WireId wire) const
    {
        const unsigned type = wire_slot_type[CycloneV::rn2t(wire.node)];
        const int x = int(CycloneV::rn2x(wire.node)), y = int(CycloneV::rn2y(wire.node));
        if (type == 0 || x >= wire_slot_dim_x || y >= wire_slot_dim_y)
            return -1;
        const auto &group = wire_slot_groups[(size_t(type - 1) * wire_slot_dim_x + x) * wire_slot_dim_y + y];
        const uint32_t z = CycloneV::rn2z(wire.node);
        return z < group.second ? int(group.first + z) : -1;
    }
    // The routing state of `wire`, or null for an id that numbers no slot. An empty slot answers
    // as an unbound, unflagged wire, as the dictionary's miss did.
    const WireRoute *find_wire_route(WireId wire) const
    {
        const int slot = wire_slot(wire);
        return slot < 0 ? nullptr : &wire_routes[slot];
    }
    const WireRoute &wire_route(WireId wire) const
    {
        const WireRoute *route = find_wire_route(wire);
        NPNR_ASSERT(route != nullptr);
        return *route;
    }
    WireRoute &wire_route(WireId wire)
    {
        const int slot = wire_slot(wire);
        NPNR_ASSERT(slot >= 0 && wire_infos[slot] != nullptr);
        return wire_routes[slot];
    }
    const WireInfo &wire_info(WireId wire) const
    {
        const int slot = wire_slot(wire);
        NPNR_ASSERT(slot >= 0 && wire_infos[slot] != nullptr);
        return *wire_infos[slot];
    }
    uint64_t wire_flags(WireId wire) const { return wire_route(wire).flags; }
    void set_wire_flags(WireId wire, uint64_t flags)
    {
        NPNR_ASSERT(flags <= UINT32_MAX);
        wire_route(wire).flags = uint32_t(flags);
    }

    // List of LABs
    std::vector<LABInfo> labs;

    // WIP to link without failure
    std::vector<BelPin> empty_belpin_list;

    // Conversion between numbers and rnode types and IdString, for fast wire name implementation
    std::vector<IdString> int2id;
    dict<IdString, int> id2int;

    std::vector<IdString> rn_t2id;
    dict<IdString, CycloneV::rnode_type_t> id2rn_t;

    // This structure is only used for nextpnr-created wires
    dict<IdStringList, WireId> npnr_wirebyname;

    std::vector<std::vector<BelInfo>> bels_by_tile;
    std::vector<BelId> all_bels;

    size_t pos2idx(int x, int y) const
    {
        NPNR_ASSERT(x >= 0 && x < int(cyclonev->get_tile_sx()));
        NPNR_ASSERT(y >= 0 && y < int(cyclonev->get_tile_sy()));
        return y * cyclonev->get_tile_sx() + x;
    }

    size_t pos2idx(CycloneV::pos_t pos) const { return pos2idx(CycloneV::pos2x(pos), CycloneV::pos2y(pos)); }

    BelInfo &bel_data(BelId bel) { return bels_by_tile.at(pos2idx(bel.pos)).at(bel.z); }
    const BelInfo &bel_data(BelId bel) const { return bels_by_tile.at(pos2idx(bel.pos)).at(bel.z); }

    // -------------------------------------------------

    void assign_default_pinmap(CellInfo *cell);
    static const dict<IdString, IdString> comb_pinmap;

    // -------------------------------------------------

    typedef dict<IdString, CellPinStyle> CellPinsData;                          // pins.cc
    static const dict<IdString, CellPinsData> cell_pins_db;                     // pins.cc
    CellPinStyle get_cell_pin_style(const CellInfo *cell, IdString port) const; // pins.cc

    // -------------------------------------------------

    // List of IO constraints, used by QSF parser
    dict<IdString, dict<IdString, Property>> io_attr;
    void read_qsf(std::istream &in); // qsf.cc

    // -------------------------------------------------

    void build_bitstream(); // bitstream.cc
};

NEXTPNR_NAMESPACE_END

#endif
