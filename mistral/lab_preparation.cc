/* SPDX-License-Identifier: ISC */
#include "lab_preparation.h"

#include <cinttypes>
#include <sstream>
#include "lab_replay.h"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

PreparationTicket::PreparationTicket(LabControlCapture capture, uint32_t lab, uint64_t request_id,
                                     NpnrLabControlResultV1 selected_result,
                                     std::optional<NpnrLabControlResultV1> comparison_result)
        : capture_(std::move(capture)), lab_(lab), request_id_(request_id), selected_result_(selected_result),
          comparison_result_(comparison_result)
{
}

const char *preparation_status_name(PreparationStatus status)
{
    switch (status) {
    case PreparationStatus::Legal:
        return "legal";
    case PreparationStatus::Illegal:
        return "illegal";
    case PreparationStatus::Unsupported:
        return "unsupported";
    case PreparationStatus::TransportFailure:
        return "transport failure";
    case PreparationStatus::MalformedResult:
        return "malformed result";
    case PreparationStatus::Mismatch:
        return "mismatch";
    }
    NPNR_ASSERT_FALSE("unknown preparation status");
}

namespace {
PreparationStatus result_status(const NpnrLabControlResultV1 &result)
{
    switch (result.status) {
    case NPNR_CONTROL_LEGAL:
        return PreparationStatus::Legal;
    case NPNR_CONTROL_ILLEGAL:
        return PreparationStatus::Illegal;
    case NPNR_CONTROL_UNSUPPORTED_RULES:
        return PreparationStatus::Unsupported;
    default:
        return PreparationStatus::MalformedResult;
    }
}
} // namespace

PreparationTranslation translate_preparation_ticket(PreparationTicket &&ticket)
{
    if (ticket.request_id_ != ticket.capture_.input.request_id)
        return {};

    auto translate = [&](const NpnrLabControlResultV1 &result) -> std::optional<ValidatedControlPlan> {
        if (result.request_id != ticket.request_id_ || !lab_control_result_valid(ticket.capture_.input, result) ||
            result.status != NPNR_CONTROL_LEGAL)
            return std::nullopt;
        std::array<ControlSig, NPNR_LAB_ALLOCATION_COUNT> signals;
        for (unsigned i = 0; i < signals.size(); ++i) {
            const auto &encoded = result.allocation[i];
            const NetInfo *net = nullptr;
            if (encoded.net_id) {
                if (encoded.net_id > ticket.capture_.input.net_count)
                    return std::nullopt;
                net = ticket.capture_.nets[encoded.net_id - 1];
                if (!net)
                    return std::nullopt;
            }
            signals[i] = {net, bool(encoded.flags & NPNR_CONTROL_INVERTED)};
        }
        return ValidatedControlPlan{ticket.lab_, ticket.request_id_, LabControlAllocation::from_array(signals)};
    };

    auto selected = translate(ticket.selected_result_);
    if (!selected)
        return {};
    std::optional<ValidatedControlPlan> comparison;
    if (ticket.comparison_result_) {
        comparison = translate(*ticket.comparison_result_);
        if (!comparison)
            return {};
    }
    return {PreparationStatus::Legal, std::move(selected), std::move(comparison)};
}

