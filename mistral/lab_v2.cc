/* SPDX-License-Identifier: ISC */
#include "lab_v2.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdlib>
#include <sstream>
#include <type_traits>
#include "lab_model.h"
#include "lab_v2_replay.h"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

static_assert(std::is_standard_layout<NpnrLabFactsV2>::value && std::is_trivially_copyable<NpnrLabFactsV2>::value,
              "V2 facts must be C value transport");
static_assert(sizeof(NpnrLabLutV2) == 80 && sizeof(NpnrLabFfV2) == 52 && sizeof(NpnrAlmFactsV2) == 376, "V2 fact ABI");
static_assert(sizeof(NpnrLabFactsV2) == 3808 && offsetof(NpnrLabFactsV2, alm) == 48, "V2 input ABI");
static_assert(sizeof(NpnrLabAssessmentV2) == 328 && offsetof(NpnrLabAssessmentV2, control) == 112, "V2 result ABI");

namespace {

NpnrLabAssessmentV2 empty_assessment(const NpnrLabFactsV2 &input)
{
    NpnrLabAssessmentV2 result{};
    result.abi_version = NPNR_LAB_ABI_V2;
    result.struct_size = sizeof(result);
    result.request_id = input.request_id;
    result.snapshot_epoch = input.snapshot_epoch;
    result.status = NPNR_LAB_V2_LEGAL;
    result.reason = NPNR_LAB_V2_OK;
    result.query = input.query;
    result.query_alm = input.query_alm;
    result.failing_alm = UINT32_MAX;
    result.failing_slot = UINT32_MAX;
    result.control = lab_control_result(empty_lab_controls(input.request_id, input.snapshot_epoch),
                                        NPNR_CONTROL_UNSUPPORTED_RULES);
    result.control.reason = NPNR_CONTROL_BAD_RULES;
    return result;
}

bool signal_shape_valid(const NpnrControlSignalV1 &signal, uint32_t net_count)
{
    return signal.net_id <= net_count && !(signal.flags & ~3u) &&
           (signal.net_id || !(signal.flags & NPNR_CONTROL_GLOBAL));
}

bool input_shape_valid(const NpnrLabFactsV2 &input)
{
    if (input.net_count > NPNR_LAB_V2_MAX_NETS || input.reserved || input.is_mlab > 1)
        return false;
    for (const auto &alm : input.alm) {
        if (alm.reserved)
            return false;
        for (const auto &lut : alm.lut) {
            if (lut.occupied > 1 || lut.is_carry > 1 || lut.input_count > NPNR_LAB_V2_LUT_INPUTS ||
                lut.used_input_count > lut.input_count || lut.chain_shared_input_count < 0 ||
                lut.chain_shared_input_count > int32_t(lut.used_input_count) || lut.mlab_group < -1 ||
                lut.comb_out_net > input.net_count || !signal_shape_valid(lut.wclk, input.net_count) ||
                !signal_shape_valid(lut.we, input.net_count))
                return false;
            unsigned used = 0;
            for (unsigned pin = 0; pin < NPNR_LAB_V2_LUT_INPUTS; ++pin) {
                const auto net = lut.input_net[pin];
                if (net > input.net_count || (pin >= lut.input_count && net))
                    return false;
                used += net != 0;
            }
            if (lut.occupied && used != lut.used_input_count)
                return false;
            if (!lut.occupied &&
                (lut.input_count || lut.used_input_count || lut.bits_count || lut.chain_shared_input_count ||
                 lut.mlab_group || lut.constr_z || lut.is_carry || lut.comb_out_net || lut.wclk.net_id ||
                 lut.wclk.flags || lut.we.net_id || lut.we.flags))
                return false;
        }
        for (const auto &ff : alm.ff) {
            if (ff.occupied > 1 || ff.datain_net > input.net_count || ff.sdata_net > input.net_count)
                return false;
            for (const auto &signal : ff.control)
                if (!signal_shape_valid(signal, input.net_count))
                    return false;
            if (!ff.occupied && (ff.datain_net || ff.sdata_net))
                return false;
            if (!ff.occupied)
                for (const auto &signal : ff.control)
                    if (signal.net_id || signal.flags)
                        return false;
        }
    }
    return true;
}

bool same_signal(const NpnrControlSignalV1 &a, const NpnrControlSignalV1 &b)
{
    return a.net_id == b.net_id && a.flags == b.flags;
}

bool same_ctrlset(const NpnrLabFfV2 &a, const NpnrLabFfV2 &b)
{
    for (unsigned i = 0; i < NPNR_LAB_CONTROL_COUNT; ++i)
        if (!same_signal(a.control[i], b.control[i]))
            return false;
    return true;
}

bool fail(NpnrLabAssessmentV2 &result, uint32_t reason, unsigned alm, unsigned slot = UINT32_MAX, int observed = 0,
          int limit = 0)
{
    result.status = NPNR_LAB_V2_ILLEGAL;
    result.reason = reason;
    result.failing_alm = alm;
    result.failing_slot = slot;
    result.observed = observed;
    result.limit = limit;
    return false;
}

bool check_alm(const NpnrLabFactsV2 &input, unsigned alm_index, NpnrLabAssessmentV2 &result)
{
    const auto &alm = input.alm[alm_index];
    int used_lut_bits = 0, total_lut_inputs = 0;
    for (const auto &lut : alm.lut) {
        if (!lut.occupied)
            continue;
        total_lut_inputs += int(lut.input_count);
        used_lut_bits += int(lut.bits_count);
    }
    if (used_lut_bits > 64)
        return fail(result, NPNR_LAB_V2_ALM_BITS, alm_index, UINT32_MAX, used_lut_bits, 64);
    if (total_lut_inputs > 8) {
        int shared = 0;
        for (unsigned i = 0; i < alm.lut[1].input_count; ++i) {
            for (unsigned j = 0; j < alm.lut[0].input_count; ++j) {
                if (alm.lut[1].input_net[i] == alm.lut[0].input_net[j]) {
                    ++shared;
                    break;
                }
            }
        }
        if (total_lut_inputs - shared > 8)
            return fail(result, NPNR_LAB_V2_ALM_INPUTS, alm_index, UINT32_MAX, total_lut_inputs - shared, 8);
    }
    const bool carry = (alm.lut[0].occupied && alm.lut[0].is_carry) || (alm.lut[1].occupied && alm.lut[1].is_carry);
    if (alm.lut[0].occupied && alm.lut[1].occupied && alm.lut[0].is_carry != alm.lut[1].is_carry)
        return fail(result, NPNR_LAB_V2_CARRY_MIX, alm_index);
    for (unsigned half = 0; half < 2; ++half) {
        bool route_thru = !alm.lut[half].occupied && !carry && total_lut_inputs < 8 && used_lut_bits < 64;
        bool ef_available = !alm.lut[1 - half].occupied || alm.lut[1 - half].used_input_count <= 2;
        const NpnrLabFfV2 *first = nullptr;
        for (unsigned j = 0; j < 2; ++j) {
            const auto slot = 2 * half + j;
            const auto &ff = alm.ff[slot];
            if (!ff.occupied)
                continue;
            if (j == 1)
                return fail(result, NPNR_LAB_V2_ODD_FF, alm_index, slot);
            if (first && !same_ctrlset(*first, ff))
                return fail(result, NPNR_LAB_V2_FF_CONTROL, alm_index, slot);
            if (!first)
                first = &ff;
            if (ff.sdata_net) {
                if (!ef_available)
                    return fail(result, NPNR_LAB_V2_SDATA_PATH, alm_index, slot);
                ef_available = false;
            }
            if (ff.datain_net && (!alm.lut[half].occupied || ff.datain_net != alm.lut[half].comb_out_net)) {
                if (route_thru)
                    route_thru = false;
                else if (ef_available)
                    ef_available = false;
                else
                    return fail(result, NPNR_LAB_V2_DATAIN_PATH, alm_index, slot);
            }
        }
    }
    return true;
}

int recompute_alm_inputs(const NpnrAlmFactsV2 &alm)
{
    int total_lut_inputs = 0;
    for (const auto &lut : alm.lut) {
        if (!lut.occupied)
            continue;
        if (lut.mlab_group != -1 && lut.constr_z > 2)
            return 0;
        total_lut_inputs += int(lut.used_input_count) - lut.chain_shared_input_count;
    }
    int shared = 0;
    if (alm.lut[0].occupied && alm.lut[1].occupied) {
        for (unsigned i = 0; i < alm.lut[1].input_count; ++i) {
            const auto net = alm.lut[1].input_net[i];
            if (!net)
                continue;
            for (unsigned j = 0; j < alm.lut[0].input_count; ++j) {
                if (net == alm.lut[0].input_net[j]) {
                    ++shared;
                    break;
                }
            }
            if (shared >= 2 && alm.lut[0].mlab_group == -1)
                break;
        }
    }
    int total = std::max(0, total_lut_inputs - shared);
    for (unsigned i = 0; i < 4; ++i) {
        const auto &ff = alm.ff[i];
        if (!ff.occupied)
            continue;
        if (ff.sdata_net)
            ++total;
        if (ff.datain_net && (!alm.lut[i / 2].occupied || ff.datain_net != alm.lut[i / 2].comb_out_net))
            ++total;
    }
    return total;
}

struct ProjectedControls
{
    NpnrLabControlsV1 input;
    std::array<uint32_t, NPNR_LAB_MAX_NETS + 1> v2_net{};
};

ProjectedControls project_controls(const NpnrLabFactsV2 &input)
{
    ProjectedControls projected{empty_lab_controls(input.request_id, input.snapshot_epoch), {}};
    auto &controls = projected.input;
    std::array<uint32_t, NPNR_LAB_V2_MAX_NETS + 1> local{};
    for (unsigned alm = 0; alm < 10; ++alm) {
        for (unsigned ff = 0; ff < 4; ++ff) {
            const auto &source = input.alm[alm].ff[ff];
            if (!source.occupied)
                continue;
            auto &dest = controls.ff[4 * alm + ff];
            dest.occupied = 1;
            for (unsigned kind = 0; kind < NPNR_LAB_CONTROL_COUNT; ++kind) {
                dest.control[kind].flags = source.control[kind].flags;
                const auto net = source.control[kind].net_id;
                if (!net)
                    continue;
                if (!local[net]) {
                    local[net] = ++controls.net_count;
                    projected.v2_net[controls.net_count] = net;
                }
                dest.control[kind].net_id = local[net];
            }
        }
    }
    return projected;
}

void translate_control_result(const ProjectedControls &projected, NpnrLabControlResultV1 &result)
{
    auto translate = [&](NpnrControlSignalV1 &signal) {
        if (signal.net_id) {
            NPNR_ASSERT(signal.net_id <= projected.input.net_count);
            signal.net_id = projected.v2_net[signal.net_id];
        }
    };
    for (auto &signal : result.allocation)
        translate(signal);
    translate(result.incoming);
    for (auto &blocker : result.blockers)
        translate(blocker.signal);
}

bool check_mlab(const NpnrLabFactsV2 &input, NpnrLabAssessmentV2 &result)
{
    if (!input.is_mlab)
        return true;
    int found_group = -2;
    for (unsigned alm = 0; alm < 10; ++alm) {
        for (const auto &lut : input.alm[alm].lut) {
            if (!lut.occupied)
                continue;
            if (found_group == -2)
                found_group = lut.mlab_group;
            else if (found_group != lut.mlab_group)
                return fail(result, NPNR_LAB_V2_MLAB_GROUP, alm, UINT32_MAX, lut.mlab_group, found_group);
        }
    }
    if (found_group >= 0) {
        for (unsigned alm = 0; alm < 10; ++alm)
            for (unsigned ff = 0; ff < 4; ++ff)
                if (input.alm[alm].ff[ff].occupied)
                    return fail(result, NPNR_LAB_V2_MLAB_FF, alm, ff);
    }
    return true;
}

} // namespace

