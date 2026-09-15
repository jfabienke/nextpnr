/* SPDX-License-Identifier: ISC */
#include <array>
#include <cstring>
#include "gtest/gtest.h"
#include "lab_dispatch.h"
#include "lab_v2.h"

USING_NEXTPNR_NAMESPACE

TEST(LabControlFfi, EnvelopeErrorsLeaveHostStorageAlone)
{
    auto input = empty_lab_controls();
    auto sentinel = lab_control_result(input, NPNR_CONTROL_INTERNAL_ERROR);
    sentinel.reason = 12345;
    auto output = sentinel;
    EXPECT_EQ(npnr_mistral_eval_controls_v1(nullptr, 0, nullptr, 0), NPNR_LAB_CALL_OK);
    EXPECT_EQ(npnr_mistral_eval_controls_v1(&input, 0, &output, 0), NPNR_LAB_CALL_OK);
    EXPECT_EQ(npnr_mistral_eval_controls_v1(&input, 65, &output, 1), NPNR_LAB_CALL_BAD_COUNT);
    EXPECT_EQ(npnr_mistral_eval_controls_v1(&input, 1, &output, 0), NPNR_LAB_CALL_BAD_CAPACITY);
    EXPECT_EQ(npnr_mistral_eval_controls_v1(nullptr, 1, &output, 1), NPNR_LAB_CALL_NULL);
    EXPECT_EQ(npnr_mistral_eval_controls_v1(&input, 1, nullptr, 1), NPNR_LAB_CALL_NULL);
    auto misaligned = reinterpret_cast<const NpnrLabControlsV1 *>(reinterpret_cast<const char *>(&input) + 1);
    EXPECT_EQ(npnr_mistral_eval_controls_v1(misaligned, 1, &output, 1), NPNR_LAB_CALL_MISALIGNED);
    EXPECT_EQ(npnr_mistral_eval_controls_v1(&input, 1, reinterpret_cast<NpnrLabControlResultV1 *>(&input), 1),
              NPNR_LAB_CALL_OVERLAP);
    EXPECT_TRUE(lab_control_first_difference(output, sentinel).empty());
    const auto original = empty_lab_controls();
    EXPECT_EQ(std::memcmp(&input, &original, sizeof(input)), 0);
}

TEST(LabControlFfi, NativeBatchTransportAndMixedStatuses)
{
    std::array<NpnrLabControlsV1, NPNR_LAB_MAX_BATCH> inputs;
    for (unsigned i = 0; i < inputs.size(); ++i) {
        auto &input = inputs[i];
        input = empty_lab_controls(UINT64_MAX - i, UINT64_MAX - 100);
        if (i % 4 == 1)
            ++input.rules_version;
        else if (i % 4 == 2)
            input.ff[39].reserved = 1;
        else if (i % 4 == 3) {
            input.net_count = 2;
            input.ff[0].occupied = input.ff[39].occupied = 1;
            input.ff[0].control[0] = {1, NPNR_CONTROL_GLOBAL};
            input.ff[39].control[0] = {2, NPNR_CONTROL_INVERTED};
        }
    }
    const auto original = inputs;
    // Deliberately uninitialized: the ABI must initialize every requested field.
    std::array<NpnrLabControlResultV1, NPNR_LAB_MAX_BATCH + 1> outputs;
    const auto sentinel = lab_control_result(inputs[0], NPNR_CONTROL_INTERNAL_ERROR);
    outputs.back() = sentinel;
    ASSERT_EQ(npnr_mistral_eval_controls_v1(inputs.data(), inputs.size(), outputs.data(), outputs.size()),
              NPNR_LAB_CALL_OK);
    for (unsigned i = 0; i < inputs.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(lab_control_first_difference(evaluate_lab_controls_cpp(inputs[i]), outputs[i]), "");
    }
    EXPECT_EQ(lab_control_first_difference(sentinel, outputs.back()), "");
    EXPECT_EQ(std::memcmp(inputs.data(), original.data(), sizeof(inputs)), 0);
}

