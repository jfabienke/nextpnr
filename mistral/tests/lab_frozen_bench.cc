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

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

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

bool request_performance_qos(unsigned worker)
{
#ifdef __APPLE__
    (void)worker;
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#else
    (void)worker;
    return false;
#endif
}

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

struct ParallelResult
{
    uint64_t elapsed_ns = 0;
    uint64_t fastest_worker_ns = 0;        // time until the first worker finished its work
    uint64_t slowest_worker_ns = 0;        // time until the last worker finished (== elapsed minus join skew)
    uint64_t units_min = 0, units_max = 0; // dynamic mode: fewest/most work units claimed by one worker
};

// `dynamic`: instead of a fixed contiguous range per worker, the record range is
// cut into DYNAMIC_CHUNK-record units and every worker claims the next unit from
// a shared counter until the whole repeats x records workload is consumed. A slow
// core then does less work instead of bounding the batch.
uint32_t DYNAMIC_CHUNK = 4; // records per claimed unit; set from the scheduling argument (dynamic:N)

ParallelResult frozen_parallel(const RustFrozenLabBatchV2 &batch,
                               const std::array<NpnrLabAssessmentV2, RECORDS> &expected, unsigned workers,
                               unsigned repeats, bool performance_qos, bool dynamic)
{
    require(workers > 0 && workers <= RECORDS, "worker count must fit batch size");
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> policy_failed{false};
    std::atomic<uint64_t> next_unit{0};
    const uint64_t units_per_pass = (RECORDS + DYNAMIC_CHUNK - 1) / DYNAMIC_CHUNK;
    const uint64_t total_units = units_per_pass * repeats;
    std::vector<std::thread> threads;
    std::vector<std::vector<NpnrLabAssessmentV2>> outputs(workers);
    std::vector<uint32_t> statuses(workers, NPNR_LAB_CALL_PANIC);
    std::vector<uint64_t> finished_ns(workers, 0), units(workers, 0);
    std::chrono::steady_clock::time_point start;
    for (unsigned worker = 0; worker < workers; ++worker) {
        const uint32_t begin = uint64_t(RECORDS) * worker / workers;
        const uint32_t end = uint64_t(RECORDS) * (worker + 1) / workers;
        outputs[worker].resize(dynamic ? DYNAMIC_CHUNK : end - begin);
        threads.emplace_back([&, worker] {
            const uint32_t begin = uint64_t(RECORDS) * worker / workers;
            const uint32_t end = uint64_t(RECORDS) * (worker + 1) / workers;
            const uint32_t count = end - begin;
            if (performance_qos && !request_performance_qos(worker))
                policy_failed.store(true, std::memory_order_relaxed);
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            statuses[worker] = NPNR_LAB_CALL_OK;
            if (dynamic) {
                for (uint64_t unit = next_unit.fetch_add(1, std::memory_order_relaxed); unit < total_units;
                     unit = next_unit.fetch_add(1, std::memory_order_relaxed)) {
                    const uint32_t offset = uint32_t((unit % units_per_pass) * DYNAMIC_CHUNK);
                    const uint32_t n = std::min<uint32_t>(DYNAMIC_CHUNK, RECORDS - offset);
                    statuses[worker] = batch.evaluate(offset, n, outputs[worker].data(), n);
                    consume(outputs[worker]);
                    ++units[worker];
                    if (statuses[worker] != NPNR_LAB_CALL_OK)
                        break;
                    // Validate only the first pass over the records so that checking
                    // costs the same fixed amount as the static mode's post-timing check.
                    if (unit < units_per_pass)
                        for (uint32_t i = 0; i < n; ++i)
                            if (!lab_v2_results_match(expected[offset + i], outputs[worker][i]))
                                statuses[worker] = NPNR_LAB_CALL_PANIC;
                }
            } else {
                for (unsigned iteration = 0; iteration < repeats; ++iteration) {
                    statuses[worker] = batch.evaluate(begin, count, outputs[worker].data(), count);
                    consume(outputs[worker]);
                    if (statuses[worker] != NPNR_LAB_CALL_OK)
                        break;
                }
                units[worker] = repeats;
            }
            finished_ns[worker] = uint64_t(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                            .count());
        });
    }
    while (ready.load(std::memory_order_acquire) != workers)
        std::this_thread::yield();
    start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto &thread : threads)
        thread.join();
    const auto finish = std::chrono::steady_clock::now();
    require(!policy_failed.load(std::memory_order_relaxed), "performance scheduling request failed");
    for (auto status : statuses)
        require(status == NPNR_LAB_CALL_OK, "frozen evaluation failed or mismatched");
    if (!dynamic) {
        for (unsigned worker = 0; worker < workers; ++worker) {
            const uint32_t begin = uint64_t(RECORDS) * worker / workers;
            for (uint32_t index = 0; index < outputs[worker].size(); ++index)
                require(lab_v2_results_match(expected[begin + index], outputs[worker][index]),
                        "parallel frozen result mismatch");
        }
    }
    ParallelResult result;
    result.elapsed_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    result.fastest_worker_ns = *std::min_element(finished_ns.begin(), finished_ns.end());
    result.slowest_worker_ns = *std::max_element(finished_ns.begin(), finished_ns.end());
    result.units_min = *std::min_element(units.begin(), units.end());
    result.units_max = *std::max_element(units.begin(), units.end());
    return result;
}
} // namespace

