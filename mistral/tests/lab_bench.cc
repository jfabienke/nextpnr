/* SPDX-License-Identifier: ISC */
// Warm live-corpus microbenchmark. JSON parsing, binding and correctness checks
// are outside the timed/allocation-counted regions. No production allocator changes.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <new>
#include "lab_control_edits.h"
#include "lab_preparation.h"
#include "lab_replay.h"
#include "lab_snapshot.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

namespace {
thread_local bool counting = false;
thread_local uint64_t allocations = 0, allocated_bytes = 0;
void *allocate(size_t size, size_t alignment = 0)
{
    void *ptr = nullptr;
    if (alignment) {
        if (posix_memalign(&ptr, alignment, size ? size : 1))
            throw std::bad_alloc();
    } else {
        ptr = std::malloc(size ? size : 1);
        if (!ptr)
            throw std::bad_alloc();
    }
    if (counting) {
        ++allocations;
        allocated_bytes += size;
    }
    return ptr;
}
// Opaque compiler barrier consumes whole objects, and prevents invariant inputs
// being hoisted from the repeated-call loops, including under ThinLTO.
template <typename T> inline void consume(const T &value) { asm volatile("" : : "g"(&value) : "memory"); }
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
struct Record
{
    NpnrLabControlsV1 input;
    NpnrLabControlResultV1 expected;
};
struct Measurement
{
    uint64_t ns = 0, calls = 0, news = 0, bytes = 0, cases = 0;
};
} // namespace

void *operator new(size_t n) { return allocate(n); }
void *operator new[](size_t n) { return allocate(n); }
void *operator new(size_t n, std::align_val_t a) { return allocate(n, size_t(a)); }
void *operator new[](size_t n, std::align_val_t a) { return allocate(n, size_t(a)); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t, std::align_val_t) noexcept { std::free(p); }

