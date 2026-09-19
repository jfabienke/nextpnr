/* SPDX-License-Identifier: ISC */
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include "alm_pairing.h"
#include "build_state.h"
#include "checkpoint.h"
#include "gtest/gtest.h"
#include "json11.hpp"
#include "lab_control_edits.h"
#include "lab_control_plan.h"
#include "lab_preparation.h"
#include "lab_replay.h"
#include "lab_resident.h"
#include "lab_reuse.h"
#include "lab_snapshot.h"
#include "lab_v2.h"
#include "lab_v2_replay.h"
#include "log.h"
#include "nextpnr.h"
#include "placement_coordinator.h"
#include "placement_pool.h"
#include "placement_reuse.h"
#include "placement_transaction.h"
#include "placer_heap.h"
#include "register_packing.h"
#include "reuse_plan.h"
#include "route_reuse.h"
#include "telemetry.h"

USING_NEXTPNR_NAMESPACE

namespace {
void expect_no_plan(const NpnrLabControlResultV1 &result)
{
    for (const auto &signal : result.allocation) {
        EXPECT_EQ(signal.net_id, 0u);
        EXPECT_EQ(signal.flags, 0u);
    }
}

#ifndef NO_RUST
void expect_v2_rust_parity(const NpnrLabFactsV2 &input, const NpnrLabAssessmentV2 &cpp)
{
    NpnrLabAssessmentV2 rust{};
    ASSERT_EQ(npnr_mistral_eval_lab_v2(&input, 1, &rust, 1), NPNR_LAB_CALL_OK);
    EXPECT_EQ(rust.abi_version, cpp.abi_version);
    EXPECT_EQ(rust.struct_size, cpp.struct_size);
    EXPECT_EQ(rust.request_id, cpp.request_id);
    EXPECT_EQ(rust.snapshot_epoch, cpp.snapshot_epoch);
    EXPECT_EQ(rust.status, cpp.status);
    EXPECT_EQ(rust.reason, cpp.reason);
    EXPECT_EQ(rust.query, cpp.query);
    EXPECT_EQ(rust.query_alm, cpp.query_alm);
    EXPECT_EQ(rust.failing_alm, cpp.failing_alm);
    EXPECT_EQ(rust.failing_slot, cpp.failing_slot);
    EXPECT_EQ(rust.observed, cpp.observed);
    EXPECT_EQ(rust.limit, cpp.limit);
    EXPECT_EQ(rust.recomputed_valid_mask, cpp.recomputed_valid_mask);
    EXPECT_EQ(rust.control_valid, cpp.control_valid);
    EXPECT_EQ(rust.reserved, cpp.reserved);
    for (unsigned i = 0; i < NPNR_LAB_V2_ALMS; ++i)
        EXPECT_EQ(rust.recomputed_input_count[i], cpp.recomputed_input_count[i]);
    EXPECT_EQ(lab_control_first_difference(rust.control, cpp.control), "");
}
#endif

WireId reserved_source(const Context &ctx, WireId destination)
{
    const auto &wire = ctx.wires.at(destination);
    EXPECT_NE(wire.flags & WireInfo::RESERVED_ROUTE, 0u);
    const auto index = unsigned(wire.flags & 0xff);
    EXPECT_LT(index, wire.wires_uphill.size());
    return index < wire.wires_uphill.size() ? wire.wires_uphill[index] : WireId();
}

TEST(PlacementRevision, TypedKeysAndAbaSafeMutationStamps)
{
    static_assert(!std::is_same<PlacementCellKey, PlacementNetKey>::value, "cell and net keys must not mix");
    PlacementRevisionState state(77);
    const auto before = state.stamp();
    const auto cell = state.issue_cell_key();
    const auto net = state.issue_net_key();
    EXPECT_TRUE(cell);
    EXPECT_TRUE(net);
    EXPECT_EQ(cell.session.value, 77u);
    EXPECT_EQ(net.session.value, 77u);
    EXPECT_NE(cell.value, net.value);

    EXPECT_TRUE(state.note_mutation(PlacementMutation::BelBinding));
    const auto provisional = state.stamp();
    EXPECT_FALSE(state.is_current(before));
    EXPECT_TRUE(state.note_mutation(PlacementMutation::BelBinding)); // restoration is another mutation
    EXPECT_FALSE(state.is_current(provisional));
    EXPECT_EQ(state.mutation_count(PlacementMutation::BelBinding), 2u);

    PlacementRevisionState exhausted(78, std::numeric_limits<uint64_t>::max());
    EXPECT_FALSE(exhausted.note_mutation(PlacementMutation::BelBinding));
    EXPECT_FALSE(exhausted.valid());
    EXPECT_FALSE(exhausted.stamp());
}

TEST(LabControlModel, EmptyAndDisconnectedPolarity)
{
    auto input = empty_lab_controls(UINT64_MAX, UINT64_MAX - 1);
    input.ff[39].occupied = 1;
    input.ff[39].control[NPNR_CONTROL_CLK].flags = NPNR_CONTROL_INVERTED;
    const auto before = input;
    auto result = evaluate_lab_controls_cpp(input);
    EXPECT_EQ(result.status, NPNR_CONTROL_LEGAL);
    EXPECT_EQ(result.request_id, UINT64_MAX);
    EXPECT_EQ(result.snapshot_epoch, UINT64_MAX - 1);
    EXPECT_EQ(result.control_kind, UINT32_MAX);
    EXPECT_EQ(result.ff_slot, UINT32_MAX);
    expect_no_plan(result); // disconnected inverted inputs never allocate a resource
    EXPECT_EQ(std::memcmp(&before, &input, sizeof(input)), 0);
}

TEST(LabControlModel, PoolCapacitiesAndFirstConflict)
{
    const std::array<unsigned, 5> capacity{1, 1, 1, 2, 3};
    for (unsigned kind = 0; kind < capacity.size(); ++kind) {
        SCOPED_TRACE(kind);
        auto input = empty_lab_controls();
        input.net_count = capacity[kind] + 1;
        for (unsigned i = 0; i < input.net_count; ++i) {
            input.ff[i].occupied = 1;
            input.ff[i].control[kind].net_id = i + 1;
        }
        auto result = evaluate_lab_controls_cpp(input);
        EXPECT_EQ(result.status, NPNR_CONTROL_ILLEGAL);
        EXPECT_EQ(result.reason, kind + 1);
        EXPECT_EQ(result.control_kind, kind);
        EXPECT_EQ(result.ff_slot, capacity[kind]);
        EXPECT_EQ(result.incoming.net_id, capacity[kind] + 1);
        EXPECT_EQ(result.resource_mask, (1u << capacity[kind]) - 1);
        for (unsigned i = 0; i < capacity[kind]; ++i) {
            EXPECT_EQ(result.blockers[i].signal.net_id, i + 1);
            EXPECT_EQ(result.blockers[i].ff_slot, i);
        }
        expect_no_plan(result);
    }
}

TEST(LabControlModel, GlobalClockFreesDatainZero)
{
    auto input = empty_lab_controls();
    input.net_count = 4;
    for (unsigned i = 0; i < 3; ++i) {
        input.ff[i].occupied = 1;
        input.ff[i].control[NPNR_CONTROL_CLK] = {1, 0};
        input.ff[i].control[NPNR_CONTROL_ENA] = {i + 2, 0};
    }
    auto local = evaluate_lab_controls_cpp(input);
    EXPECT_EQ(local.status, NPNR_CONTROL_ILLEGAL);
    EXPECT_EQ(local.reason, NPNR_CONTROL_DATAIN_CONFLICT);
    EXPECT_EQ(local.control_kind, NPNR_CONTROL_ENA);
    EXPECT_EQ(local.ff_slot, 2u);
    EXPECT_EQ(local.resource_mask, 13u); // DATAIN 2, 3, 0
    for (unsigned i = 0; i < 3; ++i)
        input.ff[i].control[NPNR_CONTROL_CLK].flags = NPNR_CONTROL_GLOBAL;
    auto global = evaluate_lab_controls_cpp(input);
    ASSERT_EQ(global.status, NPNR_CONTROL_LEGAL);
    EXPECT_EQ(global.allocation[8].net_id, 4u);
    EXPECT_EQ(global.allocation[10].net_id, 2u);
    EXPECT_EQ(global.allocation[11].net_id, 3u);
}

TEST(LabControlModel, GlobalResetStillConsumesDatain)
{
    auto input = empty_lab_controls();
    input.net_count = 3;
    for (unsigned i = 0; i < 2; ++i) {
        input.ff[i].occupied = 1;
        input.ff[i].control[NPNR_CONTROL_SCLR] = {1, 0};
        input.ff[i].control[NPNR_CONTROL_ACLR] = {i + 2, NPNR_CONTROL_GLOBAL};
    }
    auto result = evaluate_lab_controls_cpp(input);
    EXPECT_EQ(result.status, NPNR_CONTROL_ILLEGAL);
    EXPECT_EQ(result.reason, NPNR_CONTROL_DATAIN_CONFLICT);
    EXPECT_EQ(result.control_kind, NPNR_CONTROL_ACLR);
    EXPECT_EQ(result.resource_mask, 12u);
    EXPECT_EQ(result.blockers[3].signal.net_id, 1u);
    EXPECT_EQ(result.blockers[2].signal.net_id, 2u);
}

TEST(LabControlModel, MalformedSnapshotsAreNotIllegalPlacements)
{
    std::vector<std::pair<NpnrLabControlsV1, uint32_t>> cases;
    auto add = [&](auto change, uint32_t reason) {
        auto input = empty_lab_controls();
        change(input);
        cases.emplace_back(input, reason);
    };
    add([](auto &s) { s.abi_version = 2; }, NPNR_CONTROL_BAD_ABI);
    add([](auto &s) { --s.struct_size; }, NPNR_CONTROL_BAD_SIZE);
    add([](auto &s) { s.rules_version = 2; }, NPNR_CONTROL_BAD_RULES);
    add([](auto &s) { s.net_count = 201; }, NPNR_CONTROL_BAD_NET_COUNT);
    add([](auto &s) { s.ff[0].occupied = 2; }, NPNR_CONTROL_BAD_OCCUPANCY);
    add([](auto &s) { s.ff[39].reserved = 1; }, NPNR_CONTROL_BAD_RESERVED);
    add([](auto &s) { s.ff[0].control[0].flags = 1; }, NPNR_CONTROL_BAD_SIGNAL);
    add(
            [](auto &s) {
                s.ff[0].occupied = 1;
                s.ff[0].control[0].flags = 4;
            },
            NPNR_CONTROL_BAD_SIGNAL);
    add(
            [](auto &s) {
                s.ff[0].occupied = 1;
                s.ff[0].control[0].flags = 2;
            },
            NPNR_CONTROL_BAD_SIGNAL);
    add(
            [](auto &s) {
                s.ff[0].occupied = 1;
                s.ff[0].control[0].net_id = UINT32_MAX;
            },
            NPNR_CONTROL_BAD_SIGNAL);
    add([](auto &s) { s.net_count = 1; }, NPNR_CONTROL_SPARSE_NET_IDS);
    add(
            [](auto &s) {
                s.net_count = 1;
                s.ff[0].occupied = 1;
                s.ff[0].control[0] = {1, 0};
                s.ff[0].control[1] = {1, NPNR_CONTROL_GLOBAL};
            },
            NPNR_CONTROL_INCONSISTENT_GLOBAL);
    for (const auto &test : cases) {
        SCOPED_TRACE(test.second);
        auto result = evaluate_lab_controls_cpp(test.first);
        EXPECT_EQ(result.reason, test.second);
        EXPECT_EQ(result.status,
                  test.second == NPNR_CONTROL_BAD_RULES ? NPNR_CONTROL_UNSUPPORTED_RULES : NPNR_CONTROL_BAD_SNAPSHOT);
        expect_no_plan(result);
    }
}

TEST(LabControlModel, ResultsMustBelongToTheSameRequest)
{
    auto result = evaluate_lab_controls_cpp(empty_lab_controls(7, 11));
    auto other = result;
    ++other.request_id;
    EXPECT_FALSE(lab_control_results_match(result, other));
    other = result;
    ++other.snapshot_epoch;
    EXPECT_FALSE(lab_control_results_match(result, other));
    other = result;
    other.allocation[8] = {1, 0};
    EXPECT_FALSE(lab_control_results_match(result, other));
}

TEST(LabControlReplay, PreservesWideProvenanceAndRejectsTruncation)
{
    auto input = empty_lab_controls(UINT64_MAX, UINT64_MAX - 1);
    auto reference = evaluate_lab_controls_cpp(input);
    std::ostringstream out;
    ASSERT_TRUE(write_lab_control_replay(out, input, reference, "fixture \"quoted\""));
    auto decoded = empty_lab_controls();
    NpnrLabControlResultV1 result{};
    std::string error;
    ASSERT_TRUE(read_lab_control_replay(out.str(), decoded, result, error)) << error;
    EXPECT_EQ(std::memcmp(&input, &decoded, sizeof(input)), 0);
    EXPECT_TRUE(lab_control_results_match(reference, result));
    EXPECT_FALSE(read_lab_control_replay(out.str().substr(0, 70), decoded, result, error));
    EXPECT_EQ(decoded.request_id, UINT64_MAX); // unsuccessful reads leave outputs alone
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(read_lab_control_replay(out.str(), decoded, result, error)) << error;
}

TEST(LabControlReplay, CommittedFixtures)
{
    for (const auto *name : {"greedy-abc.json", "greedy-bca.json"}) {
        SCOPED_TRACE(name);
        std::ifstream file(std::string(LAB_CONTROL_FIXTURE_DIR) + "/" + name);
        ASSERT_TRUE(file.good());
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        NpnrLabControlsV1 input{};
        NpnrLabControlResultV1 expected{};
        std::string error;
        ASSERT_TRUE(read_lab_control_replay(text, input, expected, error)) << error;
        EXPECT_TRUE(lab_control_results_match(expected, evaluate_lab_controls_cpp(input)));
    }
}

class LabControlCaptureTest : public ::testing::Test
{
  protected:
    static std::unique_ptr<Context> ctx;
    static std::array<CellInfo *, 40> cells;
    static std::array<NetInfo *, 200> nets;

    static void SetUpTestSuite()
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        for (unsigned i = 0; i < nets.size(); ++i)
            nets[i] = ctx->createNet(ctx->idf("lab_test_net_%u", i));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        for (unsigned i = 0; i < cells.size(); ++i)
            cells[i] = ctx->createCell(ctx->idf("lab_test_ff_%u", i), id_MISTRAL_FF);
    }

    static void TearDownTestSuite() { ctx.reset(); }

    void SetUp() override
    {
        ctx->args.lab_controls = LabControlMode::Legacy;
        ctx->args.lab_legality = LabLegalityMode::Legacy;
        for (auto *net : nets)
            net->is_global = false;
        for (auto *cell : cells) {
            for (const auto &port : cell->ports)
                if (port.second.net)
                    cell->disconnectPort(port.first);
            cell->ports.clear();
            cell->pin_data.clear();
            cell->ffInfo.ctrlset = {};
            cell->ffInfo.datain = nullptr;
            cell->ffInfo.sdata = nullptr;
        }
    }

    void TearDown() override { clear_bindings(); }

    void clear_bindings()
    {
        for (auto *cell : cells)
            if (cell->bel != BelId())
                ctx->unbindBel(cell->bel);
    }

    void bind(unsigned slot)
    {
        ctx->bindBel(ctx->labs.at(0).alms.at(slot / 4).ff_bels.at(slot % 4), cells[slot], STRENGTH_WEAK);
    }

    NpnrLabControlResultV1 compare()
    {
        const auto capture = capture_lab_controls(*ctx, 0, 123, 456);
        const auto native = evaluate_lab_controls_native(*ctx, 0);
        const auto legacy = evaluate_lab_controls_legacy(*ctx, 0, capture);
        const auto detached = evaluate_lab_controls_cpp(capture.input);
        EXPECT_EQ(native.legal, legacy.status == NPNR_CONTROL_LEGAL);
        EXPECT_EQ(native.allocation.has_value(), native.legal);
        if (native.allocation) {
            const auto signals = native.allocation->as_array();
            for (unsigned i = 0; i < signals.size(); ++i) {
                const auto encoded = capture.encode_existing(signals[i]);
                EXPECT_EQ(encoded.net_id, legacy.allocation[i].net_id);
                EXPECT_EQ(encoded.flags, legacy.allocation[i].flags);
            }
        } else {
            expect_no_plan(legacy);
        }
        EXPECT_TRUE(lab_control_results_match(legacy, detached))
                << "status " << detached.status << " reason " << detached.reason;
        EXPECT_EQ(ctx->is_lab_ctrlset_legal(0), legacy.status == NPNR_CONTROL_LEGAL);
#ifndef NO_RUST
        NpnrLabControlResultV1 rust;
        EXPECT_EQ(npnr_mistral_eval_controls_v1(&capture.input, 1, &rust, 1), NPNR_LAB_CALL_OK);
        EXPECT_EQ(lab_control_first_difference(detached, rust), "");
        EXPECT_TRUE(lab_control_result_valid(capture.input, rust));
        for (auto mode : {LabControlMode::Shadow, LabControlMode::Verify, LabControlMode::Rust}) {
            ctx->args.lab_controls = mode;
            EXPECT_EQ(ctx->is_lab_ctrlset_legal(0), legacy.status == NPNR_CONTROL_LEGAL);
        }
        ctx->args.lab_controls = LabControlMode::Legacy;
#endif
        return detached;
    }
};

std::unique_ptr<Context> LabControlCaptureTest::ctx;
std::array<CellInfo *, 40> LabControlCaptureTest::cells{};
std::array<NetInfo *, 200> LabControlCaptureTest::nets{};

TEST_F(LabControlCaptureTest, HeAPClusterRollbackRestoresOriginalStrength)
{
    const BelId bel = ctx->labs.at(0).alms.at(0).ff_bels.at(0);
    const auto before = ctx->placement_revision.stamp();
    const uint64_t before_count = ctx->placement_revision.mutation_count(PlacementMutation::BelBinding);
    ctx->bindBel(bel, cells[0], STRENGTH_STRONG);
    const auto displaced_stamp = ctx->placement_revision.stamp();
    HeAPDisplacedBindings displaced;
    displaced[bel] = {cells[0], cells[0]->belStrength};

    ctx->unbindBel(bel);
    ctx->bindBel(bel, cells[1], STRENGTH_STRONG);
    restore_heap_cluster_bindings(ctx.get(), displaced);

    EXPECT_FALSE(ctx->placement_revision.is_current(before));
    EXPECT_FALSE(ctx->placement_revision.is_current(displaced_stamp));
    EXPECT_EQ(ctx->placement_revision.mutation_count(PlacementMutation::BelBinding), before_count + 5);
    EXPECT_EQ(ctx->getBoundBelCell(bel), cells[0]);
    EXPECT_EQ(cells[0]->bel, bel);
    EXPECT_EQ(cells[0]->belStrength, STRENGTH_STRONG);
    EXPECT_EQ(cells[1]->bel, BelId());
    EXPECT_EQ(cells[1]->belStrength, STRENGTH_NONE);
}

TEST_F(LabControlCaptureTest, PlacementRevisionTracksCellFactsAndConnectivity)
{
    const auto stamp = ctx->placement_revision.stamp();
    const uint64_t fact_count = ctx->placement_revision.mutation_count(PlacementMutation::CellFacts);
    const uint64_t connection_count = ctx->placement_revision.mutation_count(PlacementMutation::Connectivity);
    const IdString port = ctx->id("REVISION_TEST_INPUT");

    cells[0]->addInput(port);
    cells[0]->connectPort(port, nets[0]);
    cells[0]->disconnectPort(port);

    EXPECT_FALSE(ctx->placement_revision.is_current(stamp));
    EXPECT_EQ(ctx->placement_revision.mutation_count(PlacementMutation::CellFacts), fact_count + 1);
    EXPECT_EQ(ctx->placement_revision.mutation_count(PlacementMutation::Connectivity), connection_count + 2);
}

TEST_F(LabControlCaptureTest, SerialPlacementCommitPreflightsAndRejectsStaleWork)
{
    const BelId first = ctx->labs.at(0).alms.at(0).ff_bels.at(0);
    const BelId second = ctx->labs.at(0).alms.at(0).ff_bels.at(1);
    const BelId unrelated = ctx->labs.at(0).alms.at(1).ff_bels.at(0);
    ctx->bindBel(first, cells[0], STRENGTH_WEAK);
    ctx->bindBel(second, cells[1], STRENGTH_STRONG);

    std::vector<PlacementBindingEdit> swap{{first, cells[0], STRENGTH_WEAK, cells[1], STRENGTH_STRONG},
                                           {second, cells[1], STRENGTH_STRONG, cells[0], STRENGTH_WEAK}};
    auto prepared = prepare_placement_transaction(*ctx, swap);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(commit_placement_transaction(*ctx, std::move(prepared)), PlacementCommitOutcome::Committed);
    EXPECT_EQ(ctx->getBoundBelCell(first), cells[1]);
    EXPECT_EQ(ctx->getBoundBelCell(second), cells[0]);
    EXPECT_EQ(cells[1]->belStrength, STRENGTH_STRONG);
    EXPECT_EQ(cells[0]->belStrength, STRENGTH_WEAK);

    std::vector<PlacementBindingEdit> swap_back{{first, cells[1], STRENGTH_STRONG, cells[0], STRENGTH_WEAK},
                                                {second, cells[0], STRENGTH_WEAK, cells[1], STRENGTH_STRONG}};
    auto stale = prepare_placement_transaction(*ctx, std::move(swap_back));
    ASSERT_TRUE(stale);
    ctx->bindBel(unrelated, cells[2], STRENGTH_WEAK);
    ctx->unbindBel(unrelated);
    EXPECT_EQ(commit_placement_transaction(*ctx, std::move(stale)), PlacementCommitOutcome::Stale);
    EXPECT_EQ(ctx->getBoundBelCell(first), cells[1]);
    EXPECT_EQ(ctx->getBoundBelCell(second), cells[0]);

    std::vector<PlacementBindingEdit> duplicate{{first, cells[1], STRENGTH_STRONG, nullptr, STRENGTH_NONE},
                                                {first, cells[1], STRENGTH_STRONG, nullptr, STRENGTH_NONE}};
    EXPECT_FALSE(prepare_placement_transaction(*ctx, std::move(duplicate)));
}

