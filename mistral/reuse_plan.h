/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_REUSE_PLAN_H
#define MISTRAL_REUSE_PLAN_H

#include <cstdint>
#include <string>
#include <vector>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct Context;

// Stage 5 (3a): the reuse plan. Every decision about reusing a previous
// run's placement or routing is recorded here before anything is applied,
// with the reason, so a plan can be emitted without being applied
// (--reuse-plan-out, --reuse-dry-run), compared against a controlled edit,
// and revised by the 3b region expansion. C++ validates each decision when
// it applies it; the plan is a record, not an authority.
enum class ReuseDecision
{
    Reuse,            // previous artifact adopted
    Changed,          // same name, different signature (cells)
    Added,            // no previous object of that name
    UserConstrained,  // a user or packer binding wins over the previous placement
    MissingBel,       // the previous BEL does not resolve on this device
    Released,         // reusable, but released by region expansion after a failed repair
    AlreadyRouted,    // the global router bound the net before reuse ran
    EndpointMismatch, // source or sinks differ from the previous route
    Unresolved,       // a wire or pip name this device does not have
    Unavailable       // a wire another net holds, or a pip the current flags forbid
};

const char *reuse_decision_name(ReuseDecision d);

struct CellReuseDecision
{
    std::string cell;
    ReuseDecision decision;
    std::string bel;    // previous BEL when known
    std::string reason; // human-readable, one line
};

struct NetReuseDecision
{
    std::string net;
    ReuseDecision decision;
    size_t wires = 0; // entries in the previous route
    std::string reason;
    int survived = -1; // after the router: 1 bound exactly as applied, 0 re-routed, -1 not measured
};

struct ReusePlan
{
    std::string previous_path;
    std::vector<CellReuseDecision> cells;
    std::vector<NetReuseDecision> nets;
    uint64_t previous_cells = 0, previous_routethru = 0, previous_nets = 0;
    uint64_t removed_cells = 0;           // previous cells with no current counterpart
    unsigned placement_attempts = 0;      // 3b: placer attempts, 1 when the first succeeded
    unsigned released_cells = 0;          // 3b: transplants released by region expansion
    bool placement_full_fallback = false; // 3b: every transplant released

    uint64_t count_cells(ReuseDecision d) const;
    uint64_t count_nets(ReuseDecision d) const;
};

// Serialises the plan as JSON: the decisions per cell and net with reasons,
// and the counts. Written to a temporary name and renamed.
void write_reuse_plan(const ReusePlan &plan, const std::string &path);

NEXTPNR_NAMESPACE_END

#endif
