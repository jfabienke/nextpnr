/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ROUTE_REUSE_H
#define MISTRAL_ROUTE_REUSE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct Context;

// Stage 5 (3c): conservative route reuse for an edited design.
//
// A previous run's routes (a routed checkpoint's `physical.routes`, or the
// `ROUTING` attributes of any routed nextpnr output) are matched to the
// current nets by name. A route is preserved only when every check passes
// under the current design: the net's source wire is the route's source,
// every current sink is a leaf of the route, the route has no other leaf,
// every wire is named and free, and every pip is available under the current
// reservations and blocked wires. Preserved routes are bound at
// STRENGTH_STRONG: router2 records them as pre-routed arcs it never
// revisits and keeps their wires from other nets, so the dirty nets route
// around them; its final pass rebinds them weak like its own work. Nets the
// global router already routed are left to it. If the router then fails,
// every preserved route is dropped and the router runs again from the same
// RNG state.

struct PreviousRouteEntry
{
    std::string wire;
    std::string pip; // empty for the source wire
    int strength = 1;
};

// Routes per net name, each in the previous run's `wires` map order.
using PreviousRoutes = std::unordered_map<std::string, std::vector<PreviousRouteEntry>>;

struct RouteReuseReport
{
    uint64_t previous_nets = 0;
    uint64_t current_nets = 0;
    uint64_t already_routed = 0;    // current nets the global router bound before reuse ran
    uint64_t no_previous = 0;       // no previous net of that name
    uint64_t endpoint_mismatch = 0; // source or sinks differ from the previous route
    uint64_t unresolved = 0;        // a wire or pip name the device does not have
    uint64_t unavailable = 0;       // a wire already bound, or a pip the current flags forbid
    uint64_t reused = 0;
    uint64_t wires_bound = 0;
    uint64_t dropped = 0; // reused routes unbound by the fallback
    std::vector<std::string> reused_names;
};

PreviousRoutes load_previous_routes(const std::string &path);

// Must run after routing preparation (LAB reservations and globals) and
// before the router. Never mutates a net whose checks fail.
RouteReuseReport apply_route_reuse(Context &ctx, const PreviousRoutes &previous);
RouteReuseReport apply_route_reuse(Context &ctx, const std::string &path);

// Fallback: unbind every route the report preserved.
void drop_reused_routes(Context &ctx, RouteReuseReport &report);

// Fallback, second step: unbind every wire and pip bound below
// STRENGTH_LOCKED, so that only the global router's work remains. A router
// that gave up may have left a legalised routing behind for every net;
// routing from scratch means from the state the uninterrupted run starts
// its router in. Returns the number of nets unrouted.
size_t unroute_below_locked(Context &ctx);

void report_route_reuse(const RouteReuseReport &report);

NEXTPNR_NAMESPACE_END

#endif
