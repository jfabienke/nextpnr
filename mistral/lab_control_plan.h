/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_CONTROL_PLAN_H
#define MISTRAL_LAB_CONTROL_PLAN_H

#include <array>
#include <optional>
#include "archdefs.h"
#include "lab_control_abi.h"

NEXTPNR_NAMESPACE_BEGIN

struct Arch;

// Host-native allocation in the same order as the V1 wire result. This type is
// deliberately independent of both the legacy worker and the transport ABI.
struct LabControlAllocation
{
    ControlSig clk{}, sload{}, sclr{};
    std::array<ControlSig, 2> aclr{};
    std::array<ControlSig, 3> ena{};
    std::array<ControlSig, 4> datain{};

    std::array<ControlSig, NPNR_LAB_ALLOCATION_COUNT> as_array() const;
    static LabControlAllocation from_array(const std::array<ControlSig, NPNR_LAB_ALLOCATION_COUNT> &signals);
};

struct LabControlEvaluation
{
    bool legal = false;
    // An illegal evaluation must not publish an allocation for preparation.
    std::optional<LabControlAllocation> allocation;
};

LabControlEvaluation evaluate_lab_controls_native(const Arch &arch, uint32_t lab);
void consume_lab_control_allocation(Arch &arch, uint32_t lab, const LabControlAllocation &allocation);

NEXTPNR_NAMESPACE_END
#endif