TEST_F(LabControlCaptureTest, FrozenPlacementOverlayEvaluatesWithoutLiveMutation)
{
    const BelId even = ctx->labs.at(0).alms.at(0).ff_bels.at(0);
    const BelId odd = ctx->labs.at(0).alms.at(0).ff_bels.at(1);
    const auto original = ctx->placement_revision.stamp();

    auto legal_transaction =
            prepare_placement_transaction(*ctx, {{even, nullptr, STRENGTH_NONE, cells[0], STRENGTH_STRONG}});
    ASSERT_TRUE(legal_transaction);
    const auto legal_frozen = freeze_placement_candidate(*ctx, legal_transaction);
    ASSERT_EQ(legal_frozen.status, FrozenPlacementStatus::Ready);
    ASSERT_EQ(legal_frozen.queries.size(), 1u);
    const auto legal = evaluate_placement_candidate(legal_frozen);
    EXPECT_EQ(legal.status, FrozenPlacementStatus::Ready);
    EXPECT_TRUE(legal.legal);
    EXPECT_TRUE(placement_candidate_rust_matches(legal_frozen, legal));
    EXPECT_EQ(ctx->getBoundBelCell(even), nullptr);
    EXPECT_TRUE(ctx->placement_revision.is_current(original));
#ifndef NO_RUST
    expect_v2_rust_parity(legal_frozen.queries[0], legal.results[0]);
#endif

    auto illegal_transaction =
            prepare_placement_transaction(*ctx, {{odd, nullptr, STRENGTH_NONE, cells[1], STRENGTH_STRONG}});
    ASSERT_TRUE(illegal_transaction);
    const auto illegal_frozen = freeze_placement_candidate(*ctx, illegal_transaction);
    const auto illegal = evaluate_placement_candidate(illegal_frozen);
    ASSERT_EQ(illegal.results.size(), 1u);
    EXPECT_FALSE(illegal.legal);
    EXPECT_TRUE(placement_candidate_rust_matches(illegal_frozen, illegal));
    EXPECT_EQ(illegal.results[0].reason, NPNR_LAB_V2_ODD_FF);
    EXPECT_EQ(ctx->getBoundBelCell(odd), nullptr);
    EXPECT_TRUE(ctx->placement_revision.is_current(original));

    ctx->bindBel(even, cells[2], STRENGTH_WEAK);
    ctx->unbindBel(even);
    EXPECT_EQ(freeze_placement_candidate(*ctx, illegal_transaction).status, FrozenPlacementStatus::Stale);
}

namespace {
// A HeAP-shaped candidate: `cell` moves onto `bel`, displacing whatever is bound there.
HeAPClusterCandidate cluster_candidate(const Context &ctx, CellInfo *cell, BelId bel)
{
    HeAPClusterCandidate candidate;
    candidate.targets.emplace_back(cell, bel);
    CellInfo *bound = ctx.getBoundBelCell(bel);
    candidate.displaced[bel] = {bound, bound == nullptr ? STRENGTH_NONE : bound->belStrength};
    return candidate;
}

BelId first_non_lab_bel(const Context &ctx)
{
    for (BelId bel : ctx.getBels()) {
        IdString type = ctx.getBelType(bel);
        if (!type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF))
            return bel;
    }
    return BelId();
}
} // namespace

TEST(PlacementWorkerPool, CoversEveryIndexOnceCapturesFailuresAndRunsInline)
{
    for (unsigned workers : {1u, 2u, 5u, 9u}) {
        SCOPED_TRACE(workers);
        PlacementWorkerPool pool(workers);
        EXPECT_EQ(pool.workers(), workers);
        // Every index exactly once, by whichever worker claims it; the worker id is in range.
        std::vector<std::atomic<int>> hits(1000);
        std::vector<std::atomic<int>> per_worker(workers);
        for (auto &h : hits)
            h.store(0);
        for (auto &w : per_worker)
            w.store(0);
        const auto failure = pool.run(hits.size(), [&](unsigned worker, size_t i) {
            ASSERT_LT(worker, workers);
            hits[i].fetch_add(1);
            per_worker[worker].fetch_add(1);
        });
        EXPECT_TRUE(failure.empty());
        for (const auto &h : hits)
            EXPECT_EQ(h.load(), 1);
        int total = 0;
        for (const auto &w : per_worker)
            total += w.load();
        EXPECT_EQ(total, 1000);
        if (workers == 1)
            EXPECT_EQ(per_worker[0].load(), 1000); // inline, no threads
        // A throwing job is reported, the run completes, and the pool is still usable.
        std::atomic<int> after{0};
        const auto message = pool.run(64, [&](unsigned, size_t i) {
            if (i == 17)
                throw std::runtime_error("job seventeen failed");
            after.fetch_add(1);
        });
        EXPECT_EQ(message, "job seventeen failed");
        EXPECT_TRUE(pool.run(8, [&](unsigned, size_t) { after.fetch_add(1); }).empty());
        EXPECT_GE(after.load(), 8);
        // Zero work is a no-op.
        EXPECT_TRUE(pool.run(0, [&](unsigned, size_t) { FAIL() << "must not run"; }).empty());
    }
}

TEST_F(LabControlCaptureTest, BatchCoordinatorCommitsFirstLegalInProposalOrderForEveryWorkerCount)
{
    const auto &alms = ctx->labs.at(0).alms;
    const BelId odd_a = alms.at(0).ff_bels.at(1);
    const BelId odd_b = alms.at(1).ff_bels.at(3);
    const BelId even_a = alms.at(2).ff_bels.at(0);
    const BelId even_b = alms.at(3).ff_bels.at(2);

    for (unsigned workers : {1u, 2u, 4u, 8u}) {
        SCOPED_TRACE(workers);
        std::vector<HeAPClusterCandidate> batch;
        batch.push_back(cluster_candidate(*ctx, cells[0], odd_a));  // illegal: odd FF slot
        batch.push_back(cluster_candidate(*ctx, cells[1], odd_b));  // illegal: odd FF slot
        batch.push_back(cluster_candidate(*ctx, cells[2], even_a)); // first legal
        batch.push_back(cluster_candidate(*ctx, cells[3], even_b)); // legal, must be discarded

        const auto before = ctx->placement_revision.stamp();
        const auto bindings_before = ctx->placement_revision.mutation_count(PlacementMutation::BelBinding);
        PlacementCandidateCoordinator coordinator(*ctx, workers);
        EXPECT_EQ(coordinator.workers(), workers);
        const auto outcome = coordinator.place(batch);
        EXPECT_EQ(outcome.status, HeAPClusterBatchStatus::Committed);
        EXPECT_EQ(outcome.index, 2u);
        EXPECT_EQ(ctx->getBoundBelCell(even_a), cells[2]);
        EXPECT_EQ(cells[2]->belStrength, STRENGTH_STRONG);
        EXPECT_EQ(ctx->getBoundBelCell(even_b), nullptr);
        EXPECT_EQ(ctx->getBoundBelCell(odd_a), nullptr);
        EXPECT_EQ(ctx->getBoundBelCell(odd_b), nullptr);
        // Exactly one live mutation: the commit itself. Nothing was bound provisionally.
        EXPECT_EQ(ctx->placement_revision.mutation_count(PlacementMutation::BelBinding), bindings_before + 1);
        EXPECT_FALSE(ctx->placement_revision.is_current(before));

        const auto &s = coordinator.stats();
        EXPECT_EQ(s.batches, 1u);
        EXPECT_EQ(s.candidates, 4u);
        EXPECT_EQ(s.evaluated, 4u);
        EXPECT_EQ(s.queries, 4u);
        EXPECT_EQ(s.committed, 1u);
        EXPECT_EQ(s.rejected, 2u);
        EXPECT_EQ(s.discarded, 1u);
        EXPECT_EQ(s.unsupported, 0u);
        EXPECT_EQ(s.stale, 0u);
        EXPECT_EQ(s.stale_retries, 0u);
        EXPECT_EQ(s.synchronous_fallbacks, 0u);
#ifndef NO_RUST
        EXPECT_EQ(s.rust_batches, 4u); // one handle per candidate, created by its worker
        EXPECT_EQ(s.rust_direct, 0u);
#endif
        ctx->unbindBel(even_a);
    }
}

TEST_F(LabControlCaptureTest, BatchCoordinatorCommitsDisplacingMoveAndRejectsEverythingElse)
{
    const auto &alms = ctx->labs.at(0).alms;
    const BelId home = alms.at(0).ff_bels.at(0);
    const BelId odd = alms.at(0).ff_bels.at(1);
    ctx->bindBel(home, cells[4], STRENGTH_WEAK);

    // Every candidate illegal: no mutation, revision untouched, NoneLegal.
    {
        std::vector<HeAPClusterCandidate> batch;
        batch.push_back(cluster_candidate(*ctx, cells[5], odd));
        batch.push_back(cluster_candidate(*ctx, cells[6], odd));
        const auto before = ctx->placement_revision.stamp();
        PlacementCandidateCoordinator coordinator(*ctx, 3);
        const auto outcome = coordinator.place(batch);
        EXPECT_EQ(outcome.status, HeAPClusterBatchStatus::NoneLegal);
        EXPECT_TRUE(ctx->placement_revision.is_current(before));
        EXPECT_EQ(ctx->getBoundBelCell(home), cells[4]);
        EXPECT_EQ(coordinator.stats().rejected, 2u);
        EXPECT_EQ(coordinator.stats().committed, 0u);
        EXPECT_EQ(coordinator.stats().discarded, 0u);
    }

    // A displacing move: cells[5] takes cells[4]'s BEL; cells[4] ends up unplaced.
    {
        std::vector<HeAPClusterCandidate> batch;
        batch.push_back(cluster_candidate(*ctx, cells[5], odd));
        batch.push_back(cluster_candidate(*ctx, cells[5], home));
        ASSERT_EQ(batch[1].displaced.at(home).cell, cells[4]);
        ASSERT_EQ(batch[1].displaced.at(home).strength, STRENGTH_WEAK);
        PlacementCandidateCoordinator coordinator(*ctx, 2);
        const auto outcome = coordinator.place(batch);
        EXPECT_EQ(outcome.status, HeAPClusterBatchStatus::Committed);
        EXPECT_EQ(outcome.index, 1u);
        EXPECT_EQ(ctx->getBoundBelCell(home), cells[5]);
        EXPECT_EQ(cells[5]->belStrength, STRENGTH_STRONG);
        EXPECT_EQ(cells[4]->bel, BelId());
        EXPECT_EQ(cells[4]->belStrength, STRENGTH_NONE);
        ctx->unbindBel(home);
    }
}

TEST_F(LabControlCaptureTest, BatchCoordinatorTruncatesAtUnsupportedCandidate)
{
    const auto &alms = ctx->labs.at(0).alms;
    const BelId odd = alms.at(0).ff_bels.at(1);
    const BelId even = alms.at(1).ff_bels.at(0);
    const BelId foreign = first_non_lab_bel(*ctx);
    ASSERT_NE(foreign, BelId());
    ASSERT_EQ(ctx->getBoundBelCell(foreign), nullptr);

    std::vector<HeAPClusterCandidate> batch;
    batch.push_back(cluster_candidate(*ctx, cells[0], odd));     // illegal
    batch.push_back(cluster_candidate(*ctx, cells[1], foreign)); // not a LAB bel: unsupported
    batch.push_back(cluster_candidate(*ctx, cells[2], even));    // legal, but never proposed
    const auto before = ctx->placement_revision.stamp();
    PlacementCandidateCoordinator coordinator(*ctx, 4);
    const auto outcome = coordinator.place(batch);
    EXPECT_EQ(outcome.status, HeAPClusterBatchStatus::Unsupported);
    EXPECT_EQ(outcome.index, 1u);
    EXPECT_TRUE(ctx->placement_revision.is_current(before));
    EXPECT_EQ(ctx->getBoundBelCell(even), nullptr);
    EXPECT_EQ(ctx->getBoundBelCell(foreign), nullptr);
    const auto &s = coordinator.stats();
    EXPECT_EQ(s.candidates, 3u);
    EXPECT_EQ(s.evaluated, 1u);
    EXPECT_EQ(s.rejected, 1u);
    EXPECT_EQ(s.unsupported, 1u);
    EXPECT_EQ(s.committed, 0u);
}

TEST_F(LabControlCaptureTest, BatchCoordinatorHandlesLargeBatchesAndKeepsOrder)
{
    const auto &alms = ctx->labs.at(0).alms;
    const BelId odd = alms.at(0).ff_bels.at(1);
    const BelId even = alms.at(1).ff_bels.at(0);
    // 100 single-query candidates, more than one Rust handle's worth; only the last is legal.
    std::vector<HeAPClusterCandidate> batch;
    for (unsigned i = 0; i < 99; ++i)
        batch.push_back(cluster_candidate(*ctx, cells[i % 40], odd));
    batch.push_back(cluster_candidate(*ctx, cells[7], even));
    for (unsigned workers : {1u, 5u}) {
        SCOPED_TRACE(workers);
        PlacementCandidateCoordinator coordinator(*ctx, workers);
        const auto outcome = coordinator.place(batch);
        EXPECT_EQ(outcome.status, HeAPClusterBatchStatus::Committed);
        EXPECT_EQ(outcome.index, 99u);
        EXPECT_EQ(ctx->getBoundBelCell(even), cells[7]);
        const auto &s = coordinator.stats();
        EXPECT_EQ(s.rejected, 99u);
        EXPECT_EQ(s.discarded, 0u);
        EXPECT_EQ(s.max_candidates, 100u);
        EXPECT_EQ(s.max_queries, 100u);
#ifndef NO_RUST
        EXPECT_EQ(s.rust_batches, 100u); // one handle per candidate
        EXPECT_EQ(s.rust_direct, 0u);
#endif
        ctx->unbindBel(even);
    }
}

TEST_F(LabControlCaptureTest, BatchCoordinatorParallelFreezeMatchesOwnerFreeze)
{
    // Workers capture each candidate's overlay themselves. Every frozen fact record must be
    // byte-identical to what the owner would have captured, results must agree, and the live
    // design must not change: the coordinator only reads while the owner is blocked.
    const auto &alms = ctx->labs.at(0).alms;
    for (unsigned i = 0; i < 8; ++i) {
        cells[i]->addInput(id_CLK);
        cells[i]->connectPort(id_CLK, nets[i % 2]);
        ctx->assign_ff_info(cells[i]);
    }
    ctx->bindBel(alms.at(5).ff_bels.at(0), cells[9], STRENGTH_WEAK); // an occupant some candidates displace
    std::vector<PreparedPlacementTransaction> prepared;
    for (unsigned i = 0; i < 24; ++i) {
        const BelId bel = alms.at(i % 10).ff_bels.at(i % 4);
        CellInfo *bound = ctx->getBoundBelCell(bel);
        prepared.push_back(prepare_placement_transaction(
                *ctx, {{bel, bound, bound ? bound->belStrength : STRENGTH_NONE, cells[i % 8], STRENGTH_STRONG}}));
        ASSERT_TRUE(prepared.back());
    }
    std::vector<FrozenPlacementCandidate> reference;
    for (const auto &transaction : prepared)
        reference.push_back(freeze_placement_candidate(*ctx, transaction));
    const auto before = ctx->placement_revision.stamp();
    for (unsigned workers : {1u, 4u, 8u}) {
        SCOPED_TRACE(workers);
        PlacementCandidateCoordinator coordinator(*ctx, workers);
        std::vector<FrozenPlacementCandidate> frozen;
        std::vector<PlacementCandidateAssessment> results;
        std::vector<uint8_t> agrees;
        coordinator.freeze_and_evaluate(prepared, frozen, results, agrees);
        ASSERT_EQ(frozen.size(), prepared.size());
        for (size_t i = 0; i < prepared.size(); ++i) {
            EXPECT_EQ(frozen[i].status, FrozenPlacementStatus::Ready);
            EXPECT_EQ(frozen[i].stamp.revision, reference[i].stamp.revision);
            ASSERT_EQ(frozen[i].queries.size(), reference[i].queries.size());
            for (size_t q = 0; q < frozen[i].queries.size(); ++q)
                EXPECT_EQ(std::memcmp(&frozen[i].queries[q], &reference[i].queries[q], sizeof(NpnrLabFactsV2)), 0);
            EXPECT_TRUE(agrees[i]);
            EXPECT_EQ(results[i].legal, evaluate_placement_candidate(reference[i]).legal);
            EXPECT_EQ(results[i].legal, (i % 4) % 2 == 0);
        }
        const auto &s = coordinator.stats();
        EXPECT_EQ(s.evaluated, 24u);
        EXPECT_EQ(s.pool_runs, 1u);
        if (workers > 1)
            EXPECT_GT(s.frozen_by_workers, 0u);
        else
            EXPECT_EQ(s.frozen_by_workers, 0u);
#ifndef NO_RUST
        EXPECT_EQ(s.rust_batches, 24u);
        EXPECT_EQ(s.rust_direct, 0u);
#endif
    }
    EXPECT_TRUE(ctx->placement_revision.is_current(before));
    ctx->unbindBel(alms.at(5).ff_bels.at(0));
    for (unsigned i = 0; i < 8; ++i)
        cells[i]->disconnectPort(id_CLK);
    for (auto *cell : cells)
        EXPECT_EQ(cell->bel, BelId());
}

