/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The Mistral LAB work
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifndef MISTRAL_MONITOR_ABI_H
#define MISTRAL_MONITOR_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The live monitor's C ABI (`rust/npnr_mistral_lab_ffi/src/monitor.rs`, the frame in
// `rust/npnr_mistral_monitor`). Strings are borrowed for the call and copied by the handle; the
// snapshot is read for the call; the frame buffer is written for the call. Call statuses are the
// NPNR_LAB_CALL_* values of lab_control_abi.h.

typedef struct NpnrMonitorStringV1
{
    const uint8_t *ptr; // may be null when len is 0
    size_t len;
} NpnrMonitorStringV1;

typedef struct NpnrMonitorConfigV1
{
    NpnrMonitorStringV1 design, device, log_path, legality_mode, controls_mode;
    const NpnrMonitorStringV1 *options; // one line each, rendered verbatim
    size_t option_count;
} NpnrMonitorConfigV1;

#define NPNR_MONITOR_PHASES 9
enum NpnrMonitorPhase
{
    NPNR_MONITOR_PHASE_INIT = 0,
    NPNR_MONITOR_PHASE_LOAD = 1,
    NPNR_MONITOR_PHASE_PACK = 2,
    NPNR_MONITOR_PHASE_PLACE = 3,
    NPNR_MONITOR_PHASE_ROUTE_PREPARE = 4,
    NPNR_MONITOR_PHASE_ROUTE = 5,
    NPNR_MONITOR_PHASE_SIGNOFF = 6,
    NPNR_MONITOR_PHASE_DONE = 7,
    NPNR_MONITOR_PHASE_FAILED = 8,
};

typedef struct NpnrMonitorSnapshotV1
{
    uint32_t phase, columns, rows, checksum;
    double run_seconds;
    double phase_seconds[NPNR_MONITOR_PHASES];
    uint64_t legality[7]; // evaluations, legal, illegal, errors, mismatches, stale_cache, stale_revision
    uint64_t resident[5]; // evaluations, resets, trials, commits, restored
    uint64_t controls[7]; // evaluations, preparation, legal, illegal, errors, mismatches, fallbacks
    uint64_t cells, nets;
} NpnrMonitorSnapshotV1;

#define NPNR_MONITOR_DRY 1u // create and render: never write to the terminal
#define NPNR_MONITOR_MAX_OPTION_LINES 16

typedef struct NpnrMonitor NpnrMonitor;

uint32_t npnr_mistral_monitor_create(const NpnrMonitorConfigV1 *config, uint32_t flags, NpnrMonitor **output);
uint32_t npnr_mistral_monitor_log(NpnrMonitor *handle, const uint8_t *text, size_t len);
uint32_t npnr_mistral_monitor_render(NpnrMonitor *handle, const NpnrMonitorSnapshotV1 *snapshot, uint32_t flags,
                                     uint8_t *frame, size_t frame_capacity, size_t *frame_len);
void npnr_mistral_monitor_destroy(NpnrMonitor *handle);

#ifdef __cplusplus
}
#endif

#endif
