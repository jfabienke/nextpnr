/* SPDX-License-Identifier: ISC */
#include "lab_snapshot.h"

#include <cinttypes>
#include <sstream>
#include "lab_replay.h"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
NpnrControlSignalV1 encode_signal(ControlSig signal, uint32_t id)
{
    return {id, (signal.inverted ? uint32_t(NPNR_CONTROL_INVERTED) : 0u) |
                        ((signal.net && signal.net->is_global) ? uint32_t(NPNR_CONTROL_GLOBAL) : 0u)};
}

} // namespace

NpnrControlSignalV1 LabControlCapture::encode_existing(ControlSig signal) const
{
    if (!signal.net)
        return encode_signal(signal, 0);
    for (uint32_t i = 0; i < input.net_count; ++i)
        if (nets[i] == signal.net)
            return encode_signal(signal, i + 1);
    NPNR_ASSERT_FALSE("control signal missing from LAB capture");
}

LabControlCapture capture_lab_controls(const Arch &arch, uint32_t lab, uint64_t request_id, uint64_t epoch)
{
    LabControlCapture capture{empty_lab_controls(request_id, epoch), {}};
    const auto &lab_data = arch.labs.at(lab);
    for (unsigned alm = 0; alm < 10; ++alm) {
        for (unsigned ff = 0; ff < 4; ++ff) {
            const auto *cell = arch.getBoundBelCell(lab_data.alms[alm].ff_bels[ff]);
            if (!cell)
                continue;
            NPNR_ASSERT(cell->type == id_MISTRAL_FF);
            auto &record = capture.input.ff[4 * alm + ff];
            record.occupied = 1;
            const auto &cs = cell->ffInfo.ctrlset;
            const std::array<ControlSig, NPNR_LAB_CONTROL_COUNT> controls{cs.clk, cs.sload, cs.sclr, cs.aclr, cs.ena};
            for (unsigned kind = 0; kind < controls.size(); ++kind) {
                const auto &signal = controls[kind];
                uint32_t net_id = 0;
                if (signal.net) {
                    uint32_t i = 0;
                    while (i < capture.input.net_count && capture.nets[i] != signal.net)
                        ++i;
                    if (i == capture.input.net_count) {
                        NPNR_ASSERT(i < capture.nets.size());
                        capture.nets[i] = signal.net;
                        ++capture.input.net_count;
                    }
                    net_id = i + 1;
                }
                record.control[kind] = encode_signal(signal, net_id);
            }
        }
    }
    return capture;
}

bool verify_lab_controls_cpp(const Arch &arch, uint32_t lab)
{
    // Immediate synchronous use only: epoch zero deliberately provides no reuse certificate.
    const auto capture = capture_lab_controls(arch, lab, lab);
    const auto reference = evaluate_lab_controls_legacy(arch, lab, capture);
    const auto candidate = evaluate_lab_controls_cpp(capture.input);
    if (!lab_control_results_match(reference, candidate)) {
        std::ostringstream replay;
        write_lab_control_replay(replay, capture.input, reference, "live C++ verification mismatch");
        log_error("LAB %u control snapshot mismatch: legacy status %u, detached status %u, reason %u.\nReplay: %s\n",
                  lab, reference.status, candidate.status, candidate.reason, replay.str().c_str());
    }
    return reference.status == NPNR_CONTROL_LEGAL;
}

const char *lab_control_mode_name(LabControlMode mode)
{
    switch (mode) {
    case LabControlMode::Legacy:
        return "legacy";
    case LabControlMode::Shadow:
        return "shadow";
    case LabControlMode::Verify:
        return "verify";
    case LabControlMode::Rust:
        return "rust";
    }
    NPNR_ASSERT_FALSE("unknown LAB control mode");
}

void require_lab_control_mode(LabControlMode mode)
{
#ifdef NO_RUST
    if (mode != LabControlMode::Legacy)
        log_error("LAB control mode '%s' requires BUILD_RUST=ON.\n", lab_control_mode_name(mode));
#else
    (void)mode;
#endif
}

