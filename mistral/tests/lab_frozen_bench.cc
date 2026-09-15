/* SPDX-License-Identifier: ISC */
// Warm frozen-V2 throughput benchmark. Thread creation, handle creation and
// output allocation are outside the parallel evaluation timed region.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lab_frozen_batch.h"
#include "lab_v2.h"

USING_NEXTPNR_NAMESPACE

namespace {
constexpr uint32_t RECORDS = NPNR_LAB_MAX_BATCH;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template <typename T> inline void consume(const T &value) { asm volatile("" : : "g"(&value) : "memory"); }

std::vector<NpnrLabFactsV2> make_inputs()
{
    std::vector<NpnrLabFactsV2> inputs(RECORDS);
    for (uint32_t i = 0; i < RECORDS; ++i) {
        auto &input = inputs[i];
        input.abi_version = NPNR_LAB_ABI_V2;
        input.struct_size = sizeof(input);
        input.request_id = i + 1;
        input.snapshot_epoch = 1;
        input.query = NPNR_LAB_QUERY_WHOLE_LAB;
        input.query_alm = UINT32_MAX;
        input.input_limit = 42;
        input.net_count = 8;
        for (unsigned alm = 0; alm < NPNR_LAB_V2_ALMS; ++alm) {
            auto &lut = input.alm[alm].lut[0];
            lut.occupied = 1;
            lut.input_count = 4;
            lut.used_input_count = 4;
            lut.bits_count = 16;
            lut.chain_shared_input_count = 0;
            lut.mlab_group = -1;
            for (unsigned pin = 0; pin < 4; ++pin)
                lut.input_net[pin] = 1 + ((i + alm + pin) % 8);
            lut.comb_out_net = 1 + ((i + alm + 4) % 8);
        }
    }
    return inputs;
}

uint64_t frozen_parallel(const RustFrozenLabBatchV2 &batch,
                         const std::array<NpnrLabAssessmentV2, RECORDS> &expected, unsigned workers,
                         unsigned repeats)
{
    require(workers > 0 && workers <= RECORDS, "worker count must fit batch size");
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    std::vector<std::vector<NpnrLabAssessmentV2>> outputs(workers);
    std::vector<uint32_t> statuses(workers, NPNR_LAB_CALL_PANIC);
    for (unsigned worker = 0; worker < workers; ++worker) {
        const uint32_t begin = uint64_t(RECORDS) * worker / workers;
        const uint32_t end = uint64_t(RECORDS) * (worker + 1) / workers;
        const uint32_t count = end - begin;
        outputs[worker].resize(count);
        threads.emplace_back([&, worker] {
            const uint32_t begin = uint64_t(RECORDS) * worker / workers;
            const uint32_t end = uint64_t(RECORDS) * (worker + 1) / workers;
            const uint32_t count = end - begin;
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (unsigned iteration = 0; iteration < repeats; ++iteration) {
                statuses[worker] = batch.evaluate(begin, count, outputs[worker].data(), count);
                consume(outputs[worker]);
                if (statuses[worker] != NPNR_LAB_CALL_OK)
                    return;
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != workers)
        std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto &thread : threads)
        thread.join();
    const auto finish = std::chrono::steady_clock::now();
    for (auto status : statuses)
        require(status == NPNR_LAB_CALL_OK, "frozen evaluation failed");
    for (unsigned worker = 0; worker < workers; ++worker) {
        const uint32_t begin = uint64_t(RECORDS) * worker / workers;
        for (uint32_t index = 0; index < outputs[worker].size(); ++index)
            require(lab_v2_results_match(expected[begin + index], outputs[worker][index]),
                    "parallel frozen result mismatch");
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
}
} // namespace

int main(int argc, char **argv)
try {
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: lab-frozen-bench OUTPUT.csv [rounds=7] [repeats=20000]\n";
        return 2;
    }
    const unsigned rounds = argc > 2 ? std::stoul(argv[2]) : 7;
    const unsigned repeats = argc > 3 ? std::stoul(argv[3]) : 20000;
    require(rounds > 0 && repeats > 0, "rounds/repeats must be positive");
    auto inputs = make_inputs();
    std::array<NpnrLabAssessmentV2, RECORDS> direct_outputs;
    require(npnr_mistral_eval_lab_v2(inputs.data(), RECORDS, direct_outputs.data(), RECORDS) == NPNR_LAB_CALL_OK,
            "direct V2 validation failed");
    uint32_t status = NPNR_LAB_CALL_PANIC;
    auto batch = RustFrozenLabBatchV2::create(inputs, 0, status);
    require(status == NPNR_LAB_CALL_OK && batch, "frozen batch creation failed");
    std::array<NpnrLabAssessmentV2, RECORDS> frozen_outputs;
    require(batch.evaluate(0, RECORDS, frozen_outputs.data(), RECORDS) == NPNR_LAB_CALL_OK,
            "frozen V2 validation failed");
    for (uint32_t i = 0; i < RECORDS; ++i)
        require(lab_v2_results_match(direct_outputs[i], frozen_outputs[i]), "direct/frozen result mismatch");

    std::ofstream output(argv[1]);
    require(bool(output), "cannot write CSV");
    output << "round,phase,workers,records,calls,nanoseconds,ns_per_record\n";
    for (unsigned round = 0; round < rounds; ++round) {
        auto direct = [&] {
            const auto start = std::chrono::steady_clock::now();
            for (unsigned iteration = 0; iteration < repeats; ++iteration) {
                require(npnr_mistral_eval_lab_v2(inputs.data(), RECORDS, direct_outputs.data(), RECORDS) ==
                                NPNR_LAB_CALL_OK,
                        "direct evaluation failed");
                consume(direct_outputs);
            }
            const auto finish = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
        };
        const uint64_t records = uint64_t(RECORDS) * repeats;
        const auto direct_ns = direct();
        output << round << ",direct_v2,1," << records << ',' << repeats << ',' << direct_ns << ','
               << double(direct_ns) / records << '\n';
        for (unsigned workers : {1, 2, 4, 8, 12, 16}) {
            const auto elapsed = frozen_parallel(batch, direct_outputs, workers, repeats);
            output << round << ",frozen_v2," << workers << ',' << records << ',' << uint64_t(repeats) * workers << ','
                   << elapsed << ',' << double(elapsed) / records << '\n';
        }

        const unsigned creation_repeats = std::max(1u, repeats / 20);
        const auto create_start = std::chrono::steady_clock::now();
        for (unsigned iteration = 0; iteration < creation_repeats; ++iteration) {
            uint32_t create_status = NPNR_LAB_CALL_PANIC;
            auto temporary = RustFrozenLabBatchV2::create(inputs, 1, create_status);
            require(create_status == NPNR_LAB_CALL_OK && temporary, "timed batch creation failed");
            consume(temporary);
        }
        const auto create_finish = std::chrono::steady_clock::now();
        const auto create_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(create_finish - create_start).count();
        output << round << ",create_destroy,1," << uint64_t(RECORDS) * creation_repeats << ',' << creation_repeats
               << ',' << create_ns << ',' << double(create_ns) / (uint64_t(RECORDS) * creation_repeats) << '\n';
        output.flush();
        std::cerr << "completed frozen benchmark round " << round + 1 << '/' << rounds << '\n';
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << "benchmark failed: " << e.what() << '\n';
    return 1;
}