TEST(LabControlFfi, HostRejectsInvalidResultHeadersPlansAndDiagnostics)
{
    auto input = empty_lab_controls(7, 9);
    NpnrLabControlResultV1 result;
    ASSERT_EQ(npnr_mistral_eval_controls_v1(&input, 1, &result, 1), NPNR_LAB_CALL_OK);
    ASSERT_TRUE(lab_control_result_valid(input, result));
    const auto legal = result;
    for (auto *field : {&result.abi_version, &result.struct_size, &result.status, &result.reason, &result.reserved,
                        &result.control_kind, &result.ff_slot, &result.resource_mask, &result.allocation[0].net_id,
                        &result.allocation[0].flags, &result.incoming.net_id, &result.blockers[0].ff_slot,
                        &result.blockers[0].reserved}) {
        ++*field;
        EXPECT_FALSE(lab_control_result_valid(input, result));
        result = legal;
    }
    ++result.snapshot_epoch;
    EXPECT_FALSE(lab_control_result_valid(input, result));
    result = legal;
    ++result.request_id;
    EXPECT_FALSE(lab_control_result_valid(input, result));
    input.net_count = 2;
    input.ff[0].occupied = input.ff[39].occupied = 1;
    input.ff[0].control[0] = {1, 0};
    input.ff[39].control[0] = {2, 0};
    ASSERT_EQ(npnr_mistral_eval_controls_v1(&input, 1, &result, 1), NPNR_LAB_CALL_OK);
    ASSERT_TRUE(lab_control_result_valid(input, result));
    const auto illegal = result;
    for (auto *field : {&result.reason, &result.control_kind, &result.ff_slot, &result.resource_mask,
                        &result.incoming.net_id, &result.blockers[0].ff_slot, &result.blockers[0].signal.flags}) {
        ++*field;
        EXPECT_FALSE(lab_control_result_valid(input, result));
        result = illegal;
    }
}

TEST(LabControlFfi, V2EnvelopeAndStructuredResults)
{
    NpnrLabFactsV2 input{};
    input.abi_version = NPNR_LAB_ABI_V2;
    input.struct_size = sizeof(input);
    input.request_id = UINT64_MAX;
    input.snapshot_epoch = UINT64_MAX - 1;
    input.query = NPNR_LAB_QUERY_WHOLE_LAB;
    input.query_alm = UINT32_MAX;
    input.input_limit = 42;
    NpnrLabAssessmentV2 sentinel{};
    sentinel.reason = 9999;
    auto output = sentinel;
    EXPECT_EQ(npnr_mistral_eval_lab_v2(nullptr, 0, nullptr, 0), NPNR_LAB_CALL_OK);
    EXPECT_EQ(npnr_mistral_eval_lab_v2(&input, 1, &output, 0), NPNR_LAB_CALL_BAD_CAPACITY);
    EXPECT_EQ(output.reason, sentinel.reason);
    ASSERT_EQ(npnr_mistral_eval_lab_v2(&input, 1, &output, 1), NPNR_LAB_CALL_OK);
    const auto cpp = evaluate_lab_v2_cpp(input);
    EXPECT_EQ(output.status, cpp.status);
    EXPECT_EQ(output.reason, cpp.reason);
    EXPECT_EQ(output.request_id, input.request_id);
    EXPECT_EQ(output.snapshot_epoch, input.snapshot_epoch);
    EXPECT_EQ(output.recomputed_valid_mask, 0x3ffu);

    input.alm[0].ff[1].occupied = 1;
    ASSERT_EQ(npnr_mistral_eval_lab_v2(&input, 1, &output, 1), NPNR_LAB_CALL_OK);
    EXPECT_EQ(output.status, NPNR_LAB_V2_ILLEGAL);
    EXPECT_EQ(output.reason, NPNR_LAB_V2_ODD_FF);
    EXPECT_EQ(output.failing_alm, 0u);
    EXPECT_EQ(output.failing_slot, 1u);
}
