// SPDX-License-Identifier: ISC
//! Synchronous, caller-owned batch transport. No pointers are retained.
#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(not(panic = "unwind"))]
compile_error!("LAB FFI requires panic=unwind for its containment boundary");

use npnr_mistral_lab::{
    ResidentError, ResidentLabs, ValidatedLabSnapshotV2, evaluate_lab_v2,
    evaluate_lab_v2_wire_into, evaluate_wire_into, v2_wire::BelPatchV2, v2_wire::LAB_BELS,
    v2_wire::*, wire::*,
};
use std::collections::HashMap;
use std::mem::{align_of, size_of};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

pub const MAX_BATCH: u32 = 64;
pub const CALL_OK: u32 = 0;
pub const CALL_BAD_COUNT: u32 = 1;
pub const CALL_BAD_CAPACITY: u32 = 2;
pub const CALL_NULL: u32 = 3;
pub const CALL_MISALIGNED: u32 = 4;
pub const CALL_BAD_RANGE: u32 = 5;
pub const CALL_OVERLAP: u32 = 6;
pub const CALL_PANIC: u32 = 7;
pub const CALL_BAD_SNAPSHOT: u32 = 8;
pub const CALL_LIMIT: u32 = 9;
pub const CALL_CANCELLED: u32 = 10;
pub const MAX_BATCHES_PER_WORKER: u32 = 2;
pub const MAX_RETAINED_BYTES: usize = 64 * 1024 * 1024;

struct BatchQuota {
    retained_bytes: usize,
    workers: HashMap<u64, u32>,
}

fn batch_quota() -> &'static Mutex<BatchQuota> {
    static QUOTA: OnceLock<Mutex<BatchQuota>> = OnceLock::new();
    QUOTA.get_or_init(|| {
        Mutex::new(BatchQuota {
            retained_bytes: 0,
            workers: HashMap::new(),
        })
    })
}

#[repr(C)]
pub struct NpnrLabFrozenBatchV2 {
    worker_id: u64,
    retained_bytes: usize,
    cancelled: AtomicBool,
    snapshots: Box<[ValidatedLabSnapshotV2]>,
}

fn retained_batch_bytes(count: usize) -> Option<usize> {
    size_of::<NpnrLabFrozenBatchV2>()
        .checked_add(count.checked_mul(size_of::<ValidatedLabSnapshotV2>())?)
}

fn reserve_batch(worker_id: u64, bytes: usize) -> Result<(), u32> {
    let mut quota = batch_quota()
        .lock()
        .unwrap_or_else(|poison| poison.into_inner());
    let worker_count = quota.workers.get(&worker_id).copied().unwrap_or(0);
    if worker_count >= MAX_BATCHES_PER_WORKER
        || quota.retained_bytes.checked_add(bytes).is_none()
        || quota.retained_bytes + bytes > MAX_RETAINED_BYTES
    {
        return Err(CALL_LIMIT);
    }
    quota.retained_bytes += bytes;
    quota.workers.insert(worker_id, worker_count + 1);
    Ok(())
}

fn release_batch(worker_id: u64, bytes: usize) {
    let mut quota = batch_quota()
        .lock()
        .unwrap_or_else(|poison| poison.into_inner());
    quota.retained_bytes -= bytes;
    let worker_count = quota
        .workers
        .get_mut(&worker_id)
        .expect("owned batch quota");
    *worker_count -= 1;
    if *worker_count == 0 {
        quota.workers.remove(&worker_id);
    }
}

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

fn check_frozen_create(
    inputs: *const LabFactsV2,
    count: u32,
    output: *mut *mut NpnrLabFrozenBatchV2,
) -> Result<(), u32> {
    if count == 0 || count > MAX_BATCH {
        return Err(CALL_BAD_COUNT);
    }
    if inputs.is_null() || output.is_null() {
        return Err(CALL_NULL);
    }
    let start = inputs.addr();
    let output_start = output.addr();
    if start % align_of::<LabFactsV2>() != 0
        || output_start % align_of::<*mut NpnrLabFrozenBatchV2>() != 0
    {
        return Err(CALL_MISALIGNED);
    }
    let end = start
        .checked_add(count as usize * size_of::<LabFactsV2>())
        .ok_or(CALL_BAD_RANGE)?;
    let output_end = output_start
        .checked_add(size_of::<*mut NpnrLabFrozenBatchV2>())
        .ok_or(CALL_BAD_RANGE)?;
    if start < output_end && output_start < end {
        return Err(CALL_OVERLAP);
    }
    Ok(())
}

