// SPDX-License-Identifier: ISC
//! Synchronous, caller-owned batch transport. No pointers are retained.
#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(not(panic = "unwind"))]
compile_error!("LAB FFI requires panic=unwind for its containment boundary");

use npnr_mistral_lab::{evaluate_lab_v2_wire_into, evaluate_wire_into, v2_wire::*, wire::*};
use std::mem::{align_of, size_of};
use std::panic::{AssertUnwindSafe, catch_unwind};

pub const MAX_BATCH: u32 = 64;
pub const CALL_OK: u32 = 0;
pub const CALL_BAD_COUNT: u32 = 1;
pub const CALL_BAD_CAPACITY: u32 = 2;
pub const CALL_NULL: u32 = 3;
pub const CALL_MISALIGNED: u32 = 4;
pub const CALL_BAD_RANGE: u32 = 5;
pub const CALL_OVERLAP: u32 = 6;
pub const CALL_PANIC: u32 = 7;

fn check_buffers(
    inputs: *const LabControlsV1,
    count: u32,
    outputs: *mut LabControlResultV1,
    capacity: u32,
) -> Result<(), u32> {
    if count > MAX_BATCH {
        return Err(CALL_BAD_COUNT);
    }
    if count == 0 {
        return Ok(());
    }
    if capacity < count {
        return Err(CALL_BAD_CAPACITY);
    }
    if inputs.is_null() || outputs.is_null() {
        return Err(CALL_NULL);
    }
    let start_in = inputs.addr();
    let start_out = outputs.addr();
    if start_in % align_of::<LabControlsV1>() != 0
        || start_out % align_of::<LabControlResultV1>() != 0
    {
        return Err(CALL_MISALIGNED);
    }
    // count <= 64 bounds both byte lengths well below isize::MAX. Check address
    // overflow before constructing references; integer ranges do not prove liveness.
    let end_in = start_in
        .checked_add(count as usize * size_of::<LabControlsV1>())
        .ok_or(CALL_BAD_RANGE)?;
    let end_out = start_out
        .checked_add(count as usize * size_of::<LabControlResultV1>())
        .ok_or(CALL_BAD_RANGE)?;
    if start_in < end_out && start_out < end_in {
        return Err(CALL_OVERLAP);
    }
    Ok(())
}

fn failed_result(input: &LabControlsV1) -> LabControlResultV1 {
    LabControlResultV1 {
        request_id: input.request_id,
        snapshot_epoch: input.snapshot_epoch,
        ..LabControlResultV1::default()
    }
}

// This helper is private so tests can inject a panic without a production fault
// switch or another exported symbol. Its caller has checked all buffer conditions.
unsafe fn run_batch(
    inputs: *const LabControlsV1,
    count: usize,
    outputs: *mut LabControlResultV1,
    mut evaluate: impl FnMut(&LabControlsV1, &mut LabControlResultV1),
) -> u32 {
    // SAFETY: caller guarantees initialized, aligned, live inputs in one allocation,
    // no mutation for this call, and no overlap with the output write range.
    let inputs = unsafe { std::slice::from_raw_parts(inputs, count) };
    let initialize = || {
        for (i, input) in inputs.iter().enumerate() {
            // SAFETY: i < count, output storage is live/aligned/exclusive. ptr::write
            // accepts uninitialized storage; no &mut reference to uninitialized T.
            unsafe { outputs.add(i).write(failed_result(input)) };
        }
    };
    initialize();
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        for (i, input) in inputs.iter().enumerate() {
            // SAFETY: initialization above established a live value, and the
            // active output range remains exclusive for this call.
            let result = unsafe { &mut *outputs.add(i) };
            evaluate(input, result);
        }
    }));
    match outcome {
        Ok(()) => CALL_OK,
        Err(payload) => {
            // Invalidate even earlier successful entries. The caller must ignore
            // the complete batch when the call status is not CALL_OK.
            initialize();
            // An arbitrary panic payload can itself panic when dropped. Contain
            // that too; leak only the secondary payload on this exceptional path.
            if let Err(secondary) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
                std::mem::forget(secondary);
            }
            CALL_PANIC
        }
    }
}

