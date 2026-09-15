/* SPDX-License-Identifier: ISC */
// Standalone test oracle: links the actual detached C++ evaluator, with no device database.
#include <cstddef>
#include <iostream>
#include <string>
#include "json11.hpp"
#include "lab_model.h"
#include "lab_replay.h"

USING_NEXTPNR_NAMESPACE
using json11::Json;

namespace {
Json signal_json(const NpnrControlSignalV1 &s)
{
    return Json::object{{"net_id", double(s.net_id)}, {"flags", double(s.flags)}};
}

Json result_json(const NpnrLabControlResultV1 &result)
{
    Json::array allocation, blockers;
    for (const auto &s : result.allocation)
        allocation.push_back(signal_json(s));
    for (const auto &b : result.blockers)
        blockers.push_back(Json::object{
                {"signal", signal_json(b.signal)}, {"ff_slot", double(b.ff_slot)}, {"reserved", double(b.reserved)}});
    return Json::object{{"abi_version", double(result.abi_version)},
                        {"struct_size", double(result.struct_size)},
                        {"status", double(result.status)},
                        {"reason", double(result.reason)},
                        {"request_id", std::to_string(result.request_id)},
                        {"snapshot_epoch", std::to_string(result.snapshot_epoch)},
                        {"allocation", allocation},
                        {"control_kind", double(result.control_kind)},
                        {"ff_slot", double(result.ff_slot)},
                        {"resource_mask", double(result.resource_mask)},
                        {"reserved", double(result.reserved)},
                        {"incoming", signal_json(result.incoming)},
                        {"blockers", blockers}};
}

#define FIELD(type, field) {#field, double(offsetof(type, field))}
template <typename T> Json layout(Json::object fields)
{
    return Json::object{{"size", double(sizeof(T))}, {"align", double(alignof(T))}, {"offsets", fields}};
}

Json layout_json()
{
    using S = NpnrControlSignalV1;
    using F = NpnrLabFfV1;
    using I = NpnrLabControlsV1;
    using B = NpnrLabControlBlockerV1;
    using R = NpnrLabControlResultV1;
    return Json::object{
            {"signal", layout<S>({FIELD(S, net_id), FIELD(S, flags)})},
            {"ff", layout<F>({FIELD(F, occupied), FIELD(F, reserved), FIELD(F, control)})},
            {"input", layout<I>({FIELD(I, abi_version), FIELD(I, struct_size), FIELD(I, rules_version),
                                 FIELD(I, net_count), FIELD(I, request_id), FIELD(I, snapshot_epoch), FIELD(I, ff)})},
            {"blocker", layout<B>({FIELD(B, signal), FIELD(B, ff_slot), FIELD(B, reserved)})},
            {"result", layout<R>({FIELD(R, abi_version), FIELD(R, struct_size), FIELD(R, status), FIELD(R, reason),
                                  FIELD(R, request_id), FIELD(R, snapshot_epoch), FIELD(R, allocation),
                                  FIELD(R, control_kind), FIELD(R, ff_slot), FIELD(R, resource_mask),
                                  FIELD(R, reserved), FIELD(R, incoming), FIELD(R, blockers)})}};
}
#undef FIELD
} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--layout") {
        std::cout << layout_json().dump() << '\n';
        return 0;
    }
    if (argc != 1)
        return 2;
    unsigned line_number = 0;
    for (std::string line; std::getline(std::cin, line);) {
        ++line_number;
        NpnrLabControlsV1 input{};
        NpnrLabControlResultV1 ignored{};
        std::string error;
        if (!read_lab_control_replay(line, input, ignored, error)) {
            std::cerr << "record " << line_number << ": " << error << '\n';
            return 1;
        }
#ifdef LAB_CONTROL_FFI_DRIVER
        NpnrLabControlResultV1 result;
        const auto status = npnr_mistral_eval_controls_v1(&input, 1, &result, 1);
        if (status != NPNR_LAB_CALL_OK) {
            std::cerr << "record " << line_number << ": FFI call status " << status << '\n';
            return 1;
        }
#else
        const auto result = evaluate_lab_controls_cpp(input);
#endif
        std::cout << result_json(result).dump() << '\n';
    }
    return std::cin.bad() || !std::cout ? 1 : 0;
}