int resolved_lab_input_limit()
{
    static const int limit =
            std::getenv("MISTRAL_LAB_INPUT_LIMIT") ? std::atoi(std::getenv("MISTRAL_LAB_INPUT_LIMIT")) : 42;
    return limit;
}

NpnrLabFactsV2 capture_lab_v2(const Arch &arch, uint32_t lab, NpnrLabQueryV2 query, uint32_t query_alm,
                              uint64_t request_id, uint64_t epoch)
{
    NpnrLabFactsV2 input{};
    input.abi_version = NPNR_LAB_ABI_V2;
    input.struct_size = sizeof(input);
    input.request_id = request_id;
    input.snapshot_epoch = epoch;
    input.query = query;
    input.query_alm = query_alm;
    input.input_limit = resolved_lab_input_limit();
    input.is_mlab = arch.labs.at(lab).is_mlab;
    std::array<const NetInfo *, NPNR_LAB_V2_MAX_NETS> nets{};
    auto net_id = [&](const NetInfo *net) {
        if (!net)
            return uint32_t(0);
        uint32_t i = 0;
        while (i < input.net_count && nets[i] != net)
            ++i;
        if (i == input.net_count) {
            NPNR_ASSERT(i < nets.size());
            nets[i] = net;
            ++input.net_count;
        }
        return i + 1;
    };
    auto signal = [&](ControlSig value) {
        return NpnrControlSignalV1{net_id(value.net),
                                   (value.inverted ? uint32_t(NPNR_CONTROL_INVERTED) : 0u) |
                                           ((value.net && value.net->is_global) ? uint32_t(NPNR_CONTROL_GLOBAL) : 0u)};
    };
    const auto &lab_data = arch.labs.at(lab);
    for (unsigned alm = 0; alm < 10; ++alm) {
        auto &dest = input.alm[alm];
        const auto &source = lab_data.alms[alm];
        dest.cached_input_count = source.unique_input_count;
        for (unsigned i = 0; i < 2; ++i) {
            const auto *cell = arch.getBoundBelCell(source.lut_bels[i]);
            if (!cell)
                continue;
            auto &lut = dest.lut[i];
            lut.occupied = 1;
            lut.input_count = cell->combInfo.lut_input_count;
            lut.used_input_count = cell->combInfo.used_lut_input_count;
            lut.bits_count = cell->combInfo.lut_bits_count;
            lut.chain_shared_input_count = cell->combInfo.chain_shared_input_count;
            lut.mlab_group = cell->combInfo.mlab_group;
            lut.constr_z = cell->constr_z;
            lut.is_carry = cell->combInfo.is_carry;
            for (unsigned pin = 0; pin < lut.input_count; ++pin)
                lut.input_net[pin] = net_id(cell->combInfo.lut_in[pin]);
            lut.comb_out_net = net_id(cell->combInfo.comb_out);
            lut.wclk = signal(cell->combInfo.wclk);
            lut.we = signal(cell->combInfo.we);
        }
        for (unsigned i = 0; i < 4; ++i) {
            const auto *cell = arch.getBoundBelCell(source.ff_bels[i]);
            if (!cell)
                continue;
            auto &ff = dest.ff[i];
            ff.occupied = 1;
            ff.datain_net = net_id(cell->ffInfo.datain);
            ff.sdata_net = net_id(cell->ffInfo.sdata);
            const auto &cs = cell->ffInfo.ctrlset;
            const std::array<ControlSig, 5> controls{cs.clk, cs.sload, cs.sclr, cs.aclr, cs.ena};
            for (unsigned kind = 0; kind < controls.size(); ++kind)
                ff.control[kind] = signal(controls[kind]);
        }
    }
    return input;
}

