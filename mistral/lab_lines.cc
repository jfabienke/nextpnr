/* SPDX-License-Identifier: ISC */
#include "lab_lines.h"

#include <algorithm>
#include <cinttypes>
#include <unordered_map>

#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

using mistral::CycloneV;

bool is_td_wire(const Context &ctx, WireId w)
{
    return w != WireId() && !w.is_nextpnr_created() && CycloneV::rn2t(w.node) == CycloneV::rnode_type_t::TD;
}

struct Sink
{
    NetInfo *net;
    WireId wire;
    std::vector<std::pair<int, PipId>> lines; // (line index, pip from that line into this pin)
    uint64_t allowed = 0;
    PipId bound_pip; // set when the pin arrived bound to its net through a line
};

struct Group
{
    NetInfo *net;
    uint64_t allowed;
    std::vector<size_t> sinks;
    int fixed_line = -1; // a line one of its pins is already bound through
    int line = -1;
};

// Kuhn's augmenting path over at most 46 lines.
bool augment(size_t g, const std::vector<Group> &groups, std::vector<int> &line_of, std::vector<int> &group_of,
             uint64_t &visited, int rotate)
{
    // Scan the allowed lines from a per-group offset so a LAB's nets spread over the 46 lines
    // (and the fabric muxes that feed them) instead of piling onto the lowest indices.
    const uint64_t allowed = groups[g].allowed;
    for (int step = 0; step < 64; step++) {
        const int l = (step + rotate) % 64;
        if (!(allowed & (uint64_t(1) << l)) || (visited & (uint64_t(1) << l)))
            continue;
        visited |= uint64_t(1) << l;
        if (group_of[l] < 0 || augment(size_t(group_of[l]), groups, line_of, group_of, visited, rotate)) {
            group_of[l] = int(g);
            line_of[g] = l;
            return true;
        }
    }
    return false;
}

} // namespace

