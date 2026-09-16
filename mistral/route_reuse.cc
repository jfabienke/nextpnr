/* SPDX-License-Identifier: ISC */
#include "route_reuse.h"

#include <cinttypes>
#include <fstream>

#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
using json11::Json;

std::vector<PreviousRouteEntry> parse_routing_attr(const std::string &routing)
{
    // wire;pip;strength;wire;pip;strength;... as archInfoToAttributes writes it
    std::vector<PreviousRouteEntry> entries;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= routing.size()) {
        size_t end = routing.find(';', start);
        if (end == std::string::npos)
            end = routing.size();
        parts.push_back(routing.substr(start, end - start));
        start = end + 1;
    }
    for (size_t i = 0; i + 2 < parts.size(); i += 3) {
        PreviousRouteEntry e;
        e.wire = parts[i];
        e.pip = parts[i + 1];
        e.strength = std::stoi(parts[i + 2]);
        entries.push_back(std::move(e));
    }
    return entries;
}

// Walks wire -> pip source until the route's own source wire; false when the
// chain leaves the route (a stale or truncated tree).
bool chain_reaches_source(Context &ctx, const dict<WireId, PipId> &pip_into, WireId source, WireId leaf)
{
    WireId cursor = leaf;
    for (size_t steps = 0; steps <= pip_into.size(); ++steps) {
        if (cursor == source)
            return true;
        auto it = pip_into.find(cursor);
        if (it == pip_into.end())
            return false;
        cursor = ctx.getPipSrcWire(it->second);
    }
    return false;
}
} // namespace

PreviousRoutes load_previous_routes(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
        log_error("route reuse: cannot open '%s'.\n", path.c_str());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    Json root = Json::parse(text, err);
    if (!err.empty())
        log_error("route reuse: '%s' is not valid JSON: %s\n", path.c_str(), err.c_str());
    PreviousRoutes routes;
    const Json &checkpoint = root["nextpnr_checkpoint"];
    if (checkpoint.is_object() && checkpoint["physical"]["routes"].is_array()) {
        if (checkpoint["manifest"]["phase"].string_value() != "routed")
            log_error("route reuse: '%s' is a '%s' checkpoint; routes come from a routed one.\n", path.c_str(),
                      checkpoint["manifest"]["phase"].string_value().c_str());
        for (const auto &route : checkpoint["physical"]["routes"].array_items()) {
            std::vector<PreviousRouteEntry> entries;
            for (const auto &v : route[1].array_items()) {
                PreviousRouteEntry e;
                e.wire = v[0].string_value();
                e.pip = v[1].string_value();
                e.strength = v[2].int_value();
                entries.push_back(std::move(e));
            }
            routes[route[0].string_value()] = std::move(entries);
        }
        log_info("route reuse: %zu routes from the routed checkpoint '%s'.\n", routes.size(), path.c_str());
        return routes;
    }
    const Json &modules = root["modules"];
    if (!modules.is_object())
        log_error("route reuse: '%s' has neither a routed checkpoint nor modules.\n", path.c_str());
    for (const auto &mod : modules.object_items())
        for (const auto &net : mod.second["netnames"].object_items()) {
            const Json &routing = net.second["attributes"]["ROUTING"];
            if (!routing.is_string() || routing.string_value().empty())
                continue;
            routes[net.first] = parse_routing_attr(routing.string_value());
        }
    log_info("route reuse: %zu routes from the ROUTING attributes of '%s'.\n", routes.size(), path.c_str());
    return routes;
}