NpnrLabAssessmentV2 evaluate_lab_v2_cpp(const NpnrLabFactsV2 &input)
{
    auto result = empty_assessment(input);
    if (input.abi_version != NPNR_LAB_ABI_V2 || input.struct_size != sizeof(input)) {
        result.status = NPNR_LAB_V2_MALFORMED;
        result.reason = NPNR_LAB_V2_BAD_HEADER;
        return result;
    }
    if (input.query > NPNR_LAB_QUERY_WHOLE_LAB ||
        (input.query != NPNR_LAB_QUERY_WHOLE_LAB && input.query_alm >= NPNR_LAB_V2_ALMS) ||
        (input.query == NPNR_LAB_QUERY_WHOLE_LAB && input.query_alm != UINT32_MAX)) {
        result.status = NPNR_LAB_V2_MALFORMED;
        result.reason = NPNR_LAB_V2_BAD_QUERY;
        return result;
    }
    if (!input_shape_valid(input)) {
        result.status = NPNR_LAB_V2_MALFORMED;
        result.reason = NPNR_LAB_V2_BAD_SHAPE;
        return result;
    }
    int total_inputs = 0;
    for (unsigned alm = 0; alm < 10; ++alm) {
        result.recomputed_input_count[alm] = recompute_alm_inputs(input.alm[alm]);
        result.recomputed_valid_mask |= 1u << alm;
        total_inputs += result.recomputed_input_count[alm];
    }
    if (input.query == NPNR_LAB_QUERY_WHOLE_LAB) {
        for (unsigned alm = 0; alm < 10; ++alm)
            if (!check_alm(input, alm, result))
                return result;
    } else if (!check_alm(input, input.query_alm, result)) {
        return result;
    }
    if (total_inputs > input.input_limit) {
        fail(result, NPNR_LAB_V2_LAB_INPUT_LIMIT, UINT32_MAX, UINT32_MAX, total_inputs, input.input_limit);
        return result;
    }
    if (input.query != NPNR_LAB_QUERY_COMB_BEL) {
        const auto projected = project_controls(input);
        result.control = evaluate_lab_controls_cpp(projected.input);
        translate_control_result(projected, result.control);
        result.control_valid = 1;
        if (result.control.status != NPNR_CONTROL_LEGAL) {
            fail(result, NPNR_LAB_V2_CONTROL_CONFLICT, result.control.ff_slot / 4, result.control.ff_slot % 4,
                 result.control.reason, 0);
            return result;
        }
    }
    check_mlab(input, result);
    return result;
}

