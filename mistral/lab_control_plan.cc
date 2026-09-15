/* SPDX-License-Identifier: ISC */
#include "lab_control_plan.h"

#include <algorithm>
#include "lab_control_edits.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
bool check_assign_sig(ControlSig &sig_set, const ControlSig &sig)
{
    if (sig.net == nullptr || sig_set == sig)
        return true;
    if (sig_set.net == nullptr) {
        sig_set = sig;
        return true;
    }
    return false;
}

template <size_t N> bool check_assign_sig(std::array<ControlSig, N> &sig_set, const ControlSig &sig)
{
    if (sig.net == nullptr)
        return true;
    for (size_t i = 0; i < N; i++) {
        if (sig_set[i] == sig)
            return true;
        if (sig_set[i].net == nullptr) {
            sig_set[i] = sig;
            return true;
        }
    }
    return false;
}

// DATAIN mapping rules: which LAB DATAIN signals can be used for ENA and ACLR.
constexpr std::array<int, 3> ena_datain{2, 3, 0};
constexpr std::array<int, 2> aclr_datain{3, 2};
} // namespace

std::array<ControlSig, NPNR_LAB_ALLOCATION_COUNT> LabControlAllocation::as_array() const
{
    return {clk, sload, sclr, aclr[0], aclr[1], ena[0], ena[1], ena[2], datain[0], datain[1], datain[2], datain[3]};
}

LabControlAllocation LabControlAllocation::from_array(const std::array<ControlSig, NPNR_LAB_ALLOCATION_COUNT> &signals)
{
    LabControlAllocation allocation;
    allocation.clk = signals[0];
    allocation.sload = signals[1];
    allocation.sclr = signals[2];
    std::copy_n(signals.begin() + 3, allocation.aclr.size(), allocation.aclr.begin());
    std::copy_n(signals.begin() + 5, allocation.ena.size(), allocation.ena.begin());
    std::copy_n(signals.begin() + 8, allocation.datain.size(), allocation.datain.begin());
    return allocation;
}

namespace {
template <typename Bound> LabControlEvaluation evaluate_lab_controls_with(const Arch &arch, uint32_t lab, Bound bound)
{
    LabControlAllocation allocation;
    // Strictly speaking the constraint is up to 2 unique CLK and 3 CLK+ENA
    // pairs. Preserve the existing conservative model of 1 CLK and 3 ENA.
    for (uint8_t alm = 0; alm < 10; alm++) {
        for (uint8_t i = 0; i < 4; i++) {
            const CellInfo *ff = bound(arch.labs.at(lab).alms.at(alm).ff_bels.at(i));
            if (ff == nullptr)
                continue;
            if (!check_assign_sig(allocation.clk, ff->ffInfo.ctrlset.clk) ||
                !check_assign_sig(allocation.sload, ff->ffInfo.ctrlset.sload) ||
                !check_assign_sig(allocation.sclr, ff->ffInfo.ctrlset.sclr) ||
                !check_assign_sig(allocation.aclr, ff->ffInfo.ctrlset.aclr) ||
                !check_assign_sig(allocation.ena, ff->ffInfo.ctrlset.ena))
                return {};
        }
    }
    if (allocation.clk.net != nullptr && !allocation.clk.net->is_global &&
        !check_assign_sig(allocation.datain[0], allocation.clk))
        return {};
    if (!check_assign_sig(allocation.datain[1], allocation.sload) ||
        !check_assign_sig(allocation.datain[3], allocation.sclr))
        return {};
    for (const auto &signal : allocation.aclr) {
        if (check_assign_sig(allocation.datain[aclr_datain[0]], signal) ||
            check_assign_sig(allocation.datain[aclr_datain[1]], signal))
            continue;
        return {};
    }
    for (const auto &signal : allocation.ena) {
        if (check_assign_sig(allocation.datain[ena_datain[0]], signal) ||
            check_assign_sig(allocation.datain[ena_datain[1]], signal) ||
            check_assign_sig(allocation.datain[ena_datain[2]], signal))
            continue;
        return {};
    }
    return {true, allocation};
}
} // namespace

LabControlEvaluation evaluate_lab_controls_native(const Arch &arch, uint32_t lab)
{
    return evaluate_lab_controls_with(arch, lab, [&](BelId b) { return arch.getBoundBelCell(b); });
}

LabControlEvaluation evaluate_lab_controls_native_overlay(const Arch &arch, uint32_t lab, const BelOverlay &overlay)
{
    return evaluate_lab_controls_with(arch, lab, [&](BelId b) { return overlay.lookup(b, arch.getBoundBelCell(b)); });
}

void consume_lab_control_allocation(Arch &arch, uint32_t lab, const LabControlAllocation &allocation)
{
    ValidatedControlPlan plan{lab, 0, allocation};
    auto prepared = prepare_control_edits(arch, plan);
    NPNR_ASSERT(prepared.status == ControlEditStatus::Ready && prepared.prepared);
    NPNR_ASSERT(apply_prepared_control_edits(arch, std::move(*prepared.prepared)) == ControlEditStatus::Applied);
}

NEXTPNR_NAMESPACE_END
