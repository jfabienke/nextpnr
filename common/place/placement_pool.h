/* SPDX-License-Identifier: ISC */
#ifndef PLACEMENT_POOL_H
#define PLACEMENT_POOL_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

// A persistent pool of (workers - 1) threads plus the calling owner thread.
// run() hands out indices [0, count) dynamically to every worker including the
// caller (worker 0); results must be addressed by index so claim order never
// influences the caller. Workers spin briefly before blocking, since the jobs
// this pool serves are tens of microseconds long. Exceptions thrown by a job
// are caught on the worker and returned to the caller as text; the run always
// completes. With workers <= 1 no threads exist and run() executes inline.
class PlacementWorkerPool
{
  public:
    using Job = std::function<void(unsigned worker, size_t index)>;

    explicit PlacementWorkerPool(unsigned workers);
    ~PlacementWorkerPool();
    PlacementWorkerPool(const PlacementWorkerPool &) = delete;
    PlacementWorkerPool &operator=(const PlacementWorkerPool &) = delete;

    unsigned workers() const { return workers_; }
    // Returns the first job failure message, or an empty string.
    std::string run(size_t count, const Job &job);
    uint64_t spin_wakes() const;
    uint64_t block_wakes() const;

  private:
    struct Impl;
    unsigned workers_;
    std::unique_ptr<Impl> impl_;
};

NEXTPNR_NAMESPACE_END

#endif
