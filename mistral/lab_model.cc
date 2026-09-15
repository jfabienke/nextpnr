/* SPDX-License-Identifier: ISC */
#include "lab_model.h"

#include <array>
#include <cstddef>
#include <type_traits>

NEXTPNR_NAMESPACE_BEGIN

static_assert(std::is_standard_layout<NpnrLabControlsV1>::value, "LAB input must have C layout");
static_assert(std::is_trivially_copyable<NpnrLabControlsV1>::value, "LAB input must be value transport");
static_assert(sizeof(NpnrControlSignalV1) == 8 && offsetof(NpnrControlSignalV1, flags) == 4, "signal ABI");
static_assert(sizeof(NpnrLabFfV1) == 48 && offsetof(NpnrLabFfV1, control) == 8, "FF ABI");
static_assert(sizeof(NpnrLabControlsV1) == 1952, "input ABI");
static_assert(offsetof(NpnrLabControlsV1, request_id) == 16, "input request ABI");
static_assert(offsetof(NpnrLabControlsV1, snapshot_epoch) == 24, "input epoch ABI");
static_assert(offsetof(NpnrLabControlsV1, ff) == 32, "input slots ABI");
static_assert(sizeof(NpnrLabControlBlockerV1) == 16, "blocker ABI");
static_assert(sizeof(NpnrLabControlResultV1) == 216, "result ABI");
static_assert(offsetof(NpnrLabControlResultV1, allocation) == 32, "result plan ABI");
static_assert(offsetof(NpnrLabControlResultV1, control_kind) == 128, "result conflict ABI");
static_assert(offsetof(NpnrLabControlResultV1, incoming) == 144, "result signal ABI");
static_assert(offsetof(NpnrLabControlResultV1, blockers) == 152, "result blockers ABI");
static_assert(alignof(NpnrLabControlsV1) == alignof(uint64_t), "input alignment ABI");
static_assert(alignof(NpnrLabControlResultV1) == alignof(uint64_t), "result alignment ABI");

NpnrLabControlsV1 empty_lab_controls(uint64_t request_id, uint64_t snapshot_epoch)
{
    NpnrLabControlsV1 input{};
    input.abi_version = NPNR_LAB_ABI_V1;
    input.struct_size = sizeof(input);
    input.rules_version = NPNR_LAB_RULES_V1;
    input.request_id = request_id;
    input.snapshot_epoch = snapshot_epoch;
    return input;
}

NpnrLabControlResultV1 lab_control_result(const NpnrLabControlsV1 &input, uint32_t status)
{
    NpnrLabControlResultV1 result{};
    result.abi_version = NPNR_LAB_ABI_V1;
    result.struct_size = sizeof(result);
    result.status = status;
    result.request_id = input.request_id;
    result.snapshot_epoch = input.snapshot_epoch;
    result.control_kind = UINT32_MAX;
    result.ff_slot = UINT32_MAX;
    for (auto &blocker : result.blockers)
        blocker.ff_slot = UINT32_MAX;
    return result;
}

