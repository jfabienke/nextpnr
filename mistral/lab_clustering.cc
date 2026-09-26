/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The Mistral LAB work
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

#include "lab_clustering.h"

#include <algorithm>

#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

constexpr int NET_FANOUT_MAX = 64; // nets with more sinks (clocks, resets, enables) do not attract

// A placement unit: a single LAB cell, or a pack-time cluster (an ALM pair, a LUT and its register) with its root
// first.
struct Unit
{
    std::vector<CellInfo *> cells;
    std::vector<const NetInfo *> nets; // the priced nets it touches, once each
    bool assigned = false;
};

bool lab_cell(const Arch &arch, const CellInfo *cell)
{
    return cell->type == id_MISTRAL_FF || (arch.is_comb_cell(cell->type) && cell->type != id_MISTRAL_ALUT_ARITH);
}

bool priced_net(const NetInfo *net)
{
    return net != nullptr && net->driver.cell != nullptr && net->users.entries() >= 1 &&
           int(net->users.entries()) <= NET_FANOUT_MAX;
}

struct Clusterer
{
    Context *ctx;
    Arch &arch;
    const LabClusteringCfg &cfg;
    std::vector<Unit> units;
    dict<IdString, int> unit_of;                // cell -> unit
    dict<IdString, std::vector<int>> net_units; // net name -> units touching it (by name: deterministic)
    uint32_t scratch = 0;                       // the empty LAB clusters are checked on
    std::vector<BelId> bound;                   // bels bound in the scratch LAB
    // try_place failures: no free bel shape, or every shape refused by the LAB rules
    long refuse_nobel = 0, refuse_rules = 0;
    // why each cluster closed: fill reached, no candidate, best at or below the floor, every tried candidate refused
    int stop_fill = 0, stop_empty = 0, stop_floor = 0, stop_refused = 0, alone = 0;

    Clusterer(Context *ctx, const LabClusteringCfg &cfg) : ctx(ctx), arch(*ctx), cfg(cfg) {}

    bool build_units()
    {
        std::vector<IdString> names;
        for (auto &cell : ctx->cells)
            names.push_back(cell.first);
        std::sort(names.begin(), names.end(), [&](IdString a, IdString b) { return a.str(ctx) < b.str(ctx); });
        for (IdString name : names) {
            CellInfo *cell = ctx->cells.at(name).get();
            if (!lab_cell(arch, cell) || cell->bel != BelId())
                continue;
            if (cell->cluster != ClusterId() && cell->cluster != cell->name)
                continue; // a member of another cell's cluster
            Unit unit;
            unit.cells.push_back(cell);
            bool ok = true;
            for (CellInfo *child : cell->constr_children) {
                if (!lab_cell(arch, child) || child->bel != BelId())
                    ok = false; // carry chains and anything already placed stay with HeAP
                unit.cells.push_back(child);
            }
            if (!ok)
                continue;
            for (CellInfo *c : unit.cells)
                for (auto &port : c->ports) {
                    const NetInfo *net = port.second.net;
                    if (priced_net(net) && std::find(unit.nets.begin(), unit.nets.end(), net) == unit.nets.end())
                        unit.nets.push_back(net);
                }
            int index = int(units.size());
            for (CellInfo *c : unit.cells)
                unit_of[c->name] = index;
            for (const NetInfo *net : unit.nets)
                net_units[net->name].push_back(index);
            units.push_back(std::move(unit));
        }
        for (uint32_t lab = 0; lab < arch.labs.size(); lab++)
            if (!arch.labs[lab].is_mlab) {
                scratch = lab;
                return true;
            }
        return false;
    }

