// SPDX-License-Identifier: ISC
//! The C ABI of the live monitor (`--monitor`). The arch creates a handle with
//! the run's names and option lines, feeds it log lines from the log hook, and
//! passes a counter snapshot per tick; the frame is written to the terminal
//! here. The handle locks internally, since the log hook and the ticker run on
//! different threads, and a panic poisons it as it does the LAB handles.
use crate::{CALL_BAD_COUNT, CALL_BAD_RANGE, CALL_MISALIGNED, CALL_NULL, CALL_OK, CALL_PANIC};
use npnr_mistral_monitor::{
    Config, Controls, Legality, Monitor, Phase, Resident, Snapshot, TERMINAL_ENTER, TERMINAL_LEAVE,
    frame_text,
};
use std::io::Write as _;
use std::mem::{align_of, offset_of, size_of};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};

/// A borrowed string for the duration of one call; `ptr` may be null when `len` is 0.
#[repr(C)]
pub struct NpnrMonitorStringV1 {
    pub ptr: *const u8,
    pub len: usize,
}

#[repr(C)]
pub struct NpnrMonitorConfigV1 {
    pub design: NpnrMonitorStringV1,
    pub device: NpnrMonitorStringV1,
    pub log_path: NpnrMonitorStringV1,
    pub legality_mode: NpnrMonitorStringV1,
    pub controls_mode: NpnrMonitorStringV1,
    pub options: *const NpnrMonitorStringV1,
    pub option_count: usize,
}

/// The counters per tick; the arrays are in the order of the crate's structs.
#[repr(C)]
pub struct NpnrMonitorSnapshotV1 {
    pub phase: u32,
    pub columns: u32,
    pub rows: u32,
    pub checksum: u32,
    pub run_seconds: f64,
    pub phase_seconds: [f64; Phase::COUNT],
    pub legality: [u64; 7],
    pub resident: [u64; 5],
    pub controls: [u64; 7],
    pub cells: u64,
    pub nets: u64,
}

const _: () = assert!(size_of::<NpnrMonitorSnapshotV1>() == 264);
const _: () = assert!(align_of::<NpnrMonitorSnapshotV1>() == 8);
const _: () = assert!(offset_of!(NpnrMonitorSnapshotV1, run_seconds) == 16);
const _: () = assert!(offset_of!(NpnrMonitorSnapshotV1, phase_seconds) == 24);
const _: () = assert!(offset_of!(NpnrMonitorSnapshotV1, legality) == 96);
const _: () = assert!(offset_of!(NpnrMonitorSnapshotV1, resident) == 152);
const _: () = assert!(offset_of!(NpnrMonitorSnapshotV1, controls) == 192);
const _: () = assert!(offset_of!(NpnrMonitorSnapshotV1, cells) == 248);
const _: () = assert!(offset_of!(NpnrMonitorSnapshotV1, nets) == 256);
const _: () = assert!(size_of::<NpnrMonitorConfigV1>() == 96);

/// Create and render flag: never write to the terminal (tests, and a caller that
/// takes the frame through the buffer).
pub const MONITOR_DRY: u32 = 1;
pub const MAX_OPTION_LINES: usize = 16;
pub const MAX_STRING: usize = 4096;
pub const MAX_LOG_LINE: usize = 4096;

pub struct NpnrMonitor {
    inner: Mutex<Monitor>,
    dry: bool,
    poisoned: AtomicBool,
}

impl NpnrMonitor {
    fn poison(&self) {
        self.poisoned.store(true, Ordering::Release);
    }

    fn poisoned(&self) -> bool {
        self.poisoned.load(Ordering::Acquire)
    }
}

fn terminal_write(text: &str) {
    let mut out = std::io::stdout().lock();
    if out.write_all(text.as_bytes()).is_ok() {
        let _ = out.flush();
    }
}