/// Copy and validate a bounded batch into Rust-owned immutable storage.
///
/// # Safety
/// `inputs` must cover `count` initialized immutable records and `output` must
/// identify exclusive writable pointer storage. Their live ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_frozen_batch_v2_create(
    inputs: *const LabFactsV2,
    count: u32,
    worker_id: u64,
    output: *mut *mut NpnrLabFrozenBatchV2,
) -> u32 {
    if let Err(status) = check_frozen_create(inputs, count, output) {
        return status;
    }
    // SAFETY: the checked envelope plus caller obligations establish live storage.
    unsafe { output.write(std::ptr::null_mut()) };
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: checked above; caller owns the remaining liveness obligation.
        let inputs = unsafe { std::slice::from_raw_parts(inputs, count as usize) };
        let snapshots = inputs
            .iter()
            .map(ValidatedLabSnapshotV2::try_from)
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| CALL_BAD_SNAPSHOT)?
            .into_boxed_slice();
        let retained_bytes = retained_batch_bytes(snapshots.len()).ok_or(CALL_BAD_RANGE)?;
        let batch = Box::new(NpnrLabFrozenBatchV2 {
            worker_id,
            retained_bytes,
            cancelled: AtomicBool::new(false),
            snapshots,
        });
        reserve_batch(worker_id, retained_bytes)?;
        // No fallible operation follows quota reservation.
        let raw = Box::into_raw(batch);
        // SAFETY: checked exclusive pointer storage remains live for this call.
        unsafe { output.write(raw) };
        Ok::<(), u32>(())
    }));
    match outcome {
        Ok(Ok(())) => CALL_OK,
        Ok(Err(status)) => status,
        Err(payload) => {
            if let Err(secondary) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
                std::mem::forget(secondary);
            }
            CALL_PANIC
        }
    }
}

unsafe fn run_frozen_range(
    batch: &NpnrLabFrozenBatchV2,
    offset: usize,
    count: usize,
    outputs: *mut LabAssessmentV2,
    mut evaluate: impl FnMut(&ValidatedLabSnapshotV2) -> LabAssessmentV2,
) -> u32 {
    if batch.cancelled.load(Ordering::Acquire) {
        return CALL_CANCELLED;
    }
    for i in 0..count {
        // SAFETY: exported caller checked writable capacity and exclusivity.
        unsafe { outputs.add(i).write(LabAssessmentV2::default()) };
    }
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        for (i, snapshot) in batch.snapshots[offset..offset + count].iter().enumerate() {
            if batch.cancelled.load(Ordering::Acquire) {
                return Err(CALL_CANCELLED);
            }
            // SAFETY: initialization above established a live exclusive value.
            unsafe { *outputs.add(i) = evaluate(snapshot) };
        }
        Ok(())
    }));
    match outcome {
        Ok(Ok(())) => CALL_OK,
        Ok(Err(status)) => status,
        Err(payload) => {
            for i in 0..count {
                // SAFETY: same active output range as above.
                unsafe { outputs.add(i).write(LabAssessmentV2::default()) };
            }
            if let Err(secondary) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
                std::mem::forget(secondary);
            }
            CALL_PANIC
        }
    }
}

/// Evaluate an immutable range. Calls on the same handle may run concurrently
/// when each caller exclusively owns its output range.
///
/// # Safety
/// `batch` must remain live for the call. `outputs` must cover `count` exclusive,
/// aligned records and must not alias the handle allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_frozen_batch_v2_evaluate(
    batch: *const NpnrLabFrozenBatchV2,
    offset: u32,
    count: u32,
    outputs: *mut LabAssessmentV2,
    output_capacity: u32,
) -> u32 {
    if batch.is_null() {
        return CALL_NULL;
    }
    if batch.addr() % align_of::<NpnrLabFrozenBatchV2>() != 0 {
        return CALL_MISALIGNED;
    }
    // SAFETY: caller guarantees this aligned pointer identifies a live handle.
    let batch = unsafe { &*batch };
    let Some(end) = (offset as usize).checked_add(count as usize) else {
        return CALL_BAD_RANGE;
    };
    if end > batch.snapshots.len() {
        return CALL_BAD_RANGE;
    }
    if count == 0 {
        return if batch.cancelled.load(Ordering::Acquire) {
            CALL_CANCELLED
        } else {
            CALL_OK
        };
    }
    if output_capacity < count {
        return CALL_BAD_CAPACITY;
    }
    if outputs.is_null() {
        return CALL_NULL;
    }
    if outputs.addr() % align_of::<LabAssessmentV2>() != 0 {
        return CALL_MISALIGNED;
    }
    if outputs
        .addr()
        .checked_add(count as usize * size_of::<LabAssessmentV2>())
        .is_none()
    {
        return CALL_BAD_RANGE;
    }
    // SAFETY: envelope checked; caller guarantees liveness and exclusive outputs.
    unsafe {
        run_frozen_range(
            batch,
            offset as usize,
            count as usize,
            outputs,
            evaluate_lab_v2,
        )
    }
}

