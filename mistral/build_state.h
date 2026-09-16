/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_BUILD_STATE_H
#define MISTRAL_BUILD_STATE_H

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct Context;

// Stage 5 (3a): the typed build state machine, in C++. A Build<P> is a
// move-only handle over the context that exists only while the context is
// in phase P; the transitions consume their input, so a caller cannot keep
// using an obsolete phase, and a transition that does not exist (routing a
// packed build) does not compile. The legacy entry points (Arch::place,
// Arch::route, the bitstream writer) adopt the context into the phase they
// expect, which checks at run time what the types cannot see across that
// boundary, and then run through these transitions. A checkpoint restore
// sets the phase it restored; only the checked loader constructs it.
enum class BuildPhase
{
    Loaded,        // netlist imported, not packed
    Packed,        // Arch::pack() complete
    Placed,        // placement complete
    RoutePrepared, // LAB preparation and globals complete, router not run
    Routed,        // router complete
    Validated      // checked: consistent and every arc routed; bitstream may be built
};

const char *build_phase_name(BuildPhase phase);

template <BuildPhase P> class Build
{
  public:
    // Runtime check at the legacy boundary: the context must be in phase P.
    static Build adopt(Context &ctx);
    Context &context() const { return *ctx_; }
    Build(Build &&) noexcept = default;
    Build &operator=(Build &&) noexcept = default;
    Build(const Build &) = delete;
    Build &operator=(const Build &) = delete;

  private:
    explicit Build(Context &ctx) : ctx_(&ctx) {}
    Context *ctx_;
    template <BuildPhase Q> friend class Build;
    friend Build<BuildPhase::Placed> place_build(Build<BuildPhase::Packed> &&);
    friend Build<BuildPhase::RoutePrepared> prepare_build(Build<BuildPhase::Placed> &&);
    friend Build<BuildPhase::Routed> route_build(Build<BuildPhase::RoutePrepared> &&);
    friend Build<BuildPhase::Validated> validate_build(Build<BuildPhase::Routed> &&);
};

// Transitions. Each consumes its input and fails through log_error, which
// leaves no handle behind: there is no allegedly intact prior phase.
Build<BuildPhase::Placed> place_build(Build<BuildPhase::Packed> &&packed);
Build<BuildPhase::RoutePrepared> prepare_build(Build<BuildPhase::Placed> &&placed);
Build<BuildPhase::Routed> route_build(Build<BuildPhase::RoutePrepared> &&prepared);
// Validation: the context is internally consistent and every arc of every
// driven net reaches a sink wire through bound pips. Bitstream generation
// requires it.
Build<BuildPhase::Validated> validate_build(Build<BuildPhase::Routed> &&routed);

extern template class Build<BuildPhase::Loaded>;
extern template class Build<BuildPhase::Packed>;
extern template class Build<BuildPhase::Placed>;
extern template class Build<BuildPhase::RoutePrepared>;
extern template class Build<BuildPhase::Routed>;
extern template class Build<BuildPhase::Validated>;

NEXTPNR_NAMESPACE_END

#endif