namespace {
uint32_t validate(const NpnrLabControlsV1 &input)
{
    if (input.abi_version != NPNR_LAB_ABI_V1)
        return NPNR_CONTROL_BAD_ABI;
    if (input.struct_size != sizeof(input))
        return NPNR_CONTROL_BAD_SIZE;
    if (input.rules_version != NPNR_LAB_RULES_V1)
        return NPNR_CONTROL_BAD_RULES;
    if (input.net_count > NPNR_LAB_MAX_NETS)
        return NPNR_CONTROL_BAD_NET_COUNT;
    std::array<uint8_t, NPNR_LAB_MAX_NETS + 1> globals{}; // 0 = unseen, 1 = local, 2 = global
    for (const auto &ff : input.ff) {
        if (ff.occupied > 1)
            return NPNR_CONTROL_BAD_OCCUPANCY;
        if (ff.reserved != 0)
            return NPNR_CONTROL_BAD_RESERVED;
        for (const auto &signal : ff.control) {
            if ((!ff.occupied && (signal.net_id || signal.flags)) || signal.net_id > input.net_count ||
                (signal.flags & ~(NPNR_CONTROL_INVERTED | NPNR_CONTROL_GLOBAL)) ||
                (!signal.net_id && (signal.flags & NPNR_CONTROL_GLOBAL)))
                return NPNR_CONTROL_BAD_SIGNAL;
            if (!signal.net_id)
                continue;
            uint8_t global = (signal.flags & NPNR_CONTROL_GLOBAL) ? 2 : 1;
            if (globals[signal.net_id] && globals[signal.net_id] != global)
                return NPNR_CONTROL_INCONSISTENT_GLOBAL;
            globals[signal.net_id] = global;
        }
    }
    for (uint32_t id = 1; id <= input.net_count; ++id)
        if (!globals[id])
            return NPNR_CONTROL_SPARSE_NET_IDS;
    return NPNR_CONTROL_OK;
}

bool same_signal(const NpnrControlSignalV1 &a, const NpnrControlSignalV1 &b)
{
    return a.net_id == b.net_id && ((a.flags ^ b.flags) & NPNR_CONTROL_INVERTED) == 0;
}

struct ControlWorker
{
    struct Slot
    {
        NpnrControlSignalV1 signal{};
        uint32_t origin = UINT32_MAX;
    };
    std::array<Slot, NPNR_LAB_ALLOCATION_COUNT> slots{};
    NpnrLabControlResultV1 result;

    // First empty slot wins, even if a later slot already contains the same signal.
    template <size_t N>
    bool assign(NpnrControlSignalV1 signal, uint32_t origin, uint32_t kind, const std::array<unsigned, N> &indices,
                uint32_t reason)
    {
        if (!signal.net_id)
            return true;
        for (auto index : indices) {
            auto &slot = slots[index];
            if (same_signal(slot.signal, signal))
                return true;
            if (!slot.signal.net_id) {
                slot = {signal, origin};
                return true;
            }
        }
        result.status = NPNR_CONTROL_ILLEGAL;
        result.reason = reason;
        result.control_kind = kind;
        result.ff_slot = origin;
        result.incoming = signal;
        for (size_t i = 0; i < N; ++i) {
            auto index = indices[i];
            auto bit = reason == NPNR_CONTROL_DATAIN_CONFLICT ? index - 8 : unsigned(i);
            result.resource_mask |= 1u << bit;
            result.blockers[bit].signal = slots[index].signal;
            result.blockers[bit].ff_slot = slots[index].origin;
        }
        return false;
    }

    template <size_t N> bool datain(unsigned source, uint32_t kind, const std::array<unsigned, N> &indices)
    {
        return assign(slots[source].signal, slots[source].origin, kind, indices, NPNR_CONTROL_DATAIN_CONFLICT);
    }

    NpnrLabControlResultV1 run(const NpnrLabControlsV1 &input)
    {
        for (uint32_t i = 0; i < NPNR_LAB_FF_COUNT; ++i) {
            const auto &ff = input.ff[i];
            if (!ff.occupied)
                continue;
            if (!assign(ff.control[0], i, NPNR_CONTROL_CLK, std::array<unsigned, 1>{0}, NPNR_CONTROL_CLOCK_CONFLICT) ||
                !assign(ff.control[1], i, NPNR_CONTROL_SLOAD, std::array<unsigned, 1>{1},
                        NPNR_CONTROL_SLOAD_CONFLICT) ||
                !assign(ff.control[2], i, NPNR_CONTROL_SCLR, std::array<unsigned, 1>{2}, NPNR_CONTROL_SCLR_CONFLICT) ||
                !assign(ff.control[3], i, NPNR_CONTROL_ACLR, std::array<unsigned, 2>{3, 4},
                        NPNR_CONTROL_ACLR_CAPACITY) ||
                !assign(ff.control[4], i, NPNR_CONTROL_ENA, std::array<unsigned, 3>{5, 6, 7},
                        NPNR_CONTROL_ENA_CAPACITY))
                return result;
        }
        if (!(slots[0].signal.flags & NPNR_CONTROL_GLOBAL) && !datain(0, NPNR_CONTROL_CLK, std::array<unsigned, 1>{8}))
            return result;
        if (!datain(1, NPNR_CONTROL_SLOAD, std::array<unsigned, 1>{9}) ||
            !datain(2, NPNR_CONTROL_SCLR, std::array<unsigned, 1>{11}))
            return result;
        for (unsigned i = 3; i < 5; ++i)
            if (!datain(i, NPNR_CONTROL_ACLR, std::array<unsigned, 2>{11, 10}))
                return result;
        for (unsigned i = 5; i < 8; ++i)
            if (!datain(i, NPNR_CONTROL_ENA, std::array<unsigned, 3>{10, 11, 8}))
                return result;
        for (size_t i = 0; i < slots.size(); ++i)
            result.allocation[i] = slots[i].signal;
        return result;
    }
};
} // namespace

