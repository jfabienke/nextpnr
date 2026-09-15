/* SPDX-License-Identifier: ISC */
#include "lab_replay.h"

#include <charconv>
#include <cmath>
#include <ostream>
#include "json11.hpp"

NEXTPNR_NAMESPACE_BEGIN

namespace {
using json11::Json;
Json signal_json(const NpnrControlSignalV1 &signal)
{
    return Json::object{{"net_id", double(signal.net_id)}, {"flags", double(signal.flags)}};
}

Json result_json(const NpnrLabControlResultV1 &result)
{
    Json::array allocation, blockers;
    for (const auto &signal : result.allocation)
        allocation.push_back(signal_json(signal));
    for (const auto &blocker : result.blockers)
        blockers.push_back(Json::object{{"signal", signal_json(blocker.signal)},
                                        {"ff_slot", double(blocker.ff_slot)},
                                        {"reserved", double(blocker.reserved)}});
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

bool read_u32(const Json &value, uint32_t &out)
{
    if (!value.is_number())
        return false;
    double number = value.number_value();
    if (!std::isfinite(number) || number < 0 || number > UINT32_MAX || std::floor(number) != number)
        return false;
    out = uint32_t(number);
    return true;
}

bool read_u64(const Json &value, uint64_t &out)
{
    if (!value.is_string())
        return false;
    const auto &text = value.string_value();
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool read_signal(const Json &value, NpnrControlSignalV1 &out)
{
    return read_u32(value["net_id"], out.net_id) && read_u32(value["flags"], out.flags);
}
} // namespace

bool write_lab_control_replay(std::ostream &out, const NpnrLabControlsV1 &input,
                              const NpnrLabControlResultV1 &reference, const std::string &provenance,
                              const NpnrLabControlResultV1 *candidate, const std::string &difference)
{
    Json::array ffs;
    for (const auto &ff : input.ff) {
        Json::array controls;
        for (const auto &signal : ff.control)
            controls.push_back(signal_json(signal));
        ffs.push_back(Json::object{{"occupied", double(ff.occupied)},
                                   {"reserved", double(ff.reserved)},
                                   {"control", std::move(controls)}});
    }
    Json::array allocation;
    if (reference.status == NPNR_CONTROL_LEGAL)
        for (const auto &signal : reference.allocation)
            allocation.push_back(signal_json(signal));
    Json::object record{
            {"schema", 1},
            {"provenance", provenance},
            {"input", Json::object{{"abi_version", double(input.abi_version)},
                                   {"struct_size", double(input.struct_size)},
                                   {"rules_version", double(input.rules_version)},
                                   {"net_count", double(input.net_count)},
                                   {"request_id", std::to_string(input.request_id)},
                                   {"snapshot_epoch", std::to_string(input.snapshot_epoch)},
                                   {"ff", std::move(ffs)}}},
            {"expected", Json::object{{"status", double(reference.status)}, {"allocation", std::move(allocation)}}}};
    if (candidate) {
        record["reference_details"] = result_json(reference);
        record["candidate"] = result_json(*candidate);
        record["first_difference"] = difference;
    }
    out << Json(record).dump() << '\n';
    return bool(out);
}

bool read_lab_control_replay(const std::string &text, NpnrLabControlsV1 &input, NpnrLabControlResultV1 &reference,
                             std::string &error)
{
    error.clear();
    auto record = Json::parse(text, error);
    if (!error.empty())
        return false;
    error = "invalid LAB control replay schema or fields";
    uint32_t schema = 0;
    if (!read_u32(record["schema"], schema) || schema != 1)
        return false;
    // Parse into temporaries: a failed read never partially replaces caller state.
    NpnrLabControlsV1 decoded{};
    const auto &fields = record["input"];
    if (!read_u32(fields["abi_version"], decoded.abi_version) ||
        !read_u32(fields["struct_size"], decoded.struct_size) ||
        !read_u32(fields["rules_version"], decoded.rules_version) ||
        !read_u32(fields["net_count"], decoded.net_count) || !read_u64(fields["request_id"], decoded.request_id) ||
        !read_u64(fields["snapshot_epoch"], decoded.snapshot_epoch) || !fields["ff"].is_array() ||
        fields["ff"].array_items().size() != NPNR_LAB_FF_COUNT)
        return false;
    for (unsigned i = 0; i < NPNR_LAB_FF_COUNT; ++i) {
        const auto &ff = fields["ff"][i];
        if (!read_u32(ff["occupied"], decoded.ff[i].occupied) || !read_u32(ff["reserved"], decoded.ff[i].reserved) ||
            !ff["control"].is_array() || ff["control"].array_items().size() != NPNR_LAB_CONTROL_COUNT)
            return false;
        for (unsigned j = 0; j < NPNR_LAB_CONTROL_COUNT; ++j)
            if (!read_signal(ff["control"][j], decoded.ff[i].control[j]))
                return false;
    }
    uint32_t status = 0;
    const auto &expected = record["expected"];
    if (!read_u32(expected["status"], status) || (status != NPNR_CONTROL_LEGAL && status != NPNR_CONTROL_ILLEGAL) ||
        !expected["allocation"].is_array() ||
        expected["allocation"].array_items().size() != (status == NPNR_CONTROL_LEGAL ? NPNR_LAB_ALLOCATION_COUNT : 0))
        return false;
    auto result = lab_control_result(decoded, status);
    if (status == NPNR_CONTROL_LEGAL)
        for (unsigned i = 0; i < NPNR_LAB_ALLOCATION_COUNT; ++i)
            if (!read_signal(expected["allocation"][i], result.allocation[i]))
                return false;
    input = decoded;
    reference = result;
    error.clear();
    return true;
}

NEXTPNR_NAMESPACE_END