bool lab_v2_result_valid(const NpnrLabFactsV2 &input, const NpnrLabAssessmentV2 &result)
{
    if (result.abi_version != NPNR_LAB_ABI_V2 || result.struct_size != sizeof(result) ||
        result.request_id != input.request_id || result.snapshot_epoch != input.snapshot_epoch ||
        result.query != input.query || result.query_alm != input.query_alm || result.reserved ||
        result.status > NPNR_LAB_V2_MALFORMED || result.control_valid > 1)
        return false;
    if (result.status == NPNR_LAB_V2_MALFORMED)
        return result.reason >= NPNR_LAB_V2_BAD_HEADER && result.reason <= NPNR_LAB_V2_BAD_SHAPE &&
               result.recomputed_valid_mask == 0 && !result.control_valid;
    if (result.recomputed_valid_mask != 0x3ffu)
        return false;
    if (result.status == NPNR_LAB_V2_LEGAL)
        return result.reason == NPNR_LAB_V2_OK && result.failing_alm == UINT32_MAX &&
               result.failing_slot == UINT32_MAX && result.observed == 0 && result.limit == 0 &&
               result.control_valid == (input.query != NPNR_LAB_QUERY_COMB_BEL);
    switch (result.reason) {
    case NPNR_LAB_V2_ALM_BITS:
    case NPNR_LAB_V2_ALM_INPUTS:
    case NPNR_LAB_V2_CARRY_MIX:
    case NPNR_LAB_V2_ODD_FF:
    case NPNR_LAB_V2_FF_CONTROL:
    case NPNR_LAB_V2_SDATA_PATH:
    case NPNR_LAB_V2_DATAIN_PATH:
        return result.failing_alm < 10 && !result.control_valid;
    case NPNR_LAB_V2_LAB_INPUT_LIMIT:
        return result.failing_alm == UINT32_MAX && result.failing_slot == UINT32_MAX && !result.control_valid;
    case NPNR_LAB_V2_CONTROL_CONFLICT:
        return result.control_valid && result.failing_alm < 10 && result.failing_slot < 4 &&
               result.control.status == NPNR_CONTROL_ILLEGAL;
    case NPNR_LAB_V2_MLAB_GROUP:
    case NPNR_LAB_V2_MLAB_FF:
        return result.failing_alm < 10 && result.control_valid == (input.query != NPNR_LAB_QUERY_COMB_BEL);
    default:
        return false;
    }
}

