/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_PREPARATION_H
#define MISTRAL_LAB_PREPARATION_H

#include <optional>
#include "lab_control_plan.h"
#include "lab_snapshot.h"

NEXTPNR_NAMESPACE_BEGIN

enum class PreparationStatus
{
    Legal,
    Illegal,
    Unsupported,
    TransportFailure,
    MalformedResult,
    Mismatch
};

class PreparationTicket;
struct PreparationTranslation;
PreparationTranslation translate_preparation_ticket(PreparationTicket &&ticket);

class PreparationTicket
{
  public:
    PreparationTicket(LabControlCapture capture, uint32_t lab, uint64_t request_id,
                      NpnrLabControlResultV1 selected_result,
                      std::optional<NpnrLabControlResultV1> comparison_result = std::nullopt);
    PreparationTicket(const PreparationTicket &) = delete;
    PreparationTicket &operator=(const PreparationTicket &) = delete;
    PreparationTicket(PreparationTicket &&) = default;
    PreparationTicket &operator=(PreparationTicket &&) = default;

  private:
    LabControlCapture capture_;
    uint32_t lab_;
    uint64_t request_id_;
    NpnrLabControlResultV1 selected_result_;
    std::optional<NpnrLabControlResultV1> comparison_result_;

    friend PreparationTranslation translate_preparation_ticket(PreparationTicket &&ticket);
};

struct PreparationDispatchResult
{
    PreparationStatus selected_status = PreparationStatus::MalformedResult;
    PreparationStatus comparison_status = PreparationStatus::MalformedResult;
    std::optional<PreparationTicket> ticket;
};

struct ValidatedControlPlan
{
    uint32_t lab;
    uint64_t request_id;
    LabControlAllocation allocation;
};

struct PreparationTranslation
{
    PreparationStatus status = PreparationStatus::MalformedResult;
    std::optional<ValidatedControlPlan> plan;
    std::optional<ValidatedControlPlan> comparison_plan;
};

const char *preparation_status_name(PreparationStatus status);
PreparationDispatchResult dispatch_lab_controls_for_preparation(const Arch &arch, uint32_t lab);

#ifndef NO_RUST
// Host acceptance seam for malformed, transport, mismatch, and mode-authority tests.
PreparationDispatchResult accept_lab_control_preparation_candidate(const Arch &arch, uint32_t lab,
                                                                   LabControlCapture capture, uint32_t call,
                                                                   const NpnrLabControlResultV1 &candidate);
#endif

NEXTPNR_NAMESPACE_END
#endif
