/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_REUSE_H
#define MISTRAL_LAB_REUSE_H

#include <cstdint>
#include <vector>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct Arch;

// Stage 4E-1: same-session reuse of the LAB-level legality sub-results that
// the live legacy query composes (LAB input budget, FF control sets, MLAB
// grouping). Each LAB carries a monotonic binding version bumped by every
// bind/unbind of one of its BELs, and the whole design carries a facts epoch
// bumped by every audited connectivity, cell-fact, net-fact, constraint, or
// generated-object mutation and by Mistral's own fact rewrites. A cached
// sub-result is reused only while both stamps still match; returning to an
// identical occupancy never returns to an old version (ABA-safe).
enum class LabReuseMode
{
    Off,    // live evaluation only (default)
    Shadow, // evaluate live, compare against the cache, count mismatches
    On,     // return cached sub-results while their stamps are current
    Content // On, plus a bounded content-keyed tier consulted when the stamps are stale
};

// Explicit per-LAB state (design 6.7). Derived from stamps, never stored
// independently, so it cannot disagree with the invalidation rules.
enum class LabReuseState
{
    Dirty,     // no current assessment
    Evaluated, // a current assessment exists for at least one sub-result
    Prepared,  // control preparation ran against the current stamps
    Routed     // prepared, and no routing mutation since routing completed
};

struct LabStamp
{
    uint64_t lab_version = 0;
    uint64_t facts_epoch = 0;
    bool valid = false;
};

struct LabAssessmentEntry
{
    uint64_t lab_version = 0;
    uint64_t facts_epoch = 0;
    uint8_t known = 0; // bit 0: input budget, bit 1: control sets, bit 2: MLAB groups
    uint8_t legal = 0; // same bits, meaningful only where `known` is set
};

// One slot of the content tier: the complete normalized whole-LAB facts are
// stored and compared in full on a hit; the hash only selects the slot.
struct LabContentEntry
{
    uint64_t hash = 0;
    uint8_t known = 0;
    uint8_t legal = 0;
    bool occupied = false;
    std::vector<uint8_t> facts; // NpnrLabFactsV2 bytes with provenance fields zeroed
};

struct LabReuseStats
{
    uint64_t queries = 0;                    // LAB-level composite queries while active
    uint64_t precise_cell_invalidations = 0; // a bound cell's facts or connectivity changed: its LAB only
    uint64_t precise_net_invalidations = 0;  // a net's facts changed: LABs of its bound driver/users only
    uint64_t unbound_cell_mutations = 0;     // mutations on cells no LAB depends on: nothing invalidated
    uint64_t content_lookups = 0;
    uint64_t content_hits = 0;
    uint64_t content_stores = 0;
    uint64_t content_evictions = 0;
    uint64_t hits = 0;                // sub-results served from a current entry
    uint64_t misses = 0;              // sub-results evaluated live and stored
    uint64_t stale_lab = 0;           // entries dropped because their LAB version moved
    uint64_t stale_facts = 0;         // entries dropped because the facts epoch moved
    uint64_t lab_invalidations = 0;   // per-LAB version bumps while active
    uint64_t facts_invalidations = 0; // facts epoch bumps while active
    uint64_t mismatches = 0;          // shadow: cached sub-result differed from live
    uint64_t diagnostics = 0;
};

const char *lab_reuse_mode_name(LabReuseMode mode);
const char *lab_reuse_state_name(LabReuseState state);
LabReuseState lab_reuse_state(const Arch &arch, uint32_t lab);

// Composite LAB-level check with the same short-circuit order as the live
// legacy query: input budget, then control sets (FF queries only), then MLAB.
bool lab_level_legal(const Arch &arch, uint32_t lab, bool need_ctrlset);

void report_lab_reuse_stats(const Arch &arch);

NEXTPNR_NAMESPACE_END

#endif