bool lab_v2_results_match(const NpnrLabAssessmentV2 &a, const NpnrLabAssessmentV2 &b)
{
    if (a.abi_version != b.abi_version || a.struct_size != b.struct_size || a.request_id != b.request_id ||
        a.snapshot_epoch != b.snapshot_epoch || a.status != b.status || a.reason != b.reason || a.query != b.query ||
        a.query_alm != b.query_alm || a.failing_alm != b.failing_alm || a.failing_slot != b.failing_slot ||
        a.observed != b.observed || a.limit != b.limit || a.recomputed_valid_mask != b.recomputed_valid_mask ||
        a.control_valid != b.control_valid || a.reserved != b.reserved)
        return false;
    for (unsigned i = 0; i < 10; ++i)
        if (a.recomputed_input_count[i] != b.recomputed_input_count[i])
            return false;
    return lab_control_first_difference(a.control, b.control).empty();
}

const char *lab_legality_mode_name(LabLegalityMode mode)
{
    switch (mode) {
    case LabLegalityMode::Legacy:
        return "legacy";
    case LabLegalityMode::Shadow:
        return "shadow";
    case LabLegalityMode::Verify:
        return "verify";
    case LabLegalityMode::Rust:
        return "rust";
    }
    NPNR_ASSERT_FALSE("unknown LAB legality mode");
}