/// Mark a live batch cancelled. Concurrent readers observe cancellation at a
/// record boundary and must discard the complete range on `CALL_CANCELLED`.
///
/// # Safety
/// `batch` must identify a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_frozen_batch_v2_cancel(
    batch: *mut NpnrLabFrozenBatchV2,
) -> u32 {
    if batch.is_null() {
        return CALL_NULL;
    }
    if batch.addr() % align_of::<NpnrLabFrozenBatchV2>() != 0 {
        return CALL_MISALIGNED;
    }
    // SAFETY: caller guarantees a live handle; AtomicBool permits shared access.
    unsafe { &*batch }.cancelled.store(true, Ordering::Release);
    CALL_OK
}

/// Release a handle after all readers have completed.
///
/// # Safety
/// `batch` must be null or a live handle returned by create and not yet destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_frozen_batch_v2_destroy(batch: *mut NpnrLabFrozenBatchV2) {
    if batch.is_null() {
        return;
    }
    // SAFETY: caller guarantees unique ownership and no active readers.
    let batch = unsafe { Box::from_raw(batch) };
    release_batch(batch.worker_id, batch.retained_bytes);
    drop(batch);
}

#[cfg(test)]
mod tests;

/// Resident LAB snapshots (`npnr_mistral_lab::ResidentLabs`) behind an owner-only
/// handle. One handle per placement session, one thread at a time; the caller
/// resets a LAB before its first query and sends the ALMs that changed since
/// its last query as patches. A Rust panic poisons the handle: every later call
/// returns `CALL_BAD_SNAPSHOT` until it is destroyed.
pub struct NpnrLabResidentV2 {
    labs: ResidentLabs,
    poisoned: bool,
}

pub const MAX_RESIDENT_LABS: u32 = 1 << 16;
/// Evaluate flag: recompute the ALM input counts from the facts instead of
/// taking the caller's (the harness modes).
pub const RESIDENT_RECOMPUTE_COUNTS: u32 = 1;

/// # Safety
/// `output` must be exclusive writable pointer storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_resident_v2_create(
    lab_count: u32,
    input_limit: i32,
    output: *mut *mut NpnrLabResidentV2,
) -> u32 {
    if lab_count == 0 || lab_count > MAX_RESIDENT_LABS {
        return CALL_BAD_COUNT;
    }
    if output.is_null() {
        return CALL_NULL;
    }
    if output.addr() % align_of::<*mut NpnrLabResidentV2>() != 0 {
        return CALL_MISALIGNED;
    }
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        ResidentLabs::new(lab_count as usize, input_limit).map(|labs| {
            Box::into_raw(Box::new(NpnrLabResidentV2 {
                labs,
                poisoned: false,
            }))
        })
    }));
    match outcome {
        Ok(Some(raw)) => {
            // SAFETY: checked non-null and aligned above.
            unsafe { output.write(raw) };
            CALL_OK
        }
        Ok(None) => CALL_LIMIT,
        Err(payload) => {
            if let Err(secondary) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
                std::mem::forget(secondary);
            }
            CALL_PANIC
        }
    }
}

fn resident_mut<'a>(handle: *mut NpnrLabResidentV2) -> Result<&'a mut NpnrLabResidentV2, u32> {
    if handle.is_null() {
        return Err(CALL_NULL);
    }
    if handle.addr() % align_of::<NpnrLabResidentV2>() != 0 {
        return Err(CALL_MISALIGNED);
    }
    // SAFETY: the caller holds the only reference to a live handle from create.
    let resident = unsafe { &mut *handle };
    if resident.poisoned {
        return Err(CALL_BAD_SNAPSHOT);
    }
    Ok(resident)
}