TEST_F(LabControlCaptureTest, SwapSeamAssessesAndCommitsWithoutProvisionalBinding)
{
    const auto &alms = ctx->labs.at(0).alms;
    const BelId a = alms.at(0).ff_bels.at(0);
    const BelId b = alms.at(1).ff_bels.at(0);
    const BelId odd = alms.at(2).ff_bels.at(1);
    ctx->bindBel(a, cells[0], STRENGTH_WEAK);
    ctx->bindBel(b, cells[1], STRENGTH_WEAK);
    const auto before = ctx->placement_revision.stamp();
    const auto bindings_before = ctx->placement_revision.mutation_count(PlacementMutation::BelBinding);

    // A legal two-cell swap: assessed without any binding change, then committed with exactly
    // the four binding operations the live path would have needed for an accepted swap.
    std::vector<Placer1SwapEdit> swap{{b, cells[1], STRENGTH_WEAK, cells[0], STRENGTH_WEAK},
                                      {a, cells[0], STRENGTH_WEAK, cells[1], STRENGTH_WEAK}};
    auto assessment = mistral_assess_swap(ctx.get(), swap);
    EXPECT_EQ(assessment.status, Placer1SwapAssessment::Status::Legal);
    EXPECT_EQ(assessment.stamp_revision, before.revision);
    EXPECT_TRUE(ctx->placement_revision.is_current(before));
    EXPECT_EQ(ctx->placement_revision.mutation_count(PlacementMutation::BelBinding), bindings_before);
    EXPECT_TRUE(mistral_commit_swap(ctx.get(), swap, assessment));
    EXPECT_EQ(ctx->getBoundBelCell(a), cells[1]);
    EXPECT_EQ(ctx->getBoundBelCell(b), cells[0]);
    EXPECT_EQ(cells[0]->belStrength, STRENGTH_WEAK);
    EXPECT_EQ(ctx->placement_revision.mutation_count(PlacementMutation::BelBinding), bindings_before + 4);
    EXPECT_FALSE(mistral_commit_swap(ctx.get(), swap, assessment)); // stamp moved: refused

    // An illegal move (odd FF slot) is Illegal and leaves everything alone.
    std::vector<Placer1SwapEdit> illegal{{odd, nullptr, STRENGTH_NONE, cells[2], STRENGTH_WEAK},
                                         {alms.at(3).ff_bels.at(0), nullptr, STRENGTH_NONE, nullptr, STRENGTH_NONE}};
    const auto stamp_illegal = ctx->placement_revision.stamp();
    auto bad = mistral_assess_swap(ctx.get(), illegal);
    EXPECT_EQ(bad.status, Placer1SwapAssessment::Status::Illegal);
    EXPECT_TRUE(ctx->placement_revision.is_current(stamp_illegal));
    EXPECT_FALSE(mistral_commit_swap(ctx.get(), illegal, bad));

    // A non-LAB BEL is Unsupported so the annealer keeps its live path.
    const BelId foreign = first_non_lab_bel(*ctx);
    std::vector<Placer1SwapEdit> unsupported{
            {foreign, nullptr, STRENGTH_NONE, cells[3], STRENGTH_WEAK},
            {alms.at(4).ff_bels.at(0), nullptr, STRENGTH_NONE, nullptr, STRENGTH_NONE}};
    EXPECT_EQ(mistral_assess_swap(ctx.get(), unsupported).status, Placer1SwapAssessment::Status::Unsupported);

    // A legal assessment that the design outruns cannot commit.
    std::vector<Placer1SwapEdit> back{{a, cells[1], STRENGTH_WEAK, cells[0], STRENGTH_WEAK},
                                      {b, cells[0], STRENGTH_WEAK, cells[1], STRENGTH_WEAK}};
    auto stale = mistral_assess_swap(ctx.get(), back);
    EXPECT_EQ(stale.status, Placer1SwapAssessment::Status::Legal);
    ctx->bindBel(alms.at(5).ff_bels.at(0), cells[4], STRENGTH_WEAK);
    ctx->unbindBel(alms.at(5).ff_bels.at(0));
    EXPECT_FALSE(mistral_commit_swap(ctx.get(), back, stale));
    EXPECT_EQ(ctx->getBoundBelCell(a), cells[1]);

    // The overlay rules agree with binding and asking, over every FF slot pattern in two ALMs.
    for (unsigned i = 0; i < 6; ++i) {
        cells[10 + i]->addInput(id_CLK);
        cells[10 + i]->connectPort(id_CLK, nets[i % 2]);
        ctx->assign_ff_info(cells[10 + i]);
    }
    unsigned agreed = 0, illegal_seen = 0;
    for (unsigned pattern = 0; pattern < 64; ++pattern) {
        BelOverlay overlay;
        std::vector<std::pair<BelId, CellInfo *>> binds;
        for (unsigned slot = 0; slot < 6 && overlay.count < BelOverlay::MAX; ++slot) {
            if (!(pattern & (1u << slot)))
                continue;
            const BelId bel = alms.at(6 + slot / 4).ff_bels.at(slot % 4);
            overlay.add(bel, cells[10 + slot]);
            binds.emplace_back(bel, cells[10 + slot]);
        }
        const bool detached = ctx->overlay_bels_legal(overlay);
        for (const auto &bind : binds)
            ctx->bindBel(bind.first, bind.second, STRENGTH_WEAK);
        bool live = true;
        for (const auto &bind : binds)
            live = live && ctx->isBelLocationValid(bind.first);
        for (const auto &bind : binds)
            ctx->unbindBel(bind.first);
        EXPECT_EQ(detached, live) << "pattern " << pattern;
        agreed += detached == live;
        illegal_seen += !live;
    }
    EXPECT_EQ(agreed, 64u);
    EXPECT_GT(illegal_seen, 0u);
    for (unsigned i = 0; i < 6; ++i)
        cells[10 + i]->disconnectPort(id_CLK);
    ctx->unbindBel(a);
    ctx->unbindBel(b);
}

TEST_F(LabControlCaptureTest, BatchCoordinatorDetachedEvaluationMatchesSerialAndNeverMutates)
{
    const auto &alms = ctx->labs.at(0).alms;
    std::vector<FrozenPlacementCandidate> frozen;
    std::vector<PlacementCandidateAssessment> serial;
    for (unsigned i = 0; i < 24; ++i) {
        const BelId bel = alms.at(i % 10).ff_bels.at(i % 4); // mixes legal even and illegal odd slots
        auto transaction =
                prepare_placement_transaction(*ctx, {{bel, nullptr, STRENGTH_NONE, cells[i], STRENGTH_STRONG}});
        ASSERT_TRUE(transaction);
        frozen.push_back(freeze_placement_candidate(*ctx, transaction));
        ASSERT_EQ(frozen.back().status, FrozenPlacementStatus::Ready);
        serial.push_back(evaluate_placement_candidate(frozen.back()));
    }
    const auto before = ctx->placement_revision.stamp();
    for (unsigned workers : {1u, 3u, 8u}) {
        SCOPED_TRACE(workers);
        PlacementCandidateCoordinator coordinator(*ctx, workers);
        std::vector<PlacementCandidateAssessment> results;
        std::vector<uint8_t> agrees;
        coordinator.evaluate(frozen, results, agrees);
        ASSERT_EQ(results.size(), frozen.size());
        for (size_t i = 0; i < frozen.size(); ++i) {
            EXPECT_TRUE(agrees[i]);
            EXPECT_EQ(results[i].status, FrozenPlacementStatus::Ready);
            EXPECT_EQ(results[i].legal, serial[i].legal);
            EXPECT_EQ(results[i].legal, (i % 4) % 2 == 0);
            ASSERT_EQ(results[i].results.size(), serial[i].results.size());
            for (size_t q = 0; q < results[i].results.size(); ++q)
                EXPECT_TRUE(lab_v2_results_match(results[i].results[q], serial[i].results[q]));
        }
    }
    EXPECT_TRUE(ctx->placement_revision.is_current(before));
    for (auto *cell : cells)
        EXPECT_EQ(cell->bel, BelId());
}

namespace {
struct LabReuseScope
{
    Context &ctx;
    LabReuseScope(Context &ctx, LabReuseMode mode) : ctx(ctx)
    {
        ctx.args.lab_reuse = mode;
        ctx.lab_reuse_begin();
    }
    ~LabReuseScope()
    {
        ctx.lab_reuse_active = false;
        ctx.lab_assessments.clear();
        ctx.args.lab_reuse = LabReuseMode::Off;
    }
};
} // namespace

TEST_F(LabControlCaptureTest, LabReuseStampsFollowBindingsAndFacts)
{
    const auto &alms = ctx->labs.at(0).alms;
    const BelId even = alms.at(0).ff_bels.at(0);
    const BelId other_lab = ctx->labs.at(1).alms.at(0).ff_bels.at(0);
    LabReuseScope scope(*ctx, LabReuseMode::On);
    ASSERT_EQ(ctx->lab_versions.size(), ctx->labs.size());
    const uint64_t v0 = ctx->lab_versions[0];
    const uint64_t v1 = ctx->lab_versions[1];
    const uint64_t epoch = ctx->lab_facts_epoch;

    ctx->bindBel(even, cells[0], STRENGTH_WEAK);
    EXPECT_EQ(ctx->lab_versions[0], v0 + 1);
    EXPECT_EQ(ctx->lab_versions[1], v1);
    ctx->unbindBel(even);
    EXPECT_EQ(ctx->lab_versions[0], v0 + 2); // ABA: restoring occupancy is a new version
    EXPECT_EQ(ctx->lab_facts_epoch, epoch);
    ctx->bindBel(other_lab, cells[1], STRENGTH_WEAK);
    EXPECT_EQ(ctx->lab_versions[0], v0 + 2);
    EXPECT_EQ(ctx->lab_versions[1], v1 + 1);

    // Facts of an unbound cell are nobody's dependency: nothing moves.
    cells[2]->setAttr(ctx->id("lab_reuse_probe"), Property(1));
    cells[2]->unsetAttr(ctx->id("lab_reuse_probe"));
    cells[3]->addInput(id_CLK);
    cells[3]->connectPort(id_CLK, nets[0]);
    ctx->assign_ff_info(cells[3]);
    cells[3]->disconnectPort(id_CLK);
    EXPECT_EQ(ctx->lab_facts_epoch, epoch);
    EXPECT_EQ(ctx->lab_versions[0], v0 + 2);
    EXPECT_EQ(ctx->lab_versions[1], v1 + 1);
    EXPECT_EQ(ctx->lab_reuse_stats.unbound_cell_mutations, 6u);

    // Facts of a bound cell invalidate exactly its LAB: kernel attrs, connectivity, and Mistral's rewrite.
    cells[1]->setAttr(ctx->id("lab_reuse_probe"), Property(1));
    EXPECT_EQ(ctx->lab_versions[1], v1 + 2);
    cells[1]->unsetAttr(ctx->id("lab_reuse_probe"));
    EXPECT_EQ(ctx->lab_versions[1], v1 + 3);
    cells[1]->addInput(id_CLK);
    cells[1]->connectPort(id_CLK, nets[0]);
    const uint64_t after_connect = ctx->lab_versions[1];
    EXPECT_GT(after_connect, v1 + 3);
    ctx->assign_ff_info(cells[1]);
    EXPECT_EQ(ctx->lab_versions[1], after_connect + 1);
    EXPECT_EQ(ctx->lab_versions[0], v0 + 2);
    EXPECT_EQ(ctx->lab_facts_epoch, epoch);
    EXPECT_GE(ctx->lab_reuse_stats.precise_cell_invalidations, 5u);

    // A net's facts invalidate the LABs of its bound driver and users only.
    const uint64_t v1_before_net = ctx->lab_versions[1];
    ctx->renameNet(nets[0]->name, ctx->id("lab_reuse_renamed_net"));
    EXPECT_EQ(ctx->lab_versions[1], v1_before_net + 1); // cells[1] is a bound user
    EXPECT_EQ(ctx->lab_versions[0], v0 + 2);            // cells[3] is a user but unbound
    EXPECT_EQ(ctx->lab_facts_epoch, epoch);
    EXPECT_EQ(ctx->lab_reuse_stats.precise_net_invalidations, 1u);
    ctx->renameNet(ctx->id("lab_reuse_renamed_net"), ctx->id("lab_test_net_0"));

    // Mutations with no object still fall back to the global epoch, except object creation.
    ctx->addClock(ctx->id("lab_test_net_0"), 100.0f);
    EXPECT_EQ(ctx->lab_facts_epoch, epoch + 1);
    EXPECT_EQ(ctx->lab_reuse_stats.facts_invalidations, 1u);
    nets[0]->clkconstr.reset();
    ctx->createNet(ctx->id("lab_reuse_generated_net"));
    ctx->createCell(ctx->id("lab_reuse_generated_cell"), id_MISTRAL_FF);
    EXPECT_EQ(ctx->lab_facts_epoch, epoch + 1);
    EXPECT_EQ(ctx->lab_reuse_stats.facts_invalidations, 1u);
    ctx->cells.erase(ctx->id("lab_reuse_generated_cell"));
    ctx->nets.erase(ctx->id("lab_reuse_generated_net"));
    ctx->net_aliases.erase(ctx->id("lab_reuse_generated_net"));

    cells[1]->disconnectPort(id_CLK);
    ctx->unbindBel(other_lab);
}

TEST_F(LabControlCaptureTest, LabReuseServesCurrentEntriesAndInvalidatesPrecisely)
{
    const auto &alms = ctx->labs.at(0).alms;
    const BelId even_a = alms.at(0).ff_bels.at(0);
    const BelId even_b = alms.at(1).ff_bels.at(0);
    const BelId other_lab = ctx->labs.at(1).alms.at(0).ff_bels.at(0);
    LabReuseScope scope(*ctx, LabReuseMode::On);
    auto &s = ctx->lab_reuse_stats;

    ctx->bindBel(even_a, cells[0], STRENGTH_WEAK);
    EXPECT_TRUE(ctx->isBelLocationValid(even_a)); // FF query: inputs, ctrlset, mlab all live
    EXPECT_EQ(s.queries, 1u);
    EXPECT_EQ(s.misses, 3u);
    EXPECT_EQ(s.hits, 0u);
    EXPECT_TRUE(ctx->isBelLocationValid(even_a)); // unchanged LAB: all three served
    EXPECT_EQ(s.hits, 3u);
    EXPECT_EQ(s.misses, 3u);

    // A binding elsewhere leaves LAB 0's entry current.
    ctx->bindBel(other_lab, cells[1], STRENGTH_WEAK);
    EXPECT_TRUE(ctx->isBelLocationValid(even_a));
    EXPECT_EQ(s.hits, 6u);
    EXPECT_EQ(s.stale_lab, 0u);

    // A binding in LAB 0 invalidates exactly that entry.
    ctx->bindBel(even_b, cells[2], STRENGTH_WEAK);
    EXPECT_TRUE(ctx->isBelLocationValid(even_b));
    EXPECT_EQ(s.stale_lab, 1u);
    EXPECT_EQ(s.misses, 6u);
    EXPECT_TRUE(ctx->isBelLocationValid(other_lab)); // LAB 1 entry still fresh from nothing: misses
    EXPECT_EQ(s.misses, 9u);
    EXPECT_TRUE(ctx->isBelLocationValid(other_lab));
    EXPECT_EQ(s.hits, 9u);

    // A bound cell's fact change invalidates its LAB only, even though no BEL moved.
    cells[0]->setAttr(ctx->id("lab_reuse_probe"), Property(1));
    EXPECT_TRUE(ctx->isBelLocationValid(even_a));
    EXPECT_EQ(s.stale_lab, 2u);
    EXPECT_TRUE(ctx->isBelLocationValid(other_lab));
    EXPECT_EQ(s.hits, 12u); // LAB 1 untouched: served
    cells[0]->unsetAttr(ctx->id("lab_reuse_probe"));
    EXPECT_TRUE(ctx->isBelLocationValid(even_a));
    EXPECT_EQ(s.stale_lab, 3u);

    // A constraint change carries no object and invalidates every LAB.
    ctx->addClock(ctx->id("lab_test_net_9"), 50.0f);
    EXPECT_TRUE(ctx->isBelLocationValid(even_a));
    EXPECT_EQ(s.stale_facts, 1u);
    EXPECT_TRUE(ctx->isBelLocationValid(other_lab));
    EXPECT_EQ(s.stale_facts, 2u);
    nets[9]->clkconstr.reset();

    // A cached illegal result stays illegal until the LAB changes: odd FF slot makes the ALM
    // illegal, which is not cached, but a control-set conflict is a LAB-level cached result.
    ctx->unbindBel(even_a);
    ctx->unbindBel(even_b);
    ctx->unbindBel(other_lab);
    EXPECT_EQ(s.mismatches, 0u);
}

TEST_F(LabControlCaptureTest, LabReuseShadowAgreesWithLiveAcrossRandomTraffic)
{
    // Random bind/unbind/query traffic over three LABs, including control-set conflicts and
    // full LABs, in shadow mode: every cached sub-result must equal the live evaluation.
    for (unsigned i = 0; i < 12; ++i) {
        cells[i]->addInput(id_CLK);
        cells[i]->connectPort(id_CLK, nets[i % 3]); // three distinct clocks force control-set conflicts
        ctx->assign_ff_info(cells[i]);
    }
    LabReuseScope scope(*ctx, LabReuseMode::Shadow);
    auto &s = ctx->lab_reuse_stats;
    std::vector<BelId> slots;
    for (unsigned lab = 0; lab < 3; ++lab)
        for (unsigned alm = 0; alm < 4; ++alm)
            slots.push_back(ctx->labs.at(lab).alms.at(alm).ff_bels.at(0));
    std::mt19937 rng(20260915);
    std::vector<CellInfo *> bound(slots.size(), nullptr);
    unsigned legal_count = 0, illegal_count = 0;
    for (unsigned step = 0; step < 600; ++step) {
        const unsigned k = rng() % slots.size();
        if (bound[k] == nullptr) {
            CellInfo *cell = cells[k];
            if (cell->bel != BelId())
                continue;
            ctx->bindBel(slots[k], cell, STRENGTH_WEAK);
            bound[k] = cell;
        } else if (rng() % 3 == 0) {
            ctx->unbindBel(slots[k]);
            bound[k] = nullptr;
        }
        for (unsigned q = 0; q < 3; ++q) {
            const unsigned j = rng() % slots.size();
            if (bound[j] == nullptr)
                continue;
            if (ctx->isBelLocationValid(slots[j]))
                ++legal_count;
            else
                ++illegal_count;
        }
    }
    for (unsigned k = 0; k < slots.size(); ++k)
        if (bound[k])
            ctx->unbindBel(slots[k]);
    EXPECT_EQ(s.mismatches, 0u);
    EXPECT_GT(s.hits, 0u);
    EXPECT_GT(s.stale_lab, 0u);
    EXPECT_GT(legal_count, 0u);
    EXPECT_GT(illegal_count, 0u);
    for (unsigned i = 0; i < 12; ++i)
        cells[i]->disconnectPort(id_CLK);
}

TEST_F(LabControlCaptureTest, LabReuseStatesFollowEvaluationPreparationAndRouting)
{
    const auto &alms = ctx->labs.at(0).alms;
    const BelId even = alms.at(0).ff_bels.at(0);
    LabReuseScope scope(*ctx, LabReuseMode::On);
    EXPECT_EQ(lab_reuse_state(*ctx, 0), LabReuseState::Dirty);

    ctx->bindBel(even, cells[0], STRENGTH_WEAK);
    EXPECT_TRUE(ctx->isBelLocationValid(even));
    EXPECT_EQ(lab_reuse_state(*ctx, 0), LabReuseState::Evaluated);
    EXPECT_EQ(lab_reuse_state(*ctx, 1), LabReuseState::Dirty);

    ctx->note_lab_prepared(0);
    EXPECT_EQ(lab_reuse_state(*ctx, 0), LabReuseState::Prepared);
    ctx->note_routing_complete();
    EXPECT_EQ(lab_reuse_state(*ctx, 0), LabReuseState::Routed);
    EXPECT_EQ(lab_reuse_state(*ctx, 1), LabReuseState::Dirty); // never prepared or evaluated

    // A routing mutation anywhere makes routed dependencies stale but keeps preparation.
    const WireId wire = *ctx->getWires().begin();
    ctx->bindWire(wire, nets[0], STRENGTH_WEAK);
    EXPECT_EQ(lab_reuse_state(*ctx, 0), LabReuseState::Prepared);
    ctx->unbindWire(wire);
    ctx->note_routing_complete();
    EXPECT_EQ(lab_reuse_state(*ctx, 0), LabReuseState::Routed);

    // A relevant edit drops the LAB all the way to dirty; re-evaluation restores only Evaluated.
    cells[0]->setAttr(ctx->id("lab_reuse_probe"), Property(1));
    EXPECT_EQ(lab_reuse_state(*ctx, 0), LabReuseState::Dirty);
    EXPECT_TRUE(ctx->isBelLocationValid(even));
    EXPECT_EQ(lab_reuse_state(*ctx, 0), LabReuseState::Evaluated);
    cells[0]->unsetAttr(ctx->id("lab_reuse_probe"));
    ctx->unbindBel(even);
    EXPECT_EQ(lab_reuse_state(*ctx, 0), LabReuseState::Dirty);
    EXPECT_STREQ(lab_reuse_state_name(LabReuseState::Routed), "routed");
}

TEST_F(LabControlCaptureTest, LabReuseContentTierServesIdenticalFactsAcrossVersionsAndLabs)
{
    const auto &alms0 = ctx->labs.at(0).alms;
    const auto &alms1 = ctx->labs.at(1).alms;
    for (unsigned i = 0; i < 4; ++i) {
        cells[i]->addInput(id_CLK);
        cells[i]->connectPort(id_CLK, nets[0]);
        ctx->assign_ff_info(cells[i]);
    }
    LabReuseScope scope(*ctx, LabReuseMode::Content);
    auto &s = ctx->lab_reuse_stats;
    const BelId a = alms0.at(0).ff_bels.at(0);
    const BelId b = alms0.at(1).ff_bels.at(0);

    ctx->bindBel(a, cells[0], STRENGTH_WEAK);
    EXPECT_TRUE(ctx->isBelLocationValid(a)); // stale stamps, empty tier: lookup, store, live
    EXPECT_EQ(s.content_lookups, 1u);
    EXPECT_EQ(s.content_hits, 0u);
    EXPECT_EQ(s.content_stores, 1u);
    EXPECT_EQ(s.misses, 3u);

    // ABA: bind and unbind another cell, then query. Stamps are stale, content is identical.
    ctx->bindBel(b, cells[1], STRENGTH_WEAK);
    ctx->unbindBel(b);
    EXPECT_TRUE(ctx->isBelLocationValid(a));
    EXPECT_EQ(s.content_lookups, 2u);
    EXPECT_EQ(s.content_hits, 1u);
    EXPECT_EQ(s.misses, 3u); // all three sub-results came from the content entry
    EXPECT_EQ(s.hits, 3u);

    // A structurally identical LAB elsewhere hits too, with different nets and cells.
    const BelId c = alms1.at(0).ff_bels.at(0);
    ctx->bindBel(c, cells[2], STRENGTH_WEAK);
    EXPECT_TRUE(ctx->isBelLocationValid(c));
    EXPECT_EQ(s.content_hits, 2u);
    EXPECT_EQ(s.misses, 3u);

    // Different content: a second occupant with the same control set is a new key.
    ctx->bindBel(b, cells[1], STRENGTH_WEAK);
    EXPECT_TRUE(ctx->isBelLocationValid(b));
    EXPECT_EQ(s.content_hits, 2u);
    EXPECT_EQ(s.content_stores, 2u);
    EXPECT_EQ(s.misses, 6u);
    EXPECT_EQ(s.mismatches, 0u);
    EXPECT_EQ(ctx->lab_content_cache.size(), 4096u);

    ctx->unbindBel(a);
    ctx->unbindBel(b);
    ctx->unbindBel(c);
    for (unsigned i = 0; i < 4; ++i)
        cells[i]->disconnectPort(id_CLK);
}