bool lab_control_result_valid(const NpnrLabControlsV1 &input, const NpnrLabControlResultV1 &result)
{
    if (result.abi_version != NPNR_LAB_ABI_V1 || result.struct_size != sizeof(result) ||
        result.request_id != input.request_id || result.snapshot_epoch != input.snapshot_epoch || result.reserved)
        return false;
    // The input is a host capture, with valid dense IDs. Check that every returned
    // signal is either canonical empty or an exact signal occurring in that capture.
    auto empty = [](const NpnrControlSignalV1 &s) { return s.net_id == 0 && s.flags == 0; };
    auto same = [](const NpnrControlSignalV1 &a, const NpnrControlSignalV1 &b) {
        return a.net_id == b.net_id && a.flags == b.flags;
    };
    auto present = [&](const NpnrControlSignalV1 &s) {
        if (empty(s))
            return true;
        if (!s.net_id || s.net_id > input.net_count || (s.flags & ~3u))
            return false;
        for (const auto &ff : input.ff)
            if (ff.occupied)
                for (const auto &signal : ff.control)
                    if (same(s, signal))
                        return true;
        return false;
    };
    for (const auto &signal : result.allocation)
        if (result.status == NPNR_CONTROL_LEGAL ? !present(signal) : !empty(signal))
            return false;
    if (result.status != NPNR_CONTROL_ILLEGAL) {
        if (result.control_kind != UINT32_MAX || result.ff_slot != UINT32_MAX || result.resource_mask ||
            !empty(result.incoming))
            return false;
        for (const auto &blocker : result.blockers)
            if (!empty(blocker.signal) || blocker.ff_slot != UINT32_MAX || blocker.reserved)
                return false;
        return (result.status == NPNR_CONTROL_LEGAL && result.reason == NPNR_CONTROL_OK) ||
               (result.status == NPNR_CONTROL_UNSUPPORTED_RULES && result.reason == NPNR_CONTROL_BAD_RULES);
    }
    if (result.reason < NPNR_CONTROL_CLOCK_CONFLICT || result.reason > NPNR_CONTROL_DATAIN_CONFLICT ||
        result.control_kind >= NPNR_LAB_CONTROL_COUNT || result.ff_slot >= NPNR_LAB_FF_COUNT ||
        !input.ff[result.ff_slot].occupied || !result.incoming.net_id ||
        !same(result.incoming, input.ff[result.ff_slot].control[result.control_kind]))
        return false;
    unsigned mask;
    if (result.reason == NPNR_CONTROL_DATAIN_CONFLICT) {
        const std::array<unsigned, 5> masks{1, 2, 8, 12, 13};
        mask = masks[result.control_kind];
    } else {
        if (result.control_kind != result.reason - 1)
            return false;
        const std::array<unsigned, 5> masks{1, 1, 1, 3, 7};
        mask = masks[result.control_kind];
    }
    if (result.resource_mask != mask)
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        const auto &blocker = result.blockers[i];
        if (blocker.reserved)
            return false;
        if (!(mask & (1u << i))) {
            if (!empty(blocker.signal) || blocker.ff_slot != UINT32_MAX)
                return false;
        } else {
            if (!blocker.signal.net_id || !present(blocker.signal) || blocker.ff_slot >= NPNR_LAB_FF_COUNT ||
                !input.ff[blocker.ff_slot].occupied)
                return false;
            bool found = false;
            for (const auto &signal : input.ff[blocker.ff_slot].control)
                found |= same(blocker.signal, signal);
            if (!found)
                return false;
        }
    }
    return true;
}

bool dispatch_lab_controls(const Arch &arch, uint32_t lab)
{
    const auto mode = arch.args.lab_controls;
    if (mode == LabControlMode::Legacy)
        return arch.is_lab_ctrlset_legal(lab);
    require_lab_control_mode(mode);
#ifndef NO_RUST
    auto &stats = arch.lab_control_stats;
    const auto request = stats.evaluations.fetch_add(1, std::memory_order_relaxed) + 1;
    // Immediate synchronous use. Epoch zero is not a freshness/reuse certificate.
    const auto capture = capture_lab_controls(arch, lab, request);
    NpnrLabControlResultV1 candidate{};
    // Caller audit: two disjoint aligned local objects, initialized input, exclusive
    // output, no mutation or foreign callback during the synchronous Rust call.
    const auto call = npnr_mistral_eval_controls_v1(&capture.input, 1, &candidate, 1);
    return accept_lab_control_candidate(arch, lab, capture, call, candidate);
#else
    return false; // require_lab_control_mode above reports the configuration error.
#endif
}