/// # Safety
/// `handle` must come from `npnr_mistral_resident_v2_create` and not be in use elsewhere.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_resident_v2_reset(
    handle: *mut NpnrLabResidentV2,
    lab: u32,
    is_mlab: u32,
) -> u32 {
    let resident = match resident_mut(handle) {
        Ok(resident) => resident,
        Err(status) => return status,
    };
    if is_mlab > 1 {
        return CALL_BAD_SNAPSHOT;
    }
    match resident.labs.reset(lab as usize, is_mlab != 0) {
        Ok(()) => CALL_OK,
        Err(ResidentError::Lab) => CALL_BAD_RANGE,
        Err(_) => CALL_BAD_SNAPSHOT,
    }
}

fn check_resident_evaluate(
    patches: *const BelPatchV2,
    patch_count: u32,
    output: *mut LabVerdictV2,
) -> Result<(), u32> {
    if patch_count as usize > LAB_BELS {
        return Err(CALL_BAD_COUNT);
    }
    if output.is_null() || (patches.is_null() && patch_count != 0) {
        return Err(CALL_NULL);
    }
    if output.addr() % align_of::<LabVerdictV2>() != 0
        || (patch_count != 0 && patches.addr() % align_of::<BelPatchV2>() != 0)
    {
        return Err(CALL_MISALIGNED);
    }
    if patch_count != 0 {
        let start = patches.addr();
        let end = start
            .checked_add(patch_count as usize * size_of::<BelPatchV2>())
            .ok_or(CALL_BAD_RANGE)?;
        let output_start = output.addr();
        let output_end = output_start
            .checked_add(size_of::<LabVerdictV2>())
            .ok_or(CALL_BAD_RANGE)?;
        if start < output_end && output_start < end {
            return Err(CALL_OVERLAP);
        }
    }
    Ok(())
}

/// Applies the commits, evaluates the query with the trials in view, and
/// writes the verdict. A malformed patch or query is reported inside the
/// verdict (`LAB_MALFORMED`) with the LAB left as it was; a LAB that was never
/// reset, or an unknown flag, returns `CALL_BAD_SNAPSHOT`.
///
/// # Safety
/// `handle` as for reset; `patches` must cover `patch_count` initialized
/// records and `output` exclusive writable storage, non-overlapping.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_resident_v2_evaluate(
    handle: *mut NpnrLabResidentV2,
    lab: u32,
    patches: *const BelPatchV2,
    patch_count: u32,
    query: u32,
    query_alm: u32,
    flags: u32,
    output: *mut LabVerdictV2,
) -> u32 {
    let resident = match resident_mut(handle) {
        Ok(resident) => resident,
        Err(status) => return status,
    };
    if let Err(status) = check_resident_evaluate(patches, patch_count, output) {
        return status;
    }
    let patches: &[BelPatchV2] = if patch_count == 0 {
        &[]
    } else {
        // SAFETY: envelope checked above; the caller keeps the records live and unaliased.
        unsafe { std::slice::from_raw_parts(patches, patch_count as usize) }
    };
    if flags & !RESIDENT_RECOMPUTE_COUNTS != 0 {
        return CALL_BAD_SNAPSHOT;
    }
    let recompute = flags & RESIDENT_RECOMPUTE_COUNTS != 0;
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        resident
            .labs
            .evaluate(lab as usize, patches, query, query_alm, recompute)
    }));
    match outcome {
        Ok(Ok(result)) => {
            // SAFETY: output checked non-null, aligned, and disjoint from the patches.
            unsafe { output.write(result) };
            CALL_OK
        }
        Ok(Err(ResidentError::Lab)) => CALL_BAD_RANGE,
        Ok(Err(ResidentError::Uninitialised)) => CALL_BAD_SNAPSHOT,
        Ok(Err(error)) => {
            let result = LabVerdictV2 {
                status: LAB_MALFORMED,
                reason: match error {
                    ResidentError::Query => BAD_QUERY,
                    _ => BAD_SHAPE,
                },
                ..LabVerdictV2::default()
            };
            // SAFETY: as above.
            unsafe { output.write(result) };
            CALL_OK
        }
        Err(payload) => {
            resident.poisoned = true;
            if let Err(secondary) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
                std::mem::forget(secondary);
            }
            CALL_PANIC
        }
    }
}

/// # Safety
/// `handle` must come from create and must not be used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_resident_v2_destroy(handle: *mut NpnrLabResidentV2) {
    if handle.is_null() {
        return;
    }
    // SAFETY: ownership returns to Rust exactly once, per the contract.
    let resident = unsafe { Box::from_raw(handle) };
    if let Err(payload) = catch_unwind(AssertUnwindSafe(|| drop(resident))) {
        std::mem::forget(payload);
    }
}