NpnrLabControlResultV1 evaluate_lab_controls_cpp(const NpnrLabControlsV1 &input)
{
    auto result = lab_control_result(input, NPNR_CONTROL_LEGAL);
    result.reason = validate(input);
    if (result.reason != NPNR_CONTROL_OK) {
        result.status =
                result.reason == NPNR_CONTROL_BAD_RULES ? NPNR_CONTROL_UNSUPPORTED_RULES : NPNR_CONTROL_BAD_SNAPSHOT;
        return result;
    }
    ControlWorker worker{{}, result};
    return worker.run(input);
}

bool lab_control_results_match(const NpnrLabControlResultV1 &reference, const NpnrLabControlResultV1 &candidate)
{
    if (candidate.abi_version != NPNR_LAB_ABI_V1 || candidate.struct_size != sizeof(candidate) ||
        candidate.request_id != reference.request_id || candidate.snapshot_epoch != reference.snapshot_epoch ||
        candidate.status != reference.status)
        return false;
    if (candidate.status == NPNR_CONTROL_ILLEGAL)
        return true;
    if (candidate.status != NPNR_CONTROL_LEGAL)
        return false;
    for (unsigned i = 0; i < NPNR_LAB_ALLOCATION_COUNT; ++i)
        if (candidate.allocation[i].net_id != reference.allocation[i].net_id ||
            candidate.allocation[i].flags != reference.allocation[i].flags)
            return false;
    return true;
}

std::string lab_control_first_difference(const NpnrLabControlResultV1 &a, const NpnrLabControlResultV1 &b)
{
#define COMPARE(field)                                                                                                 \
    if (a.field != b.field)                                                                                            \
    return #field
    COMPARE(abi_version);
    COMPARE(struct_size);
    COMPARE(request_id);
    COMPARE(snapshot_epoch);
    COMPARE(status);
    COMPARE(reason);
    for (unsigned i = 0; i < NPNR_LAB_ALLOCATION_COUNT; ++i) {
        if (a.allocation[i].net_id != b.allocation[i].net_id)
            return "allocation[" + std::to_string(i) + "].net_id";
        if (a.allocation[i].flags != b.allocation[i].flags)
            return "allocation[" + std::to_string(i) + "].flags";
    }
    COMPARE(control_kind);
    COMPARE(ff_slot);
    COMPARE(resource_mask);
    COMPARE(reserved);
    COMPARE(incoming.net_id);
    COMPARE(incoming.flags);
    for (unsigned i = 0; i < 4; ++i) {
        if (a.blockers[i].signal.net_id != b.blockers[i].signal.net_id)
            return "blockers[" + std::to_string(i) + "].signal.net_id";
        if (a.blockers[i].signal.flags != b.blockers[i].signal.flags)
            return "blockers[" + std::to_string(i) + "].signal.flags";
        if (a.blockers[i].ff_slot != b.blockers[i].ff_slot)
            return "blockers[" + std::to_string(i) + "].ff_slot";
        if (a.blockers[i].reserved != b.blockers[i].reserved)
            return "blockers[" + std::to_string(i) + "].reserved";
    }
#undef COMPARE
    return {};
}

NEXTPNR_NAMESPACE_END
