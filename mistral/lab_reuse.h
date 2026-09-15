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
    On      // return cached sub-results while their stamps are current
};

struct LabAssessmentEntry
{
    uint64_t lab_version = 0;
    uint64_t facts_epoch = 0;
    uint8_t known = 0; // bit 0: input budget, bit 1: control sets, bit 2: MLAB groups
    uint8_t legal = 0; // same bits, meaningful only where `known` is set
};

struct LabReuseStats
{
    uint64_t queries = 0;             // LAB-level composite queries while active
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

// Composite LAB-level check with the same short-circuit order as the live
// legacy query: input budget, then control sets (FF queries only), then MLAB.
bool lab_level_legal(const Arch &arch, uint32_t lab, bool need_ctrlset);

void report_lab_reuse_stats(const Arch &arch);

NEXTPNR_NAMESPACE_END

#endif
