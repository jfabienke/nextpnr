/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_DISPATCH_H
#define MISTRAL_LAB_DISPATCH_H

#include <array>
#include <atomic>
#include "lab_model.h"

NEXTPNR_NAMESPACE_BEGIN
struct Arch;
struct LabControlCapture;

enum class LabControlMode
{
    Legacy,
    Shadow,
    Verify,
    Rust
};

struct LabControlStats
{
    std::atomic<uint64_t> evaluations{0}, preparation{0}, legal{0}, illegal{0};
    std::atomic<uint64_t> errors{0}, mismatches{0}, fallbacks{0}, diagnostics{0};
    std::array<std::atomic<uint64_t>, 109> reasons{};
};

const char *lab_control_mode_name(LabControlMode mode);
void require_lab_control_mode(LabControlMode mode);
bool dispatch_lab_controls(const Arch &arch, uint32_t lab);
void report_lab_control_stats(const Arch &arch);
bool lab_control_result_valid(const NpnrLabControlsV1 &input, const NpnrLabControlResultV1 &result);
// Host acceptance policy, separate from transport so failure paths can be tested.
bool accept_lab_control_candidate(const Arch &arch, uint32_t lab, const LabControlCapture &capture, uint32_t call,
                                  const NpnrLabControlResultV1 &candidate);

NEXTPNR_NAMESPACE_END
#endif
