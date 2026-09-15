/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_CONTROL_ABI_H
#define MISTRAL_LAB_CONTROL_ABI_H

#include <stdint.h>

/* Native value transport, not a serialized file format. No pointers or native bools. */
enum
{
    NPNR_LAB_ABI_V1 = 1,
    NPNR_LAB_RULES_V1 = 1,
    NPNR_LAB_FF_COUNT = 40,
    NPNR_LAB_CONTROL_COUNT = 5,
    NPNR_LAB_MAX_NETS = 200,
    NPNR_LAB_ALLOCATION_COUNT = 12,
    NPNR_CONTROL_INVERTED = 1,
    NPNR_CONTROL_GLOBAL = 2
};

enum
{
    NPNR_CONTROL_CLK = 0,
    NPNR_CONTROL_SLOAD = 1,
    NPNR_CONTROL_SCLR = 2,
    NPNR_CONTROL_ACLR = 3,
    NPNR_CONTROL_ENA = 4
};

enum
{
    NPNR_CONTROL_LEGAL = 0,
    NPNR_CONTROL_ILLEGAL = 1,
    NPNR_CONTROL_BAD_SNAPSHOT = 2,
    NPNR_CONTROL_UNSUPPORTED_RULES = 3,
    NPNR_CONTROL_INTERNAL_ERROR = 4
};

enum
{
    NPNR_CONTROL_OK = 0,
    NPNR_CONTROL_CLOCK_CONFLICT = 1,
    NPNR_CONTROL_SLOAD_CONFLICT = 2,
    NPNR_CONTROL_SCLR_CONFLICT = 3,
    NPNR_CONTROL_ACLR_CAPACITY = 4,
    NPNR_CONTROL_ENA_CAPACITY = 5,
    NPNR_CONTROL_DATAIN_CONFLICT = 6,
    NPNR_CONTROL_BAD_ABI = 100,
    NPNR_CONTROL_BAD_SIZE = 101,
    NPNR_CONTROL_BAD_RULES = 102,
    NPNR_CONTROL_BAD_NET_COUNT = 103,
    NPNR_CONTROL_BAD_OCCUPANCY = 104,
    NPNR_CONTROL_BAD_RESERVED = 105,
    NPNR_CONTROL_BAD_SIGNAL = 106,
    NPNR_CONTROL_INCONSISTENT_GLOBAL = 107,
    NPNR_CONTROL_SPARSE_NET_IDS = 108
};

typedef struct
{
    uint32_t net_id; /* zero = disconnected; connected IDs are dense, starting at one */
    uint32_t flags;  /* inversion is retained even for disconnected signals */
} NpnrControlSignalV1;

typedef struct
{
    uint32_t occupied;
    uint32_t reserved;
    NpnrControlSignalV1 control[NPNR_LAB_CONTROL_COUNT]; /* CLK, SLOAD, SCLR, ACLR, ENA */
} NpnrLabFfV1;

typedef struct
{
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t rules_version;
    uint32_t net_count;
    uint64_t request_id;
    uint64_t snapshot_epoch;
    NpnrLabFfV1 ff[NPNR_LAB_FF_COUNT];
} NpnrLabControlsV1;

typedef struct
{
    NpnrControlSignalV1 signal;
    uint32_t ff_slot; /* UINT32_MAX = no origin */
    uint32_t reserved;
} NpnrLabControlBlockerV1;

typedef struct
{
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t status;
    uint32_t reason;
    uint64_t request_id;
    uint64_t snapshot_epoch;
    /* Legal only: clk, sload, sclr, aclr[2], ena[3], datain[4]. Zero otherwise. */
    NpnrControlSignalV1 allocation[NPNR_LAB_ALLOCATION_COUNT];
    uint32_t control_kind; /* UINT32_MAX unless a control operation failed */
    uint32_t ff_slot;
    uint32_t resource_mask; /* DATAIN indices on DATAIN failure; pool indices otherwise */
    uint32_t reserved;
    NpnrControlSignalV1 incoming;
    NpnrLabControlBlockerV1 blockers[4]; /* indexed by the bits in resource_mask */
} NpnrLabControlResultV1;

enum
{
    NPNR_LAB_MAX_BATCH = 64,
    NPNR_LAB_CALL_OK = 0,
    NPNR_LAB_CALL_BAD_COUNT = 1,
    NPNR_LAB_CALL_BAD_CAPACITY = 2,
    NPNR_LAB_CALL_NULL = 3,
    NPNR_LAB_CALL_MISALIGNED = 4,
    NPNR_LAB_CALL_BAD_RANGE = 5,
    NPNR_LAB_CALL_OVERLAP = 6,
    NPNR_LAB_CALL_PANIC = 7
};

/* Synchronous: no ownership transfer or retained pointers. Count zero accepts null.
 * Nonzero valid envelopes require count initialized inputs and count writable
 * outputs, each in one live, aligned allocation. Active ranges must be disjoint;
 * inputs must not be mutated and outputs must be exclusively accessible until return.
 * Only count outputs are touched, even if output_capacity is larger.
 * Envelope errors leave outputs untouched. On panic, count outputs are reset to
 * INTERNAL_ERROR. Ignore the whole batch on any non-OK call status.
 * Pointer liveness is a caller obligation; numeric checks cannot establish it. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t npnr_mistral_eval_controls_v1(const NpnrLabControlsV1 *inputs, uint32_t count, NpnrLabControlResultV1 *outputs,
                                       uint32_t output_capacity);
#ifdef __cplusplus
}
#endif

#endif