/// # Safety
/// `text.ptr` must be readable for `text.len` bytes for the duration of the call.
unsafe fn read_string(text: &NpnrMonitorStringV1) -> Result<String, u32> {
    if text.len == 0 {
        return Ok(String::new());
    }
    if text.ptr.is_null() {
        return Err(CALL_NULL);
    }
    if text.len > MAX_STRING {
        return Err(CALL_BAD_RANGE);
    }
    // SAFETY: non-null and, by the caller's contract, readable for `len` bytes.
    let bytes = unsafe { std::slice::from_raw_parts(text.ptr, text.len) };
    Ok(String::from_utf8_lossy(bytes).into_owned())
}

fn drop_payload(payload: Box<dyn std::any::Any + Send>) {
    if let Err(secondary) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
        std::mem::forget(secondary);
    }
}

/// # Safety
/// `config` and the strings it names must be readable for the call; `output`
/// must be exclusive writable pointer storage. On `CALL_OK` the handle is owned
/// by the caller until `npnr_mistral_monitor_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_monitor_create(
    config: *const NpnrMonitorConfigV1,
    flags: u32,
    output: *mut *mut NpnrMonitor,
) -> u32 {
    if config.is_null() || output.is_null() {
        return CALL_NULL;
    }
    if config.addr() % align_of::<NpnrMonitorConfigV1>() != 0
        || output.addr() % align_of::<*mut NpnrMonitor>() != 0
    {
        return CALL_MISALIGNED;
    }
    // SAFETY: checked non-null and aligned; readable by the caller's contract.
    let config = unsafe { &*config };
    if config.option_count > MAX_OPTION_LINES {
        return CALL_BAD_COUNT;
    }
    if config.option_count > 0 && config.options.is_null() {
        return CALL_NULL;
    }
    // SAFETY: each string is readable for the call by the caller's contract.
    let strings = unsafe {
        (|| -> Result<Config, u32> {
            let mut options = Vec::with_capacity(config.option_count);
            for index in 0..config.option_count {
                // SAFETY: `options` is non-null and has `option_count` entries.
                let entry = &*config.options.add(index);
                options.push(read_string(entry)?);
            }
            Ok(Config {
                design: read_string(&config.design)?,
                device: read_string(&config.device)?,
                log_path: read_string(&config.log_path)?,
                options,
                legality_mode: read_string(&config.legality_mode)?,
                controls_mode: read_string(&config.controls_mode)?,
            })
        })()
    };
    let config = match strings {
        Ok(config) => config,
        Err(status) => return status,
    };
    let dry = flags & MONITOR_DRY != 0;
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        Box::into_raw(Box::new(NpnrMonitor {
            inner: Mutex::new(Monitor::new(config)),
            dry,
            poisoned: AtomicBool::new(false),
        }))
    }));
    match outcome {
        Ok(raw) => {
            if !dry {
                terminal_write(TERMINAL_ENTER);
            }
            // SAFETY: checked non-null and aligned above.
            unsafe { output.write(raw) };
            CALL_OK
        }
        Err(payload) => {
            drop_payload(payload);
            CALL_PANIC
        }
    }
}

/// One log line (with or without its newline), from any thread.
///
/// # Safety
/// `handle` is a live handle from `npnr_mistral_monitor_create`; `text` is
/// readable for `len` bytes for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_monitor_log(
    handle: *mut NpnrMonitor,
    text: *const u8,
    len: usize,
) -> u32 {
    if handle.is_null() || (text.is_null() && len > 0) {
        return CALL_NULL;
    }
    if len > MAX_LOG_LINE {
        return CALL_BAD_RANGE;
    }
    // SAFETY: a live handle by the caller's contract.
    let monitor = unsafe { &*handle };
    if monitor.poisoned() {
        return CALL_PANIC;
    }
    // SAFETY: readable for `len` bytes by the caller's contract (or empty).
    let line = String::from_utf8_lossy(unsafe {
        if len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(text, len)
        }
    })
    .into_owned();
    let Ok(mut inner) = monitor.inner.lock() else {
        monitor.poison();
        return CALL_PANIC;
    };
    match catch_unwind(AssertUnwindSafe(|| inner.note_log_line(&line))) {
        Ok(()) => CALL_OK,
        Err(payload) => {
            drop_payload(payload);
            monitor.poison();
            CALL_PANIC
        }
    }
}

