/* SPDX-License-Identifier: ISC */
#include "placement_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

NEXTPNR_NAMESPACE_BEGIN

namespace {
inline void cpu_relax()
{
#if defined(__aarch64__)
    asm volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
} // namespace

// A persistent pool of (workers - 1) threads plus the calling owner thread.
// Each job is a range of indices claimed dynamically; results are addressed by
// index, so claim order never influences what the owner sees. Workers spin for
// a bounded time before blocking, because a batch of eight candidates holds
// tens of microseconds of work, comparable to a condition-variable wake.
struct PlacementWorkerPool::Impl
{
    using Job = std::function<void(unsigned worker, size_t index)>;
    static constexpr auto SPIN_LIMIT = std::chrono::microseconds(50);

    explicit Impl(unsigned threads) : failures(threads + 1)
    {
        for (unsigned i = 0; i < threads; ++i)
            workers.emplace_back([this, i] { worker_loop(i + 1); });
    }
    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop.store(true, std::memory_order_release);
        }
        wake.notify_all();
        for (auto &thread : workers)
            thread.join();
    }

    // Runs `fn` over [0, count) on all workers plus the caller. Returns the first
    // worker failure message, or an empty string.
    std::string run(size_t count, const Job &fn)
    {
        for (auto &failure : failures)
            failure.clear();
        {
            std::lock_guard<std::mutex> lock(mutex);
            job = &fn;
            job_count = count;
            next.store(0, std::memory_order_relaxed);
            finished.store(0, std::memory_order_relaxed);
            generation.fetch_add(1, std::memory_order_release);
        }
        wake.notify_all();
        claim(0, fn, count);
        if (!spin_until([&] { return finished.load(std::memory_order_acquire) == workers.size(); })) {
            std::unique_lock<std::mutex> lock(mutex);
            done.wait(lock, [&] { return finished.load(std::memory_order_acquire) == workers.size(); });
        }
        job = nullptr;
        for (const auto &failure : failures)
            if (!failure.empty())
                return failure;
        return {};
    }

    uint64_t spin_wakes = 0, block_wakes = 0; // written by workers under the mutex, read by the owner between runs

  private:
    template <typename Predicate> static bool spin_until(Predicate ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + SPIN_LIMIT;
        while (true) {
            for (unsigned i = 0; i < 64; ++i) {
                if (ready())
                    return true;
                cpu_relax();
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return ready();
        }
    }

    void claim(unsigned worker, const Job &fn, size_t count)
    {
        for (size_t i = next.fetch_add(1, std::memory_order_relaxed); i < count;
             i = next.fetch_add(1, std::memory_order_relaxed)) {
            if (!failures[worker].empty())
                continue; // drain: a failed worker keeps claiming and skipping so the run completes
            try {
                fn(worker, i);
            } catch (const std::exception &e) {
                failures[worker] = e.what();
            } catch (...) {
                failures[worker] = "unknown exception";
            }
        }
    }

    void worker_loop(unsigned worker)
    {
        uint64_t seen = 0;
        while (true) {
            const bool spun = spin_until([&] {
                return stop.load(std::memory_order_acquire) || generation.load(std::memory_order_acquire) != seen;
            });
            const Job *fn;
            size_t count;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (!spun)
                    wake.wait(lock, [&] {
                        return stop.load(std::memory_order_acquire) ||
                               generation.load(std::memory_order_acquire) != seen;
                    });
                if (stop.load(std::memory_order_acquire))
                    return;
                seen = generation.load(std::memory_order_acquire);
                fn = job;
                count = job_count;
                if (spun)
                    ++spin_wakes;
                else
                    ++block_wakes;
            }
            claim(worker, *fn, count);
            {
                std::lock_guard<std::mutex> lock(mutex);
                finished.fetch_add(1, std::memory_order_release);
            }
            done.notify_all();
        }
    }

    std::vector<std::thread> workers;
    std::vector<std::string> failures;
    std::mutex mutex;
    std::condition_variable wake, done;
    const Job *job = nullptr;
    size_t job_count = 0;
    std::atomic<size_t> next{0};
    std::atomic<size_t> finished{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> stop{false};
};

PlacementWorkerPool::PlacementWorkerPool(unsigned workers) : workers_(workers < 1 ? 1 : workers)
{
    if (workers_ > 1)
        impl_ = std::make_unique<Impl>(workers_ - 1);
}

PlacementWorkerPool::~PlacementWorkerPool() = default;

std::string PlacementWorkerPool::run(size_t count, const Job &job)
{
    if (count == 0)
        return {};
    if (!impl_) {
        // Same contract as the threaded path: a failing job is reported, not thrown.
        for (size_t i = 0; i < count; ++i) {
            try {
                job(0, i);
            } catch (const std::exception &e) {
                return e.what();
            } catch (...) {
                return "unknown exception";
            }
        }
        return {};
    }
    return impl_->run(count, job);
}

uint64_t PlacementWorkerPool::spin_wakes() const { return impl_ ? impl_->spin_wakes : 0; }
uint64_t PlacementWorkerPool::block_wakes() const { return impl_ ? impl_->block_wakes : 0; }

NEXTPNR_NAMESPACE_END
