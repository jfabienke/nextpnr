/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_PROFILE_H
#define MISTRAL_LAB_PROFILE_H
#include <cstddef>
#include <vector>
#include "lab_model.h"

NEXTPNR_NAMESPACE_BEGIN
struct Arch;
struct LabControlProfile
{
    struct Sample
    {
        NpnrLabControlsV1 input;
        NpnrLabControlResultV1 expected;
        uint32_t lab;
        bool preparation;
    };
    uint64_t queries = 0;
    std::vector<Sample> samples;
};
// Opt-in deterministic reservoir of live control queries; no timing is done here.
void sample_lab_controls(const Arch &arch, uint32_t lab, bool preparation);
void write_lab_control_profile(const Arch &arch);
size_t lab_control_legacy_worker_size();
NEXTPNR_NAMESPACE_END
#endif
