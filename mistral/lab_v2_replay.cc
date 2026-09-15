/* SPDX-License-Identifier: ISC */
#include "lab_v2_replay.h"

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

Json control_json(const NpnrLabControlResultV1 &control)
{
    Json::array allocation, blockers;
    for (const auto &signal : control.allocation)
        allocation.push_back(signal_json(signal));
    for (const auto &blocker : control.blockers)
        blockers.push_back(Json::object{{"signal", signal_json(blocker.signal)},
                                        {"ff_slot", double(blocker.ff_slot)},
                                        {"reserved", double(blocker.reserved)}});
    return Json::object{{"abi_version", double(control.abi_version)},
                        {"struct_size", double(control.struct_size)},
                        {"status", double(control.status)},
                        {"reason", double(control.reason)},
                        {"request_id", std::to_string(control.request_id)},
                        {"snapshot_epoch", std::to_string(control.snapshot_epoch)},
                        {"allocation", std::move(allocation)},
                        {"control_kind", double(control.control_kind)},
                        {"ff_slot", double(control.ff_slot)},
                        {"resource_mask", double(control.resource_mask)},
                        {"reserved", double(control.reserved)},
                        {"incoming", signal_json(control.incoming)},
                        {"blockers", std::move(blockers)}};
}

bool read_u32(const Json &value, uint32_t &out)
{
    if (!value.is_number())
        return false;
    const double number = value.number_value();
    if (!std::isfinite(number) || number < 0 || number > UINT32_MAX || std::floor(number) != number)
        return false;
    out = uint32_t(number);
    return true;
}

bool read_i32(const Json &value, int32_t &out)
{
    if (!value.is_number())
        return false;
    const double number = value.number_value();
    if (!std::isfinite(number) || number < INT32_MIN || number > INT32_MAX || std::floor(number) != number)
        return false;
    out = int32_t(number);
    return true;
}

bool read_u64(const Json &value, uint64_t &out)
{
    if (!value.is_string())
        return false;
    const auto &text = value.string_value();
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool read_signal(const Json &value, NpnrControlSignalV1 &out)
{
    return read_u32(value["net_id"], out.net_id) && read_u32(value["flags"], out.flags);
}

bool read_control(const Json &value, NpnrLabControlResultV1 &out)
{
    if (!read_u32(value["abi_version"], out.abi_version) || !read_u32(value["struct_size"], out.struct_size) ||
        !read_u32(value["status"], out.status) || !read_u32(value["reason"], out.reason) ||
        !read_u64(value["request_id"], out.request_id) || !read_u64(value["snapshot_epoch"], out.snapshot_epoch) ||
        !read_u32(value["control_kind"], out.control_kind) || !read_u32(value["ff_slot"], out.ff_slot) ||
        !read_u32(value["resource_mask"], out.resource_mask) || !read_u32(value["reserved"], out.reserved) ||
        !read_signal(value["incoming"], out.incoming) || !value["allocation"].is_array() ||
        value["allocation"].array_items().size() != NPNR_LAB_ALLOCATION_COUNT || !value["blockers"].is_array() ||
        value["blockers"].array_items().size() != 4)
        return false;
    for (unsigned i = 0; i < NPNR_LAB_ALLOCATION_COUNT; ++i)
        if (!read_signal(value["allocation"][i], out.allocation[i]))
            return false;
    for (unsigned i = 0; i < 4; ++i)
        if (!read_signal(value["blockers"][i]["signal"], out.blockers[i].signal) ||
            !read_u32(value["blockers"][i]["ff_slot"], out.blockers[i].ff_slot) ||
            !read_u32(value["blockers"][i]["reserved"], out.blockers[i].reserved))
            return false;
    return true;
}
} // namespace