    // Lays a unit on free bels of the scratch LAB, each bound bel certified by isBelLocationValid; false leaves
    // nothing new bound.
    bool try_place(const Unit &unit)
    {
        const auto &lab = arch.labs[scratch];
        CellInfo *root = unit.cells.front();
        const bool is_ff = root->type == id_MISTRAL_FF;
        bool shape = false;
        for (const auto &alm : lab.alms) {
            std::vector<BelId> roots;
            if (is_ff)
                roots.assign(alm.ff_bels.begin(), alm.ff_bels.end());
            else
                roots.assign(alm.lut_bels.begin(), alm.lut_bels.end());
            for (BelId root_bel : roots) {
                std::vector<std::pair<CellInfo *, BelId>> placement;
                if (unit.cells.size() > 1) {
                    if (!ctx->getClusterPlacement(root->cluster, root_bel, placement))
                        continue;
                } else {
                    placement.emplace_back(root, root_bel);
                }
                bool free = true;
                for (auto &p : placement)
                    free = free && ctx->checkBelAvail(p.second) && ctx->isValidBelForCellType(p.first->type, p.second);
                if (!free)
                    continue;
                shape = true;
                for (auto &p : placement)
                    ctx->bindBel(p.second, p.first, STRENGTH_WEAK);
                bool valid = true;
                for (auto &p : placement)
                    valid = valid && ctx->isBelLocationValid(p.second);
                if (valid) {
                    for (auto &p : placement)
                        bound.push_back(p.second);
                    return true;
                }
                for (auto &p : placement)
                    ctx->unbindBel(p.second);
            }
        }
        ++(shape ? refuse_rules : refuse_nobel);
        return false;
    }

    void clear_scratch()
    {
        for (BelId bel : bound)
            ctx->unbindBel(bel);
        bound.clear();
    }

    // Attraction of a candidate to the cluster: shared nets weighted by 1 / fanout, doubled when the net would have
    // every pin inside, less a price per external input net the candidate adds.
    float attraction(int candidate, const pool<int> &members, const pool<IdString> &inputs)
    {
        float gain = 0;
        int new_lines = 0;
        for (const NetInfo *net : units[candidate].nets) {
            int inside = 0, total = 0;
            for (int u : net_units.at(net->name)) {
                ++total;
                if (u == candidate || members.count(u))
                    ++inside;
            }
            bool shared = false;
            for (int u : net_units.at(net->name))
                shared = shared || members.count(u);
            if (shared)
                gain += (1.0f / float(net->users.entries())) * (inside == total ? 2.0f : 1.0f);
            // a net driven outside the cluster and the candidate enters the LAB on a line
            const CellInfo *driver = net->driver.cell;
            bool driven_inside = unit_of.count(driver->name) &&
                                 (unit_of.at(driver->name) == candidate || members.count(unit_of.at(driver->name)));
            if (!driven_inside && !inputs.count(net->name))
                ++new_lines;
        }
        return gain - cfg.line_price * float(new_lines);
    }

