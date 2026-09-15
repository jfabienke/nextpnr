/* SPDX-License-Identifier: ISC */
#include "lab_reuse.h"

#include <cinttypes>
#include <cstring>

#include "arch.h"
#include "lab_v2.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

const char *lab_reuse_mode_name(LabReuseMode mode)
{
    switch (mode) {
    case LabReuseMode::Off:
        return "off";
    case LabReuseMode::Shadow:
        return "shadow";
    case LabReuseMode::On:
        return "on";
    case LabReuseMode::Content:
        return "content";
    }
    return "?";
}

const char *lab_reuse_state_name(LabReuseState state)
{
    switch (state) {
    case LabReuseState::Dirty:
        return "dirty";
    case LabReuseState::Evaluated:
        return "evaluated";
    case LabReuseState::Prepared:
        return "prepared";
    case LabReuseState::Routed:
        return "routed";
    }
    return "?";
}

LabReuseState lab_reuse_state(const Arch &arch, uint32_t lab)
{
    if (lab < arch.lab_prepared.size() && arch.lab_stamp_current(lab, arch.lab_prepared[lab])) {
        if (arch.lab_routed_epoch != 0 && arch.lab_routed_epoch == arch.lab_routing_epoch)
            return LabReuseState::Routed;
        return LabReuseState::Prepared;
    }
    if (lab < arch.lab_assessments.size()) {
        const auto &entry = arch.lab_assessments[lab];
        if (entry.known != 0 && arch.lab_stamp_current(lab, LabStamp{entry.lab_version, entry.facts_epoch, true}))
            return LabReuseState::Evaluated;
    }
    return LabReuseState::Dirty;
}