#ifndef NO_RUST
PreparationDispatchResult accept_lab_control_preparation_candidate(const Arch &arch, uint32_t lab,
                                                                   LabControlCapture capture, uint32_t call,
                                                                   const NpnrLabControlResultV1 &candidate)
{
    const auto cpp = evaluate_lab_controls_legacy(arch, lab, capture);
    PreparationStatus comparison_status;
    std::string difference;
    auto &stats = arch.lab_control_stats;
    if (call == NPNR_LAB_CALL_OK && candidate.reason < stats.reasons.size())
        stats.reasons[candidate.reason].fetch_add(1, std::memory_order_relaxed);
    if (call == NPNR_LAB_CALL_OK && candidate.status == NPNR_CONTROL_LEGAL)
        stats.legal.fetch_add(1, std::memory_order_relaxed);
    else if (call == NPNR_LAB_CALL_OK && candidate.status == NPNR_CONTROL_ILLEGAL)
        stats.illegal.fetch_add(1, std::memory_order_relaxed);
    if (call != NPNR_LAB_CALL_OK) {
        comparison_status = PreparationStatus::TransportFailure;
        difference = "FFI call status";
    } else if (!lab_control_result_valid(capture.input, candidate)) {
        comparison_status = PreparationStatus::MalformedResult;
        difference = "invalid Rust result";
    } else if (candidate.status == NPNR_CONTROL_UNSUPPORTED_RULES) {
        comparison_status = PreparationStatus::Unsupported;
        difference = "unsupported Rust rules";
    } else {
        const auto detached = evaluate_lab_controls_cpp(capture.input);
        difference = lab_control_first_difference(detached, candidate);
        if (difference.empty())
            difference = lab_control_first_difference(cpp, candidate);
        comparison_status = difference.empty() ? result_status(candidate) : PreparationStatus::Mismatch;
    }

    const bool unsupported_fallback =
            arch.args.lab_controls == LabControlMode::Rust && comparison_status == PreparationStatus::Unsupported;
    const bool rust_mismatch =
            arch.args.lab_controls == LabControlMode::Rust && comparison_status == PreparationStatus::Mismatch;
    if (!difference.empty()) {
        if (comparison_status == PreparationStatus::Mismatch)
            stats.mismatches.fetch_add(1, std::memory_order_relaxed);
        else if (!unsupported_fallback)
            stats.errors.fetch_add(1, std::memory_order_relaxed);
        if (stats.diagnostics.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::ostringstream replay;
            write_lab_control_replay(replay, capture.input, cpp, "live LAB " + std::to_string(lab) + " preparation",
                                     &candidate, difference);
            log_warning("LAB controls %s preparation: LAB %u, request %" PRIu64 ", call %u, C++ %u, Rust %u/%u, "
                        "%s.\nReplay: %s\n",
                        lab_control_mode_name(arch.args.lab_controls), lab, capture.input.request_id, call, cpp.status,
                        candidate.status, candidate.reason, difference.c_str(), replay.str().c_str());
        }
        if (arch.args.lab_controls != LabControlMode::Shadow && !unsupported_fallback && !rust_mismatch)
            log_error("LAB control %s preparation failed at LAB %u: %s.\n",
                      lab_control_mode_name(arch.args.lab_controls), lab, difference.c_str());
    }

    const NpnrLabControlResultV1 *selected = &cpp;
    std::optional<NpnrLabControlResultV1> comparison;
    if (arch.args.lab_controls == LabControlMode::Verify && difference.empty()) {
        selected = &candidate;
        if (cpp.status == NPNR_CONTROL_LEGAL)
            comparison = cpp;
    } else if (arch.args.lab_controls == LabControlMode::Rust && !unsupported_fallback && call == NPNR_LAB_CALL_OK &&
               lab_control_result_valid(capture.input, candidate)) {
        selected = &candidate;
        if (difference.empty() && cpp.status == NPNR_CONTROL_LEGAL)
            comparison = cpp;
    } else if (arch.args.lab_controls == LabControlMode::Shadow && call == NPNR_LAB_CALL_OK &&
               lab_control_result_valid(capture.input, candidate) && candidate.status == NPNR_CONTROL_LEGAL) {
        comparison = candidate;
    }
    if (unsupported_fallback)
        stats.fallbacks.fetch_add(1, std::memory_order_relaxed);

    const auto selected_status = result_status(*selected);
    PreparationDispatchResult dispatch{selected_status, comparison_status, std::nullopt};
    if (selected_status == PreparationStatus::Legal) {
        dispatch.ticket.emplace(std::move(capture), lab, selected->request_id, *selected, comparison);
    }
    return dispatch;
}
#endif

PreparationDispatchResult dispatch_lab_controls_for_preparation(const Arch &arch, uint32_t lab)
{
    auto &stats = arch.lab_control_stats;
    const bool compare_rust = arch.args.lab_controls != LabControlMode::Legacy;
    const auto request = compare_rust ? stats.evaluations.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (compare_rust)
        stats.preparation.fetch_add(1, std::memory_order_relaxed);
    auto capture = capture_lab_controls(arch, lab, request);
    if (!compare_rust) {
        if (arch.args.verify_lab_controls)
            NPNR_ASSERT(verify_lab_controls_cpp(arch, lab));
        const auto selected = evaluate_lab_controls_legacy(arch, lab, capture);
        const auto status = result_status(selected);
        PreparationDispatchResult dispatch{status, status, std::nullopt};
        if (status == PreparationStatus::Legal)
            dispatch.ticket.emplace(std::move(capture), lab, request, selected);
        return dispatch;
    }
    require_lab_control_mode(arch.args.lab_controls);
#ifndef NO_RUST
    NpnrLabControlResultV1 candidate{};
    const auto call = npnr_mistral_eval_controls_v1(&capture.input, 1, &candidate, 1);
    return accept_lab_control_preparation_candidate(arch, lab, std::move(capture), call, candidate);
#else
    return {};
#endif
}

NEXTPNR_NAMESPACE_END
