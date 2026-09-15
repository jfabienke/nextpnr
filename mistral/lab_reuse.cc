/* SPDX-License-Identifier: ISC */
#include "lab_reuse.h"

#include <cinttypes>

#include "arch.h"
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
    }
    return "?";
}

namespace {
enum : unsigned
{
    CHECK_INPUTS = 0,
    CHECK_CTRLSET = 1,
    CHECK_MLAB = 2
};

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
    if (entry.lab_version != version || entry.facts_epoch != epoch) {
        if (entry.known != 0) {
            if (entry.lab_version != version)
                ++stats.stale_lab;
            else
                ++stats.stale_facts;
        }
        entry = LabAssessmentEntry{version, epoch, 0, 0};
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
}

NEXTPNR_NAMESPACE_END
