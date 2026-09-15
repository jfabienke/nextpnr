/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_FROZEN_BATCH_H
#define MISTRAL_LAB_FROZEN_BATCH_H

#include <cstdint>
#include <vector>

#include "lab_v2_abi.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

class RustFrozenLabBatchV2
{
  public:
    RustFrozenLabBatchV2() = default;
    RustFrozenLabBatchV2(const RustFrozenLabBatchV2 &) = delete;
    RustFrozenLabBatchV2 &operator=(const RustFrozenLabBatchV2 &) = delete;
    RustFrozenLabBatchV2(RustFrozenLabBatchV2 &&other) noexcept;
    RustFrozenLabBatchV2 &operator=(RustFrozenLabBatchV2 &&other) noexcept;
    ~RustFrozenLabBatchV2();

    static RustFrozenLabBatchV2 create(const std::vector<NpnrLabFactsV2> &inputs, uint64_t worker_id, uint32_t &status);

    explicit operator bool() const { return handle_ != nullptr; }
    uint32_t size() const { return size_; }
    uint32_t evaluate(uint32_t offset, uint32_t count, NpnrLabAssessmentV2 *outputs, uint32_t output_capacity) const;
    uint32_t cancel();
    void reset();

  private:
    NpnrLabFrozenBatchV2 *handle_ = nullptr;
    uint32_t size_ = 0;
};

NEXTPNR_NAMESPACE_END

#endif