    LabClustering run()
    {
        LabClustering result;
        if (!build_units()) {
            log_warning("LAB clustering: no LAB to check clusters on.\n");
            return result;
        }
        // Seeds: most priced nets first, then by name (units are in name order already).
        std::vector<int> order(units.size());
        for (size_t i = 0; i < units.size(); i++)
            order[i] = int(i);
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return units[a].nets.size() > units[b].nets.size(); });
        for (int seed : order) {
            if (units[seed].assigned)
                continue;
            if (!try_place(units[seed])) {
                units[seed].assigned = true; // cannot sit in a LAB alone; leave it to HeAP
                ++alone;
                continue;
            }
            pool<int> members{seed};
            units[seed].assigned = true;
            int cells = int(units[seed].cells.size());
            pool<IdString> inputs; // nets the cluster already brings in
            auto note_inputs = [&](int u) {
                for (const NetInfo *net : units[u].nets)
                    inputs.insert(net->name);
            };
            note_inputs(seed);
            while (cells < cfg.fill_target) {
                // candidates: unassigned units sharing a priced net with the cluster
                pool<int> seen;
                std::vector<std::pair<float, int>> ranked;
                for (int m : members)
                    for (const NetInfo *net : units[m].nets)
                        for (int u : net_units.at(net->name))
                            if (!units[u].assigned && !seen.count(u)) {
                                seen.insert(u);
                                ranked.emplace_back(attraction(u, members, inputs), u);
                            }
                if (ranked.empty()) {
                    ++stop_empty;
                    break;
                }
                std::stable_sort(ranked.begin(), ranked.end(), [&](const auto &a, const auto &b) {
                    return a.first > b.first || (a.first == b.first && a.second < b.second);
                });
                bool grown = false, floor = false;
                for (int t = 0; t < std::min<int>(cfg.tries_per_step, int(ranked.size())); t++) {
                    if (ranked[t].first <= cfg.attraction_floor) {
                        floor = true;
                        break;
                    }
                    int u = ranked[t].second;
                    if (cells + int(units[u].cells.size()) > cfg.fill_target)
                        continue;
                    if (try_place(units[u])) {
                        members.insert(u);
                        units[u].assigned = true;
                        cells += int(units[u].cells.size());
                        note_inputs(u);
                        grown = true;
                        break;
                    }
                }
                if (!grown) {
                    ++(floor ? stop_floor : stop_refused);
                    break;
                }
            }
            if (cells >= cfg.fill_target)
                ++stop_fill;
            clear_scratch();
            std::vector<CellInfo *> cluster;
            std::vector<int> sorted(members.begin(), members.end());
            std::sort(sorted.begin(), sorted.end());
            for (int u : sorted)
                for (CellInfo *c : units[u].cells)
                    cluster.push_back(c);
            int index = int(result.clusters.size());
            for (CellInfo *c : cluster)
                result.cluster_of[c->name] = index;
            result.clusters.push_back(std::move(cluster));
        }
        // The pass leaves nothing bound: every bel of the scratch LAB is free again.
        for (const auto &alm : arch.labs[scratch].alms) {
            for (BelId bel : alm.lut_bels)
                NPNR_ASSERT(ctx->checkBelAvail(bel));
            for (BelId bel : alm.ff_bels)
                NPNR_ASSERT(ctx->checkBelAvail(bel));
        }
        // Statistics: absorbed nets (every pin in one cluster) and external nets per cluster.
        for (auto &cluster : result.clusters) {
            result.cells += int(cluster.size());
            dict<IdString, const NetInfo *> nets;
            for (CellInfo *c : cluster)
                for (auto &port : c->ports)
                    if (port.second.net && port.second.type == PORT_IN)
                        nets[port.second.net->name] = port.second.net;
            int index = result.cluster_of.at(cluster.front()->name);
            for (auto &entry : nets) {
                const NetInfo *net = entry.second;
                const CellInfo *driver = net->driver.cell;
                bool driven_inside =
                        driver && result.cluster_of.count(driver->name) && result.cluster_of.at(driver->name) == index;
                if (!driven_inside)
                    ++result.external_nets;
            }
        }
        for (auto &net_pair : ctx->nets) {
            const NetInfo *net = net_pair.second.get();
            if (!net->driver.cell || !result.cluster_of.count(net->driver.cell->name) || net->users.empty())
                continue;
            int index = result.cluster_of.at(net->driver.cell->name);
            bool all = true;
            for (auto &usr : net->users)
                all = all && result.cluster_of.count(usr.cell->name) && result.cluster_of.at(usr.cell->name) == index;
            result.absorbed_nets += all;
        }
        return result;
    }
};

} // namespace

LabClustering run_lab_clustering(Context *ctx, const LabClusteringCfg &cfg)
{
    Clusterer clusterer(ctx, cfg);
    LabClustering result = clusterer.run();
    const int n = int(result.clusters.size());
    log_info("LAB clustering: units=%d clusters=%d cells=%d cells-per-cluster=%.2f absorbed-nets=%d "
             "external-nets-per-cluster=%.2f fill=%d line-price=%.2f\n",
             int(clusterer.units.size()), n, result.cells, n ? double(result.cells) / n : 0.0, result.absorbed_nets,
             n ? double(result.external_nets) / n : 0.0, cfg.fill_target, double(cfg.line_price));
    log_info("LAB clustering stops: fill=%d no-candidate=%d floor=%d refused=%d alone=%d refusals: no-bel=%ld "
             "rules=%ld\n",
             clusterer.stop_fill, clusterer.stop_empty, clusterer.stop_floor, clusterer.stop_refused, clusterer.alone,
             clusterer.refuse_nobel, clusterer.refuse_rules);
    return result;
}

NEXTPNR_NAMESPACE_END