TEST_F(LabControlCaptureTest, LabReuseIsGatedToLegacyModesAndInactiveOutsidePlacement)
{
    const BelId even = ctx->labs.at(0).alms.at(0).ff_bels.at(0);
    ctx->bindBel(even, cells[0], STRENGTH_WEAK);
    {
        ctx->args.lab_legality = LabLegalityMode::Shadow;
        LabReuseScope scope(*ctx, LabReuseMode::On);
        EXPECT_EQ(ctx->lab_reuse_effective, LabReuseMode::Off);
        ctx->args.lab_legality = LabLegalityMode::Legacy;
    }
    {
        ctx->args.verify_lab_controls = true;
        LabReuseScope scope(*ctx, LabReuseMode::On);
        EXPECT_EQ(ctx->lab_reuse_effective, LabReuseMode::Off);
        ctx->args.verify_lab_controls = false;
    }
    {
        LabReuseScope scope(*ctx, LabReuseMode::On);
        EXPECT_EQ(ctx->lab_reuse_effective, LabReuseMode::On);
        EXPECT_TRUE(ctx->isBelLocationValid(even));
        EXPECT_EQ(ctx->lab_reuse_stats.queries, 1u);
        ctx->lab_reuse_active = false; // outside place(): queries bypass the cache entirely
        EXPECT_TRUE(ctx->isBelLocationValid(even));
        EXPECT_EQ(ctx->lab_reuse_stats.queries, 1u);
    }
    ctx->unbindBel(even);
}

TEST_F(LabControlCaptureTest, PlacementReuseClassifiesCellsBySignatureAndTransplantsBels)
{
    // Build a previous-output JSON around the fixture: nets 0..3 and FF cells 0..6.
    const auto &alms = ctx->labs.at(0).alms;
    auto bel_name = [&](BelId bel) { return ctx->getBelName(bel).str(ctx.get()); };
    for (unsigned i = 0; i < 7; ++i) {
        cells[i]->addInput(id_CLK);
        cells[i]->addInput(id_DATAIN);
        cells[i]->connectPort(id_CLK, nets[0]);
        cells[i]->connectPort(id_DATAIN, nets[1 + (i % 3)]);
    }
    cells[3]->setParam(ctx->id("INIT"), Property(1, 1)); // current param differs from the previous run below
    cells[4]->disconnectPort(id_DATAIN);
    cells[4]->connectPort(id_DATAIN, nets[3]);                       // reconnected: connectivity differs
    ctx->bindBel(alms.at(9).ff_bels.at(0), cells[5], STRENGTH_USER); // already bound (as QSF pins are) wins

    auto net_name = [&](unsigned i) { return nets[i]->name.str(ctx.get()); };
    std::ostringstream json;
    json << "{\"modules\":{\"top\":{\"cells\":{";
    auto cell_entry = [&](unsigned i, const std::string &bel, unsigned datain_net, const char *extra_params) {
        json << (i ? "," : "") << "\"" << cells[i]->name.str(ctx.get()) << "\":{\"type\":\"MISTRAL_FF\","
             << "\"attributes\":{\"NEXTPNR_BEL\":\"" << bel << "\",\"BEL_STRENGTH\":\"3\"},"
             << "\"parameters\":{" << extra_params << "},"
             << "\"connections\":{\"CLK\":[100],\"DATAIN\":[" << 101 + datain_net << "]}}";
    };
    cell_entry(0, bel_name(alms.at(0).ff_bels.at(0)), 0, "");               // reused
    cell_entry(1, bel_name(alms.at(1).ff_bels.at(0)), 1, "");               // reused
    cell_entry(2, bel_name(alms.at(2).ff_bels.at(0)), 2, "");               // reused, through a route-through
    cell_entry(3, bel_name(alms.at(3).ff_bels.at(0)), 0, "\"INIT\":\"0\""); // param changed
    cell_entry(4, bel_name(alms.at(4).ff_bels.at(0)), 1, "");               // connectivity changed
    cell_entry(5, bel_name(alms.at(5).ff_bels.at(0)), 2, "");               // user-constrained now
    cell_entry(6, "MISTRAL_FF.999.999.0", 0, "");                           // BEL no longer resolves
    // A previous cell that no longer exists, and a route-through buffer feeding cell 2.
    json << ",\"vanished\":{\"type\":\"MISTRAL_FF\",\"attributes\":{\"NEXTPNR_BEL\":\""
         << bel_name(alms.at(6).ff_bels.at(0)) << "\"},\"parameters\":{},\"connections\":{\"CLK\":[100]}}";
    json << ",\"rt$ROUTETHRU\":{\"type\":\"MISTRAL_BUF\",\"attributes\":{},\"parameters\":{},"
            "\"connections\":{\"A\":[103],\"Q\":[200]}}";
    json << "},\"netnames\":{";
    json << "\"" << net_name(0) << "\":{\"bits\":[100]},\"" << net_name(1) << "\":{\"bits\":[101]},\"" << net_name(2)
         << "\":{\"bits\":[102]},\"" << net_name(3) << "\":{\"bits\":[103]},"
         << "\"rt_out\":{\"bits\":[200]}}}}}";
    // Cell 2 previously read DATAIN from the route-through output (bit 200), whose input is net 3;
    // it currently reads net 3 directly, so folding the buffer out must make it match.
    std::string text = json.str();
    const std::string cell2 = "\"" + cells[2]->name.str(ctx.get()) + "\":";
    const auto at = text.find(cell2);
    ASSERT_NE(at, std::string::npos);
    const auto datain = text.find("\"DATAIN\":[103]", at);
    ASSERT_NE(datain, std::string::npos);
    text.replace(datain, std::string("\"DATAIN\":[103]").size(), "\"DATAIN\":[200]");
    cells[2]->disconnectPort(id_DATAIN);
    cells[2]->connectPort(id_DATAIN, nets[3]);

    const std::string path = std::string(::testing::TempDir()) + "/lab_reuse_previous.json";
    {
        std::ofstream out(path);
        out << text;
    }
    const auto report = apply_placement_reuse(*ctx, path);
    EXPECT_EQ(report.previous_cells, 8u);
    EXPECT_EQ(report.previous_routethru, 1u);
    EXPECT_EQ(report.current_cells, cells.size());
    EXPECT_EQ(report.matched, 3u);
    EXPECT_EQ(report.changed, 2u);
    EXPECT_EQ(report.user_constrained, 1u);
    EXPECT_EQ(report.missing_bel, 1u);
    EXPECT_EQ(report.removed, 1u);
    EXPECT_EQ(report.added, cells.size() - 7);
    const IdString id_bel = ctx->id("BEL");
    EXPECT_EQ(ctx->getBelByNameStr(cells[0]->attrs.at(id_bel).as_string()), alms.at(0).ff_bels.at(0));
    EXPECT_EQ(ctx->getBelByNameStr(cells[1]->attrs.at(id_bel).as_string()), alms.at(1).ff_bels.at(0));
    EXPECT_EQ(ctx->getBelByNameStr(cells[2]->attrs.at(id_bel).as_string()), alms.at(2).ff_bels.at(0));
    EXPECT_EQ(cells[3]->attrs.count(id_bel), 0u);
    EXPECT_EQ(cells[4]->attrs.count(id_bel), 0u);
    EXPECT_EQ(cells[5]->attrs.count(id_bel), 0u);
    EXPECT_EQ(cells[5]->bel, alms.at(9).ff_bels.at(0));
    EXPECT_EQ(cells[6]->attrs.count(id_bel), 0u);

    // Stage 5 (3a): the same decisions as a plan, with reasons, computed
    // without touching the design; (3b): region release around dirty cells.
    {
        for (unsigned i = 0; i < 3; ++i)
            cells[i]->attrs.erase(id_bel); // undo the apply above: the plan sees the design as it was
        ReusePlan plan;
        plan_placement_reuse(*ctx, path, plan);
        auto decision = [&](unsigned i) {
            for (auto &d : plan.cells)
                if (d.cell == cells[i]->name.str(ctx.get()))
                    return d;
            return CellReuseDecision{};
        };
        EXPECT_EQ(decision(0).decision, ReuseDecision::Reuse);
        EXPECT_EQ(decision(3).decision, ReuseDecision::Changed);
        EXPECT_EQ(decision(3).reason, "parameter INIT differs");
        EXPECT_EQ(decision(4).decision, ReuseDecision::Changed);
        EXPECT_EQ(decision(4).reason, "connectivity of port DATAIN differs");
        EXPECT_EQ(decision(5).decision, ReuseDecision::UserConstrained);
        EXPECT_EQ(decision(6).decision, ReuseDecision::MissingBel);
        EXPECT_EQ(plan.count_cells(ReuseDecision::Reuse), 3u);
        EXPECT_EQ(plan.count_cells(ReuseDecision::Added), cells.size() - 7);
        EXPECT_EQ(plan.removed_cells, 1u);
        // Radius 0 around the changed cells' previous BELs (all in LAB 0)
        // releases every transplant in that tile and clears its BEL attribute.
        EXPECT_EQ(release_placement_region(*ctx, plan, 0), 3u);
        EXPECT_EQ(plan.count_cells(ReuseDecision::Released), 3u);
        EXPECT_EQ(plan.count_cells(ReuseDecision::Reuse), 0u);
        EXPECT_EQ(cells[0]->attrs.count(id_bel), 0u);
        EXPECT_EQ(plan.released_cells, 3u);
        EXPECT_FALSE(plan.placement_full_fallback);
        // A fresh plan, released entirely.
        ReusePlan again;
        plan_placement_reuse(*ctx, path, again);
        EXPECT_EQ(release_placement_region(*ctx, again, -1), 3u);
        EXPECT_TRUE(again.placement_full_fallback);
        const std::string plan_path = std::string(::testing::TempDir()) + "/lab_reuse_plan.json";
        write_reuse_plan(again, plan_path);
        std::ifstream in(plan_path);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("\"placement_full_fallback\": true"), std::string::npos);
        EXPECT_NE(text.find("\"parameter INIT differs\""), std::string::npos);
        std::remove(plan_path.c_str());
    }
    for (unsigned i = 0; i < 7; ++i)
        if (i != 5)
            EXPECT_EQ(cells[i]->bel, BelId()); // annotation only; the placer binds
    ctx->unbindBel(alms.at(9).ff_bels.at(0));
    for (unsigned i = 0; i < 7; ++i) {
        cells[i]->unsetAttr(id_bel);
        cells[i]->unsetParam(ctx->id("INIT"));
    }
}

TEST_F(LabControlCaptureTest, ProfilingReservoirPreservesLiveStateAndIsReproducible)
{
    cells[0]->ffInfo.ctrlset.clk = {nets[0], false};
    bind(0);
    const auto original = capture_lab_controls(*ctx, 0);
    const auto rng = ctx->rngstate;
    auto sample = [&] {
        for (unsigned i = 0; i < 12000; ++i)
            sample_lab_controls(*ctx, 0, i % 3 == 0);
    };
    sample();
    ASSERT_EQ(ctx->lab_control_profile.queries, 12000u);
    ASSERT_EQ(ctx->lab_control_profile.samples.size(), 4096u);
    const auto first = ctx->lab_control_profile.samples;
    EXPECT_TRUE(std::any_of(first.begin(), first.end(), [](const auto &s) { return s.input.request_id > 4096; }));
    ctx->lab_control_profile = {};
    sample();
    for (size_t i = 0; i < first.size(); ++i) {
        const auto &again = ctx->lab_control_profile.samples[i];
        EXPECT_EQ(std::memcmp(&first[i].input, &again.input, sizeof(again.input)), 0);
        EXPECT_EQ(first[i].preparation, again.preparation);
        EXPECT_TRUE(lab_control_results_match(evaluate_lab_controls_cpp(again.input), again.expected));
    }
    const auto after = capture_lab_controls(*ctx, 0);
    EXPECT_EQ(std::memcmp(&original.input, &after.input, sizeof(after.input)), 0);
    EXPECT_EQ(ctx->rngstate, rng);
    ctx->lab_control_profile = {};
}

TEST_F(LabControlCaptureTest, PackedNormalizationAndIdentity)
{
    auto *cell = cells[0];
    cell->addInput(id_CLK);
    cell->connectPort(id_CLK, nets[0]);
    cell->pin_data[id_CLK].state = PIN_INV;
    cell->addInput(id_SCLR);
    cell->connectPort(id_SCLR, nets[1]);
    cell->pin_data[id_ACLR].state = PIN_INV; // retain polarity on a disconnected control
    nets[0]->is_global = true;
    ctx->assign_ff_info(cell);
    bind(0);
    const auto capture = capture_lab_controls(*ctx, 0);
    const auto &ff = capture.input.ff[0];
    EXPECT_EQ(ff.control[0].flags, NPNR_CONTROL_INVERTED | NPNR_CONTROL_GLOBAL);
    EXPECT_EQ(capture.nets[ff.control[1].net_id - 1], ctx->nets.at(ctx->id("$PACKER_GND_NET")).get());
    EXPECT_EQ(capture.nets[ff.control[4].net_id - 1], ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get());
    EXPECT_EQ(ff.control[3].net_id, 0u);
    EXPECT_EQ(ff.control[3].flags, NPNR_CONTROL_INVERTED);
    EXPECT_EQ(compare().status, NPNR_CONTROL_LEGAL);
    const auto native = evaluate_lab_controls_native(*ctx, 0);
    ASSERT_TRUE(native.allocation);
    EXPECT_EQ(native.allocation->aclr[0].net, nullptr);
    EXPECT_FALSE(native.allocation->aclr[0].inverted);
    const auto repeat = capture_lab_controls(*ctx, 0);
    EXPECT_EQ(std::memcmp(&capture.input, &repeat.input, sizeof(capture.input)), 0);
    EXPECT_EQ(cell->bel, ctx->labs[0].alms[0].ff_bels[0]);
}

TEST_F(LabControlCaptureTest, V2ScopedQueriesMatchLiveChecks)
{
    cells[0]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[4]->ffInfo.ctrlset.clk = {nets[1], false};
    cells[8]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[8]->ffInfo.ctrlset.ena = {nets[2], true};
    bind(0);
    bind(4);
    bind(8);

    const bool lab_inputs = ctx->check_lab_input_count(0);
    const bool controls = ctx->is_lab_ctrlset_legal(0);
    const bool mlab = ctx->check_mlab_groups(0);
    for (unsigned alm = 0; alm < 10; ++alm) {
        const bool local = ctx->is_alm_legal(0, alm);
        for (auto query : {NPNR_LAB_QUERY_COMB_BEL, NPNR_LAB_QUERY_FF_BEL}) {
            const auto input = capture_lab_v2(*ctx, 0, query, alm, 700 + alm, 9);
            const auto result = evaluate_lab_v2_cpp(input);
            ASSERT_TRUE(lab_v2_result_valid(input, result));
#ifndef NO_RUST
            expect_v2_rust_parity(input, result);
#endif
            const bool expected = local && lab_inputs && mlab && (query == NPNR_LAB_QUERY_COMB_BEL || controls);
            EXPECT_EQ(result.status == NPNR_LAB_V2_LEGAL, expected) << "alm " << alm << " query " << query;
            EXPECT_EQ(result.recomputed_valid_mask, 0x3ffu);
            for (unsigned i = 0; i < 10; ++i)
                EXPECT_EQ(result.recomputed_input_count[i], input.alm[i].cached_input_count);
        }
    }
    const auto whole_input = capture_lab_v2(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX, 900, 10);
    const auto whole = evaluate_lab_v2_cpp(whole_input);
#ifndef NO_RUST
    expect_v2_rust_parity(whole_input, whole);
#endif
    bool expected = lab_inputs && controls && mlab;
    for (unsigned alm = 0; alm < 10; ++alm)
        expected &= ctx->is_alm_legal(0, alm);
    EXPECT_EQ(whole.status == NPNR_LAB_V2_LEGAL, expected);
    EXPECT_EQ(whole.control_valid, 1u);
}

TEST_F(LabControlCaptureTest, V2StructuredFailuresAndCacheDiagnostics)
{
    auto input = capture_lab_v2(*ctx, 0, NPNR_LAB_QUERY_FF_BEL, 0, 1001, 44);
    auto result = evaluate_lab_v2_cpp(input);
#ifndef NO_RUST
    expect_v2_rust_parity(input, result);
#endif
    ASSERT_EQ(result.status, NPNR_LAB_V2_LEGAL);
    input.alm[0].cached_input_count = 1234;
    result = evaluate_lab_v2_cpp(input);
#ifndef NO_RUST
    expect_v2_rust_parity(input, result);
#endif
    EXPECT_EQ(result.status, NPNR_LAB_V2_LEGAL);
    EXPECT_EQ(result.recomputed_input_count[0], 0);
    EXPECT_NE(result.recomputed_input_count[0], input.alm[0].cached_input_count);

    input.alm[0].lut[0].occupied = 1;
    input.alm[0].lut[0].bits_count = 65;
    result = evaluate_lab_v2_cpp(input);
#ifndef NO_RUST
    expect_v2_rust_parity(input, result);
#endif
    EXPECT_EQ(result.status, NPNR_LAB_V2_ILLEGAL);
    EXPECT_EQ(result.reason, NPNR_LAB_V2_ALM_BITS);
    EXPECT_EQ(result.failing_alm, 0u);
    EXPECT_EQ(result.observed, 65);
    EXPECT_EQ(result.limit, 64);

    input.abi_version = 99;
    result = evaluate_lab_v2_cpp(input);
#ifndef NO_RUST
    expect_v2_rust_parity(input, result);
#endif
    EXPECT_EQ(result.status, NPNR_LAB_V2_MALFORMED);
    EXPECT_EQ(result.reason, NPNR_LAB_V2_BAD_HEADER);
    EXPECT_TRUE(lab_v2_result_valid(input, result));
}

TEST_F(LabControlCaptureTest, V2ReplayRoundTripsNamedFieldsAtomically)
{
    cells[0]->ffInfo.ctrlset.clk = {nets[0], true};
    cells[0]->ffInfo.datain = nets[1];
    bind(0);
    const auto input = capture_lab_v2(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX, UINT64_MAX, UINT64_MAX - 1);
    const auto expected = evaluate_lab_v2_cpp(input);
    std::ostringstream out;
    ASSERT_TRUE(write_lab_v2_replay(out, input, expected, "V2 round trip"));
    EXPECT_NE(out.str().find("\"schema\": 2"), std::string::npos);
    EXPECT_NE(out.str().find("\"cached_input_count\""), std::string::npos);

    NpnrLabFactsV2 decoded{};
    NpnrLabAssessmentV2 result{};
    std::string error;
    ASSERT_TRUE(read_lab_v2_replay(out.str(), decoded, result, error)) << error;
    EXPECT_EQ(std::memcmp(&input, &decoded, sizeof(input)), 0);
    EXPECT_EQ(std::memcmp(&expected, &result, sizeof(expected)), 0);

    decoded.request_id = 17;
    result.request_id = 19;
    EXPECT_FALSE(read_lab_v2_replay(out.str().substr(0, out.str().size() / 2), decoded, result, error));
    EXPECT_EQ(decoded.request_id, 17u);
    EXPECT_EQ(result.request_id, 19u);
    EXPECT_FALSE(error.empty());
}

TEST_F(LabControlCaptureTest, V2PreservesOddFfAndControlConflictOrder)
{
    bind(1);
    auto input = capture_lab_v2(*ctx, 0, NPNR_LAB_QUERY_FF_BEL, 0, 1100);
    auto result = evaluate_lab_v2_cpp(input);
#ifndef NO_RUST
    expect_v2_rust_parity(input, result);
#endif
    EXPECT_EQ(result.reason, NPNR_LAB_V2_ODD_FF);
    EXPECT_EQ(result.failing_alm, 0u);
    EXPECT_EQ(result.failing_slot, 1u);
    clear_bindings();

    for (unsigned i = 0; i < 3; ++i) {
        cells[4 * i]->ffInfo.ctrlset.clk = {nets[i], false};
        bind(4 * i);
    }
    input = capture_lab_v2(*ctx, 0, NPNR_LAB_QUERY_FF_BEL, 0, 1101);
    result = evaluate_lab_v2_cpp(input);
#ifndef NO_RUST
    expect_v2_rust_parity(input, result);
#endif
    EXPECT_EQ(result.reason, NPNR_LAB_V2_CONTROL_CONFLICT);
    EXPECT_EQ(result.control_valid, 1u);
    EXPECT_EQ(result.control.reason, NPNR_CONTROL_CLOCK_CONFLICT);
}