bool write_lab_v2_replay(std::ostream &out, const NpnrLabFactsV2 &input, const NpnrLabAssessmentV2 &expected,
                         const std::string &provenance)
{
    Json::array alms;
    for (const auto &alm : input.alm) {
        Json::array luts, ffs;
        for (const auto &lut : alm.lut) {
            Json::array inputs;
            for (auto net : lut.input_net)
                inputs.push_back(double(net));
            luts.push_back(Json::object{{"occupied", double(lut.occupied)},
                                        {"input_count", double(lut.input_count)},
                                        {"used_input_count", double(lut.used_input_count)},
                                        {"bits_count", double(lut.bits_count)},
                                        {"chain_shared_input_count", double(lut.chain_shared_input_count)},
                                        {"mlab_group", double(lut.mlab_group)},
                                        {"constr_z", double(lut.constr_z)},
                                        {"is_carry", double(lut.is_carry)},
                                        {"input_net", std::move(inputs)},
                                        {"comb_out_net", double(lut.comb_out_net)},
                                        {"wclk", signal_json(lut.wclk)},
                                        {"we", signal_json(lut.we)}});
        }
        for (const auto &ff : alm.ff) {
            Json::array controls;
            for (const auto &signal : ff.control)
                controls.push_back(signal_json(signal));
            ffs.push_back(Json::object{{"occupied", double(ff.occupied)},
                                       {"datain_net", double(ff.datain_net)},
                                       {"sdata_net", double(ff.sdata_net)},
                                       {"control", std::move(controls)}});
        }
        alms.push_back(Json::object{{"lut", std::move(luts)},
                                    {"ff", std::move(ffs)},
                                    {"cached_input_count", double(alm.cached_input_count)},
                                    {"reserved", double(alm.reserved)}});
    }
    Json::array counts;
    for (auto count : expected.recomputed_input_count)
        counts.push_back(double(count));
    const Json record =
            Json::object{{"schema", 2},
                         {"provenance", provenance},
                         {"input", Json::object{{"abi_version", double(input.abi_version)},
                                                {"struct_size", double(input.struct_size)},
                                                {"request_id", std::to_string(input.request_id)},
                                                {"snapshot_epoch", std::to_string(input.snapshot_epoch)},
                                                {"query", double(input.query)},
                                                {"query_alm", double(input.query_alm)},
                                                {"input_limit", double(input.input_limit)},
                                                {"is_mlab", double(input.is_mlab)},
                                                {"net_count", double(input.net_count)},
                                                {"reserved", double(input.reserved)},
                                                {"alm", std::move(alms)}}},
                         {"expected", Json::object{{"abi_version", double(expected.abi_version)},
                                                   {"struct_size", double(expected.struct_size)},
                                                   {"request_id", std::to_string(expected.request_id)},
                                                   {"snapshot_epoch", std::to_string(expected.snapshot_epoch)},
                                                   {"status", double(expected.status)},
                                                   {"reason", double(expected.reason)},
                                                   {"query", double(expected.query)},
                                                   {"query_alm", double(expected.query_alm)},
                                                   {"failing_alm", double(expected.failing_alm)},
                                                   {"failing_slot", double(expected.failing_slot)},
                                                   {"observed", double(expected.observed)},
                                                   {"limit", double(expected.limit)},
                                                   {"recomputed_valid_mask", double(expected.recomputed_valid_mask)},
                                                   {"recomputed_input_count", std::move(counts)},
                                                   {"control_valid", double(expected.control_valid)},
                                                   {"reserved", double(expected.reserved)},
                                                   {"control", control_json(expected.control)}}}};
    out << record.dump() << '\n';
    return bool(out);
}