namespace {
// The checks, shared by planning and applying. On Reuse, `resolved` holds
// (wire, pip into it or PipId() for the source) in the recorded order.
ReuseDecision check_route(Context &ctx, NetInfo *net, const std::vector<PreviousRouteEntry> &entries,
                          std::vector<std::pair<WireId, PipId>> &resolved, std::string &reason)
{
    resolved.clear();
    resolved.reserve(entries.size());
    WireId route_source;
    dict<WireId, PipId> pip_into;
    for (const auto &e : entries) {
        WireId wire;
        PipId pip;
        bool ok = true;
        try {
            wire = ctx.getWireByName(IdStringList::parse(&ctx, e.wire));
            if (!e.pip.empty())
                pip = ctx.getPipByName(IdStringList::parse(&ctx, e.pip));
        } catch (const std::exception &) {
            ok = false;
        }
        if (!ok || wire == WireId() || (!e.pip.empty() && pip == PipId())) {
            reason = "wire or pip '" + (e.pip.empty() ? e.wire : e.pip) + "' does not resolve";
            return ReuseDecision::Unresolved;
        }
        if (e.pip.empty()) {
            if (route_source != WireId()) {
                reason = "two source wires";
                return ReuseDecision::Unresolved;
            }
            route_source = wire;
        } else {
            if (ctx.getPipDstWire(pip) != wire) {
                reason = "pip does not drive its wire";
                return ReuseDecision::Unresolved;
            }
            pip_into[wire] = pip;
        }
        resolved.emplace_back(wire, pip);
    }
    if (route_source == WireId()) {
        reason = "no source wire";
        return ReuseDecision::Unresolved;
    }
    WireId source = ctx.getNetinfoSourceWire(net);
    if (source == WireId() || source != route_source) {
        reason = "source wire differs";
        return ReuseDecision::EndpointMismatch;
    }
    pool<WireId> route_wires;
    for (auto &rw : resolved)
        route_wires.insert(rw.first);
    pool<WireId> sinks;
    for (auto &user : net->users) {
        bool covered = false;
        for (WireId sink : ctx.getNetinfoSinkWires(net, user)) {
            if (route_wires.count(sink) && chain_reaches_source(ctx, pip_into, source, sink)) {
                covered = true;
                sinks.insert(sink);
            }
        }
        if (!covered) {
            reason = "sink " + user.cell->name.str(&ctx) + "." + user.port.str(&ctx) + " is not on the route";
            return ReuseDecision::EndpointMismatch;
        }
    }
    pool<WireId> has_downhill;
    for (auto &kv : pip_into)
        has_downhill.insert(ctx.getPipSrcWire(kv.second));
    for (auto &rw : resolved) {
        if (rw.first == source)
            continue;
        if (!has_downhill.count(rw.first) && !sinks.count(rw.first)) {
            reason = "leaf " + ctx.getWireName(rw.first).str(&ctx) + " drives no current sink";
            return ReuseDecision::EndpointMismatch;
        }
    }
    for (auto &rw : resolved) {
        if (ctx.getBoundWireNet(rw.first) != nullptr) {
            reason = "wire " + ctx.getWireName(rw.first).str(&ctx) + " is bound to " +
                     ctx.getBoundWireNet(rw.first)->name.str(&ctx);
            return ReuseDecision::Unavailable;
        }
        if (rw.second != PipId() && (!ctx.checkPipAvail(rw.second) || ctx.getBoundPipNet(rw.second) != nullptr)) {
            reason = "pip " + ctx.getPipName(rw.second).str(&ctx) + " is not available";
            return ReuseDecision::Unavailable;
        }
    }
    reason = "endpoints and every wire and pip check out";
    return ReuseDecision::Reuse;
}
} // namespace

void plan_route_reuse(Context &ctx, const PreviousRoutes &previous, ReusePlan &plan)
{
    plan.previous_nets = previous.size();
    plan.nets.clear();
    for (auto &net_pair : ctx.nets) {
        NetInfo *net = net_pair.second.get();
        NetReuseDecision d;
        d.net = net->name.str(&ctx);
        if (!net->wires.empty()) {
            d.decision = ReuseDecision::AlreadyRouted;
            d.reason = "bound before reuse (global router)";
            plan.nets.push_back(std::move(d));
            continue;
        }
        if (net->driver.cell == nullptr || net->users.entries() == 0)
            continue; // nothing to route; router2 skips it too
        auto prev = previous.find(d.net);
        if (prev == previous.end()) {
            d.decision = ReuseDecision::Added;
            d.reason = "no previous route of this name";
            plan.nets.push_back(std::move(d));
            continue;
        }
        d.wires = prev->second.size();
        std::vector<std::pair<WireId, PipId>> resolved;
        d.decision = check_route(ctx, net, prev->second, resolved, d.reason);
        plan.nets.push_back(std::move(d));
    }
}