TEST_F(LabControlCaptureTest, V2EveryDetachedSubcheckHasRustParity)
{
    auto base = [&] { return capture_lab_v2(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX, 1300, 77); };
    auto check = [&](const NpnrLabFactsV2 &input, uint32_t reason) {
        const auto cpp = evaluate_lab_v2_cpp(input);
        EXPECT_EQ(cpp.reason, reason);
#ifndef NO_RUST
        expect_v2_rust_parity(input, cpp);
#endif
    };

    auto input = base();
    for (unsigned half = 0; half < 2; ++half) {
        auto &lut = input.alm[0].lut[half];
        lut.occupied = 1;
        lut.input_count = 5;
        lut.used_input_count = 4;
        lut.bits_count = 32;
        lut.mlab_group = -1;
    }
    input.net_count = 8;
    std::copy_n(std::array<uint32_t, 5>{1, 2, 3, 4, 0}.begin(), 5, input.alm[0].lut[0].input_net);
    std::copy_n(std::array<uint32_t, 5>{5, 6, 7, 8, 0}.begin(), 5, input.alm[0].lut[1].input_net);
    check(input, NPNR_LAB_V2_ALM_INPUTS);

    input = base();
    input.alm[0].lut[0].occupied = input.alm[0].lut[1].occupied = 1;
    input.alm[0].lut[0].bits_count = input.alm[0].lut[1].bits_count = 1;
    input.alm[0].lut[0].mlab_group = input.alm[0].lut[1].mlab_group = -1;
    input.alm[0].lut[0].is_carry = 1;
    check(input, NPNR_LAB_V2_CARRY_MIX);

    auto blocked_ef = base();
    auto &opposite = blocked_ef.alm[0].lut[1];
    opposite.occupied = 1;
    opposite.input_count = opposite.used_input_count = 3;
    opposite.bits_count = 8;
    opposite.mlab_group = -1;
    opposite.input_net[0] = 1;
    opposite.input_net[1] = 2;
    opposite.input_net[2] = 3;
    blocked_ef.net_count = 5;
    blocked_ef.alm[0].ff[0].occupied = 1;
    blocked_ef.alm[0].ff[0].sdata_net = 4;
    check(blocked_ef, NPNR_LAB_V2_SDATA_PATH);

    blocked_ef.alm[0].ff[0].sdata_net = 0;
    blocked_ef.alm[0].ff[0].datain_net = 5;
    auto &own = blocked_ef.alm[0].lut[0];
    own.occupied = 1;
    own.bits_count = 1;
    own.mlab_group = -1;
    own.comb_out_net = 4;
    check(blocked_ef, NPNR_LAB_V2_DATAIN_PATH);

    input = base();
    input.input_limit = -1;
    check(input, NPNR_LAB_V2_LAB_INPUT_LIMIT);

    input = base();
    input.query = 99;
    check(input, NPNR_LAB_V2_BAD_QUERY);
    input = base();
    input.alm[0].reserved = 1;
    check(input, NPNR_LAB_V2_BAD_SHAPE);
}

#ifndef NO_RUST
#ifndef NO_RUST
TEST_F(LabControlCaptureTest, ResidentLegalityPatchesChangedAlmsAndMatchesTheCapturePath)
{
    // Stage 6 (parity): the Rust modes evaluate through a resident LAB snapshot that receives
    // only the ALMs changed since its last query. Its verdicts must equal the capture path's,
    // a query on an unchanged LAB must send nothing, and a facts change on a bound cell must
    // reach it.
    ctx->args.lab_legality = LabLegalityMode::Rust;
    ctx->lab_resident.reset();
    ctx->lab_bel_dirty.clear();
    ctx->lab_bel_refacts.clear();
    auto &lab0 = ctx->labs.at(0);
    auto reference = [&](NpnrLabQueryV2 query, uint32_t alm) {
        return evaluate_lab_v2_cpp(capture_lab_v2(*ctx, 0, query, alm, 1, 1)).status == NPNR_LAB_V2_LEGAL;
    };
    cells[0]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[4]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[8]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[8]->ffInfo.ctrlset.ena = {nets[2], true};
    bind(0);
    bind(4);
    bind(8);
    ASSERT_TRUE(reference(NPNR_LAB_QUERY_FF_BEL, 0));
    // The first query creates the session, resets LAB 0, and commits the three bound registers
    // (a resync after a reset travels as commits); later queries on the unchanged LAB send nothing.
    auto query0 = [&](unsigned alm) { return ctx->isBelLocationValid(lab0.alms[alm].ff_bels[0]); };
    EXPECT_EQ(query0(0), reference(NPNR_LAB_QUERY_FF_BEL, 0));
    ASSERT_TRUE(ctx->lab_resident && ctx->lab_resident->available());
    EXPECT_EQ(ctx->lab_resident->resets, 1u);
    const uint64_t first = ctx->lab_resident->commits;
    EXPECT_EQ(first, 3u);
    EXPECT_EQ(ctx->lab_resident->trials, 0u);
    EXPECT_EQ(query0(1), reference(NPNR_LAB_QUERY_FF_BEL, 1));
    EXPECT_EQ(query0(2), reference(NPNR_LAB_QUERY_FF_BEL, 2));
    EXPECT_EQ(ctx->lab_resident->trials + ctx->lab_resident->commits, first);
    // One more register in the other half of ALM 0: one trial, then one commit, still legal.
    cells[2]->ffInfo.ctrlset.clk = {nets[0], false};
    bind(2);
    EXPECT_TRUE(reference(NPNR_LAB_QUERY_FF_BEL, 0));
    EXPECT_TRUE(ctx->isBelLocationValid(lab0.alms[0].ff_bels[2]));
    EXPECT_EQ(ctx->lab_resident->trials, 1u);
    EXPECT_TRUE(query0(0));
    EXPECT_EQ(ctx->lab_resident->commits, first + 1);
    // A second register in a half is illegal on both paths: a trial the session undoes, so
    // unbinding it restores the committed facts and the next query sends nothing.
    cells[1]->ffInfo.ctrlset.clk = {nets[0], false};
    bind(1);
    EXPECT_FALSE(reference(NPNR_LAB_QUERY_FF_BEL, 0));
    EXPECT_FALSE(ctx->isBelLocationValid(lab0.alms[0].ff_bels[1]));
    EXPECT_EQ(ctx->lab_resident->trials, 2u);
    const uint64_t restored = ctx->lab_resident->restored;
    ctx->unbindBel(cells[1]->bel);
    EXPECT_TRUE(query0(0));
    EXPECT_EQ(ctx->lab_resident->trials, 2u);
    EXPECT_EQ(ctx->lab_resident->commits, first + 1);
    EXPECT_EQ(ctx->lab_resident->restored, restored + 1);
    // A control-set change on a bound register, reported the way assign_ff_info reports it,
    // reaches the session: a second clock on ALM 1 makes the LAB's controls illegal.
    cells[4]->ffInfo.ctrlset.clk = {nets[3], true};
    cells[4]->ffInfo.ctrlset.ena = {nets[4], false};
    cells[4]->ffInfo.ctrlset.sclr = {nets[5], false};
    cells[4]->ffInfo.ctrlset.sload = {nets[6], false};
    ctx->note_lab_cell_mutation(cells[4]);
    const bool changed = reference(NPNR_LAB_QUERY_FF_BEL, 1);
    EXPECT_EQ(query0(1), changed);
    EXPECT_EQ(ctx->lab_resident->trials, 3u);
    EXPECT_EQ(query0(1), changed);
    EXPECT_EQ(ctx->lab_resident->commits, first + 2);
    // Every scoped query agrees with the capture path, and none sends a patch.
    for (unsigned alm = 0; alm < 10; ++alm) {
        EXPECT_EQ(ctx->isBelLocationValid(lab0.alms[alm].lut_bels[0]), reference(NPNR_LAB_QUERY_COMB_BEL, alm))
                << "comb alm " << alm;
        EXPECT_EQ(ctx->isBelLocationValid(lab0.alms[alm].ff_bels[0]), reference(NPNR_LAB_QUERY_FF_BEL, alm))
                << "ff alm " << alm;
    }
    EXPECT_EQ(ctx->lab_resident->trials + ctx->lab_resident->commits, first + 5);
    // The verify harness runs the capture path beside the resident and finds no disagreement.
    ctx->args.lab_legality = LabLegalityMode::Verify;
    const auto mismatches = ctx->lab_legality_stats.mismatches.load();
    const auto errors = ctx->lab_legality_stats.errors.load();
    for (unsigned alm = 0; alm < 10; ++alm) {
        ctx->isBelLocationValid(lab0.alms[alm].lut_bels[1]);
        ctx->isBelLocationValid(lab0.alms[alm].ff_bels[2]);
    }
    EXPECT_EQ(ctx->lab_legality_stats.mismatches.load(), mismatches);
    EXPECT_EQ(ctx->lab_legality_stats.errors.load(), errors);
    ctx->args.lab_legality = LabLegalityMode::Legacy;
    ctx->lab_resident.reset();
    ctx->lab_bel_dirty.clear();
    ctx->lab_bel_refacts.clear();
}
#endif

TEST_F(LabControlCaptureTest, V2LiveModesPreserveScopeAndRejectStaleAuthority)
{
    const auto bel = ctx->labs[0].alms[0].ff_bels[0];
    bind(0);
    ASSERT_TRUE(ctx->isBelLocationValid(bel));
    for (auto mode : {LabLegalityMode::Shadow, LabLegalityMode::Verify, LabLegalityMode::Rust}) {
        ctx->args.lab_legality = mode;
        EXPECT_TRUE(ctx->isBelLocationValid(bel));
    }
    clear_bindings();
    bind(1);
    const auto odd_bel = ctx->labs[0].alms[0].ff_bels[1];
    for (auto mode : {LabLegalityMode::Shadow, LabLegalityMode::Verify, LabLegalityMode::Rust}) {
        ctx->args.lab_legality = mode;
        EXPECT_FALSE(ctx->isBelLocationValid(odd_bel));
    }
    clear_bindings();

    // A stale cached count: the shadow mode reports it and answers live, the verify mode fails
    // closed, and the authority mode takes the arch's count as a fact (the resident session
    // recomputes counts only in the harness modes), so a count over the limit makes it reject.
    bind(0);
    auto &cached = ctx->labs[0].alms[0].unique_input_count;
    cached = 7;
    ctx->args.lab_legality = LabLegalityMode::Shadow;
    EXPECT_TRUE(dispatch_lab_legality(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX));
    ctx->args.lab_legality = LabLegalityMode::Verify;
    EXPECT_THROW(dispatch_lab_legality(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX), log_execution_error_exception);
    ctx->args.lab_legality = LabLegalityMode::Rust;
    EXPECT_TRUE(dispatch_lab_legality(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX));
    // The count travels with a bel whose facts changed, as it does in the arch (the count
    // changes when a bel's facts do): a control change on the bound register carries it.
    cached = resolved_lab_input_limit() + 1;
    cells[0]->ffInfo.ctrlset.ena = {nets[7], false};
    ctx->note_lab_cell_mutation(cells[0]);
    EXPECT_FALSE(dispatch_lab_legality(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX));
    cached = 0;
    cells[0]->ffInfo.ctrlset.ena = {};
    ctx->note_lab_cell_mutation(cells[0]);
    EXPECT_TRUE(dispatch_lab_legality(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX));
    ctx->args.lab_legality = LabLegalityMode::Legacy;
}
#endif

TEST_F(LabControlCaptureTest, NativeConsumerPreservesEnableAndClearSelections)
{
    nets[0]->is_global = true;
    std::array<WireId, 6> touched;
    std::array<uint64_t, 6> saved_flags;
    for (unsigned i = 0; i < 3; ++i) {
        auto &cs = cells[4 * i]->ffInfo.ctrlset;
        cs.clk = {nets[0], false};
        cs.ena = {nets[i + 1], false};
        bind(4 * i);
        const auto bel = ctx->labs[0].alms[i].ff_bels[0];
        touched[2 * i] = ctx->getBelPinWire(bel, id_CLK);
        touched[2 * i + 1] = ctx->getBelPinWire(bel, id_ENA);
        saved_flags[2 * i] = ctx->wires.at(touched[2 * i]).flags;
        saved_flags[2 * i + 1] = ctx->wires.at(touched[2 * i + 1]).flags;
        ctx->labs[0].alms[i].clk_ena_idx[0] = -1;
    }
    const auto enable_plan = evaluate_lab_controls_native(*ctx, 0);
    ASSERT_TRUE(enable_plan.allocation);
    EXPECT_EQ(enable_plan.allocation->datain[2].net, nets[1]);
    EXPECT_EQ(enable_plan.allocation->datain[3].net, nets[2]);
    EXPECT_EQ(enable_plan.allocation->datain[0].net, nets[3]);
    consume_lab_control_allocation(*ctx, 0, *enable_plan.allocation);
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_EQ(ctx->labs[0].alms[i].clk_ena_idx[0], int(i));
        EXPECT_EQ(reserved_source(*ctx, touched[2 * i]), ctx->labs[0].clk_wires[0]);
        EXPECT_EQ(reserved_source(*ctx, touched[2 * i + 1]), ctx->labs[0].ena_wires[i]);
    }
    for (unsigned i = 0; i < touched.size(); ++i)
        ctx->wires.at(touched[i]).flags = saved_flags[i];
    clear_bindings();

    std::array<WireId, 6> clear_touched;
    std::array<uint64_t, 6> clear_saved_flags;
    for (unsigned i = 0; i < 2; ++i) {
        cells[4 * i]->ffInfo.ctrlset = {};
        cells[4 * i]->ffInfo.ctrlset.aclr = {nets[i + 1], false};
        bind(4 * i);
        const auto bel = ctx->labs[0].alms[i].ff_bels[0];
        clear_touched[3 * i] = ctx->getBelPinWire(bel, id_CLK);
        clear_touched[3 * i + 1] = ctx->getBelPinWire(bel, id_ENA);
        clear_touched[3 * i + 2] = ctx->getBelPinWire(bel, id_ACLR);
        for (unsigned j = 0; j < 3; ++j)
            clear_saved_flags[3 * i + j] = ctx->wires.at(clear_touched[3 * i + j]).flags;
        ctx->labs[0].alms[i].aclr_idx[0] = -1;
    }
    const auto clear_plan = evaluate_lab_controls_native(*ctx, 0);
    ASSERT_TRUE(clear_plan.allocation);
    EXPECT_EQ(clear_plan.allocation->datain[3].net, nets[1]);
    EXPECT_EQ(clear_plan.allocation->datain[2].net, nets[2]);
    consume_lab_control_allocation(*ctx, 0, *clear_plan.allocation);
    for (unsigned i = 0; i < 2; ++i) {
        EXPECT_EQ(ctx->labs[0].alms[i].aclr_idx[0], int(i));
        EXPECT_TRUE(ctx->labs[0].aclr_used[i]);
        EXPECT_EQ(reserved_source(*ctx, clear_touched[3 * i + 2]), ctx->labs[0].aclr_wires[i]);
    }
    for (unsigned i = 0; i < clear_touched.size(); ++i)
        ctx->wires.at(clear_touched[i]).flags = clear_saved_flags[i];
}

TEST_F(LabControlCaptureTest, IllegalNativeEvaluationPublishesNoPreparationState)
{
    cells[0]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[4]->ffInfo.ctrlset.clk = {nets[1], false};
    bind(0);
    bind(4);
    auto &lab = ctx->labs[0];
    const auto saved_aclr_used = lab.aclr_used;
    const auto saved_clk_ena_idx = lab.alms[0].clk_ena_idx[0];
    lab.aclr_used = {true, false};
    lab.alms[0].clk_ena_idx[0] = 17;
    const auto destination = ctx->getBelPinWire(lab.alms[0].ff_bels[0], id_CLK);
    const auto flags = ctx->wires.at(destination).flags;
    const auto capture = capture_lab_controls(*ctx, 0);
    const auto evaluation = evaluate_lab_controls_native(*ctx, 0);
    EXPECT_FALSE(evaluation.legal);
    EXPECT_FALSE(evaluation.allocation);
    expect_no_plan(evaluate_lab_controls_legacy(*ctx, 0, capture));
    EXPECT_EQ(lab.aclr_used, (std::array<bool, 2>{true, false}));
    EXPECT_EQ(lab.alms[0].clk_ena_idx[0], 17);
    EXPECT_EQ(ctx->wires.at(destination).flags, flags);
    lab.aclr_used = saved_aclr_used;
    lab.alms[0].clk_ena_idx[0] = saved_clk_ena_idx;
}

TEST_F(LabControlCaptureTest, PreparationTicketOwnsAndTranslatesTheSelectedPlan)
{
    static_assert(!std::is_copy_constructible<PreparationTicket>::value, "ticket must be move-only");
    static_assert(std::is_move_constructible<PreparationTicket>::value, "ticket must be movable");
    cells[0]->ffInfo.ctrlset.clk = {nets[0], true};
    cells[0]->ffInfo.ctrlset.ena = {nets[1], false};
    bind(0);

    auto dispatched = dispatch_lab_controls_for_preparation(*ctx, 0);
    ASSERT_EQ(dispatched.selected_status, PreparationStatus::Legal);
    ASSERT_EQ(dispatched.comparison_status, PreparationStatus::Legal);
    ASSERT_TRUE(dispatched.ticket);
    auto translated = translate_preparation_ticket(std::move(*dispatched.ticket));
    ASSERT_EQ(translated.status, PreparationStatus::Legal);
    ASSERT_TRUE(translated.plan);
    EXPECT_EQ(translated.plan->lab, 0u);
    EXPECT_EQ(translated.plan->request_id, 0u);
    EXPECT_EQ(translated.plan->allocation.clk.net, nets[0]);
    EXPECT_TRUE(translated.plan->allocation.clk.inverted);
    EXPECT_EQ(translated.plan->allocation.ena[0].net, nets[1]);

    auto capture = capture_lab_controls(*ctx, 0, 91);
    auto selected = evaluate_lab_controls_legacy(*ctx, 0, capture);
    auto malformed = selected;
    --malformed.struct_size;
    EXPECT_EQ(translate_preparation_ticket(PreparationTicket(capture, 0, 91, malformed)).status,
              PreparationStatus::MalformedResult);
    EXPECT_EQ(translate_preparation_ticket(PreparationTicket(capture, 0, 92, selected)).status,
              PreparationStatus::MalformedResult);
    capture.nets[selected.allocation[0].net_id - 1] = nullptr;
    EXPECT_EQ(translate_preparation_ticket(PreparationTicket(capture, 0, 91, selected)).status,
              PreparationStatus::MalformedResult);
}

TEST_F(LabControlCaptureTest, PreparedControlEditsPreflightAndRollbackFailures)
{
    nets[0]->is_global = true;
    cells[0]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[0]->ffInfo.ctrlset.ena = {nets[1], false};
    cells[0]->ffInfo.ctrlset.aclr = {nets[2], true};
    bind(0);
    auto dispatched = dispatch_lab_controls_for_preparation(*ctx, 0);
    ASSERT_TRUE(dispatched.ticket);
    auto translated = translate_preparation_ticket(std::move(*dispatched.ticket));
    ASSERT_TRUE(translated.plan);
    auto preflight = prepare_control_edits(*ctx, *translated.plan);
    ASSERT_EQ(preflight.status, ControlEditStatus::Ready);
    ASSERT_TRUE(preflight.prepared);

    auto &lab = ctx->labs[0];
    const auto original_used = lab.aclr_used;
    std::array<std::array<int, 2>, 10> original_clk_ena, original_aclr;
    for (unsigned alm = 0; alm < 10; ++alm) {
        original_clk_ena[alm] = lab.alms[alm].clk_ena_idx;
        original_aclr[alm] = lab.alms[alm].aclr_idx;
    }
    std::vector<std::pair<WireId, uint64_t>> original_wires;
    for (const auto &edit : preflight.prepared->edits) {
        if (edit.kind != ControlEditKind::WireFlags)
            continue;
        if (std::none_of(original_wires.begin(), original_wires.end(),
                         [&](const auto &saved) { return saved.first == edit.wire; }))
            original_wires.emplace_back(edit.wire, ctx->wires.at(edit.wire).flags);
    }
    auto expect_original_state = [&] {
        EXPECT_EQ(lab.aclr_used, original_used);
        for (unsigned alm = 0; alm < 10; ++alm) {
            EXPECT_EQ(lab.alms[alm].clk_ena_idx, original_clk_ena[alm]);
            EXPECT_EQ(lab.alms[alm].aclr_idx, original_aclr[alm]);
        }
        for (const auto &saved : original_wires)
            EXPECT_EQ(ctx->wires.at(saved.first).flags, saved.second);
    };

    auto interrupted = *preflight.prepared;
    ASSERT_GT(interrupted.edits.size(), 3u);
    EXPECT_EQ(apply_prepared_control_edits(*ctx, std::move(interrupted), 3), ControlEditStatus::Interrupted);
    expect_original_state();

    auto changed = *preflight.prepared;
    const auto wire_edit = std::find_if(changed.edits.begin(), changed.edits.end(),
                                        [](const auto &edit) { return edit.kind == ControlEditKind::WireFlags; });
    ASSERT_NE(wire_edit, changed.edits.end());
    const WireId changed_wire = wire_edit->wire;
    const uint64_t changed_expected = wire_edit->expected;
    const uint64_t disturbed = changed_expected ^ WireInfo::BLOCKED;
    ctx->wires.at(changed_wire).flags = disturbed;
    EXPECT_EQ(apply_prepared_control_edits(*ctx, std::move(changed)), ControlEditStatus::ChangedValue);
    EXPECT_EQ(ctx->wires.at(changed_wire).flags, disturbed);
    ctx->wires.at(changed_wire).flags = changed_expected;
    expect_original_state();

    const auto clk_wire = ctx->getBelPinWire(lab.alms[0].ff_bels[0], id_CLK);
    auto &uphill = ctx->wires.at(clk_wire).wires_uphill;
    const auto source = std::find(uphill.begin(), uphill.end(), lab.clk_wires[0]);
    ASSERT_NE(source, uphill.end());
    const auto source_index = source - uphill.begin();
    uphill.erase(source);
    EXPECT_EQ(prepare_control_edits(*ctx, *translated.plan).status, ControlEditStatus::MissingRoute);
    expect_original_state();
    uphill.insert(uphill.begin() + source_index, lab.clk_wires[0]);

    EXPECT_EQ(apply_prepared_control_edits(*ctx, std::move(*preflight.prepared)), ControlEditStatus::Applied);
    for (unsigned alm = 0; alm < 10; ++alm) {
        lab.alms[alm].clk_ena_idx = original_clk_ena[alm];
        lab.alms[alm].aclr_idx = original_aclr[alm];
    }
    lab.aclr_used = original_used;
    for (const auto &saved : original_wires)
        ctx->wires.at(saved.first).flags = saved.second;
}