namespace {
enum : unsigned
{
    CHECK_INPUTS = 0,
    CHECK_CTRLSET = 1,
    CHECK_MLAB = 2
};

constexpr size_t CONTENT_SLOTS = 4096; // direct-mapped; about 15 MiB of retained facts at most

uint64_t fnv1a(const uint8_t *data, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Complete normalized whole-LAB facts with provenance zeroed: the key of the
// content tier. Everything the three live checks read is in these bytes.
std::vector<uint8_t> content_key(const Arch &arch, uint32_t lab)
{
    NpnrLabFactsV2 facts = capture_lab_v2(arch, lab, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX, 0, 0);
    facts.request_id = 0;
    facts.snapshot_epoch = 0;
    std::vector<uint8_t> bytes(sizeof(facts));
    std::memcpy(bytes.data(), &facts, sizeof(facts));
    return bytes;
}

bool live_check(const Arch &arch, uint32_t lab, unsigned which)
{
    switch (which) {
    case CHECK_INPUTS:
        return arch.check_lab_input_count(lab);
    case CHECK_CTRLSET:
        return arch.is_lab_ctrlset_legal(lab);
    default:
        return arch.check_mlab_groups(lab);
    }
}
} // namespace

bool lab_level_legal(const Arch &arch, uint32_t lab, bool need_ctrlset)
{
    const auto mode = arch.lab_reuse_effective;
    if (!arch.lab_reuse_active || mode == LabReuseMode::Off || lab >= arch.lab_assessments.size())
        return live_check(arch, lab, CHECK_INPUTS) && (!need_ctrlset || live_check(arch, lab, CHECK_CTRLSET)) &&
               live_check(arch, lab, CHECK_MLAB);

    auto &stats = arch.lab_reuse_stats;
    ++stats.queries;
    auto &entry = arch.lab_assessments[lab];
    const uint64_t version = arch.lab_versions[lab];
    const uint64_t epoch = arch.lab_facts_epoch;
    LabContentEntry *content = nullptr;
    std::vector<uint8_t> key;
    if (entry.lab_version != version || entry.facts_epoch != epoch) {
        if (entry.known != 0) {
            if (entry.lab_version != version)
                ++stats.stale_lab;
            else
                ++stats.stale_facts;
        }
        entry = LabAssessmentEntry{version, epoch, 0, 0};
        if (mode == LabReuseMode::Content) {
            // Stale stamps: consult the content tier. An identical LAB seen
            // before (this one after an ABA move, or a structurally identical
            // one elsewhere) yields its sub-results without re-evaluation.
            if (arch.lab_content_cache.empty())
                arch.lab_content_cache.resize(CONTENT_SLOTS);
            key = content_key(arch, lab);
            const uint64_t hash = fnv1a(key.data(), key.size());
            content = &arch.lab_content_cache[hash % CONTENT_SLOTS];
            ++stats.content_lookups;
            if (content->occupied && content->hash == hash && content->facts == key) {
                ++stats.content_hits;
                entry.known = content->known;
                entry.legal = content->legal;
            } else {
                if (content->occupied)
                    ++stats.content_evictions;
                content->occupied = true;
                content->hash = hash;
                content->facts = key;
                content->known = 0;
                content->legal = 0;
                ++stats.content_stores;
            }
        }
    }

    auto sub_result = [&](unsigned which) -> bool {
        const uint8_t bit = uint8_t(1u << which);
        if (entry.known & bit) {
            ++stats.hits;
            const bool cached = (entry.legal & bit) != 0;
            if (mode != LabReuseMode::Shadow)
                return cached;
            const bool live = live_check(arch, lab, which);
            if (live != cached) {
                ++stats.mismatches;
                if (stats.diagnostics++ < 4)
                    log_warning("LAB reuse shadow mismatch: LAB %u check %u cached %d live %d (version %" PRIu64
                                ", epoch %" PRIu64 ").\n",
                                lab, which, int(cached), int(live), version, epoch);
            }
            return live;
        }
        ++stats.misses;
        const bool live = live_check(arch, lab, which);
        entry.known |= bit;
        if (live)
            entry.legal |= bit;
        if (content != nullptr) {
            content->known |= bit;
            if (live)
                content->legal |= bit;
        }
        return live;
    };
    return sub_result(CHECK_INPUTS) && (!need_ctrlset || sub_result(CHECK_CTRLSET)) && sub_result(CHECK_MLAB);
}

void report_lab_reuse_stats(const Arch &arch)
{
    const auto &s = arch.lab_reuse_stats;
    if (arch.lab_reuse_effective == LabReuseMode::Off && s.queries == 0)
        return;
    log_info("LAB assessment reuse (%s): queries=%" PRIu64 ", hits=%" PRIu64 ", misses=%" PRIu64 " (%.1f%% hit)\n",
             lab_reuse_mode_name(arch.lab_reuse_effective), s.queries, s.hits, s.misses,
             (s.hits + s.misses) ? 100.0 * double(s.hits) / double(s.hits + s.misses) : 0.0);
    log_info("  stale entries: lab-version=%" PRIu64 ", facts-epoch=%" PRIu64 "; invalidations: lab=%" PRIu64
             ", facts=%" PRIu64 "; shadow mismatches=%" PRIu64 "\n",
             s.stale_lab, s.stale_facts, s.lab_invalidations, s.facts_invalidations, s.mismatches);
    log_info("  precise incidence: cell=%" PRIu64 ", net=%" PRIu64 ", unbound-cell mutations ignored=%" PRIu64 "\n",
             s.precise_cell_invalidations, s.precise_net_invalidations, s.unbound_cell_mutations);
    if (arch.lab_reuse_effective == LabReuseMode::Content)
        log_info("  content tier: lookups=%" PRIu64 ", hits=%" PRIu64 " (%.1f%%), stores=%" PRIu64
                 ", evictions=%" PRIu64 ", slots=%zu\n",
                 s.content_lookups, s.content_hits,
                 s.content_lookups ? 100.0 * double(s.content_hits) / double(s.content_lookups) : 0.0, s.content_stores,
                 s.content_evictions, size_t(CONTENT_SLOTS));
}

NEXTPNR_NAMESPACE_END