RouteReuseReport apply_route_reuse(Context &ctx, const PreviousRoutes &previous, const ReusePlan &plan)
{
    RouteReuseReport report;
    report.previous_nets = previous.size();
    report.current_nets = ctx.nets.size();
    for (const auto &d : plan.nets) {
        switch (d.decision) {
        case ReuseDecision::AlreadyRouted:
            ++report.already_routed;
            continue;
        case ReuseDecision::Added:
            ++report.no_previous;
            continue;
        case ReuseDecision::EndpointMismatch:
            ++report.endpoint_mismatch;
            continue;
        case ReuseDecision::Unresolved:
            ++report.unresolved;
            continue;
        case ReuseDecision::Unavailable:
            ++report.unavailable;
            continue;
        case ReuseDecision::Reuse:
            break;
        default:
            continue;
        }
        auto it = ctx.nets.find(ctx.id(d.net));
        auto prev = previous.find(d.net);
        if (it == ctx.nets.end() || prev == previous.end())
            log_error("route reuse: plan names unknown net '%s'.\n", d.net.c_str());
        NetInfo *net = it->second.get();
        // Validate again against the live design: the plan is a record.
        std::vector<std::pair<WireId, PipId>> resolved;
        std::string reason;
        if (check_route(ctx, net, prev->second, resolved, reason) != ReuseDecision::Reuse) {
            ++report.unavailable;
            continue;
        }
        // Bind strong: router2 marks the wires unavailable to other nets from
        // its first iteration and records the arcs as pre-routed, which it
        // never revisits anyway; its final pass rebinds every wire up to
        // STRENGTH_STRONG at STRENGTH_WEAK, so the output carries the same
        // strengths as an uninterrupted run. Reverse of the recorded order,
        // so the wires map iterates as the previous run's did.
        for (auto rit = resolved.rbegin(); rit != resolved.rend(); ++rit) {
            if (rit->second == PipId())
                ctx.bindWire(rit->first, net, STRENGTH_STRONG);
            else
                ctx.bindPip(rit->second, net, STRENGTH_STRONG);
            ++report.wires_bound;
        }
        ++report.reused;
        report.reused_names.push_back(d.net);
    }
    return report;
}

RouteReuseReport apply_route_reuse(Context &ctx, const PreviousRoutes &previous)
{
    ReusePlan plan;
    plan_route_reuse(ctx, previous, plan);
    return apply_route_reuse(ctx, previous, plan);
}

RouteReuseReport apply_route_reuse(Context &ctx, const std::string &path)
{
    return apply_route_reuse(ctx, load_previous_routes(path));
}

void drop_reused_routes(Context &ctx, RouteReuseReport &report)
{
    for (const std::string &name : report.reused_names) {
        auto it = ctx.nets.find(ctx.id(name));
        if (it == ctx.nets.end())
            continue;
        NetInfo *net = it->second.get();
        std::vector<std::pair<WireId, PipId>> bound;
        for (auto &w : net->wires)
            bound.emplace_back(w.first, w.second.pip);
        for (auto &b : bound) {
            if (b.second == PipId())
                ctx.unbindWire(b.first);
            else
                ctx.unbindPip(b.second);
        }
        ++report.dropped;
    }
    report.reused_names.clear();
}

size_t unroute_below_locked(Context &ctx)
{
    size_t nets_unrouted = 0;
    for (auto &net_pair : ctx.nets) {
        NetInfo *net = net_pair.second.get();
        std::vector<std::pair<WireId, PipId>> bound;
        for (auto &w : net->wires)
            if (w.second.strength < STRENGTH_LOCKED)
                bound.emplace_back(w.first, w.second.pip);
        if (bound.empty())
            continue;
        for (auto &b : bound) {
            if (b.second == PipId())
                ctx.unbindWire(b.first);
            else
                ctx.unbindPip(b.second);
        }
        ++nets_unrouted;
    }
    return nets_unrouted;
}

void report_route_reuse(const RouteReuseReport &r)
{
    log_info("Route reuse: %" PRIu64 " previous routes, %" PRIu64 " current nets: %" PRIu64 " reused (%" PRIu64
             " wires), %" PRIu64 " already routed by the global router, %" PRIu64 " with no previous route, %" PRIu64
             " endpoint mismatches, %" PRIu64 " unresolved, %" PRIu64 " unavailable, %" PRIu64
             " dropped by the fallback\n",
             r.previous_nets, r.current_nets, r.reused, r.wires_bound, r.already_routed, r.no_previous,
             r.endpoint_mismatch, r.unresolved, r.unavailable, r.dropped);
}

NEXTPNR_NAMESPACE_END
