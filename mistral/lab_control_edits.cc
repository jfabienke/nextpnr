/* SPDX-License-Identifier: ISC */
#include "lab_control_edits.h"

#include <algorithm>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
constexpr std::array<int, 3> ena_datain{2, 3, 0};
constexpr std::array<int, 2> aclr_datain{3, 2};

bool same_target(const PreparedControlEdit &a, const PreparedControlEdit &b)
{
    if (a.kind != b.kind)
        return false;
    if (a.kind == ControlEditKind::WireFlags)
        return a.wire == b.wire;
    if (a.kind == ControlEditKind::AclrUsed)
        return a.index == b.index;
    return a.alm == b.alm && a.index == b.index;
}

uint64_t read_value(const Arch &arch, uint32_t lab, const PreparedControlEdit &edit)
{
    const auto &lab_data = arch.labs.at(lab);
    switch (edit.kind) {
    case ControlEditKind::WireFlags:
        return arch.wire_flags(edit.wire);
    case ControlEditKind::AclrUsed:
        return lab_data.aclr_used.at(edit.index);
    case ControlEditKind::ClkEnaIndex:
        return uint64_t(int64_t(lab_data.alms.at(edit.alm).clk_ena_idx.at(edit.index)));
    case ControlEditKind::AclrIndex:
        return uint64_t(int64_t(lab_data.alms.at(edit.alm).aclr_idx.at(edit.index)));
    }
    NPNR_ASSERT_FALSE("unknown control edit");
}

void write_value(Arch &arch, uint32_t lab, const PreparedControlEdit &edit, uint64_t value)
{
    auto &lab_data = arch.labs.at(lab);
    switch (edit.kind) {
    case ControlEditKind::WireFlags:
        arch.set_wire_flags(edit.wire, value);
        return;
    case ControlEditKind::AclrUsed:
        lab_data.aclr_used.at(edit.index) = bool(value);
        return;
    case ControlEditKind::ClkEnaIndex:
        lab_data.alms.at(edit.alm).clk_ena_idx.at(edit.index) = int(int64_t(value));
        return;
    case ControlEditKind::AclrIndex:
        lab_data.alms.at(edit.alm).aclr_idx.at(edit.index) = int(int64_t(value));
        return;
    }
    NPNR_ASSERT_FALSE("unknown control edit");
}

uint64_t planned_value(const Arch &arch, uint32_t lab, const std::vector<PreparedControlEdit> &edits,
                       const PreparedControlEdit &target)
{
    for (auto it = edits.rbegin(); it != edits.rend(); ++it)
        if (same_target(*it, target))
            return it->value;
    return read_value(arch, lab, target);
}
} // namespace

