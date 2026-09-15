/* SPDX-License-Identifier: ISC */
#include "lab_frozen_batch.h"

#include <utility>

NEXTPNR_NAMESPACE_BEGIN

RustFrozenLabBatchV2::RustFrozenLabBatchV2(RustFrozenLabBatchV2 &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)), size_(std::exchange(other.size_, 0))
{
}

RustFrozenLabBatchV2 &RustFrozenLabBatchV2::operator=(RustFrozenLabBatchV2 &&other) noexcept
{
    if (this != &other) {
        reset();
        handle_ = std::exchange(other.handle_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

RustFrozenLabBatchV2::~RustFrozenLabBatchV2() { reset(); }

RustFrozenLabBatchV2 RustFrozenLabBatchV2::create(const std::vector<NpnrLabFactsV2> &inputs, uint64_t worker_id,
                                                  uint32_t &status)
{
    RustFrozenLabBatchV2 batch;
    if (inputs.empty() || inputs.size() > NPNR_LAB_MAX_BATCH) {
        status = NPNR_LAB_CALL_BAD_COUNT;
        return batch;
    }
#ifndef NO_RUST
    status = npnr_mistral_frozen_batch_v2_create(inputs.data(), uint32_t(inputs.size()), worker_id, &batch.handle_);
    if (status == NPNR_LAB_CALL_OK)
        batch.size_ = uint32_t(inputs.size());
#else
    (void)inputs;
    (void)worker_id;
    status = NPNR_LAB_CALL_LIMIT;
#endif
    return batch;
}

uint32_t RustFrozenLabBatchV2::evaluate(uint32_t offset, uint32_t count, NpnrLabAssessmentV2 *outputs,
                                        uint32_t output_capacity) const
{
#ifndef NO_RUST
    return npnr_mistral_frozen_batch_v2_evaluate(handle_, offset, count, outputs, output_capacity);
#else
    (void)offset;
    (void)count;
    (void)outputs;
    (void)output_capacity;
    return NPNR_LAB_CALL_LIMIT;
#endif
}

uint32_t RustFrozenLabBatchV2::cancel()
{
#ifndef NO_RUST
    return npnr_mistral_frozen_batch_v2_cancel(handle_);
#else
    return NPNR_LAB_CALL_LIMIT;
#endif
}

void RustFrozenLabBatchV2::reset()
{
#ifndef NO_RUST
    npnr_mistral_frozen_batch_v2_destroy(handle_);
#endif
    handle_ = nullptr;
    size_ = 0;
}

NEXTPNR_NAMESPACE_END