/// Render one tick. The frame goes to the terminal unless the handle or
/// `flags` is dry, and into `frame` (up to `frame_capacity` bytes, the count
/// written to `frame_len`) when `frame` is not null.
///
/// # Safety
/// `handle` is a live handle; `snapshot` is readable; `frame` is writable for
/// `frame_capacity` bytes when not null; `frame_len` is writable when not null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_monitor_render(
    handle: *mut NpnrMonitor,
    snapshot: *const NpnrMonitorSnapshotV1,
    flags: u32,
    frame: *mut u8,
    frame_capacity: usize,
    frame_len: *mut usize,
) -> u32 {
    if handle.is_null() || snapshot.is_null() {
        return CALL_NULL;
    }
    if snapshot.addr() % align_of::<NpnrMonitorSnapshotV1>() != 0
        || (!frame_len.is_null() && frame_len.addr() % align_of::<usize>() != 0)
    {
        return CALL_MISALIGNED;
    }
    // SAFETY: a live handle and a readable, aligned snapshot by the caller's contract.
    let (monitor, wire) = unsafe { (&*handle, &*snapshot) };
    if monitor.poisoned() {
        return CALL_PANIC;
    }
    let Some(phase) = Phase::from_index(wire.phase) else {
        return CALL_BAD_RANGE;
    };
    let l = &wire.legality;
    let r = &wire.resident;
    let c = &wire.controls;
    let snapshot = Snapshot {
        phase,
        columns: wire.columns,
        rows: wire.rows,
        checksum: wire.checksum,
        run_seconds: wire.run_seconds,
        phase_seconds: wire.phase_seconds,
        legality: Legality {
            evaluations: l[0],
            legal: l[1],
            illegal: l[2],
            errors: l[3],
            mismatches: l[4],
            stale_cache: l[5],
            stale_revision: l[6],
        },
        resident: Resident {
            evaluations: r[0],
            resets: r[1],
            trials: r[2],
            commits: r[3],
            restored: r[4],
        },
        controls: Controls {
            evaluations: c[0],
            preparation: c[1],
            legal: c[2],
            illegal: c[3],
            errors: c[4],
            mismatches: c[5],
            fallbacks: c[6],
        },
        cells: wire.cells,
        nets: wire.nets,
    };
    let Ok(mut inner) = monitor.inner.lock() else {
        monitor.poison();
        return CALL_PANIC;
    };
    let lines = match catch_unwind(AssertUnwindSafe(|| inner.frame(&snapshot))) {
        Ok(lines) => lines,
        Err(payload) => {
            drop_payload(payload);
            monitor.poison();
            return CALL_PANIC;
        }
    };
    drop(inner);
    if !monitor.dry && flags & MONITOR_DRY == 0 {
        terminal_write(&frame_text(&lines));
    }
    if !frame.is_null() {
        let text = lines.join("\n");
        let count = text.len().min(frame_capacity);
        // SAFETY: `frame` is writable for `frame_capacity` bytes by the caller's contract.
        unsafe { std::ptr::copy_nonoverlapping(text.as_ptr(), frame, count) };
        if !frame_len.is_null() {
            // SAFETY: checked aligned; writable by the caller's contract.
            unsafe { frame_len.write(count) };
        }
    } else if !frame_len.is_null() {
        // SAFETY: as above.
        unsafe { frame_len.write(0) };
    }
    CALL_OK
}

/// Ends the monitor: the terminal gets its cursor back (unless dry) and the
/// handle is freed. A null handle is a no-op.
///
/// # Safety
/// `handle` is null or a live handle from `npnr_mistral_monitor_create`, not
/// used again afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn npnr_mistral_monitor_destroy(handle: *mut NpnrMonitor) {
    if handle.is_null() {
        return;
    }
    // SAFETY: a live handle, owned by the caller, by the contract.
    let monitor = unsafe { Box::from_raw(handle) };
    if !monitor.dry {
        terminal_write(TERMINAL_LEAVE);
    }
    if let Err(payload) = catch_unwind(AssertUnwindSafe(|| drop(monitor))) {
        drop_payload(payload);
    }
}