#ifndef NO_RUST
TEST_F(LabControlCaptureTest, PreparationCandidateFailuresAreClassifiedAndNeverSelected)
{
    cells[0]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[0]->ffInfo.ctrlset.ena = {nets[1], false};
    bind(0);
    ctx->args.lab_controls = LabControlMode::Shadow;
    auto capture = capture_lab_controls(*ctx, 0, 101);
    const auto cpp = evaluate_lab_controls_legacy(*ctx, 0, capture);

    auto check_cpp_ticket = [&](PreparationDispatchResult result, PreparationStatus expected) {
        EXPECT_EQ(result.comparison_status, expected);
        ASSERT_TRUE(result.ticket);
        auto translated = translate_preparation_ticket(std::move(*result.ticket));
        ASSERT_EQ(translated.status, PreparationStatus::Legal);
        ASSERT_TRUE(translated.plan);
        EXPECT_EQ(translated.plan->allocation.clk.net, nets[0]);
        EXPECT_EQ(translated.plan->allocation.ena[0].net, nets[1]);
    };

    check_cpp_ticket(accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_PANIC, {}),
                     PreparationStatus::TransportFailure);
    auto malformed = cpp;
    malformed.allocation[0].net_id = capture.input.net_count + 1;
    check_cpp_ticket(accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, malformed),
                     PreparationStatus::MalformedResult);
    auto unsupported = lab_control_result(capture.input, NPNR_CONTROL_UNSUPPORTED_RULES);
    unsupported.reason = NPNR_CONTROL_BAD_RULES;
    check_cpp_ticket(accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, unsupported),
                     PreparationStatus::Unsupported);
    auto mismatch = cpp;
    mismatch.allocation[0] = capture.input.ff[0].control[NPNR_CONTROL_ENA];
    check_cpp_ticket(accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, mismatch),
                     PreparationStatus::Mismatch);
    check_cpp_ticket(accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, cpp),
                     PreparationStatus::Legal);
    ctx->args.lab_controls = LabControlMode::Legacy;
}

TEST_F(LabControlCaptureTest, ShadowPreparationEditListsMatch)
{
    nets[0]->is_global = true;
    for (unsigned i = 0; i < 3; ++i) {
        cells[4 * i]->ffInfo.ctrlset.clk = {nets[0], false};
        cells[4 * i]->ffInfo.ctrlset.ena = {nets[i + 1], bool(i & 1)};
        bind(4 * i);
    }
    ctx->args.lab_controls = LabControlMode::Shadow;
    auto dispatched = dispatch_lab_controls_for_preparation(*ctx, 0);
    ASSERT_EQ(dispatched.comparison_status, PreparationStatus::Legal);
    ASSERT_TRUE(dispatched.ticket);
    auto translated = translate_preparation_ticket(std::move(*dispatched.ticket));
    ASSERT_TRUE(translated.plan);
    ASSERT_TRUE(translated.comparison_plan);
    auto selected = prepare_control_edits(*ctx, *translated.plan);
    auto comparison = prepare_control_edits(*ctx, *translated.comparison_plan);
    ASSERT_TRUE(selected.prepared);
    ASSERT_TRUE(comparison.prepared);
    EXPECT_TRUE(control_edit_lists_equal(*selected.prepared, *comparison.prepared));
    comparison.prepared->edits.back().value ^= 1;
    EXPECT_FALSE(control_edit_lists_equal(*selected.prepared, *comparison.prepared));
    ctx->args.lab_controls = LabControlMode::Legacy;
}

TEST_F(LabControlCaptureTest, PreparationModesSelectTheDocumentedAuthority)
{
    cells[0]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[0]->ffInfo.ctrlset.ena = {nets[1], false};
    bind(0);
    auto capture = capture_lab_controls(*ctx, 0, 303);
    const auto cpp = evaluate_lab_controls_legacy(*ctx, 0, capture);
    auto mismatched = cpp;
    mismatched.allocation[0] = capture.input.ff[0].control[NPNR_CONTROL_ENA];

    ctx->args.lab_controls = LabControlMode::Shadow;
    auto shadow = accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, mismatched);
    ASSERT_TRUE(shadow.ticket);
    auto shadow_plan = translate_preparation_ticket(std::move(*shadow.ticket));
    ASSERT_TRUE(shadow_plan.plan);
    EXPECT_EQ(shadow_plan.plan->allocation.clk.net, nets[0]);

    ctx->args.lab_controls = LabControlMode::Verify;
    EXPECT_THROW(accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, mismatched),
                 log_execution_error_exception);
    auto verified = accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, cpp);
    ASSERT_TRUE(verified.ticket);
    auto verified_plan = translate_preparation_ticket(std::move(*verified.ticket));
    ASSERT_TRUE(verified_plan.plan);
    ASSERT_TRUE(verified_plan.comparison_plan);

    ctx->args.lab_controls = LabControlMode::Rust;
    auto rust = accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, mismatched);
    ASSERT_TRUE(rust.ticket);
    auto rust_plan = translate_preparation_ticket(std::move(*rust.ticket));
    ASSERT_TRUE(rust_plan.plan);
    EXPECT_EQ(rust_plan.plan->allocation.clk.net, nets[1]);
    EXPECT_FALSE(rust_plan.comparison_plan);

    const auto fallbacks = ctx->lab_control_stats.fallbacks.load();
    auto unsupported = lab_control_result(capture.input, NPNR_CONTROL_UNSUPPORTED_RULES);
    unsupported.reason = NPNR_CONTROL_BAD_RULES;
    auto fallback = accept_lab_control_preparation_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, unsupported);
    EXPECT_EQ(ctx->lab_control_stats.fallbacks.load(), fallbacks + 1);
    ASSERT_TRUE(fallback.ticket);
    auto fallback_plan = translate_preparation_ticket(std::move(*fallback.ticket));
    ASSERT_TRUE(fallback_plan.plan);
    EXPECT_EQ(fallback_plan.plan->allocation.clk.net, nets[0]);
    ctx->args.lab_controls = LabControlMode::Legacy;
}
#endif

TEST_F(LabControlCaptureTest, EveryPhysicalSlotAndSeparateAlmRestrictions)
{
    for (unsigned slot = 0; slot < 40; ++slot) {
        SCOPED_TRACE(slot);
        cells[slot]->ffInfo.ctrlset.clk = {nets[0], false};
        bind(slot);
        const auto capture = capture_lab_controls(*ctx, 0);
        EXPECT_EQ(capture.input.ff[slot].occupied, 1u);
        EXPECT_EQ(compare().status, NPNR_CONTROL_LEGAL);
        EXPECT_EQ(ctx->is_alm_legal(0, slot / 4), (slot % 2) == 0);
        clear_bindings();
    }
}

TEST_F(LabControlCaptureTest, AllTwoHundredNetIdentitiesFit)
{
    for (unsigned slot = 0; slot < 40; ++slot) {
        auto &cs = cells[slot]->ffInfo.ctrlset;
        cs.clk = {nets[5 * slot], false};
        cs.sload = {nets[5 * slot + 1], false};
        cs.sclr = {nets[5 * slot + 2], false};
        cs.aclr = {nets[5 * slot + 3], false};
        cs.ena = {nets[5 * slot + 4], false};
        bind(slot);
    }
    const auto capture = capture_lab_controls(*ctx, 0);
    EXPECT_EQ(capture.input.net_count, 200u);
    EXPECT_EQ(capture.input.ff[39].control[4].net_id, 200u);
    EXPECT_EQ(compare().status, NPNR_CONTROL_ILLEGAL);
}

TEST_F(LabControlCaptureTest, GreedyAllocationDependsOnPhysicalOrder)
{
    nets[4]->is_global = true;
    for (unsigned order = 0; order < 2; ++order) {
        for (unsigned i = 0; i < 3; ++i) {
            auto &cs = cells[2 * i]->ffInfo.ctrlset;
            cs.clk = {nets[4], false};
            cs.sload = {nets[3], false};
            cs.sclr = {nets[0], false};
            cs.ena = {nets[(i + order) % 3], false};
            bind(2 * i);
        }
        EXPECT_EQ(compare().status, order == 0 ? NPNR_CONTROL_ILLEGAL : NPNR_CONTROL_LEGAL);
        clear_bindings();
    }
}

TEST_F(LabControlCaptureTest, ExhaustiveSmallControlDomains)
{
    for (unsigned kind = 0; kind < 5; ++kind) {
        for (unsigned assignment = 0; assignment < 216; ++assignment) {
            SCOPED_TRACE(kind * 216 + assignment);
            unsigned remaining = assignment;
            for (unsigned slot : {0u, 2u, 39u}) {
                auto &cs = cells[slot]->ffInfo.ctrlset;
                cs = {};
                std::array<ControlSig *, 5> controls{&cs.clk, &cs.sload, &cs.sclr, &cs.aclr, &cs.ena};
                unsigned choice = remaining % 6;
                remaining /= 6;
                *controls[kind] = {choice < 2 ? nullptr : nets[(choice - 2) / 2], bool(choice % 2)};
                bind(slot);
            }
            compare();
            clear_bindings();
        }
    }
}

TEST_F(LabControlCaptureTest, GeneratedMixedControls)
{
    std::mt19937 rng(0x4c4142);
    for (unsigned trial = 0; trial < 3000; ++trial) {
        SCOPED_TRACE(trial);
        for (unsigned i = 0; i < 8; ++i)
            nets[i]->is_global = rng() % 2;
        std::array<ControlSig, 5> base{};
        for (auto &signal : base) {
            unsigned choice = rng() % 9;
            signal = {choice == 8 ? nullptr : nets[choice], bool(rng() % 2)};
        }
        for (unsigned slot = 0; slot < 40; ++slot) {
            if (rng() % 3 == 0)
                continue;
            auto &cs = cells[slot]->ffInfo.ctrlset;
            std::array<ControlSig *, 5> controls{&cs.clk, &cs.sload, &cs.sclr, &cs.aclr, &cs.ena};
            for (unsigned kind = 0; kind < controls.size(); ++kind) {
                *controls[kind] = base[kind];
                if (trial % 2 == 0 && rng() % 8 == 0) {
                    unsigned choice = rng() % 9;
                    *controls[kind] = {choice == 8 ? nullptr : nets[choice], bool(rng() % 2)};
                }
            }
            bind(slot);
        }
        compare();
        clear_bindings();
    }
}

TEST_F(LabControlCaptureTest, MlabGroupingAndWriteReservationsRemainHostOwned)
{
    uint32_t lab = 0;
    while (lab < ctx->labs.size() && !ctx->labs[lab].is_mlab)
        ++lab;
    ASSERT_LT(lab, ctx->labs.size());
    std::array<CellInfo *, 2> rams;
    std::array<WireId, 4> wires;
    std::array<uint32_t, 4> saved_flags;
    for (unsigned i = 0; i < 2; ++i) {
        auto *ram = ctx->createCell(ctx->idf("lab_test_ram_%u", i), id_MISTRAL_MLAB);
        rams[i] = ram;
        ram->addInput(id_CLK1);
        ram->connectPort(id_CLK1, nets[0]);
        ram->pin_data[id_CLK1].state = PIN_INV;
        ram->addInput(id_A1EN);
        ram->connectPort(id_A1EN, nets[1]);
        ctx->assign_comb_info(ram);
        const auto bel = ctx->labs[lab].alms[i].lut_bels[0];
        ctx->bindBel(bel, ram, STRENGTH_WEAK);
        wires[2 * i] = ctx->getBelPinWire(bel, id_WCLK);
        wires[2 * i + 1] = ctx->getBelPinWire(bel, id_WE);
    }
    for (unsigned i = 0; i < wires.size(); ++i)
        saved_flags[i] = ctx->wires.at(wires[i]).flags;
    EXPECT_TRUE(ctx->check_mlab_groups(lab));
    auto v2_input = capture_lab_v2(*ctx, lab, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX, 1200);
    auto v2_result = evaluate_lab_v2_cpp(v2_input);
#ifndef NO_RUST
    expect_v2_rust_parity(v2_input, v2_result);
#endif
    EXPECT_EQ(v2_result.status, NPNR_LAB_V2_LEGAL);
    for (unsigned alm = 0; alm < 10; ++alm)
        EXPECT_EQ(v2_result.recomputed_input_count[alm], v2_input.alm[alm].cached_input_count);
    const auto capture = capture_lab_controls(*ctx, lab);
    EXPECT_EQ(capture.input.net_count, 0u); // LUTRAM write controls are outside the FF snapshot.
    ctx->assign_control_sets(lab);
    std::array<uint32_t, 4> reference_flags;
    for (unsigned i = 0; i < wires.size(); ++i)
        reference_flags[i] = ctx->wires.at(wires[i]).flags;
    for (unsigned i = 0; i < 2; ++i) {
        EXPECT_EQ(reserved_source(*ctx, wires[2 * i]), ctx->labs[lab].clk_wires[0]);
        EXPECT_EQ(reserved_source(*ctx, wires[2 * i + 1]), ctx->labs[lab].ena_wires[0]);
    }
#ifndef NO_RUST
    for (auto mode : {LabControlMode::Shadow, LabControlMode::Verify, LabControlMode::Rust}) {
        ctx->args.lab_controls = mode;
        for (unsigned i = 0; i < wires.size(); ++i)
            ctx->wires.at(wires[i]).flags = saved_flags[i];
        ctx->assign_control_sets(lab);
        for (unsigned i = 0; i < wires.size(); ++i)
            EXPECT_EQ(ctx->wires.at(wires[i]).flags, reference_flags[i]);
        ctx->bindBel(ctx->labs[lab].alms[9].ff_bels[0], cells[0], STRENGTH_WEAK);
        EXPECT_TRUE(ctx->is_lab_ctrlset_legal(lab));
        EXPECT_FALSE(ctx->check_mlab_groups(lab)); // FF-only legality does not certify this LAB.
        v2_input = capture_lab_v2(*ctx, lab, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX, 1201);
        v2_result = evaluate_lab_v2_cpp(v2_input);
#ifndef NO_RUST
        expect_v2_rust_parity(v2_input, v2_result);
#endif
        EXPECT_EQ(v2_result.reason, NPNR_LAB_V2_MLAB_FF);
        ctx->unbindBel(cells[0]->bel);
    }
#endif
    ++rams[1]->combInfo.mlab_group;
    EXPECT_FALSE(ctx->check_mlab_groups(lab));
    v2_input = capture_lab_v2(*ctx, lab, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX, 1202);
    v2_result = evaluate_lab_v2_cpp(v2_input);
#ifndef NO_RUST
    expect_v2_rust_parity(v2_input, v2_result);
#endif
    EXPECT_EQ(v2_result.reason, NPNR_LAB_V2_MLAB_GROUP);
    for (auto *ram : rams)
        ctx->unbindBel(ram->bel);
    for (unsigned i = 0; i < wires.size(); ++i)
        ctx->wires.at(wires[i]).flags = saved_flags[i];
    ctx->args.lab_controls = LabControlMode::Legacy;
}

#ifndef NO_RUST
TEST_F(LabControlCaptureTest, ShadowDiagnosticsAreCappedAndStrictModesFailClosed)
{
    // Preserve global logging hooks even if a test assertion fails.
    struct CaptureLog
    {
        log_write_type saved = log_write_function;
        std::string text;
        CaptureLog()
        {
            log_write_function = [this](std::string s) { text += s; };
        }
        ~CaptureLog() { log_write_function = saved; }
    } log;
    auto &stats = ctx->lab_control_stats;
    stats.diagnostics = stats.mismatches = stats.errors = stats.fallbacks = 0;
    cells[0]->ffInfo.ctrlset.clk = {nets[0], false};
    cells[2]->ffInfo.ctrlset.clk = {nets[1], false};
    bind(0);
    bind(2);
    const auto capture = capture_lab_controls(*ctx, 0, 777);
    // Well-formed legal result, intentionally wrong for this two-clock LAB.
    const auto wrong = lab_control_result(capture.input, NPNR_CONTROL_LEGAL);
    ctx->args.lab_controls = LabControlMode::Shadow;
    for (unsigned i = 0; i < 10; ++i)
        EXPECT_FALSE(accept_lab_control_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, wrong));
    EXPECT_EQ(stats.mismatches.load(), 10u);
    size_t records = 0;
    for (size_t pos = 0; (pos = log.text.find("Replay: ", pos)) != std::string::npos; pos += 8)
        ++records;
    EXPECT_EQ(records, 4u);
    EXPECT_NE(log.text.find("first_difference"), std::string::npos);
    EXPECT_NE(log.text.find("candidate"), std::string::npos);
    // Simulate a broken capture adapter: detached C++ and Rust agree, but the live
    // worker sees two clocks. The replay must preserve the actual live reference.
    auto drifted = capture;
    drifted.input.ff[2].control[0].net_id = 1;
    drifted.input.net_count = 1;
    const auto drifted_result = evaluate_lab_controls_cpp(drifted.input);
    stats.diagnostics = 0;
    log.text.clear();
    EXPECT_FALSE(accept_lab_control_candidate(*ctx, 0, drifted, NPNR_LAB_CALL_OK, drifted_result));
    EXPECT_NE(log.text.find("live.status"), std::string::npos);
    const auto replay_start = log.text.find("Replay: ");
    ASSERT_NE(replay_start, std::string::npos);
    const auto replay_end = log.text.find('\n', replay_start);
    NpnrLabControlsV1 replay_input;
    NpnrLabControlResultV1 replay_reference;
    std::string replay_error;
    ASSERT_TRUE(read_lab_control_replay(log.text.substr(replay_start + 8, replay_end - replay_start - 8), replay_input,
                                        replay_reference, replay_error))
            << replay_error;
    EXPECT_EQ(replay_reference.status, NPNR_CONTROL_ILLEGAL);
    ctx->args.lab_controls = LabControlMode::Verify;
    EXPECT_THROW(accept_lab_control_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, wrong),
                 log_execution_error_exception);
    auto broken = wrong;
    broken.allocation[0].net_id = 201;
    ctx->args.lab_controls = LabControlMode::Rust;
    EXPECT_THROW(accept_lab_control_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, broken),
                 log_execution_error_exception);
    EXPECT_THROW(accept_lab_control_candidate(*ctx, 0, capture, NPNR_LAB_CALL_PANIC, wrong),
                 log_execution_error_exception);
    auto unsupported = lab_control_result(capture.input, NPNR_CONTROL_UNSUPPORTED_RULES);
    unsupported.reason = NPNR_CONTROL_BAD_RULES;
    ctx->args.lab_controls = LabControlMode::Shadow;
    EXPECT_FALSE(accept_lab_control_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, unsupported));
    ctx->args.lab_controls = LabControlMode::Verify;
    EXPECT_THROW(accept_lab_control_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, unsupported),
                 log_execution_error_exception);
    ctx->args.lab_controls = LabControlMode::Rust;
    EXPECT_FALSE(accept_lab_control_candidate(*ctx, 0, capture, NPNR_LAB_CALL_OK, unsupported));
    EXPECT_EQ(stats.fallbacks.load(), 1u);
    ctx->args.lab_controls = LabControlMode::Legacy;
}
#endif