bool read_lab_v2_replay(const std::string &text, NpnrLabFactsV2 &input, NpnrLabAssessmentV2 &expected,
                        std::string &error)
{
    error.clear();
    const auto record = Json::parse(text, error);
    if (!error.empty())
        return false;
    error = "invalid LAB V2 replay schema or fields";
    uint32_t schema = 0;
    if (!read_u32(record["schema"], schema) || schema != 2)
        return false;
    NpnrLabFactsV2 decoded{};
    const auto &fields = record["input"];
    if (!read_u32(fields["abi_version"], decoded.abi_version) ||
        !read_u32(fields["struct_size"], decoded.struct_size) || !read_u64(fields["request_id"], decoded.request_id) ||
        !read_u64(fields["snapshot_epoch"], decoded.snapshot_epoch) || !read_u32(fields["query"], decoded.query) ||
        !read_u32(fields["query_alm"], decoded.query_alm) || !read_i32(fields["input_limit"], decoded.input_limit) ||
        !read_u32(fields["is_mlab"], decoded.is_mlab) || !read_u32(fields["net_count"], decoded.net_count) ||
        !read_u32(fields["reserved"], decoded.reserved) || !fields["alm"].is_array() ||
        fields["alm"].array_items().size() != NPNR_LAB_V2_ALMS)
        return false;
    for (unsigned a = 0; a < NPNR_LAB_V2_ALMS; ++a) {
        const auto &source = fields["alm"][a];
        auto &alm = decoded.alm[a];
        if (!source["lut"].is_array() || source["lut"].array_items().size() != NPNR_LAB_V2_LUTS ||
            !source["ff"].is_array() || source["ff"].array_items().size() != NPNR_LAB_V2_FFS ||
            !read_i32(source["cached_input_count"], alm.cached_input_count) ||
            !read_u32(source["reserved"], alm.reserved))
            return false;
        for (unsigned i = 0; i < NPNR_LAB_V2_LUTS; ++i) {
            const auto &value = source["lut"][i];
            auto &lut = alm.lut[i];
            if (!read_u32(value["occupied"], lut.occupied) || !read_u32(value["input_count"], lut.input_count) ||
                !read_u32(value["used_input_count"], lut.used_input_count) ||
                !read_u32(value["bits_count"], lut.bits_count) ||
                !read_i32(value["chain_shared_input_count"], lut.chain_shared_input_count) ||
                !read_i32(value["mlab_group"], lut.mlab_group) || !read_i32(value["constr_z"], lut.constr_z) ||
                !read_u32(value["is_carry"], lut.is_carry) || !read_u32(value["comb_out_net"], lut.comb_out_net) ||
                !read_signal(value["wclk"], lut.wclk) || !read_signal(value["we"], lut.we) ||
                !value["input_net"].is_array() || value["input_net"].array_items().size() != NPNR_LAB_V2_LUT_INPUTS)
                return false;
            for (unsigned pin = 0; pin < NPNR_LAB_V2_LUT_INPUTS; ++pin)
                if (!read_u32(value["input_net"][pin], lut.input_net[pin]))
                    return false;
        }
        for (unsigned i = 0; i < NPNR_LAB_V2_FFS; ++i) {
            const auto &value = source["ff"][i];
            auto &ff = alm.ff[i];
            if (!read_u32(value["occupied"], ff.occupied) || !read_u32(value["datain_net"], ff.datain_net) ||
                !read_u32(value["sdata_net"], ff.sdata_net) || !value["control"].is_array() ||
                value["control"].array_items().size() != NPNR_LAB_CONTROL_COUNT)
                return false;
            for (unsigned kind = 0; kind < NPNR_LAB_CONTROL_COUNT; ++kind)
                if (!read_signal(value["control"][kind], ff.control[kind]))
                    return false;
        }
    }
    NpnrLabAssessmentV2 result{};
    const auto &value = record["expected"];
    if (!read_u32(value["abi_version"], result.abi_version) || !read_u32(value["struct_size"], result.struct_size) ||
        !read_u64(value["request_id"], result.request_id) ||
        !read_u64(value["snapshot_epoch"], result.snapshot_epoch) || !read_u32(value["status"], result.status) ||
        !read_u32(value["reason"], result.reason) || !read_u32(value["query"], result.query) ||
        !read_u32(value["query_alm"], result.query_alm) || !read_u32(value["failing_alm"], result.failing_alm) ||
        !read_u32(value["failing_slot"], result.failing_slot) || !read_i32(value["observed"], result.observed) ||
        !read_i32(value["limit"], result.limit) ||
        !read_u32(value["recomputed_valid_mask"], result.recomputed_valid_mask) ||
        !read_u32(value["control_valid"], result.control_valid) || !read_u32(value["reserved"], result.reserved) ||
        !value["recomputed_input_count"].is_array() ||
        value["recomputed_input_count"].array_items().size() != NPNR_LAB_V2_ALMS ||
        !read_control(value["control"], result.control))
        return false;
    for (unsigned i = 0; i < NPNR_LAB_V2_ALMS; ++i)
        if (!read_i32(value["recomputed_input_count"][i], result.recomputed_input_count[i]))
            return false;
    input = decoded;
    expected = result;
    error.clear();
    return true;
}

NEXTPNR_NAMESPACE_END
