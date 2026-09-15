/* SPDX-License-Identifier: ISC */
#include "lab_profile.h"
#include <cinttypes>
#include <fstream>
#include "lab_replay.h"
#include "lab_snapshot.h"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN
void sample_lab_controls(const Arch &arch, uint32_t lab, bool preparation)
{
    constexpr size_t capacity = 4096;
    auto &profile = arch.lab_control_profile;
    uint64_t seen = ++profile.queries;
    // SplitMix64 keyed by query order: deterministic reservoir without touching
    // placement RNG state. Modulo bias at supported run sizes is negligible.
    uint64_t random = seen + 0x9e3779b97f4a7c15ull;
    random = (random ^ (random >> 30)) * 0xbf58476d1ce4e5b9ull;
    random = (random ^ (random >> 27)) * 0x94d049bb133111ebull;
    random ^= random >> 31;
    const auto index = seen <= capacity ? seen - 1 : random % seen;
    if (index >= capacity)
        return;
    if (profile.samples.empty())
        profile.samples.reserve(capacity);
    const auto capture = capture_lab_controls(arch, lab, seen);
    LabControlProfile::Sample sample{capture.input, evaluate_lab_controls_legacy(arch, lab, capture), lab, preparation};
    if (index < profile.samples.size())
        profile.samples[index] = sample;
    else
        profile.samples.push_back(sample);
}

void write_lab_control_profile(const Arch &arch)
{
    if (arch.args.lab_control_profile_path.empty())
        return;
    auto stream = open_ofstream_and_log_error(arch.args.lab_control_profile_path, "LAB profiling corpus");
    for (const auto &sample : arch.lab_control_profile.samples)
        if (!write_lab_control_replay(stream, sample.input, sample.expected,
                                      "live LAB " + std::to_string(sample.lab) +
                                              (sample.preparation ? " preparation" : " placement")))
            log_error("Failed to write LAB profiling corpus.\n");
    log_info("LAB profiling corpus: queries=%" PRIu64 " samples=%zu bytes=%zu.\n", arch.lab_control_profile.queries,
             arch.lab_control_profile.samples.size(),
             arch.lab_control_profile.samples.capacity() * sizeof(LabControlProfile::Sample));
}
NEXTPNR_NAMESPACE_END