ControlEditPreparation prepare_control_edits(const Arch &arch, const ValidatedControlPlan &plan)
{
    if (plan.lab >= arch.labs.size())
        return {};
    PreparedControlEdits prepared{plan.lab, plan.request_id, {}};
    prepared.edits.reserve(2 + 10 * (4 + 4 * 6));
    const auto &lab_data = arch.labs.at(plan.lab);

    auto append = [&](PreparedControlEdit edit) {
        edit.expected = planned_value(arch, plan.lab, prepared.edits, edit);
        prepared.edits.push_back(edit);
    };
    auto reserve = [&](WireId source, WireId destination) {
        const auto &uphill = arch.wires.at(destination).wires_uphill;
        const auto found = std::find(uphill.begin(), uphill.end(), source);
        if (found == uphill.end())
            return false;
        PreparedControlEdit edit{ControlEditKind::WireFlags};
        edit.wire = destination;
        edit.value = WireInfo::RESERVED_ROUTE | unsigned(found - uphill.begin());
        append(edit);
        return true;
    };

    for (uint8_t i = 0; i < 2; ++i) {
        PreparedControlEdit edit{ControlEditKind::AclrUsed};
        edit.index = i;
        edit.value = 0;
        append(edit);
    }
    for (uint8_t alm = 0; alm < 10; ++alm) {
        const auto &alm_data = lab_data.alms.at(alm);
        if (lab_data.is_mlab) {
            for (uint8_t i = 0; i < 2; ++i) {
                const BelId lut_bel = alm_data.lut_bels.at(i);
                const CellInfo *lut = arch.getBoundBelCell(lut_bel);
                if (!lut || lut->combInfo.mlab_group == -1)
                    continue;
                if (!reserve(lab_data.clk_wires[0], arch.getBelPinWire(lut_bel, id_WCLK)) ||
                    !reserve(lab_data.ena_wires[0], arch.getBelPinWire(lut_bel, id_WE)))
                    return {ControlEditStatus::MissingRoute, std::nullopt};
            }
        }
        for (uint8_t i = 0; i < 4; ++i) {
            const BelId ff_bel = alm_data.ff_bels.at(i);
            const CellInfo *ff = arch.getBoundBelCell(ff_bel);
            if (!ff)
                continue;
            for (uint8_t j = 0; j < 3; ++j) {
                if (ff->ffInfo.ctrlset.ena == plan.allocation.datain[ena_datain[j]]) {
                    if (!reserve(lab_data.clk_wires[0], arch.getBelPinWire(ff_bel, id_CLK)) ||
                        !reserve(lab_data.ena_wires[j], arch.getBelPinWire(ff_bel, id_ENA)))
                        return {ControlEditStatus::MissingRoute, std::nullopt};
                    PreparedControlEdit edit{ControlEditKind::ClkEnaIndex};
                    edit.alm = alm;
                    edit.index = i / 2;
                    edit.value = j;
                    append(edit);
                    break;
                }
            }
            for (uint8_t j = 0; j < 2; ++j) {
                const auto clear = ff->ffInfo.ctrlset.aclr;
                if (clear == plan.allocation.datain[aclr_datain[j]]) {
                    if (!reserve(lab_data.aclr_wires[j], arch.getBelPinWire(ff_bel, id_ACLR)))
                        return {ControlEditStatus::MissingRoute, std::nullopt};
                    PreparedControlEdit used{ControlEditKind::AclrUsed};
                    used.index = j;
                    used.value = clear.net != nullptr;
                    append(used);
                    PreparedControlEdit index{ControlEditKind::AclrIndex};
                    index.alm = alm;
                    index.index = i / 2;
                    index.value = j;
                    append(index);
                    break;
                }
            }
        }
    }
    return {ControlEditStatus::Ready, std::move(prepared)};
}

ControlEditStatus apply_prepared_control_edits(Arch &arch, PreparedControlEdits &&prepared, size_t interrupt_after)
{
    std::vector<PreparedControlEdit> undo;
    undo.reserve(prepared.edits.size());
    auto rollback = [&] {
        for (auto it = undo.rbegin(); it != undo.rend(); ++it)
            write_value(arch, prepared.lab, *it, it->expected);
    };
    for (size_t i = 0; i < prepared.edits.size(); ++i) {
        if (i == interrupt_after) {
            rollback();
            return ControlEditStatus::Interrupted;
        }
        const auto &edit = prepared.edits[i];
        if (read_value(arch, prepared.lab, edit) != edit.expected) {
            rollback();
            return ControlEditStatus::ChangedValue;
        }
        bool journaled = false;
        for (const auto &entry : undo)
            journaled |= same_target(entry, edit);
        if (!journaled) {
            auto entry = edit;
            entry.expected = edit.expected;
            undo.push_back(entry);
        }
        write_value(arch, prepared.lab, edit, edit.value);
    }
    return ControlEditStatus::Applied;
}

bool control_edit_lists_equal(const PreparedControlEdits &a, const PreparedControlEdits &b)
{
    if (a.lab != b.lab || a.request_id != b.request_id || a.edits.size() != b.edits.size())
        return false;
    for (size_t i = 0; i < a.edits.size(); ++i) {
        const auto &x = a.edits[i];
        const auto &y = b.edits[i];
        if (x.kind != y.kind || x.wire != y.wire || x.alm != y.alm || x.index != y.index || x.expected != y.expected ||
            x.value != y.value)
            return false;
    }
    return true;
}

NEXTPNR_NAMESPACE_END
