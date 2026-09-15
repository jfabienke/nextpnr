// SPDX-License-Identifier: ISC
//! One isolated test process: count allocations around repeated ABI calls only.
use npnr_mistral_lab::wire::*;
use npnr_mistral_lab_ffi::{CALL_OK, npnr_mistral_eval_controls_v1};
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};

struct CountAlloc;
static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);
// SAFETY: all operations forward the exact allocation contract to System. Counting
// adds no allocation, locks, or panicking operations.
unsafe impl GlobalAlloc for CountAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Relaxed);
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }
}
#[global_allocator]
static ALLOCATOR: CountAlloc = CountAlloc;

#[test]
fn repeated_abi_calls_do_not_allocate() {
    let mut inputs = [LabControlsV1::default(); 4];
    inputs[1].rules_version = 2;
    inputs[2].ff[0].reserved = 1;
    inputs[3].net_count = 2;
    for (slot, net) in [(0, 1), (39, 2)] {
        inputs[3].ff[slot].occupied = 1;
        inputs[3].ff[slot].control[0].net_id = net;
    }
    let mut outputs = [LabControlResultV1::default(); 4];
    let before = ALLOCATIONS.load(Relaxed);
    for _ in 0..100_000 {
        // SAFETY: disjoint live arrays, immutable input and exclusive output.
        let status = unsafe {
            npnr_mistral_eval_controls_v1(
                std::hint::black_box(inputs.as_ptr()),
                4,
                outputs.as_mut_ptr(),
                4,
            )
        };
        assert_eq!(status, CALL_OK);
        assert_eq!(
            outputs.map(|out| out.status),
            [LEGAL, UNSUPPORTED_RULES, BAD_SNAPSHOT, ILLEGAL]
        );
    }
    assert_eq!(ALLOCATIONS.load(Relaxed) - before, 0);
}