LabLinesReport assign_lab_input_lines(Context &ctx)
{
    LabLinesReport report;
    dict<WireId, std::vector<std::pair<WireId, PipId>>> td_cache; // pin wire -> (line, pip) pairs
    for (uint32_t lab = 0; lab < ctx.labs.size(); lab++) {
        const auto &lab_data = ctx.labs.at(lab);
        // Every LAB-input pin in this LAB with a net on it, in slot order.
        std::vector<Sink> sinks;
        dict<WireId, const NetInfo *>
                pin_seen; // a pin wire enters once; a register's data input can share its LUT's E/F pin
        dict<WireId, int> line_index;
        std::vector<WireId> lines;
        auto line_id = [&](WireId w) {
            auto it = line_index.find(w);
            if (it != line_index.end())
                return it->second;
            int idx = int(lines.size());
            NPNR_ASSERT(idx < 64);
            line_index[w] = idx;
            lines.push_back(w);
            return idx;
        };
        for (const auto &alm : lab_data.alms) {
            std::vector<BelId> bels(alm.lut_bels.begin(), alm.lut_bels.end());
            bels.insert(bels.end(), alm.ff_bels.begin(), alm.ff_bels.end());
            for (BelId bel : bels) {
                CellInfo *cell = ctx.getBoundBelCell(bel);
                if (cell == nullptr)
                    continue;
                for (auto &port : cell->ports) {
                    if (port.second.type != PORT_IN || port.second.net == nullptr)
                        continue;
                    NetInfo *net = port.second.net;
                    // Only nets that come from outside the LAB need an input line; a net driven in
                    // this LAB reaches its pins on the local feedback lines without leaving it.
                    if (net->driver.cell != nullptr && net->driver.cell->bel != BelId()) {
                        BelId drv = net->driver.cell->bel;
                        const auto &drv_data = ctx.bels_by_tile[ctx.pos2idx(drv.pos)][drv.z];
                        if ((drv_data.type == id_MISTRAL_COMB || drv_data.type == id_MISTRAL_MCOMB ||
                             drv_data.type == id_MISTRAL_FF) &&
                            drv_data.lab_data.lab == lab)
                            continue;
                    }
                    const PortRef &usr = net->users.at(port.second.user_idx);
                    const size_t count = ctx.getNetinfoSinkWireCount(net, usr);
                    for (size_t phys = 0; phys < count; phys++) {
                        WireId wire = ctx.getNetinfoSinkWire(net, usr, phys);
                        if (wire == WireId())
                            continue;
                        auto cached = td_cache.find(wire);
                        if (cached == td_cache.end()) {
                            std::vector<std::pair<WireId, PipId>> srcs;
                            for (PipId pip : ctx.getPipsUphill(wire)) {
                                WireId src = ctx.getPipSrcWire(pip);
                                if (is_td_wire(ctx, src))
                                    srcs.emplace_back(src, pip);
                            }
                            std::sort(srcs.begin(), srcs.end(),
                                      [](const auto &a, const auto &b) { return a.first.node < b.first.node; });
                            cached = td_cache.emplace(wire, std::move(srcs)).first;
                        }
                        if (cached->second.empty())
                            continue; // not a LAB input pin (carry, clock, ...)
                        auto seen = pin_seen.find(wire);
                        if (seen != pin_seen.end()) {
                            if (seen->second != net && ctx.debug)
                                log_info("    LAB %u: pin %s wanted by nets %s and %s; keeping the first\n", lab,
                                         ctx.nameOfWire(wire), ctx.nameOf(seen->second), ctx.nameOf(net));
                            continue;
                        }
                        pin_seen[wire] = net;
                        Sink s;
                        s.net = net;
                        s.wire = wire;
                        const NetInfo *bound = ctx.getBoundWireNet(wire);
                        if (bound != nullptr && bound != net)
                            continue; // another net holds the pin; not ours to assign
                        if (bound == net) {
                            PipId pip = net->wires.at(wire).pip;
                            if (pip == PipId() || !is_td_wire(ctx, ctx.getPipSrcWire(pip)))
                                continue; // bound some other way; leave it
                            s.bound_pip = pip;
                        }
                        for (auto &lp : cached->second) {
                            if (s.bound_pip != PipId() && lp.second != s.bound_pip)
                                continue;
                            const NetInfo *line_net = ctx.getBoundWireNet(lp.first);
                            if (line_net != nullptr && line_net != net)
                                continue; // line taken by another net (a reused route, a global)
                            if (s.bound_pip == PipId() && !ctx.checkPipAvailForNet(lp.second, net))
                                continue;
                            int idx = line_id(lp.first);
                            s.lines.emplace_back(idx, lp.second);
                            s.allowed |= uint64_t(1) << idx;
                        }
                        if (s.allowed == 0)
                            continue; // nothing can drive it any more; the router will report it
                        sinks.push_back(std::move(s));
                    }
                }
            }
        }
        if (sinks.empty())
            continue;
        report.labs++;
        // Group a net's pins by a common line; a net whose pins share none takes more than one.
        std::vector<Group> groups;
        for (size_t i = 0; i < sinks.size(); i++) {
            Sink &s = sinks[i];
            Group *home = nullptr;
            for (Group &g : groups)
                if (g.net == s.net && (g.allowed & s.allowed) != 0) {
                    home = &g;
                    break;
                }
            if (home == nullptr) {
                groups.push_back(Group{s.net, s.allowed, {}, -1, -1});
                home = &groups.back();
            } else {
                home->allowed &= s.allowed;
            }
            home->sinks.push_back(i);
            if (s.bound_pip != PipId())
                home->fixed_line = s.lines.front().first;
        }
        // Lines already bound to a net elsewhere in this LAB are that net's; nobody else may take them.
        for (Group &g : groups)
            if (g.fixed_line >= 0)
                g.allowed = uint64_t(1) << g.fixed_line;
        std::vector<int> line_of(groups.size(), -1), group_of(lines.size(), -1);
        for (size_t g = 0; g < groups.size(); g++)
            if (groups[g].fixed_line >= 0) {
                line_of[g] = groups[g].fixed_line;
                group_of[groups[g].fixed_line] = int(g);
            }
        bool feasible = true;
        for (size_t g = 0; g < groups.size() && feasible; g++) {
            if (line_of[g] >= 0)
                continue;
            uint64_t visited = 0;
            feasible = augment(g, groups, line_of, group_of, visited, int((g * 7) % 46));
        }
        if (!feasible) {
            report.labs_infeasible++;
            if (ctx.debug)
                log_info("    LAB %u: no line assignment for %zu net groups over %zu lines; left to the router\n", lab,
                         groups.size(), lines.size());
            continue;
        }
        std::unordered_map<const NetInfo *, int> nets_seen;
        for (size_t g = 0; g < groups.size(); g++) {
            Group &grp = groups[g];
            WireId line = lines.at(size_t(line_of[g]));
            if (nets_seen.count(grp.net))
                report.split_nets++;
            nets_seen[grp.net]++;
            // The line itself stays unbound: router2 cannot drive a wire that arrives bound without a
            // pip. Binding every pin through the line's pip, and refusing every other pip into those
            // pins, leaves no other net a use for the line, which reserves it just as well.
            report.lines++;
            for (size_t si : grp.sinks) {
                Sink &s = sinks[si];
                if (s.bound_pip != PipId()) {
                    report.already_bound++;
                    continue;
                }
                PipId pip;
                for (auto &lp : s.lines)
                    if (lp.first == line_of[g])
                        pip = lp.second;
                NPNR_ASSERT(pip != PipId());
                ctx.bindPip(pip, grp.net, STRENGTH_PLACER);
                report.pins++;
            }
        }
        report.nets += nets_seen.size();
    }
    return report;
}

void report_lab_input_lines(const LabLinesReport &r)
{
    log_info("LAB input lines: %" PRIu64 " LABs, %" PRIu64 " nets on %" PRIu64 " lines (%" PRIu64
             " nets split across lines), %" PRIu64 " pins bound, %" PRIu64 " pins already bound, %" PRIu64
             " LABs without a matching left to the router.\n",
             r.labs, r.nets, r.lines, r.split_nets, r.pins, r.already_bound, r.labs_infeasible);
}

NEXTPNR_NAMESPACE_END