int main(int argc, char **argv)
try {
    if (argc < 2 || argc > 5) {
        std::cerr << "usage: lab-frozen-bench OUTPUT.csv [rounds=7] [repeats=20000] "
                     "[scheduling=default|performance-qos|dynamic[:chunk]]\n";
        return 2;
    }
    const unsigned rounds = argc > 2 ? std::stoul(argv[2]) : 7;
    const unsigned repeats = argc > 3 ? std::stoul(argv[3]) : 20000;
    const std::string scheduling = argc > 4 ? argv[4] : "default";
    const bool performance_qos = scheduling == "performance-qos";
    const bool dynamic = scheduling.rfind("dynamic", 0) == 0;
    require(scheduling == "default" || performance_qos || dynamic, "unknown scheduling mode");
    if (dynamic && scheduling.size() > 8 && scheduling[7] == ':')
        DYNAMIC_CHUNK = std::stoul(scheduling.substr(8));
    require(DYNAMIC_CHUNK > 0 && DYNAMIC_CHUNK <= RECORDS, "dynamic chunk must be 1..64");
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
    output << "round,phase,scheduling,workers,records,calls,nanoseconds,ns_per_record,fastest_worker_ns,"
              "slowest_worker_ns,units_min,units_max\n";
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
        output << round << ",direct_v2," << scheduling << ",1," << records << ',' << repeats << ',' << direct_ns << ','
               << double(direct_ns) / records << ",,,,\n";
        for (unsigned workers : {1, 2, 4, 8, 12, 16, 20}) {
            const auto r = frozen_parallel(batch, direct_outputs, workers, repeats, performance_qos, dynamic);
            output << round << ",frozen_v2," << scheduling << ',' << workers << ',' << records << ','
                   << uint64_t(repeats) * workers << ',' << r.elapsed_ns << ',' << double(r.elapsed_ns) / records << ','
                   << r.fastest_worker_ns << ',' << r.slowest_worker_ns << ',' << r.units_min << ',' << r.units_max
                   << '\n';
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
        output << round << ",create_destroy," << scheduling << ",1," << uint64_t(RECORDS) * creation_repeats << ','
               << creation_repeats << ',' << create_ns << ','
               << double(create_ns) / (uint64_t(RECORDS) * creation_repeats) << ",,,,\n";
        output.flush();
        std::cerr << "completed frozen benchmark round " << round + 1 << '/' << rounds << '\n';
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << "benchmark failed: " << e.what() << '\n';
    return 1;
}
