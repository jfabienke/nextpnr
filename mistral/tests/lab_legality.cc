/* SPDX-License-Identifier: ISC */
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <type_traits>
#include "gtest/gtest.h"
#include "lab_control_edits.h"
#include "lab_control_plan.h"
#include "lab_preparation.h"
#include "lab_replay.h"
#include "lab_reuse.h"
#include "lab_snapshot.h"
#include "lab_v2.h"
#include "lab_v2_replay.h"
#include "log.h"
#include "nextpnr.h"
#include "placement_coordinator.h"
#include "placement_reuse.h"
#include "placement_transaction.h"
#include "placer_heap.h"

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
        EXPECT_EQ(s.rust_batches, 1u);
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

TEST_F(LabControlCaptureTest, BatchCoordinatorSpansSeveralFrozenHandlesAndKeepsOrder)
{
    const auto &alms = ctx->labs.at(0).alms;
    const BelId odd = alms.at(0).ff_bels.at(1);
    const BelId even = alms.at(1).ff_bels.at(0);
    // 100 single-query candidates need two 64-record Rust handles; only the last is legal.
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
        EXPECT_EQ(s.rust_batches, 2u);
        EXPECT_EQ(s.rust_direct, 0u);
#endif
        ctx->unbindBel(even);
    }
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
    ctx->unbindBel(other_lab);

    // Every audited fact path moves the global epoch: kernel notifications and Mistral's rewrites.
    cells[2]->setAttr(ctx->id("lab_reuse_probe"), Property(1));
    EXPECT_EQ(ctx->lab_facts_epoch, epoch + 1);
    cells[2]->unsetAttr(ctx->id("lab_reuse_probe"));
    EXPECT_EQ(ctx->lab_facts_epoch, epoch + 2);
    cells[3]->addInput(id_CLK);
    cells[3]->connectPort(id_CLK, nets[0]);
    const uint64_t after_connect = ctx->lab_facts_epoch;
    EXPECT_GT(after_connect, epoch + 2);
    ctx->assign_ff_info(cells[3]);
    EXPECT_EQ(ctx->lab_facts_epoch, after_connect + 1);
    cells[3]->disconnectPort(id_CLK);
    EXPECT_GT(ctx->lab_facts_epoch, after_connect + 1);
    EXPECT_EQ(ctx->lab_reuse_stats.lab_invalidations, 4u);
    EXPECT_GE(ctx->lab_reuse_stats.facts_invalidations, 5u);
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

    // A fact change invalidates every LAB even though no BEL moved.
    cells[5]->setAttr(ctx->id("lab_reuse_probe"), Property(1));
    EXPECT_TRUE(ctx->isBelLocationValid(even_a));
    EXPECT_EQ(s.stale_facts, 1u);
    EXPECT_TRUE(ctx->isBelLocationValid(other_lab));
    EXPECT_EQ(s.stale_facts, 2u);
    cells[5]->unsetAttr(ctx->id("lab_reuse_probe"));

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

    auto &cached = ctx->labs[0].alms[0].unique_input_count;
    cached = 7;
    ctx->args.lab_legality = LabLegalityMode::Shadow;
    EXPECT_TRUE(dispatch_lab_legality(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX));
    for (auto mode : {LabLegalityMode::Verify, LabLegalityMode::Rust}) {
        ctx->args.lab_legality = mode;
        EXPECT_THROW(dispatch_lab_legality(*ctx, 0, NPNR_LAB_QUERY_WHOLE_LAB, UINT32_MAX),
                     log_execution_error_exception);
    }
    cached = 0;
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
} // namespace