void require_lab_legality_mode(LabLegalityMode mode)
{
#ifdef NO_RUST
    if (mode != LabLegalityMode::Legacy)
        log_error("LAB legality mode '%s' requires BUILD_RUST=ON.\n", lab_legality_mode_name(mode));
#else
    (void)mode;
#endif
}

namespace {
bool live_lab_query(const Arch &arch, uint32_t lab, NpnrLabQueryV2 query, uint32_t alm)
{
    if (query == NPNR_LAB_QUERY_WHOLE_LAB) {
        for (unsigned i = 0; i < 10; ++i)
            if (!arch.is_alm_legal(lab, i))
                return false;
    } else if (!arch.is_alm_legal(lab, alm)) {
        return false;
    }
    if (!arch.check_lab_input_count(lab))
        return false;
    if (query != NPNR_LAB_QUERY_COMB_BEL && !arch.is_lab_ctrlset_legal(lab))
        return false;
    return arch.check_mlab_groups(lab);
}
} // namespace

bool dispatch_lab_legality(const Arch &arch, uint32_t lab, NpnrLabQueryV2 query, uint32_t query_alm)
{
    const auto mode = arch.args.lab_legality;
    if (mode == LabLegalityMode::Legacy)
        return live_lab_query(arch, lab, query, query_alm);
    require_lab_legality_mode(mode);
    auto &stats = arch.lab_legality_stats;
    const auto request = stats.evaluations.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto revision = arch.placement_revision.stamp();
    const auto input = capture_lab_v2(arch, lab, query, query_alm, request, revision.revision);
    const bool live = live_lab_query(arch, lab, query, query_alm);
#ifndef NO_RUST
    NpnrLabAssessmentV2 candidate{};
    const auto call = npnr_mistral_eval_lab_v2(&input, 1, &candidate, 1);
    const bool valid = call == NPNR_LAB_CALL_OK && lab_v2_result_valid(input, candidate);
    const bool stale_revision = !arch.placement_revision.is_current(revision);
    bool stale = false;
    if (valid && candidate.recomputed_valid_mask == 0x3ffu) {
        for (unsigned i = 0; i < 10; ++i)
            stale |= candidate.recomputed_input_count[i] != input.alm[i].cached_input_count;
    }
    if (stale)
        stats.stale_cache.fetch_add(1, std::memory_order_relaxed);
    if (stale_revision)
        stats.stale_revision.fetch_add(1, std::memory_order_relaxed);
    if (valid && candidate.status == NPNR_LAB_V2_LEGAL)
        stats.legal.fetch_add(1, std::memory_order_relaxed);
    else if (valid && candidate.status == NPNR_LAB_V2_ILLEGAL)
        stats.illegal.fetch_add(1, std::memory_order_relaxed);

    bool mismatch = !valid;
    std::optional<NpnrLabAssessmentV2> cpp;
    if (mode != LabLegalityMode::Rust) {
        cpp = evaluate_lab_v2_cpp(input);
        mismatch |= !lab_v2_results_match(*cpp, candidate);
        mismatch |= (cpp->status == NPNR_LAB_V2_LEGAL) != live;
    }
    if (mismatch)
        stats.mismatches.fetch_add(1, std::memory_order_relaxed);
    if (!valid)
        stats.errors.fetch_add(1, std::memory_order_relaxed);
    if (mismatch || stale || stale_revision) {
        if (stats.diagnostics.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::ostringstream replay;
            const auto reference = cpp ? *cpp : evaluate_lab_v2_cpp(input);
            write_lab_v2_replay(replay, input, reference, "live LAB " + std::to_string(lab));
            log_warning("LAB legality %s: LAB %u request %" PRIu64
                        ", call %u, valid %u, mismatch %u, stale-cache %u, stale-revision %u.\nReplay: %s\n",
                        lab_legality_mode_name(mode), lab, request, call, valid, mismatch, stale, stale_revision,
                        replay.str().c_str());
        }
        if (mode != LabLegalityMode::Shadow) {
            report_lab_legality_stats(arch);
            log_error("LAB legality %s failed at LAB %u.\n", lab_legality_mode_name(mode), lab);
        }
    }
    if (mode == LabLegalityMode::Shadow)
        return live;
    return candidate.status == NPNR_LAB_V2_LEGAL;
#else
    return live;
#endif
}

void report_lab_legality_stats(const Arch &arch)
{
    if (arch.args.lab_legality == LabLegalityMode::Legacy)
        return;
    const auto &s = arch.lab_legality_stats;
    auto get = [](const std::atomic<uint64_t> &value) { return value.load(std::memory_order_relaxed); };
    log_info("LAB legality %s: evaluations=%" PRIu64 " legal=%" PRIu64 " illegal=%" PRIu64 " errors=%" PRIu64
             " mismatches=%" PRIu64 " stale-cache=%" PRIu64 " stale-revision=%" PRIu64 " diagnostics=%" PRIu64 ".\n",
             lab_legality_mode_name(arch.args.lab_legality), get(s.evaluations), get(s.legal), get(s.illegal),
             get(s.errors), get(s.mismatches), get(s.stale_cache), get(s.stale_revision),
             std::min(get(s.diagnostics), uint64_t(4)));
}

NEXTPNR_NAMESPACE_END
