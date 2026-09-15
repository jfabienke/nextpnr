/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_SNAPSHOT_H
#define MISTRAL_LAB_SNAPSHOT_H

#include <array>
#include "archdefs.h"
#include "lab_model.h"

NEXTPNR_NAMESPACE_BEGIN

struct Arch;

// The snapshot is detached; the translation map is host-only and expires on live-state mutation.
struct LabControlCapture
{
    NpnrLabControlsV1 input;
    std::array<const NetInfo *, NPNR_LAB_MAX_NETS> nets{};
    NpnrControlSignalV1 encode_existing(ControlSig signal) const;
};

LabControlCapture capture_lab_controls(const Arch &arch, uint32_t lab, uint64_t request_id = 0, uint64_t epoch = 0);
NpnrLabControlResultV1 evaluate_lab_controls_legacy(const Arch &arch, uint32_t lab, const LabControlCapture &capture);

// Debug verification: the independent live worker remains authoritative. No state is modified.
bool verify_lab_controls_cpp(const Arch &arch, uint32_t lab);

NEXTPNR_NAMESPACE_END
#endif
