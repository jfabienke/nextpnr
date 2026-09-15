/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_V2_ABI_H
#define MISTRAL_LAB_V2_ABI_H

#include <stdint.h>
#include "lab_control_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    NPNR_LAB_ABI_V2 = 2,
    NPNR_LAB_V2_ALMS = 10,
    NPNR_LAB_V2_LUTS = 2,
    NPNR_LAB_V2_FFS = 4,
    NPNR_LAB_V2_LUT_INPUTS = 7,
    NPNR_LAB_V2_MAX_NETS = 512,
};

enum NpnrLabQueryV2
{
    NPNR_LAB_QUERY_COMB_BEL = 0,
    NPNR_LAB_QUERY_FF_BEL = 1,
    NPNR_LAB_QUERY_WHOLE_LAB = 2,
};

enum NpnrLabStatusV2
{
    NPNR_LAB_V2_LEGAL = 0,
    NPNR_LAB_V2_ILLEGAL = 1,
    NPNR_LAB_V2_MALFORMED = 2,
};

enum NpnrLabReasonV2
{
    NPNR_LAB_V2_OK = 0,
    NPNR_LAB_V2_BAD_HEADER = 1,
    NPNR_LAB_V2_BAD_QUERY = 2,
    NPNR_LAB_V2_BAD_SHAPE = 3,
    NPNR_LAB_V2_ALM_BITS = 10,
    NPNR_LAB_V2_ALM_INPUTS = 11,
    NPNR_LAB_V2_CARRY_MIX = 12,
    NPNR_LAB_V2_ODD_FF = 13,
    NPNR_LAB_V2_FF_CONTROL = 14,
    NPNR_LAB_V2_SDATA_PATH = 15,
    NPNR_LAB_V2_DATAIN_PATH = 16,
    NPNR_LAB_V2_LAB_INPUT_LIMIT = 20,
    NPNR_LAB_V2_CONTROL_CONFLICT = 21,
    NPNR_LAB_V2_MLAB_GROUP = 22,
    NPNR_LAB_V2_MLAB_FF = 23,
};

typedef struct NpnrLabLutV2
{
    uint32_t occupied;
    uint32_t input_count;
    uint32_t used_input_count;
    uint32_t bits_count;
    int32_t chain_shared_input_count;
    int32_t mlab_group;
    int32_t constr_z;
    uint32_t is_carry;
    uint32_t input_net[NPNR_LAB_V2_LUT_INPUTS];
    uint32_t comb_out_net;
    NpnrControlSignalV1 wclk;
    NpnrControlSignalV1 we;
} NpnrLabLutV2;

typedef struct NpnrLabFfV2
{
    uint32_t occupied;
    uint32_t datain_net;
    uint32_t sdata_net;
    NpnrControlSignalV1 control[NPNR_LAB_CONTROL_COUNT];
} NpnrLabFfV2;

typedef struct NpnrAlmFactsV2
{
    NpnrLabLutV2 lut[NPNR_LAB_V2_LUTS];
    NpnrLabFfV2 ff[NPNR_LAB_V2_FFS];
    int32_t cached_input_count;
    uint32_t reserved;
} NpnrAlmFactsV2;

typedef struct NpnrLabFactsV2
{
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t request_id;
    uint64_t snapshot_epoch;
    uint32_t query;
    uint32_t query_alm;
    int32_t input_limit;
    uint32_t is_mlab;
    uint32_t net_count;
    uint32_t reserved;
    NpnrAlmFactsV2 alm[NPNR_LAB_V2_ALMS];
} NpnrLabFactsV2;

typedef struct NpnrLabAssessmentV2
{
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t request_id;
    uint64_t snapshot_epoch;
    uint32_t status;
    uint32_t reason;
    uint32_t query;
    uint32_t query_alm;
    uint32_t failing_alm;
    uint32_t failing_slot;
    int32_t observed;
    int32_t limit;
    uint32_t recomputed_valid_mask;
    int32_t recomputed_input_count[NPNR_LAB_V2_ALMS];
    uint32_t control_valid;
    uint32_t reserved;
    NpnrLabControlResultV1 control;
} NpnrLabAssessmentV2;

typedef struct NpnrLabFrozenBatchV2 NpnrLabFrozenBatchV2;

// Same synchronous caller-owned envelope contract and status codes as V1.
uint32_t npnr_mistral_eval_lab_v2(const NpnrLabFactsV2 *inputs, uint32_t count, NpnrLabAssessmentV2 *outputs,
                                  uint32_t output_capacity);

/* Copies and validates every input before publishing an immutable Rust-owned
 * handle. At most two handles may be retained per worker and aggregate retained
 * input storage is bounded to 64 MiB. */
uint32_t npnr_mistral_frozen_batch_v2_create(const NpnrLabFactsV2 *inputs, uint32_t count, uint64_t worker_id,
                                             NpnrLabFrozenBatchV2 **output);

/* Concurrent readers may share a handle when their output ranges are disjoint.
 * The owner must keep the handle alive for every call. */
uint32_t npnr_mistral_frozen_batch_v2_evaluate(const NpnrLabFrozenBatchV2 *batch, uint32_t offset, uint32_t count,
                                               NpnrLabAssessmentV2 *outputs, uint32_t output_capacity);
uint32_t npnr_mistral_frozen_batch_v2_cancel(NpnrLabFrozenBatchV2 *batch);
void npnr_mistral_frozen_batch_v2_destroy(NpnrLabFrozenBatchV2 *batch);

#ifdef __cplusplus
}
#endif

#endif