TEST_F(LabControlCaptureTest, CheckpointRoundTripRestoresPackingStateAndIterationOrders)
{
    // Fresh cells so the fixture's own cells keep their state; every name is
    // interned before the checkpoint is written, as in a real run.
    CellInfo *root = ctx->createCell(ctx->id("ckpt_root"), id_MISTRAL_FF);
    CellInfo *child_a = ctx->createCell(ctx->id("ckpt_child_a"), id_MISTRAL_FF);
    CellInfo *child_b = ctx->createCell(ctx->id("ckpt_child_b"), id_MISTRAL_FF);
    NetInfo *clk = ctx->createNet(ctx->id("ckpt_clk"));
    NetInfo *orphan = ctx->createNet(ctx->id("ckpt_orphan")); // no driver, no users: a reload drops it
    orphan->attrs[ctx->id("src")] = Property("ckpt.v:9");
    const IdString pad = ctx->id("ckpt_pad"), io_standard = ctx->id("IO_STANDARD"), drive = ctx->id("CURRENT_STRENGTH");
    for (CellInfo *ci : {root, child_a, child_b}) {
        ci->addInput(id_CLK);
        ci->addInput(id_DATAIN);
        ci->addOutput(id_Q);
        ci->connectPort(id_CLK, clk);
        ci->setAttr(ctx->id("keep"), Property(1, 1));
        ci->setAttr(ctx->id("src"), Property("ckpt.v:1"));
        ci->setParam(ctx->id("INIT"), Property(0, 1));
    }
    root->cluster = root->name;
    root->constr_children = {child_a, child_b};
    child_a->cluster = root->name;
    child_a->constr_z = 1;
    child_b->cluster = root->name;
    child_b->constr_x = 0;
    child_b->constr_y = 1;
    child_b->constr_z = 2;
    child_b->constr_abs_z = true;
    child_a->pin_data[id_DATAIN].state = PIN_INV;
    child_a->pin_data[id_DATAIN].bel_pins = {id_DATAIN};
    child_a->pin_data[id_CLK].state = PIN_SIG;
    child_a->pin_data[id_CLK].bel_pins = {id_CLK};
    child_b->pin_data[id_DATAIN].state = PIN_1; // a constant input with its bel pin cleared, as lab_pre_route leaves
    child_b->pin_data[id_DATAIN].bel_pins.clear();
    ctx->io_attr[pad][io_standard] = Property("3.3-V LVTTL");
    ctx->io_attr[pad][drive] = Property(12, 8);
    ctx->pllclk_sel_map[123456789012345ull] = 3;
    const BelId root_bel = ctx->labs.at(0).alms.at(0).ff_bels.at(0);
    ctx->bindBel(root_bel, root, STRENGTH_LOCKED); // as the packer binds QSF-located IO cells
    clk->driver.port = id_Q;                       // stale driver port of an undriven net, as the packer leaves behind
    {
        PortInfo top;
        top.name = pad;
        top.net = clk;
        top.type = PORT_IN;
        ctx->ports[pad] = top;
    }
    const uint64_t rng_before = ctx->rngstate;

    auto cell_order = [&]() {
        std::vector<IdString> names;
        for (auto &cell : ctx->cells)
            names.push_back(cell.first);
        return names;
    };
    auto user_order = [&](const NetInfo *net) {
        std::vector<std::pair<IdString, IdString>> users;
        for (auto &user : net->users)
            users.emplace_back(user.cell->name, user.port);
        return users;
    };
    auto port_order = [&](const CellInfo *ci) {
        std::vector<IdString> names;
        for (auto &port : ci->ports)
            names.push_back(port.first);
        return names;
    };
    auto attr_order = [&](const CellInfo *ci) {
        std::vector<IdString> names;
        for (auto &attr : ci->attrs)
            names.push_back(attr.first);
        return names;
    };
    const auto cells_before = cell_order();
    const auto users_before = user_order(clk);
    const auto ports_before = port_order(root);
    const auto attrs_before = attr_order(root);
    ASSERT_EQ(users_before.size(), 3u);

    std::ostringstream packed, placed;
    ASSERT_TRUE(ctx->writeCheckpoint(packed, "packed"));
    ASSERT_TRUE(ctx->writeCheckpoint(placed, "placed"));
    EXPECT_NE(packed.str().find("\"cluster_cells\""), std::string::npos);
    EXPECT_NE(packed.str().find("\"bindings\""), std::string::npos);
    EXPECT_NE(packed.str().find("\"pllclk_sel\""), std::string::npos);

    // Wreck everything the checkpoint owns: packing state, physical choices,
    // the RNG, and the iteration orders of cells, users, ports, attributes.
    for (CellInfo *ci : {root, child_a, child_b}) {
        ci->cluster = ClusterId();
        ci->constr_children.clear();
        ci->constr_x = ci->constr_y = ci->constr_z = 0;
        ci->constr_abs_z = false;
        ci->pin_data.clear();
    }
    ctx->io_attr.clear();
    ctx->pllclk_sel_map.clear();
    ctx->ports.clear();
    ctx->unbindBel(root_bel);
    clk->driver.port = IdString();
    ctx->rngstate = rng_before + 17;
    ctx->nets.erase(orphan->name); // as the frontend would have: never materialised
    orphan = nullptr;
    {
        // root was inserted first of the three, so it iterates last; moving it
        // to the newest slot changes the order (and the erase swaps the
        // newest entry into its old slot, which perturbs the rest as well).
        auto keep = std::move(ctx->cells.at(root->name));
        ctx->cells.erase(root->name);
        ctx->cells[root->name] = std::move(keep);
    }
    child_b->disconnectPort(id_CLK); // free slot 2, then slot 1: the reconnects swap places
    child_a->disconnectPort(id_CLK);
    child_b->connectPort(id_CLK, clk);
    child_a->connectPort(id_CLK, clk);
    {
        auto keep = root->ports.at(id_CLK);
        root->ports.erase(id_CLK);
        root->ports[id_CLK] = keep;
        auto keep_attr = root->attrs.at(ctx->id("keep"));
        root->attrs.erase(ctx->id("keep"));
        root->attrs[ctx->id("keep")] = keep_attr;
    }
    EXPECT_NE(cell_order(), cells_before);
    EXPECT_NE(user_order(clk), users_before);
    EXPECT_NE(port_order(root), ports_before);
    EXPECT_NE(attr_order(root), attrs_before);

    struct LogSink
    {
        LogSink() { log_streams.emplace_back(&std::cerr, LogLevel::LOG_MSG); }
        ~LogSink() { log_streams.pop_back(); }
    } sink;
    ASSERT_TRUE(ctx->checkpointPreload(packed.str()));
    ASSERT_TRUE(ctx->checkpointRestore());
    EXPECT_EQ(ctx->checkpointPhase(), "packed");
    ASSERT_EQ(ctx->nets.count(ctx->id("ckpt_orphan")), 1u);
    orphan = ctx->nets.at(ctx->id("ckpt_orphan")).get();
    EXPECT_EQ(orphan->driver.cell, nullptr);
    EXPECT_EQ(orphan->users.entries(), 0);
    EXPECT_EQ(orphan->attrs.at(ctx->id("src")).as_string(), "ckpt.v:9");
    EXPECT_EQ(cell_order(), cells_before);
    EXPECT_EQ(user_order(clk), users_before);
    EXPECT_EQ(port_order(root), ports_before);
    EXPECT_EQ(attr_order(root), attrs_before);
    EXPECT_EQ(root->cluster, root->name);
    ASSERT_EQ(root->constr_children.size(), 2u);
    EXPECT_EQ(root->constr_children[0], child_a);
    EXPECT_EQ(root->constr_children[1], child_b);
    EXPECT_EQ(child_a->cluster, root->name);
    EXPECT_EQ(child_a->constr_z, 1);
    EXPECT_EQ(child_b->constr_y, 1);
    EXPECT_EQ(child_b->constr_z, 2);
    EXPECT_TRUE(child_b->constr_abs_z);
    ASSERT_EQ(child_a->pin_data.count(id_DATAIN), 1u);
    EXPECT_EQ(child_a->pin_data.at(id_DATAIN).state, PIN_INV);
    EXPECT_EQ(child_a->pin_data.at(id_DATAIN).bel_pins, std::vector<IdString>{id_DATAIN});
    EXPECT_EQ(child_a->pin_data.at(id_CLK).state, PIN_SIG);
    ASSERT_EQ(child_b->pin_data.count(id_DATAIN), 1u);
    EXPECT_EQ(child_b->pin_data.at(id_DATAIN).state, PIN_1);
    EXPECT_TRUE(child_b->pin_data.at(id_DATAIN).bel_pins.empty()); // recorded emptiness wins over the default
    ASSERT_EQ(ctx->io_attr.count(pad), 1u);
    EXPECT_TRUE(ctx->io_attr.at(pad).at(io_standard).is_string);
    EXPECT_EQ(ctx->io_attr.at(pad).at(io_standard).as_string(), "3.3-V LVTTL");
    EXPECT_FALSE(ctx->io_attr.at(pad).at(drive).is_string);
    EXPECT_EQ(ctx->io_attr.at(pad).at(drive).as_int64(), 12);
    EXPECT_EQ(ctx->io_attr.at(pad).at(drive).size(), 8);
    EXPECT_EQ(ctx->rngstate, rng_before);
    ASSERT_EQ(ctx->ports.count(pad), 1u);
    EXPECT_EQ(ctx->ports.at(pad).net, clk);
    EXPECT_EQ(ctx->ports.at(pad).type, PORT_IN);
    ctx->ports.clear(); // the placed restore below expects the reload's empty table
    EXPECT_EQ(root->bel, root_bel);
    EXPECT_EQ(root->belStrength, STRENGTH_LOCKED);
    EXPECT_EQ(clk->driver.cell, nullptr);
    EXPECT_EQ(clk->driver.port, id_Q);
    // Physical state travels at every phase; the placed restore below rebinds.
    ASSERT_EQ(ctx->pllclk_sel_map.count(123456789012345ull), 1u);
    ctx->unbindBel(root_bel);
    // The restore rebuilt the FF facts from the restored netlist.
    EXPECT_EQ(child_a->ffInfo.ctrlset.clk.net, clk);

    ctx->rngstate = rng_before + 5;
    ASSERT_TRUE(ctx->checkpointPreload(placed.str()));
    ASSERT_TRUE(ctx->checkpointRestore());
    EXPECT_EQ(ctx->checkpointPhase(), "placed");
    ASSERT_EQ(ctx->pllclk_sel_map.count(123456789012345ull), 1u);
    EXPECT_EQ(ctx->pllclk_sel_map.at(123456789012345ull), 3);
    EXPECT_EQ(ctx->rngstate, rng_before);
    EXPECT_EQ(root->bel, root_bel);
    ctx->unbindBel(root_bel);

    // A checkpoint for another device or an unknown phase is refused before
    // anything is touched.
    std::string wrong_device = packed.str();
    wrong_device.replace(wrong_device.find("5CSEBA6U23I7"), 12, "5CGXFC5C6F27");
    EXPECT_THROW(ctx->checkpointPreload(wrong_device), log_execution_error_exception);
    std::string unknown = packed.str();
    unknown.replace(unknown.find("\"phase\": \"packed\""), 17, "\"phase\": \"signed\"");
    EXPECT_THROW(ctx->checkpointPreload(unknown), log_execution_error_exception);

    // Lineage hashes come from a hand-written SHA-256: check it against the
    // FIPS 180-4 known answers for "abc" and the empty message.
    {
        const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/ckpt_sha.txt";
        {
            std::ofstream out(path, std::ios::binary);
            out << "abc";
        }
        EXPECT_EQ(checkpoint_sha256_file(path), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
        }
        EXPECT_EQ(checkpoint_sha256_file(path), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        std::remove(path.c_str());
        EXPECT_EQ(checkpoint_sha256_file(path), "");
    }

    // Route-prepared state (2b): LAB control allocation and ALM modes, a
    // control-wire reservation, and a bound route, restored in wires-map order.
    auto &lab0 = ctx->labs.at(0);
    const auto alm0_before = lab0.alms[0], alm1_before = lab0.alms[1];
    const auto aclr_before = lab0.aclr_used;
    lab0.alms[0].clk_ena_idx = {1, 0};
    lab0.alms[0].aclr_idx = {0, 1};
    lab0.alms[0].l6_mode = true;
    lab0.alms[1].carry_mode = true;
    lab0.aclr_used = {true, false};
    const WireId reserved = lab0.clk_wires[0];
    const uint64_t flags_before = ctx->wires.at(reserved).flags;
    ctx->wires.at(reserved).flags = WireInfo::RESERVED_ROUTE | 2;
    // A route-through buffer at a LUT BEL, as lab_pre_route leaves: the
    // restore must give it comb facts (MLAB group -1), or the bitstream
    // takes the LUTRAM path for its LAB.
    CellInfo *buffer = ctx->createCell(ctx->id("ckpt_routethru"), id_MISTRAL_BUF);
    buffer->addInput(id_A);
    buffer->addOutput(id_Q);
    buffer->connectPort(id_A, clk);
    buffer->pin_data[id_A].bel_pins = {id_C};
    buffer->combInfo.mlab_group = 0; // what a zero-filled union reads as
    ctx->bindBel(lab0.alms[1].lut_bels[0], buffer, STRENGTH_STRONG);
    const WireId src = lab0.alms[0].comb_out[0];
    PipId pip;
    for (PipId candidate : ctx->getPipsDownhill(src)) {
        pip = candidate;
        break;
    }
    ASSERT_NE(pip, PipId());
    const WireId dst = ctx->getPipDstWire(pip);
    ctx->bindWire(src, clk, STRENGTH_LOCKED);
    ctx->bindPip(pip, clk, STRENGTH_STRONG);
    auto route_order = [&]() {
        std::vector<WireId> order;
        for (auto &wire : clk->wires)
            order.push_back(wire.first);
        return order;
    };
    const auto route_before = route_order();
    ASSERT_EQ(route_before.size(), 2u);
    ctx->bindBel(root_bel, root, STRENGTH_LOCKED);
    std::ostringstream routed;
    ASSERT_TRUE(ctx->writeCheckpoint(routed, "routed"));
    EXPECT_NE(routed.str().find("\"wire_flags\""), std::string::npos);
    ctx->unbindPip(pip);
    ctx->unbindWire(src);
    lab0.alms[0] = alm0_before;
    lab0.alms[1] = alm1_before;
    lab0.aclr_used = aclr_before;
    ctx->wires.at(reserved).flags = flags_before;
    ctx->unbindBel(root_bel);
    ctx->unbindBel(lab0.alms[1].lut_bels[0]);
    ctx->ports.clear();
    ASSERT_TRUE(ctx->checkpointPreload(routed.str()));
    ASSERT_TRUE(ctx->checkpointRestore());
    EXPECT_EQ(ctx->checkpointPhase(), "routed");
    EXPECT_EQ(root->bel, root_bel);
    EXPECT_EQ(lab0.alms[0].clk_ena_idx, (std::array<int, 2>{1, 0}));
    EXPECT_EQ(lab0.alms[0].aclr_idx, (std::array<int, 2>{0, 1}));
    EXPECT_TRUE(lab0.alms[0].l6_mode);
    EXPECT_TRUE(lab0.alms[1].carry_mode);
    EXPECT_EQ(lab0.aclr_used, (std::array<bool, 2>{true, false}));
    EXPECT_EQ(ctx->wires.at(reserved).flags, WireInfo::RESERVED_ROUTE | 2);
    EXPECT_EQ(route_order(), route_before);
    EXPECT_EQ(ctx->getBoundWireNet(src), clk);
    EXPECT_EQ(ctx->getBoundPipNet(pip), clk);
    EXPECT_EQ(clk->wires.at(dst).pip, pip);
    EXPECT_EQ(clk->wires.at(dst).strength, STRENGTH_STRONG);
    EXPECT_EQ(clk->wires.at(src).strength, STRENGTH_LOCKED);
    EXPECT_EQ(buffer->bel, lab0.alms[1].lut_bels[0]);
    EXPECT_EQ(buffer->combInfo.mlab_group, -1);
    EXPECT_EQ(buffer->combInfo.lut_input_count, 1);
    ctx->unbindBel(lab0.alms[1].lut_bels[0]);
    buffer->disconnectPort(id_A);
    ctx->cells.erase(buffer->name);
    ctx->unbindPip(pip);
    ctx->unbindWire(src);
    lab0.alms[0] = alm0_before;
    lab0.alms[1] = alm1_before;
    lab0.aclr_used = aclr_before;
    ctx->wires.at(reserved).flags = flags_before;
    ctx->unbindBel(root_bel);

    for (CellInfo *ci : {root, child_a, child_b}) {
        ci->disconnectPort(id_CLK);
        ctx->cells.erase(ci->name);
    }
    clk->driver.port = IdString();
    ctx->nets.erase(clk->name);
    ctx->nets.erase(ctx->id("ckpt_orphan"));
    ctx->io_attr.clear();
    ctx->pllclk_sel_map.clear();
    ctx->ports.clear();
}

TEST_F(LabControlCaptureTest, AlmPairingFormsOnlyPairsTheAlmRuleAllows)
{
    // Stage 6 (6b): the packer's pairing rule must be at least as strict as the ALM checker,
    // the pair cluster must land on the two halves of one ALM, and levels widen the search.
    std::vector<NetInfo *> nets;
    for (int i = 0; i < 24; i++)
        nets.push_back(ctx->createNet(ctx->idf("ap_n%d", i)));
    const IdString ports[] = {id_A, id_B, id_C, id_D, id_E};
    auto lut = [&](const char *name, IdString type, std::initializer_list<int> ins) {
        CellInfo *c = ctx->createCell(ctx->id(name), type);
        int i = 0;
        for (int n : ins) {
            c->addInput(ports[i]);
            c->connectPort(ports[i], nets.at(n));
            i++;
        }
        c->addOutput(id_Q);
        return c;
    };
    CellInfo *p5a = lut("ap_p5a", id_MISTRAL_ALUT5, {0, 1, 2, 3, 4});
    CellInfo *p5b = lut("ap_p5b", id_MISTRAL_ALUT5, {0, 1, 5, 6, 7});   // shares two with p5a
    CellInfo *p5c = lut("ap_p5c", id_MISTRAL_ALUT5, {0, 8, 9, 10, 11}); // shares one with each
    CellInfo *q4a = lut("ap_q4a", id_MISTRAL_ALUT4, {12, 13, 14, 15});
    CellInfo *q4b = lut("ap_q4b", id_MISTRAL_ALUT4, {16, 17, 18, 19}); // shares nothing
    EXPECT_TRUE(alm_pair_compatible(p5a, p5b));
    EXPECT_FALSE(alm_pair_compatible(p5a, p5c));
    EXPECT_TRUE(alm_pair_compatible(q4a, q4b));
    EXPECT_FALSE(alm_pair_compatible(p5c, q4a));

    auto r1 = pair_alm_luts(*ctx, 1);
    EXPECT_EQ(r1.pairs, 1u);
    EXPECT_EQ(r1.by_shared, 1u);
    EXPECT_TRUE(is_alm_pair_root(p5a));
    EXPECT_EQ(p5b->cluster, p5a->name);
    EXPECT_EQ(p5b->constr_z, 1);

    auto &lab0 = ctx->labs.at(0);
    std::vector<std::pair<CellInfo *, BelId>> placement;
    EXPECT_TRUE(ctx->getClusterPlacement(p5a->cluster, lab0.alms[1].lut_bels[0], placement));
    ASSERT_EQ(placement.size(), 2u);
    EXPECT_EQ(placement[0].second, lab0.alms[1].lut_bels[0]);
    EXPECT_EQ(placement[1].second, lab0.alms[1].lut_bels[1]);
    EXPECT_FALSE(ctx->getClusterPlacement(p5a->cluster, lab0.alms[1].lut_bels[1], placement));

    // The checker agrees: the pair is legal in one ALM, the rejected one is not.
    ctx->assignArchInfo();
    ctx->bindBel(lab0.alms[1].lut_bels[0], p5a, STRENGTH_STRONG);
    ctx->bindBel(lab0.alms[1].lut_bels[1], p5b, STRENGTH_STRONG);
    EXPECT_TRUE(ctx->isBelLocationValid(lab0.alms[1].lut_bels[0]));
    EXPECT_TRUE(ctx->isBelLocationValid(lab0.alms[1].lut_bels[1]));
    ctx->unbindBel(lab0.alms[1].lut_bels[0]);
    ctx->unbindBel(lab0.alms[1].lut_bels[1]);
    ctx->bindBel(lab0.alms[2].lut_bels[0], p5c, STRENGTH_STRONG);
    ctx->bindBel(lab0.alms[2].lut_bels[1], q4a, STRENGTH_STRONG);
    EXPECT_FALSE(ctx->isBelLocationValid(lab0.alms[2].lut_bels[0]) &&
                 ctx->isBelLocationValid(lab0.alms[2].lut_bels[1]));
    ctx->unbindBel(lab0.alms[2].lut_bels[0]);
    ctx->unbindBel(lab0.alms[2].lut_bels[1]);

    // Level 3 pairs the unrelated 4-LUTs; the lone 5-LUT stays single.
    auto r3 = pair_alm_luts(*ctx, 3);
    EXPECT_EQ(r3.pairs, 1u);
    EXPECT_EQ(r3.by_any, 1u);
    EXPECT_TRUE(is_alm_pair_root(q4a));
    EXPECT_EQ(p5c->cluster, ClusterId());
}

TEST_F(LabControlCaptureTest, RegisterPackingClustersARegisterWithItsLut)
{
    // Stage 6 (6g): a register driven by a plain LUT joins that LUT's cluster and lands on a
    // register bel of the LUT's half, a pair's registers land on their own halves, a LUT takes
    // one (the checker admits one register per half), registers whose control sets cannot share
    // a LAB are kept apart, and the checker accepts the packed ALMs live and detached.
    std::vector<NetInfo *> nets;
    for (int i = 0; i < 30; i++)
        nets.push_back(ctx->createNet(ctx->idf("rp_n%d", i)));
    const IdString ports[] = {id_A, id_B, id_C, id_D, id_E};
    auto lut = [&](const char *name, IdString type, std::initializer_list<int> ins, int out) {
        CellInfo *cell = ctx->createCell(ctx->id(name), type);
        int i = 0;
        for (int n : ins) {
            cell->addInput(ports[i]);
            cell->connectPort(ports[i], nets.at(n));
            i++;
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, nets.at(out));
        return cell;
    };
    auto ff = [&](const char *name, int d, int ena = -1, int sclr = -1) {
        CellInfo *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        cell->addInput(id_DATAIN);
        cell->connectPort(id_DATAIN, nets.at(d));
        cell->addInput(id_CLK);
        cell->connectPort(id_CLK, nets.at(28));
        if (ena >= 0) {
            cell->addInput(id_ENA);
            cell->connectPort(id_ENA, nets.at(ena));
        }
        if (sclr >= 0) {
            cell->addInput(id_SCLR);
            cell->connectPort(id_SCLR, nets.at(sclr));
        }
        cell->addOutput(id_Q);
        return cell;
    };
    CellInfo *a = lut("rp_a", id_MISTRAL_ALUT4, {0, 1, 2, 3}, 20);
    CellInfo *b = lut("rp_b", id_MISTRAL_ALUT4, {0, 4, 5, 6}, 21); // shares one input with a: pairs at level 1
    CellInfo *c = lut("rp_c", id_MISTRAL_ALUT4, {7, 8, 9, 10}, 22);
    CellInfo *d = lut("rp_d", id_MISTRAL_ALUT4, {7, 11, 12, 13}, 23); // shares one input with c
    CellInfo *s = lut("rp_s", id_MISTRAL_ALUT3, {14, 15, 16}, 24);    // shares nothing: stays single
    CellInfo *fa = ff("rp_fa", 20), *fa2 = ff("rp_fa2", 20), *fa3 = ff("rp_fa3", 20);
    CellInfo *fb = ff("rp_fb", 21);
    // c's register takes an enable and a synchronous clear, d's a second enable: with a
    // non-global clock the LAB's shared lines cannot carry all three, so d's stays out.
    CellInfo *fc = ff("rp_fc", 22, 25, 26);
    CellInfo *fd = ff("rp_fd", 23, 27);
    CellInfo *fs = ff("rp_fs", 24);
    CellInfo *fx = ff("rp_fx", 29); // driven by no LUT
    EXPECT_EQ(pair_alm_luts(*ctx, 1).pairs, 2u);
    ctx->assignArchInfo(); // the packer reads the registers' control sets
    auto r = pack_registers(*ctx);
    EXPECT_GE(r.registers, 8u); // the fixture's own registers are counted too
    EXPECT_EQ(r.lut_driven, 7u);
    EXPECT_EQ(r.packed, 4u);
    EXPECT_EQ(r.lut_full, 2u);
    EXPECT_EQ(r.control_conflict, 1u);
    EXPECT_EQ(r.onto_pair_root, 2u);
    EXPECT_EQ(r.onto_pair_child, 1u);
    EXPECT_EQ(r.onto_single, 1u);
    EXPECT_TRUE(is_alm_cluster_root(a));
    EXPECT_FALSE(is_alm_pair_root(a));
    EXPECT_EQ(fa->cluster, a->name);
    EXPECT_EQ(fa->constr_z, 2);
    EXPECT_EQ(fa2->cluster, ClusterId());
    EXPECT_EQ(fa3->cluster, ClusterId());
    EXPECT_EQ(fb->cluster, a->name);
    EXPECT_EQ(fb->constr_z, 4);
    EXPECT_EQ(fc->cluster, c->name);
    EXPECT_EQ(fd->cluster, ClusterId());
    EXPECT_TRUE(is_alm_cluster_root(s));
    EXPECT_EQ(fs->cluster, s->name);
    EXPECT_EQ(fs->constr_z, 2);
    EXPECT_EQ(fx->cluster, ClusterId());
    EXPECT_TRUE(registers_share_a_lab({fa, fb}));
    EXPECT_FALSE(registers_share_a_lab({fc, fd}));

    auto &lab0 = ctx->labs.at(0);
    std::vector<std::pair<CellInfo *, BelId>> placement;
    ASSERT_TRUE(ctx->getClusterPlacement(a->cluster, lab0.alms[1].lut_bels[0], placement));
    ASSERT_EQ(placement.size(), 4u);
    EXPECT_EQ(placement[1].second, lab0.alms[1].lut_bels[1]); // b, the partner
    EXPECT_EQ(placement[2].second, lab0.alms[1].ff_bels[0]);  // fa, the root's half
    EXPECT_EQ(placement[3].second, lab0.alms[1].ff_bels[2]);  // fb, the partner's half
    EXPECT_FALSE(ctx->getClusterPlacement(a->cluster, lab0.alms[1].lut_bels[1], placement));
    // A single LUT seeds from either half and its register follows the half.
    ASSERT_TRUE(ctx->getClusterPlacement(s->cluster, lab0.alms[2].lut_bels[1], placement));
    ASSERT_EQ(placement.size(), 2u);
    EXPECT_EQ(placement[1].second, lab0.alms[2].ff_bels[2]);
    ASSERT_TRUE(ctx->getClusterPlacement(s->cluster, lab0.alms[2].lut_bels[0], placement));
    EXPECT_EQ(placement[1].second, lab0.alms[2].ff_bels[0]);

    // The checker accepts the packed ALMs, live and detached (HeAP places clusters through the
    // Stage 4B transaction, so the detached evaluator must agree with the live checker).
    ctx->placement_revision = PlacementRevisionState(); // earlier tests exhaust the fixture's session
    {
        std::vector<std::pair<CellInfo *, BelId>> pair_targets;
        ASSERT_TRUE(ctx->getClusterPlacement(a->cluster, lab0.alms[1].lut_bels[0], pair_targets));
        HeAPDisplacedBindings displaced; // every target bel is free
        for (auto &target : pair_targets)
            displaced[target.second] = {nullptr, STRENGTH_NONE};
        auto prepared = prepare_placement_transaction(*ctx, placement_edits_for_candidate(pair_targets, displaced));
        ASSERT_TRUE(prepared);
        auto frozen = freeze_placement_candidate(*ctx, prepared);
        ASSERT_EQ(frozen.status, FrozenPlacementStatus::Ready);
        auto assessment = evaluate_placement_candidate(frozen);
        EXPECT_EQ(assessment.status, FrozenPlacementStatus::Ready);
        EXPECT_TRUE(assessment.legal);
    }
    const std::vector<std::pair<BelId, CellInfo *>> binds = {
            {lab0.alms[1].lut_bels[0], a}, {lab0.alms[1].lut_bels[1], b}, {lab0.alms[1].ff_bels[0], fa},
            {lab0.alms[1].ff_bels[2], fb}, {lab0.alms[2].lut_bels[1], s}, {lab0.alms[2].ff_bels[2], fs}};
    for (auto &bind : binds)
        ctx->bindBel(bind.first, bind.second, STRENGTH_STRONG);
    for (auto &bind : binds)
        EXPECT_TRUE(ctx->isBelLocationValid(bind.first)) << ctx->nameOfBel(bind.first);
    EXPECT_TRUE(ctx->check_lab_input_count(0));
    for (auto &bind : binds)
        ctx->unbindBel(bind.first);
    // The fixture's context is shared: take the test's cells and nets out again.
    for (CellInfo *cell : {a, b, c, d, s, fa, fa2, fa3, fb, fc, fd, fs, fx}) {
        std::vector<IdString> connected;
        for (auto &port : cell->ports)
            if (port.second.net != nullptr)
                connected.push_back(port.first);
        for (IdString port : connected)
            cell->disconnectPort(port);
        ctx->cells.erase(cell->name);
    }
    for (NetInfo *n : nets)
        ctx->nets.erase(n->name);
}

TEST_F(LabControlCaptureTest, TelemetryFileCarriesThePhaseChecksumAndCounters)
{
    // `--telemetry`: nothing is written without a path; with one, the file parses, names the
    // phase and the checksum the flow captured, and carries every counter block the record reads.
    const auto path = std::filesystem::temp_directory_path() / "npnr_mistral_telemetry_test.json";
    std::filesystem::remove(path);
    ctx->args.telemetry_path.clear();
    write_mistral_telemetry(*ctx, "placed");
    EXPECT_FALSE(std::filesystem::exists(path));

    ctx->args.telemetry_path = path.string();
    ctx->telemetry_checksum = 0x12345678u;
    ctx->telemetry_placement_seconds = 1.5;
    write_mistral_telemetry(*ctx, "placed");
    std::ifstream in(path);
    ASSERT_TRUE(in.good());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    const json11::Json doc = json11::Json::parse(text, err);
    ASSERT_TRUE(err.empty()) << err;
    EXPECT_EQ(doc["phase"].string_value(), "placed");
    EXPECT_EQ(doc["checksum"].string_value(), "0x12345678");
    EXPECT_EQ(doc["device"].string_value(), ctx->args.device);
    EXPECT_EQ(doc["phases"]["placement_s"].number_value(), 1.5);
    EXPECT_EQ(doc["phases"]["routing_s"].number_value(), 0.0);
    for (const char *block : {"options", "legality", "controls", "resident", "phases"})
        EXPECT_TRUE(doc[block].is_object()) << block;
    EXPECT_EQ(doc["options"]["lab_legality"].string_value(), "legacy");
    EXPECT_EQ(doc["options"]["lab_controls"].string_value(), "legacy");
    EXPECT_TRUE(doc["legality"]["evaluations"].is_number());
    EXPECT_TRUE(doc["controls"]["fallbacks"].is_number());
    EXPECT_EQ(size_t(doc["cells"].number_value()), ctx->cells.size());
    EXPECT_EQ(size_t(doc["nets"].number_value()), ctx->nets.size());

    std::filesystem::remove(path);
    ctx->args.telemetry_path.clear();
    ctx->telemetry_checksum = 0;
    ctx->telemetry_placement_seconds = 0.0;
}

TEST_F(LabControlCaptureTest, RouteReuseKeepsOnlyRoutesTheCurrentDesignStillAllows)
{
    // A real pip path inside LAB 0 from an FF output to a LUT input, found by
    // a short search, so that the reuse checks see genuine endpoints.
    auto &lab0 = ctx->labs.at(0);
    std::vector<std::pair<BelId, IdString>> lut_inputs;
    for (BelId lut : {lab0.alms[0].lut_bels[0], lab0.alms[0].lut_bels[1], lab0.alms[1].lut_bels[0]})
        for (IdString pin : {id_A, id_B, id_C, id_D, id_E0, id_E1, id_F0, id_F1})
            if (ctx->getBelPinWire(lut, pin) != WireId())
                lut_inputs.emplace_back(lut, pin);
    BelId ff_bel = lab0.alms[2].ff_bels[0];
    const WireId source = ctx->getBelPinWire(ff_bel, id_Q);
    ASSERT_NE(source, WireId());
    std::vector<PipId> path;
    BelId sink_bel;
    IdString sink_pin;
    {
        dict<WireId, PipId> via;
        std::vector<WireId> frontier{source};
        via[source] = PipId();
        for (int depth = 0; depth < 5 && sink_bel == BelId(); ++depth) {
            std::vector<WireId> next;
            for (WireId w : frontier) {
                for (PipId pip : ctx->getPipsDownhill(w)) {
                    WireId dst = ctx->getPipDstWire(pip);
                    if (via.count(dst))
                        continue;
                    via[dst] = pip;
                    next.push_back(dst);
                    for (auto &li : lut_inputs)
                        if (ctx->getBelPinWire(li.first, li.second) == dst && sink_bel == BelId()) {
                            sink_bel = li.first;
                            sink_pin = li.second;
                        }
                    if (sink_bel != BelId())
                        break;
                }
                if (sink_bel != BelId())
                    break;
            }
            frontier = next;
        }
        ASSERT_NE(sink_bel, BelId()) << "no short path from an FF output to a LUT input in LAB 0";
        WireId cursor = ctx->getBelPinWire(sink_bel, sink_pin);
        while (cursor != source) {
            path.push_back(via.at(cursor));
            cursor = ctx->getPipSrcWire(via.at(cursor));
        }
    }
    const WireId sink = ctx->getBelPinWire(sink_bel, sink_pin);

    CellInfo *drv = ctx->createCell(ctx->id("rr_drv"), id_MISTRAL_FF);
    CellInfo *snk = ctx->createCell(ctx->id("rr_snk"), id_MISTRAL_ALUT2);
    NetInfo *net = ctx->createNet(ctx->id("rr_net"));
    drv->addOutput(id_Q);
    drv->pin_data[id_Q].bel_pins = {id_Q};
    snk->addInput(id_A);
    snk->pin_data[id_A].bel_pins = {sink_pin};
    drv->connectPort(id_Q, net);
    snk->connectPort(id_A, net);
    ctx->bindBel(ff_bel, drv, STRENGTH_STRONG);
    ctx->bindBel(sink_bel, snk, STRENGTH_STRONG);
    ASSERT_EQ(ctx->getNetinfoSourceWire(net), source);

    auto route_for = [&](const std::vector<PipId> &pips, WireId src) {
        // Newest first, as a wires map iterates: sink end first, source last.
        std::vector<PreviousRouteEntry> entries;
        for (PipId pip : pips)
            entries.push_back(
                    {ctx->getWireName(ctx->getPipDstWire(pip)).str(ctx.get()), ctx->getPipName(pip).str(ctx.get()), 1});
        entries.push_back({ctx->getWireName(src).str(ctx.get()), std::string(), 1});
        return entries;
    };
    auto unroute = [&]() {
        std::vector<std::pair<WireId, PipId>> bound;
        for (auto &w : net->wires)
            bound.emplace_back(w.first, w.second.pip);
        for (auto &b : bound)
            b.second == PipId() ? ctx->unbindWire(b.first) : ctx->unbindPip(b.second);
    };

    // A: the previous route still fits: reused, bound weak, source and sink bound to the net.
    PreviousRoutes previous;
    previous["rr_net"] = route_for(path, source);
    auto report = apply_route_reuse(*ctx, previous);
    EXPECT_EQ(report.reused, 1u);
    EXPECT_EQ(report.wires_bound, path.size() + 1);
    EXPECT_EQ(ctx->getBoundWireNet(source), net);
    EXPECT_EQ(ctx->getBoundWireNet(sink), net);
    EXPECT_EQ(net->wires.at(sink).strength, STRENGTH_STRONG);
    // A2: survival is what the router left of the applied route, wire for wire.
    measure_route_survival(*ctx, report, nullptr);
    EXPECT_EQ(report.survived, 1u);
    EXPECT_EQ(report.rerouted, 0u);
    ctx->unbindPip(path.front());
    measure_route_survival(*ctx, report, nullptr);
    EXPECT_EQ(report.survived, 0u);
    EXPECT_EQ(report.rerouted, 1u);
    ctx->bindPip(path.front(), net, STRENGTH_STRONG);
    {
        ReusePlan stamped;
        stamped.nets.push_back({"rr_net", ReuseDecision::Reuse, path.size() + 1, "test"});
        measure_route_survival(*ctx, report, &stamped);
        EXPECT_EQ(stamped.nets.front().survived, 1);
    }
    // E: a net that already has wires is left alone.
    report = apply_route_reuse(*ctx, previous);
    EXPECT_EQ(report.already_routed, 1u);
    EXPECT_EQ(report.reused, 0u);
    // G: the fallback drops what was preserved.
    RouteReuseReport preserved;
    preserved.reused_names.push_back("rr_net");
    drop_reused_routes(*ctx, preserved);
    EXPECT_EQ(preserved.dropped, 1u);
    EXPECT_TRUE(net->wires.empty());
    EXPECT_EQ(ctx->getBoundWireNet(source), nullptr);

    // B: a route from another source is an endpoint mismatch.
    previous["rr_net"] = route_for(path, ctx->getBelPinWire(lab0.alms[3].ff_bels[0], id_Q));
    report = apply_route_reuse(*ctx, previous);
    EXPECT_EQ(report.endpoint_mismatch, 1u);
    EXPECT_TRUE(net->wires.empty());

    // C: a leaf that drives no current sink is an endpoint mismatch.
    {
        auto extra = route_for(path, source);
        PipId stray;
        std::vector<WireId> route_wires{source};
        for (PipId pip : path)
            route_wires.push_back(ctx->getPipDstWire(pip));
        for (WireId w : route_wires) {
            for (PipId pip : ctx->getPipsDownhill(w))
                if (std::find(path.begin(), path.end(), pip) == path.end() &&
                    std::find(route_wires.begin(), route_wires.end(), ctx->getPipDstWire(pip)) == route_wires.end()) {
                    stray = pip;
                    break;
                }
            if (stray != PipId())
                break;
        }
        ASSERT_NE(stray, PipId());
        extra.insert(extra.begin(), {ctx->getWireName(ctx->getPipDstWire(stray)).str(ctx.get()),
                                     ctx->getPipName(stray).str(ctx.get()), 1});
        previous["rr_net"] = extra;
        report = apply_route_reuse(*ctx, previous);
        EXPECT_EQ(report.endpoint_mismatch, 1u);
        EXPECT_TRUE(net->wires.empty());
    }

    // D: a pip the current flags forbid makes the route unavailable.
    {
        previous["rr_net"] = route_for(path, source);
        const uint64_t flags_before = ctx->wires.at(sink).flags;
        ctx->wires.at(sink).flags = WireInfo::BLOCKED;
        report = apply_route_reuse(*ctx, previous);
        EXPECT_EQ(report.unavailable, 1u);
        EXPECT_TRUE(net->wires.empty());
        ctx->wires.at(sink).flags = flags_before;
    }

    // F: a wire taken by another net makes the route unavailable; no previous
    // route at all is counted separately.
    {
        NetInfo *other = ctx->createNet(ctx->id("rr_other"));
        ctx->bindWire(sink, other, STRENGTH_LOCKED);
        previous["rr_net"] = route_for(path, source);
        report = apply_route_reuse(*ctx, previous);
        EXPECT_EQ(report.unavailable, 1u);
        ctx->unbindWire(sink);
        ctx->nets.erase(other->name);
        previous.clear();
        report = apply_route_reuse(*ctx, previous);
        EXPECT_EQ(report.no_previous, 1u);
    }

    // A again through the same checks, then clean up.
    previous["rr_net"] = route_for(path, source);
    report = apply_route_reuse(*ctx, previous);
    EXPECT_EQ(report.reused, 1u);
    unroute();
    ctx->unbindBel(ff_bel);
    ctx->unbindBel(sink_bel);
    drv->disconnectPort(id_Q);
    snk->disconnectPort(id_A);
    ctx->nets.erase(net->name);
    ctx->cells.erase(drv->name);
    ctx->cells.erase(snk->name);
}

TEST_F(LabControlCaptureTest, BuildStatesAreTypedAndPhaseCheckedAtTheBoundary)
{
    // Transitions exist only between adjacent phases and consume their input.
    static_assert(std::is_invocable_v<decltype(place_build), Build<BuildPhase::Packed>>, "packed -> placed");
    static_assert(!std::is_invocable_v<decltype(place_build), Build<BuildPhase::Placed>>, "no placing twice");
    static_assert(!std::is_invocable_v<decltype(route_build), Build<BuildPhase::Packed>>, "no routing a packed build");
    static_assert(!std::is_invocable_v<decltype(route_build), Build<BuildPhase::Placed>>, "prepare comes first");
    static_assert(std::is_invocable_v<decltype(route_build), Build<BuildPhase::RoutePrepared>>, "prepared -> routed");
    static_assert(!std::is_invocable_v<decltype(validate_build), Build<BuildPhase::RoutePrepared>>,
                  "only a routed build validates");
    static_assert(!std::is_copy_constructible_v<Build<BuildPhase::Routed>>, "handles are move-only");
    static_assert(!std::is_invocable_v<decltype(place_build), Build<BuildPhase::Packed> &>,
                  "a transition consumes its handle");

    // At the legacy boundary the phase is checked at run time.
    const BuildPhase before = ctx->build_phase;
    ctx->build_phase = BuildPhase::Loaded;
    EXPECT_THROW(Build<BuildPhase::Packed>::adopt(*ctx), log_execution_error_exception);
    ctx->build_phase = BuildPhase::Packed;
    EXPECT_NO_THROW(Build<BuildPhase::Packed>::adopt(*ctx));
    EXPECT_THROW(Build<BuildPhase::Placed>::adopt(*ctx), log_execution_error_exception);
    // Validation refuses a design with an unrouted arc.
    CellInfo *drv = ctx->createCell(ctx->id("bs_drv"), id_MISTRAL_FF);
    CellInfo *snk = ctx->createCell(ctx->id("bs_snk"), id_MISTRAL_FF);
    NetInfo *net = ctx->createNet(ctx->id("bs_net"));
    drv->addOutput(id_Q);
    snk->addInput(id_DATAIN);
    drv->connectPort(id_Q, net);
    snk->connectPort(id_DATAIN, net);
    ctx->build_phase = BuildPhase::Routed;
    EXPECT_THROW(validate_build(Build<BuildPhase::Routed>::adopt(*ctx)), log_execution_error_exception);
    EXPECT_EQ(ctx->build_phase, BuildPhase::Routed); // a failed transition leaves no new phase behind
    drv->disconnectPort(id_Q);
    snk->disconnectPort(id_DATAIN);
    ctx->nets.erase(net->name);
    ctx->cells.erase(drv->name);
    ctx->cells.erase(snk->name);
    ctx->build_phase = before;
}
} // namespace
