/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_LINES_H
#define MISTRAL_LAB_LINES_H

#include <cstdint>

#include "nextpnr.h"
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

// Stage 6 (6d): LAB input-line assignment at routing preparation.
//
// A LAB feeds its ALM input pins from 46 TD lines, and each pin can be driven by only about half
// of them (measured: the A and C pins from one group of 25, B and D from a disjoint 21, E pins
// from 22, F pins from a disjoint 24). Every LAB the input count admits has a line assignment,
// and router2's negotiated congestion does not always find it: the paired exec probe sat on one
// overused wire for 80 iterations. So once routing preparation has fixed the pins, each LAB's
// nets are matched to lines here, from the routing graph itself (the lines that can drive all
// of a net's pins in the LAB; a net whose pins share no line is split), with augmenting paths
// over at most 46 lines, and every pin is bound through its line's pip at STRENGTH_PLACER.
// Router2 never rips placer-strength bindings up, and Arch::checkPipAvailForNet admits only
// the reserved pip into a reserved pin, so no other net has a use for the line and the router
// has only to reach it from the fabric. The line itself stays unbound, because router2 cannot
// drive a wire that arrives bound without a pip. A LAB with no
// perfect matching is left to the router and counted. Off by default (`--lab-input-lines`).
struct LabLinesReport
{
    uint64_t labs = 0;            // LABs with at least one LAB-input sink
    uint64_t nets = 0;            // nets given a line
    uint64_t lines = 0;           // lines reserved (a split net takes more than one)
    uint64_t split_nets = 0;      // nets whose pins share no line
    uint64_t pins = 0;            // pins bound to their line
    uint64_t already_bound = 0;   // pins that arrived bound (route reuse, globals), kept as they are
    uint64_t labs_infeasible = 0; // LABs without a perfect matching, left to the router
};

LabLinesReport assign_lab_input_lines(Context &ctx);
void report_lab_input_lines(const LabLinesReport &r);

NEXTPNR_NAMESPACE_END

#endif
