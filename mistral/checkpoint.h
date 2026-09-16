/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_CHECKPOINT_H
#define MISTRAL_CHECKPOINT_H

#include <string>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

// Stage 5 units 2a/2b: the versioned checkpoint (docs/mistral-checkpoint-design.md).
// The checkpoint rides in the ordinary output JSON as the top-level object
// "nextpnr_checkpoint". Beyond the design-doc sections (packing, physical) it
// records what a plain JSON reload cannot reproduce and what the reference
// trajectory depends on: the IdString table in index order, and the iteration
// order of every dict and users store the placer and router walk. With those
// restored, a resumed run is the uninterrupted run from that phase on.

// Phase ranks. -1 is an unknown phase; 0 is "no checkpoint".
int checkpoint_phase_rank(const std::string &phase);

// Phases this backend can restore today (2a: packed and placed).
bool checkpoint_phase_restorable(const std::string &phase);

NEXTPNR_NAMESPACE_END

#endif
