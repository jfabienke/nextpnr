/* SPDX-License-Identifier: ISC */
#include "reuse_plan.h"

#include <cstdio>
#include <fstream>

#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

const char *reuse_decision_name(ReuseDecision d)
{
    switch (d) {
    case ReuseDecision::Reuse:
        return "reuse";
    case ReuseDecision::Changed:
        return "changed";
    case ReuseDecision::Added:
        return "added";
    case ReuseDecision::UserConstrained:
        return "user-constrained";
    case ReuseDecision::MissingBel:
        return "missing-bel";
    case ReuseDecision::Released:
        return "released";
    case ReuseDecision::AlreadyRouted:
        return "already-routed";
    case ReuseDecision::EndpointMismatch:
        return "endpoint-mismatch";
    case ReuseDecision::Unresolved:
        return "unresolved";
    case ReuseDecision::Unavailable:
        return "unavailable";
    }
    return "?";
}

uint64_t ReusePlan::count_cells(ReuseDecision d) const
{
    uint64_t n = 0;
    for (const auto &c : cells)
        if (c.decision == d)
            ++n;
    return n;
}

uint64_t ReusePlan::count_nets(ReuseDecision d) const
{
    uint64_t n = 0;
    for (const auto &c : nets)
        if (c.decision == d)
            ++n;
    return n;
}

void write_reuse_plan(const ReusePlan &plan, const std::string &path)
{
    using json11::Json;
    Json::array cells, nets;
    cells.reserve(plan.cells.size());
    for (const auto &c : plan.cells)
        cells.push_back(Json::object{
                {"cell", c.cell}, {"decision", reuse_decision_name(c.decision)}, {"bel", c.bel}, {"reason", c.reason}});
    int survived = 0, rerouted = 0;
    for (const auto &n : plan.nets) {
        Json::object o{{"net", n.net},
                       {"decision", reuse_decision_name(n.decision)},
                       {"wires", int(n.wires)},
                       {"reason", n.reason}};
        if (n.survived >= 0) {
            o["survived"] = n.survived == 1;
            (n.survived == 1 ? survived : rerouted)++;
        }
        nets.push_back(o);
    }
    Json::object cell_counts, net_counts;
    for (ReuseDecision d : {ReuseDecision::Reuse, ReuseDecision::Changed, ReuseDecision::Added,
                            ReuseDecision::UserConstrained, ReuseDecision::MissingBel, ReuseDecision::Released})
        cell_counts[reuse_decision_name(d)] = int(plan.count_cells(d));
    for (ReuseDecision d : {ReuseDecision::Reuse, ReuseDecision::AlreadyRouted, ReuseDecision::Added,
                            ReuseDecision::EndpointMismatch, ReuseDecision::Unresolved, ReuseDecision::Unavailable})
        net_counts[reuse_decision_name(d)] = int(plan.count_nets(d));
    Json root = Json::object{
            {"previous", plan.previous_path},
            {"summary", Json::object{{"previous_cells", int(plan.previous_cells)},
                                     {"previous_routethru", int(plan.previous_routethru)},
                                     {"previous_nets", int(plan.previous_nets)},
                                     {"removed_cells", int(plan.removed_cells)},
                                     {"cells", cell_counts},
                                     {"nets", net_counts},
                                     {"route_survival", Json::object{{"survived", survived}, {"rerouted", rerouted}}},
                                     {"placement_attempts", int(plan.placement_attempts)},
                                     {"released_cells", int(plan.released_cells)},
                                     {"placement_full_fallback", plan.placement_full_fallback}}},
            {"cells", cells},
            {"nets", nets},
    };
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out)
            log_error("reuse plan: cannot write '%s'.\n", tmp.c_str());
        out << root.dump() << "\n";
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        log_error("reuse plan: cannot rename '%s' to '%s'.\n", tmp.c_str(), path.c_str());
    log_info("Reuse plan written to '%s' (%zu cell decisions, %zu net decisions).\n", path.c_str(), plan.cells.size(),
             plan.nets.size());
}

NEXTPNR_NAMESPACE_END