bool accept_lab_control_candidate(const Arch &arch, uint32_t lab, const LabControlCapture &capture, uint32_t call,
                                  const NpnrLabControlResultV1 &candidate)
{
    const auto mode = arch.args.lab_controls;
    NPNR_ASSERT(mode != LabControlMode::Legacy);
    auto &stats = arch.lab_control_stats;
    const bool valid = call == NPNR_LAB_CALL_OK && lab_control_result_valid(capture.input, candidate);
    if (call == NPNR_LAB_CALL_OK && candidate.reason < stats.reasons.size())
        stats.reasons[candidate.reason].fetch_add(1, std::memory_order_relaxed);
    if (valid && candidate.status == NPNR_CONTROL_LEGAL)
        stats.legal.fetch_add(1, std::memory_order_relaxed);
    else if (valid && candidate.status == NPNR_CONTROL_ILLEGAL)
        stats.illegal.fetch_add(1, std::memory_order_relaxed);

    if (mode == LabControlMode::Rust && valid && candidate.status == NPNR_CONTROL_UNSUPPORTED_RULES) {
        stats.fallbacks.fetch_add(1, std::memory_order_relaxed);
        return evaluate_lab_controls_legacy(arch, lab, capture).status == NPNR_CONTROL_LEGAL;
    }
    if (mode == LabControlMode::Rust && valid)
        return candidate.status == NPNR_CONTROL_LEGAL;

    const auto legacy = evaluate_lab_controls_legacy(arch, lab, capture);
    const auto detached = evaluate_lab_controls_cpp(capture.input);
    const NpnrLabControlResultV1 *reference = &detached;
    std::string difference;
    if (!valid || candidate.status == NPNR_CONTROL_UNSUPPORTED_RULES) {
        stats.errors.fetch_add(1, std::memory_order_relaxed);
        difference = call == NPNR_LAB_CALL_OK ? "invalid or unexpected Rust result" : "FFI call status";
    } else {
        difference = lab_control_first_difference(detached, candidate);
        if (difference.empty() && !lab_control_results_match(legacy, candidate)) {
            difference = "live." + lab_control_first_difference(legacy, candidate);
            reference = &legacy;
        }
        if (!difference.empty())
            stats.mismatches.fetch_add(1, std::memory_order_relaxed);
    }
    if (!difference.empty()) {
        // Stream at most four complete replay records; retain no corpus in Context.
        if (stats.diagnostics.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::ostringstream replay;
            write_lab_control_replay(replay, capture.input, *reference,
                                     "live LAB " + std::to_string(lab) + " placement", &candidate, difference);
            log_warning("LAB controls %s: LAB %u, request %" PRIu64
                        ", call %u, legacy %u, Rust %u/%u, %s.\nReplay: %s\n",
                        lab_control_mode_name(mode), lab, capture.input.request_id, call, legacy.status,
                        candidate.status, candidate.reason, difference.c_str(), replay.str().c_str());
        }
        if (mode != LabControlMode::Shadow) {
            report_lab_control_stats(arch);
            log_error("LAB control %s failed at LAB %u: %s.\n", lab_control_mode_name(mode), lab, difference.c_str());
        }
    }
    return legacy.status == NPNR_CONTROL_LEGAL;
}

void report_lab_control_stats(const Arch &arch)
{
    if (arch.args.lab_controls == LabControlMode::Legacy)
        return;
    const auto &s = arch.lab_control_stats;
    auto get = [](const std::atomic<uint64_t> &value) { return value.load(std::memory_order_relaxed); };
    log_info("LAB controls %s: evaluations=%" PRIu64 " preparation=%" PRIu64 " legal=%" PRIu64 " illegal=%" PRIu64
             " errors=%" PRIu64 " mismatches=%" PRIu64 " fallbacks=%" PRIu64 " diagnostics=%" PRIu64 ".\n",
             lab_control_mode_name(arch.args.lab_controls), get(s.evaluations), get(s.preparation), get(s.legal),
             get(s.illegal), get(s.errors), get(s.mismatches), get(s.fallbacks),
             std::min(get(s.diagnostics), uint64_t(4)));
    for (unsigned reason = 0; reason < s.reasons.size(); ++reason)
        if (get(s.reasons[reason]))
            log_info("LAB controls reason %u: %" PRIu64 ".\n", reason, get(s.reasons[reason]));
}

NEXTPNR_NAMESPACE_END
