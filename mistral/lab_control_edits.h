/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_CONTROL_EDITS_H
#define MISTRAL_LAB_CONTROL_EDITS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "lab_preparation.h"

NEXTPNR_NAMESPACE_BEGIN

enum class ControlEditKind : uint8_t
{
    WireFlags,
    AclrUsed,
    ClkEnaIndex,
    AclrIndex
};

struct PreparedControlEdit
{
    ControlEditKind kind;
    WireId wire{};
    uint8_t alm = 0;
    uint8_t index = 0;
    uint64_t expected = 0;
    uint64_t value = 0;
};

struct PreparedControlEdits
{
    uint32_t lab;
    uint64_t request_id;
    std::vector<PreparedControlEdit> edits;
};

enum class ControlEditStatus
{
    Ready,
    Applied,
    MissingRoute,
    ChangedValue,
    Interrupted
};

struct ControlEditPreparation
{
    ControlEditStatus status = ControlEditStatus::MissingRoute;
    std::optional<PreparedControlEdits> prepared;
};

ControlEditPreparation prepare_control_edits(const Arch &arch, const ValidatedControlPlan &plan);
ControlEditStatus apply_prepared_control_edits(Arch &arch, PreparedControlEdits &&prepared,
                                               size_t interrupt_after = SIZE_MAX);
bool control_edit_lists_equal(const PreparedControlEdits &a, const PreparedControlEdits &b);

NEXTPNR_NAMESPACE_END
#endif