/// Evaluate at most 64 snapshots, without retaining or freeing host storage.
///
/// Envelope errors leave outputs untouched. Zero count is a no-op accepting null
/// pointers. On success exactly `count` entries are initialized; per-record
/// validation errors are results, not call errors. A caught panic resets those
/// entries to `INTERNAL_ERROR` and returns `CALL_PANIC`.
///
/// # Safety
/// For an envelope that passes validation, `inputs` must identify `count` fully
/// initialized records in one live allocation, readable for the whole call.
/// `outputs` must identify writable storage for at least `count` records in one
/// live allocation (initialization is not required). Both pointers must be aligned,
/// their active ranges disjoint, inputs immutable, and outputs exclusively accessed
/// for the duration of the call. Capacity describes records, not bytes. Invalid
/// but numerically plausible pointers cannot be detected by this API.
///
/// The host must not install an aborting or panicking Rust panic hook. Process
/// aborts, allocation failure, foreign exceptions, and invalid memory are not
/// recoverable through `catch_unwind`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_eval_controls_v1(
    inputs: *const LabControlsV1,
    count: u32,
    outputs: *mut LabControlResultV1,
    output_capacity: u32,
) -> u32 {
    if let Err(status) = check_buffers(inputs, count, outputs, output_capacity) {
        return status;
    }
    if count == 0 {
        return CALL_OK;
    }
    // SAFETY: envelope is checked above; remaining liveness/exclusivity obligations
    // are explicitly the unsafe caller's responsibility. No callback crosses FFI.
    unsafe { run_batch(inputs, count as usize, outputs, evaluate_wire_into) }
}

fn check_buffers_v2(
    inputs: *const LabFactsV2,
    count: u32,
    outputs: *mut LabAssessmentV2,
    capacity: u32,
) -> Result<(), u32> {
    if count > MAX_BATCH {
        return Err(CALL_BAD_COUNT);
    }
    if count == 0 {
        return Ok(());
    }
    if capacity < count {
        return Err(CALL_BAD_CAPACITY);
    }
    if inputs.is_null() || outputs.is_null() {
        return Err(CALL_NULL);
    }
    let start_in = inputs.addr();
    let start_out = outputs.addr();
    if start_in % align_of::<LabFactsV2>() != 0 || start_out % align_of::<LabAssessmentV2>() != 0 {
        return Err(CALL_MISALIGNED);
    }
    let end_in = start_in
        .checked_add(count as usize * size_of::<LabFactsV2>())
        .ok_or(CALL_BAD_RANGE)?;
    let end_out = start_out
        .checked_add(count as usize * size_of::<LabAssessmentV2>())
        .ok_or(CALL_BAD_RANGE)?;
    if start_in < end_out && start_out < end_in {
        return Err(CALL_OVERLAP);
    }
    Ok(())
}

fn failed_result_v2(input: &LabFactsV2) -> LabAssessmentV2 {
    LabAssessmentV2 {
        request_id: input.request_id,
        snapshot_epoch: input.snapshot_epoch,
        query: input.query,
        query_alm: input.query_alm,
        ..LabAssessmentV2::default()
    }
}

unsafe fn run_batch_v2(
    inputs: *const LabFactsV2,
    count: usize,
    outputs: *mut LabAssessmentV2,
) -> u32 {
    // SAFETY: checked by the exported caller as documented there.
    let inputs = unsafe { std::slice::from_raw_parts(inputs, count) };
    let initialize = || {
        for (i, input) in inputs.iter().enumerate() {
            // SAFETY: active output range is live/aligned/exclusive.
            unsafe { outputs.add(i).write(failed_result_v2(input)) };
        }
    };
    initialize();
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        for (i, input) in inputs.iter().enumerate() {
            // SAFETY: initialization above established a live exclusive value.
            evaluate_lab_v2_wire_into(input, unsafe { &mut *outputs.add(i) });
        }
    }));
    match outcome {
        Ok(()) => CALL_OK,
        Err(payload) => {
            initialize();
            if let Err(secondary) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
                std::mem::forget(secondary);
            }
            CALL_PANIC
        }
    }
}

/// Evaluate at most 64 independent V2 LAB snapshots. No pointer is retained.
///
/// # Safety
/// The input and output ranges must be live, aligned, disjoint, immutable/exclusive
/// as applicable, and cover `count` records for the complete synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_eval_lab_v2(
    inputs: *const LabFactsV2,
    count: u32,
    outputs: *mut LabAssessmentV2,
    output_capacity: u32,
) -> u32 {
    if let Err(status) = check_buffers_v2(inputs, count, outputs, output_capacity) {
        return status;
    }
    if count == 0 {
        return CALL_OK;
    }
    // SAFETY: envelope checked above; caller retains liveness/exclusivity obligations.
    unsafe { run_batch_v2(inputs, count as usize, outputs) }
}

#[cfg(test)]
mod tests;
