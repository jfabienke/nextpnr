// SPDX-License-Identifier: ISC
use super::*;
use npnr_mistral_lab::evaluate_wire;
use std::mem::MaybeUninit;
use std::ptr::{null, null_mut};

#[test]
fn empty_and_rejected_envelopes_do_not_touch_outputs() {
    let input = LabControlsV1::default();
    let sentinel = LabControlResultV1 {
        reason: 12345,
        ..Default::default()
    };
    let mut output = sentinel;
    // SAFETY: rejected envelopes are checked before any pointer dereference.
    unsafe {
        assert_eq!(
            npnr_mistral_eval_controls_v1(null(), 0, null_mut(), 0),
            CALL_OK
        );
        assert_eq!(
            npnr_mistral_eval_controls_v1(&input, 0, &mut output, 0),
            CALL_OK
        );
        assert_eq!(
            npnr_mistral_eval_controls_v1(null(), MAX_BATCH + 1, &mut output, 0),
            CALL_BAD_COUNT
        );
        assert_eq!(
            npnr_mistral_eval_controls_v1(&input, 1, &mut output, 0),
            CALL_BAD_CAPACITY
        );
        assert_eq!(
            npnr_mistral_eval_controls_v1(null(), 1, &mut output, 1),
            CALL_NULL
        );
        assert_eq!(
            npnr_mistral_eval_controls_v1(&input, 1, null_mut(), 1),
            CALL_NULL
        );
        let misaligned = (&input as *const LabControlsV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast();
        assert_eq!(
            npnr_mistral_eval_controls_v1(misaligned, 1, &mut output, 1),
            CALL_MISALIGNED
        );
        let misaligned = (&mut output as *mut LabControlResultV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast();
        assert_eq!(
            npnr_mistral_eval_controls_v1(&input, 1, misaligned, 1),
            CALL_MISALIGNED
        );
    }
    assert_eq!(output, sentinel);
}

#[test]
fn ranges_are_checked_before_references_are_formed() {
    let mut input = LabControlsV1::default();
    let original = input;
    let p = &mut input as *mut LabControlsV1;
    // SAFETY: overlapping ranges are rejected without accessing either pointer.
    unsafe {
        assert_eq!(
            npnr_mistral_eval_controls_v1(p, 1, p.cast(), 1),
            CALL_OVERLAP
        );
        assert_eq!(
            npnr_mistral_eval_controls_v1(p, 1, p.cast::<u8>().add(8).cast(), 1),
            CALL_OVERLAP
        );
    }
    assert_eq!(input, original);
    let mut output = LabControlResultV1::default();
    let high = std::ptr::without_provenance::<LabControlsV1>(
        usize::MAX & !(align_of::<LabControlsV1>() - 1),
    );
    // SAFETY: checked_add rejects the wrapped range before dereferencing high.
    assert_eq!(
        unsafe { npnr_mistral_eval_controls_v1(high, 1, &mut output, 1) },
        CALL_BAD_RANGE
    );
}

#[test]
fn mixed_batch_initializes_only_the_requested_outputs() {
    let mut inputs = [LabControlsV1::default(); MAX_BATCH as usize];
    for (i, input) in inputs.iter_mut().enumerate() {
        input.request_id = u64::MAX - i as u64;
        input.snapshot_epoch = 1 << 63;
        match i % 4 {
            0 => {}
            1 => input.rules_version += 1,
            2 => input.ff[39].reserved = 1,
            _ => {
                input.net_count = 2;
                input.ff[0].occupied = 1;
                input.ff[0].control[0].net_id = 1;
                input.ff[39].occupied = 1;
                input.ff[39].control[0].net_id = 2;
            }
        }
    }
    let original = inputs;
    let mut outputs = [MaybeUninit::<LabControlResultV1>::uninit(); MAX_BATCH as usize + 1];
    let sentinel = LabControlResultV1 {
        reason: 999,
        ..Default::default()
    };
    outputs[MAX_BATCH as usize].write(sentinel);
    // SAFETY: initialized inputs and exclusive uninitialized output storage are
    // separate live arrays, correctly aligned, with the advertised capacities.
    let status = unsafe {
        npnr_mistral_eval_controls_v1(
            inputs.as_ptr(),
            MAX_BATCH,
            outputs.as_mut_ptr().cast(),
            MAX_BATCH + 1,
        )
    };
    assert_eq!(status, CALL_OK);
    for (input, output) in inputs.iter().zip(&outputs) {
        // SAFETY: CALL_OK initialized the first MAX_BATCH outputs.
        assert_eq!(unsafe { output.assume_init_ref() }, &evaluate_wire(input));
    }
    // SAFETY: initialized sentinel above, outside the active output range.
    assert_eq!(
        unsafe { outputs[MAX_BATCH as usize].assume_init() },
        sentinel
    );
    assert_eq!(inputs, original);
}

#[test]
fn panic_invalidates_the_entire_batch_and_the_bridge_recovers() {
    let inputs = [LabControlsV1::default(); 3];
    let mut outputs = [LabControlResultV1::default(); 3];
    let mut calls = 0;
    // SAFETY: separate fully initialized arrays, same contract as the exported API.
    let status = unsafe {
        run_batch(
            inputs.as_ptr(),
            inputs.len(),
            outputs.as_mut_ptr(),
            |input, output| {
                calls += 1;
                assert_ne!(calls, 2, "injected evaluator panic");
                *output = evaluate_wire(input);
            },
        )
    };
    assert_eq!(status, CALL_PANIC);
    for (input, output) in inputs.iter().zip(&outputs) {
        assert_eq!(*output, failed_result(input));
    }
    // SAFETY: same valid caller-owned arrays can be reused after a contained panic.
    assert_eq!(
        unsafe { npnr_mistral_eval_controls_v1(inputs.as_ptr(), 3, outputs.as_mut_ptr(), 3) },
        CALL_OK
    );
    assert!(outputs.iter().all(|out| out.status == LEGAL));
}

#[test]
fn a_panicking_payload_destructor_cannot_escape() {
    struct BadDrop;
    impl Drop for BadDrop {
        fn drop(&mut self) {
            panic!("injected panic payload destructor");
        }
    }
    let input = LabControlsV1::default();
    let mut output = LabControlResultV1::default();
    // SAFETY: separate live records; injected panic remains inside run_batch.
    assert_eq!(
        unsafe {
            run_batch(&input, 1, &mut output, |_, _| {
                std::panic::panic_any(BadDrop)
            })
        },
        CALL_PANIC
    );
    assert_eq!(output, failed_result(&input));
}

#[test]
fn concurrent_calls_share_only_immutable_inputs() {
    let inputs = [LabControlsV1::default(); 4];
    std::thread::scope(|scope| {
        for _ in 0..8 {
            let inputs = &inputs;
            scope.spawn(move || {
                let mut outputs = [LabControlResultV1::default(); 4];
                for _ in 0..1000 {
                    // SAFETY: shared immutable inputs; each thread exclusively owns outputs.
                    assert_eq!(
                        unsafe {
                            npnr_mistral_eval_controls_v1(
                                inputs.as_ptr(),
                                4,
                                outputs.as_mut_ptr(),
                                4,
                            )
                        },
                        CALL_OK
                    );
                    assert!(outputs.iter().all(|out| out.status == LEGAL));
                }
            });
        }
    });
}
