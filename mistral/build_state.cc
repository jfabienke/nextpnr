/* SPDX-License-Identifier: ISC */
#include "build_state.h"

#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

const char *build_phase_name(BuildPhase phase)
{
    switch (phase) {
    case BuildPhase::Loaded:
        return "loaded";
    case BuildPhase::Packed:
        return "packed";
    case BuildPhase::Placed:
        return "placed";
    case BuildPhase::RoutePrepared:
        return "route-prepared";
    case BuildPhase::Routed:
        return "routed";
    case BuildPhase::Validated:
        return "validated";
    }
    return "?";
}

template <BuildPhase P> Build<P> Build<P>::adopt(Context &ctx)
{
    if (ctx.build_phase != P)
        log_error("Build phase is '%s', but this step needs '%s'.\n", build_phase_name(ctx.build_phase),
                  build_phase_name(P));
    return Build(ctx);
}

template class Build<BuildPhase::Loaded>;
template class Build<BuildPhase::Packed>;
template class Build<BuildPhase::Placed>;
template class Build<BuildPhase::RoutePrepared>;
template class Build<BuildPhase::Routed>;
template class Build<BuildPhase::Validated>;

Build<BuildPhase::Placed> place_build(Build<BuildPhase::Packed> &&packed)
{
    Context &ctx = packed.context();
    if (!ctx.run_placement())
        log_error("Placement failed.\n");
    ctx.build_phase = BuildPhase::Placed;
    return Build<BuildPhase::Placed>(ctx);
}

Build<BuildPhase::RoutePrepared> prepare_build(Build<BuildPhase::Placed> &&placed)
{
    Context &ctx = placed.context();
    ctx.prepare_route();
    ctx.build_phase = BuildPhase::RoutePrepared;
    return Build<BuildPhase::RoutePrepared>(ctx);
}

Build<BuildPhase::Routed> route_build(Build<BuildPhase::RoutePrepared> &&prepared)
{
    Context &ctx = prepared.context();
    if (!ctx.run_router_phase())
        log_error("Routing failed.\n");
    ctx.build_phase = BuildPhase::Routed;
    return Build<BuildPhase::Routed>(ctx);
}

Build<BuildPhase::Validated> validate_build(Build<BuildPhase::Routed> &&routed)
{
    Context &ctx = routed.context();
    ctx.check();
    size_t arcs = 0, unrouted = 0;
    for (auto &net_pair : ctx.nets) {
        NetInfo *net = net_pair.second.get();
        if (net->driver.cell == nullptr || net->users.entries() == 0)
            continue;
        WireId source = ctx.getNetinfoSourceWire(net);
        if (source != WireId() && ctx.getBoundWireNet(source) != net) {
            ++unrouted;
            log_nonfatal_error("Net %s: source wire %s is not bound to it.\n", net->name.c_str(&ctx),
                               ctx.nameOfWire(source));
        }
        for (auto &user : net->users) {
            ++arcs;
            bool reached = false;
            for (WireId sink : ctx.getNetinfoSinkWires(net, user)) {
                // Walk sink -> source over the net's own pips.
                WireId cursor = sink;
                size_t steps = 0;
                while (cursor != WireId() && ctx.getBoundWireNet(cursor) == net && steps++ <= net->wires.size()) {
                    if (cursor == source) {
                        reached = true;
                        break;
                    }
                    auto it = net->wires.find(cursor);
                    if (it == net->wires.end() || it->second.pip == PipId())
                        break;
                    cursor = ctx.getPipSrcWire(it->second.pip);
                }
                if (reached)
                    break;
            }
            if (!reached) {
                ++unrouted;
                if (unrouted <= 8)
                    log_nonfatal_error("Net %s: arc to %s.%s is not routed.\n", net->name.c_str(&ctx),
                                       user.cell->name.c_str(&ctx), user.port.c_str(&ctx));
            }
        }
    }
    if (unrouted != 0)
        log_error("Validation failed: %zu of %zu arcs are not routed.\n", unrouted, arcs);
    log_info("Validated: %zu arcs routed, context consistent.\n", arcs);
    ctx.build_phase = BuildPhase::Validated;
    return Build<BuildPhase::Validated>(ctx);
}

NEXTPNR_NAMESPACE_END