int main(int argc, char **argv)
try {
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: lab-bench CORPUS.jsonl OUTPUT.csv [rounds=7] [repeats=128]\n";
        return 2;
    }
    const unsigned rounds = argc > 3 ? std::stoul(argv[3]) : 7;
    const unsigned repeats = argc > 4 ? std::stoul(argv[4]) : 128;
    require(rounds > 0 && repeats > 0, "rounds/repeats must be positive");
    std::ifstream stream(argv[1]);
    require(bool(stream), "cannot read corpus");
    std::vector<Record> records;
    std::string line, error;
    while (std::getline(stream, line)) {
        Record record;
        if (!read_lab_control_replay(line, record.input, record.expected, error))
            throw std::runtime_error(error);
        records.push_back(record);
    }
    require(!records.empty(), "empty corpus");
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    auto ctx = std::make_unique<Context>(args);
    std::array<CellInfo *, 40> cells;
    std::array<NetInfo *, 200> nets;
    for (unsigned i = 0; i < nets.size(); ++i)
        nets[i] = ctx->createNet(ctx->idf("lab_bench_net_%u", i));
    for (unsigned i = 0; i < cells.size(); ++i) {
        cells[i] = ctx->createCell(ctx->idf("lab_bench_ff_%u", i), id_MISTRAL_FF);
        cells[i]->ffInfo.datain = nullptr;
        cells[i]->ffInfo.sdata = nullptr;
    }
    const std::array<const char *, 12> phases{"harness",
                                              "legacy_verdict",
                                              "legacy_export",
                                              "capture",
                                              "cpp_detached",
                                              "rust_ffi",
                                              "host_validation",
                                              "rust_dispatch",
                                              "preparation_dispatch",
                                              "ticket_translation",
                                              "edit_preflight",
                                              "preparation_complete"};
    std::ofstream output(argv[2]);
    require(bool(output), "cannot write CSV");
    output << "round,phase,reason,cases,calls,nanoseconds,cpp_new_calls,cpp_new_bytes\n";
    for (unsigned round = 0; round < rounds; ++round) {
        std::array<std::array<Measurement, 7>, 12> measurements{};
        for (size_t case_index = 0; case_index < records.size(); ++case_index) {
            const auto &record = records[case_index];
            for (auto *cell : cells)
                if (cell->bel != BelId())
                    ctx->unbindBel(cell->bel);
            for (auto *net : nets)
                net->is_global = false;
            for (unsigned slot = 0; slot < cells.size(); ++slot) {
                if (!record.input.ff[slot].occupied)
                    continue;
                auto &cs = cells[slot]->ffInfo.ctrlset;
                std::array<ControlSig *, 5> controls{&cs.clk, &cs.sload, &cs.sclr, &cs.aclr, &cs.ena};
                for (unsigned kind = 0; kind < controls.size(); ++kind) {
                    const auto &signal = record.input.ff[slot].control[kind];
                    auto *net = signal.net_id ? nets.at(signal.net_id - 1) : nullptr;
                    if (net)
                        net->is_global = signal.flags & NPNR_CONTROL_GLOBAL;
                    *controls[kind] = {net, bool(signal.flags & NPNR_CONTROL_INVERTED)};
                }
                ctx->bindBel(ctx->labs.at(0).alms.at(slot / 4).ff_bels.at(slot % 4), cells[slot], STRENGTH_WEAK);
            }
            const auto capture = capture_lab_controls(*ctx, 0, record.input.request_id, record.input.snapshot_epoch);
            require(std::memcmp(&capture.input, &record.input, sizeof(record.input)) == 0, "capture replay differs");
            const auto legacy = evaluate_lab_controls_legacy(*ctx, 0, capture);
            const auto detached = evaluate_lab_controls_cpp(capture.input);
            NpnrLabControlResultV1 rust;
            require(npnr_mistral_eval_controls_v1(&capture.input, 1, &rust, 1) == NPNR_LAB_CALL_OK, "FFI call failed");
            require(lab_control_results_match(legacy, record.expected), "recorded legacy result differs");
            require(lab_control_results_match(legacy, detached), "C++ detached result differs");
            require(lab_control_first_difference(detached, rust).empty(), "Rust result differs");
            require(lab_control_result_valid(capture.input, rust), "host rejected result");
            require(detached.reason < 7, "unexpected corpus reason");
            std::optional<ValidatedControlPlan> validated_plan;
            if (rust.status == NPNR_CONTROL_LEGAL) {
                PreparationTicket ticket(capture, 0, capture.input.request_id, rust);
                auto translation = translate_preparation_ticket(std::move(ticket));
                require(translation.status == PreparationStatus::Legal && translation.plan,
                        "ticket translation failed");
                validated_plan = std::move(translation.plan);
            }
            for (unsigned step = 0; step < phases.size(); ++step) {
                const auto phase = (step + case_index + round) % phases.size();
                if (phase >= 9 && !validated_plan)
                    continue;
                ctx->args.lab_controls =
                        (phase == 7 || phase == 8 || phase == 11) ? LabControlMode::Rust : LabControlMode::Legacy;
                auto &m = measurements[phase][detached.reason];
                allocations = allocated_bytes = 0;
                auto measure = [&](auto operation) {
                    operation(); // prime code/data before timing this phase
                    counting = true;
                    const auto start = std::chrono::steady_clock::now();
                    for (unsigned i = 0; i < repeats; ++i) {
                        consume(capture);
                        operation();
                    }
                    const auto finish = std::chrono::steady_clock::now();
                    counting = false;
                    m.ns += std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
                };
                switch (phase) {
                case 0:
                    measure([&] { consume(capture); });
                    break;
                case 1:
                    measure([&] { consume(ctx->is_lab_ctrlset_legal(0)); });
                    break;
                case 2:
                    measure([&] { consume(evaluate_lab_controls_legacy(*ctx, 0, capture)); });
                    break;
                case 3:
                    measure([&] { consume(capture_lab_controls(*ctx, 0, capture.input.request_id)); });
                    break;
                case 4:
                    measure([&] { consume(evaluate_lab_controls_cpp(capture.input)); });
                    break;
                case 5:
                    measure([&] {
                        NpnrLabControlResultV1 result;
                        consume(npnr_mistral_eval_controls_v1(&capture.input, 1, &result, 1));
                        consume(result);
                    });
                    break;
                case 6:
                    measure([&] { consume(lab_control_result_valid(capture.input, rust)); });
                    break;
                case 7:
                    measure([&] { consume(ctx->is_lab_ctrlset_legal(0)); });
                    break;
                case 8:
                    measure([&] { consume(dispatch_lab_controls_for_preparation(*ctx, 0)); });
                    break;
                case 9:
                    measure([&] {
                        PreparationTicket ticket(capture, 0, capture.input.request_id, rust);
                        consume(translate_preparation_ticket(std::move(ticket)));
                    });
                    break;
                case 10:
                    measure([&] { consume(prepare_control_edits(*ctx, *validated_plan)); });
                    break;
                case 11:
                    measure([&] {
                        auto dispatch = dispatch_lab_controls_for_preparation(*ctx, 0);
                        require(dispatch.selected_status == PreparationStatus::Legal && dispatch.ticket,
                                "preparation dispatch became illegal");
                        auto translation = translate_preparation_ticket(std::move(*dispatch.ticket));
                        require(translation.status == PreparationStatus::Legal && translation.plan,
                                "preparation translation failed");
                        auto prepared = prepare_control_edits(*ctx, *translation.plan);
                        require(prepared.status == ControlEditStatus::Ready && prepared.prepared,
                                "preparation preflight failed");
                        if (translation.comparison_plan) {
                            auto comparison = prepare_control_edits(*ctx, *translation.comparison_plan);
                            require(comparison.status == ControlEditStatus::Ready && comparison.prepared,
                                    "comparison preflight failed");
                            require(control_edit_lists_equal(*prepared.prepared, *comparison.prepared),
                                    "preparation edit lists differ");
                        }
                        require(apply_prepared_control_edits(*ctx, std::move(*prepared.prepared)) ==
                                        ControlEditStatus::Applied,
                                "preparation application failed");
                    });
                    break;
                }
                m.calls += repeats;
                ++m.cases;
                m.news += allocations;
                m.bytes += allocated_bytes;
            }
        }
        for (unsigned phase = 0; phase < phases.size(); ++phase)
            for (unsigned reason = 0; reason < 7; ++reason) {
                const auto &m = measurements[phase][reason];
                if (m.calls)
                    output << round << ',' << phases[phase] << ',' << reason << ',' << m.cases << ',' << m.calls << ','
                           << m.ns << ',' << m.news << ',' << m.bytes << '\n';
            }
        output.flush();
        std::cerr << "completed microbenchmark round " << round + 1 << '/' << rounds << '\n';
    }
    std::cerr << "object bytes: legacy_worker=" << lab_control_legacy_worker_size()
              << " capture=" << sizeof(LabControlCapture) << " input=" << sizeof(NpnrLabControlsV1)
              << " result=" << sizeof(NpnrLabControlResultV1) << '\n';
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
