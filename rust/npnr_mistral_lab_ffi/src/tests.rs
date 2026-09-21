// SPDX-License-Identifier: ISC
use super::*;
use npnr_mistral_lab::evaluate_wire;
use std::mem::MaybeUninit;
use std::ptr::{null, null_mut};
use std::sync::Mutex;

static FROZEN_BATCH_TEST_LOCK: Mutex<()> = Mutex::new(());

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

#[test]
fn frozen_batch_owns_validated_inputs_and_supports_concurrent_ranges() {
    let _serial = FROZEN_BATCH_TEST_LOCK.lock().unwrap();
    let mut inputs = [LabFactsV2::default(); 8];
    for (i, input) in inputs.iter_mut().enumerate() {
        input.request_id = 100 + i as u64;
    }
    let mut batch = null_mut();
    // SAFETY: separate initialized inputs and exclusive output pointer storage.
    assert_eq!(
        unsafe { npnr_mistral_frozen_batch_v2_create(inputs.as_ptr(), 8, 7, &mut batch) },
        CALL_OK
    );
    assert!(!batch.is_null());
    inputs.fill(LabFactsV2::default());

    std::thread::scope(|scope| {
        for range in 0..4 {
            let batch_addr = batch.addr();
            scope.spawn(move || {
                let batch = std::ptr::with_exposed_provenance::<NpnrLabFrozenBatchV2>(batch_addr);
                let mut outputs = [LabAssessmentV2::default(); 2];
                // SAFETY: the owner outlives this scope and each thread has private outputs.
                assert_eq!(
                    unsafe {
                        npnr_mistral_frozen_batch_v2_evaluate(
                            batch,
                            2 * range,
                            2,
                            outputs.as_mut_ptr(),
                            2,
                        )
                    },
                    CALL_OK
                );
                assert_eq!(outputs[0].request_id, 100 + (2 * range) as u64);
                assert_eq!(outputs[1].request_id, 101 + (2 * range) as u64);
            });
        }
    });
    // SAFETY: all scoped readers completed and this is the unique owner.
    unsafe { npnr_mistral_frozen_batch_v2_destroy(batch) };
}

#[test]
fn frozen_batch_rejects_malformed_inputs_and_enforces_worker_limit() {
    let _serial = FROZEN_BATCH_TEST_LOCK.lock().unwrap();
    let malformed = LabFactsV2 {
        reserved: 1,
        ..Default::default()
    };
    let mut batch = null_mut();
    // SAFETY: live input and exclusive output pointer storage.
    assert_eq!(
        unsafe { npnr_mistral_frozen_batch_v2_create(&malformed, 1, 9, &mut batch) },
        CALL_BAD_SNAPSHOT
    );
    assert!(batch.is_null());

    let valid = LabFactsV2::default();
    let mut first = null_mut();
    let mut second = null_mut();
    let mut third = null_mut();
    // SAFETY: each call uses live input and separate exclusive output storage.
    unsafe {
        assert_eq!(
            npnr_mistral_frozen_batch_v2_create(&valid, 1, 11, &mut first),
            CALL_OK
        );
        assert_eq!(
            npnr_mistral_frozen_batch_v2_create(&valid, 1, 11, &mut second),
            CALL_OK
        );
        assert_eq!(
            npnr_mistral_frozen_batch_v2_create(&valid, 1, 11, &mut third),
            CALL_LIMIT
        );
        assert!(third.is_null());
        npnr_mistral_frozen_batch_v2_destroy(first);
        npnr_mistral_frozen_batch_v2_destroy(second);
    }
}

#[test]
fn frozen_batch_cancellation_and_panics_fail_the_complete_range() {
    let _serial = FROZEN_BATCH_TEST_LOCK.lock().unwrap();
    let inputs = [LabFactsV2::default(); 3];
    let mut batch = null_mut();
    // SAFETY: initialized inputs and exclusive output pointer storage.
    assert_eq!(
        unsafe { npnr_mistral_frozen_batch_v2_create(inputs.as_ptr(), 3, 13, &mut batch) },
        CALL_OK
    );
    let sentinel = LabAssessmentV2 {
        reason: 12345,
        ..Default::default()
    };
    let mut outputs = [sentinel; 3];
    // SAFETY: batch is live and outputs are exclusive.
    assert_eq!(
        unsafe { npnr_mistral_frozen_batch_v2_cancel(batch) },
        CALL_OK
    );
    assert_eq!(
        unsafe { npnr_mistral_frozen_batch_v2_evaluate(batch, 0, 3, outputs.as_mut_ptr(), 3) },
        CALL_CANCELLED
    );
    assert_eq!(outputs, [sentinel; 3]);
    // SAFETY: batch remains live; helper uses the same checked output contract.
    let batch_ref = unsafe { &*batch };
    let mut calls = 0;
    batch_ref.cancelled.store(false, Ordering::Release);
    assert_eq!(
        unsafe {
            run_frozen_range(batch_ref, 0, 3, outputs.as_mut_ptr(), |_| {
                calls += 1;
                assert_ne!(calls, 2, "injected frozen evaluator panic");
                LabAssessmentV2::default()
            })
        },
        CALL_PANIC
    );
    assert_eq!(outputs, [LabAssessmentV2::default(); 3]);
    // SAFETY: unique owner after the synchronous calls.
    unsafe { npnr_mistral_frozen_batch_v2_destroy(batch) };
}

#[test]
fn frozen_batch_aggregate_retention_is_bounded() {
    let _serial = FROZEN_BATCH_TEST_LOCK.lock().unwrap();
    let inputs = [LabFactsV2::default(); MAX_BATCH as usize];
    let bytes = retained_batch_bytes(inputs.len()).unwrap();
    let maximum = MAX_RETAINED_BYTES / bytes;
    let mut batches = Vec::with_capacity(maximum);
    for worker in 0..maximum {
        let mut batch = null_mut();
        // SAFETY: initialized inputs and exclusive output pointer storage.
        assert_eq!(
            unsafe {
                npnr_mistral_frozen_batch_v2_create(
                    inputs.as_ptr(),
                    MAX_BATCH,
                    1_000 + worker as u64,
                    &mut batch,
                )
            },
            CALL_OK
        );
        batches.push(batch);
    }
    let mut excess = null_mut();
    // SAFETY: same valid envelope; aggregate quota rejects before publication.
    assert_eq!(
        unsafe {
            npnr_mistral_frozen_batch_v2_create(inputs.as_ptr(), MAX_BATCH, u64::MAX, &mut excess)
        },
        CALL_LIMIT
    );
    assert!(excess.is_null());
    for batch in batches {
        // SAFETY: each pointer is a distinct live handle with no readers.
        unsafe { npnr_mistral_frozen_batch_v2_destroy(batch) };
    }
}

#[test]
fn resident_handle_tracks_patches_and_rejects_bad_envelopes() {
    let mut handle: *mut NpnrLabResidentV2 = null_mut();
    assert_eq!(
        unsafe { npnr_mistral_resident_v2_create(0, 42, &mut handle) },
        CALL_BAD_COUNT
    );
    assert_eq!(
        unsafe { npnr_mistral_resident_v2_create(2, 42, null_mut()) },
        CALL_NULL
    );
    assert_eq!(
        unsafe { npnr_mistral_resident_v2_create(2, 42, &mut handle) },
        CALL_OK
    );
    assert!(!handle.is_null());
    let mut out = MaybeUninit::<LabVerdictV2>::uninit();
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_evaluate(
                handle,
                0,
                null(),
                0,
                QUERY_WHOLE_LAB,
                u32::MAX,
                RESIDENT_RECOMPUTE_COUNTS,
                out.as_mut_ptr(),
            )
        },
        CALL_BAD_SNAPSHOT
    );
    assert_eq!(
        unsafe { npnr_mistral_resident_v2_reset(handle, 2, 0) },
        CALL_BAD_RANGE
    );
    assert_eq!(
        unsafe { npnr_mistral_resident_v2_reset(handle, 0, 0) },
        CALL_OK
    );
    let mut lut = BelPatchV2 {
        alm: 3,
        slot: 0,
        commit: 1,
        ..BelPatchV2::default()
    };
    lut.lut.occupied = 1;
    lut.lut.input_count = 2;
    lut.lut.used_input_count = 2;
    lut.lut.bits_count = 4;
    lut.lut.mlab_group = -1;
    lut.lut.input_net[..2].copy_from_slice(&[4_000_000, 5]);
    lut.lut.comb_out_net = 6;
    let mut ff = BelPatchV2 {
        alm: 3,
        slot: 2,
        commit: 1,
        ..BelPatchV2::default()
    };
    ff.ff.occupied = 1;
    ff.ff.datain_net = 6;
    ff.ff.control[0].net_id = 9;
    let patches = [lut, ff];
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_evaluate(
                handle,
                0,
                patches.as_ptr(),
                2,
                QUERY_FF_BEL,
                3,
                RESIDENT_RECOMPUTE_COUNTS,
                out.as_mut_ptr(),
            )
        },
        CALL_OK
    );
    let result = unsafe { out.assume_init() };
    assert_eq!(result.status, LAB_LEGAL);
    assert_eq!(result.recomputed_input_count[3], 2);
    assert_eq!(result.control_valid, 1);
    // The same facts through the capture path agree on the verdict.
    let mut facts = LabFactsV2 {
        query: QUERY_FF_BEL,
        query_alm: 3,
        input_limit: 42,
        net_count: 4,
        ..LabFactsV2::default()
    };
    facts.alm[3].lut[0] = lut.lut;
    facts.alm[3].lut[0].input_net[..2].copy_from_slice(&[1, 2]);
    facts.alm[3].lut[0].comb_out_net = 3;
    facts.alm[3].ff[0] = ff.ff;
    facts.alm[3].ff[0].datain_net = 3;
    facts.alm[3].ff[0].control[0].net_id = 4;
    let mut reference = MaybeUninit::<LabAssessmentV2>::uninit();
    assert_eq!(
        unsafe { npnr_mistral_eval_lab_v2(&facts, 1, reference.as_mut_ptr(), 1) },
        CALL_OK
    );
    let reference = unsafe { reference.assume_init() };
    assert_eq!(result, LabVerdictV2::from(&reference));
    // A malformed patch is reported in the verdict and leaves the LAB untouched.
    let mut bad = lut;
    bad.lut.used_input_count = 5;
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_evaluate(
                handle,
                0,
                &bad,
                1,
                QUERY_COMB_BEL,
                3,
                RESIDENT_RECOMPUTE_COUNTS,
                out.as_mut_ptr(),
            )
        },
        CALL_OK
    );
    let result = unsafe { out.assume_init() };
    assert_eq!((result.status, result.reason), (LAB_MALFORMED, BAD_SHAPE));
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_evaluate(
                handle,
                0,
                null(),
                0,
                QUERY_COMB_BEL,
                3,
                RESIDENT_RECOMPUTE_COUNTS,
                out.as_mut_ptr(),
            )
        },
        CALL_OK
    );
    assert_eq!(unsafe { out.assume_init() }.status, LAB_LEGAL);
    // Envelope failures.
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_evaluate(
                handle,
                0,
                null(),
                1,
                QUERY_COMB_BEL,
                3,
                RESIDENT_RECOMPUTE_COUNTS,
                out.as_mut_ptr(),
            )
        },
        CALL_NULL
    );
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_evaluate(
                handle,
                0,
                &lut,
                61,
                QUERY_COMB_BEL,
                3,
                RESIDENT_RECOMPUTE_COUNTS,
                out.as_mut_ptr(),
            )
        },
        CALL_BAD_COUNT
    );
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_evaluate(
                handle,
                5,
                null(),
                0,
                QUERY_COMB_BEL,
                3,
                RESIDENT_RECOMPUTE_COUNTS,
                out.as_mut_ptr(),
            )
        },
        CALL_BAD_RANGE
    );
    unsafe { npnr_mistral_resident_v2_destroy(handle) };
    unsafe { npnr_mistral_resident_v2_destroy(null_mut()) };
}

#[test]
fn resident_scan_names_the_first_legal_bel_and_rejects_bad_envelopes() {
    let mut handle: *mut NpnrLabResidentV2 = std::ptr::null_mut();
    assert_eq!(
        unsafe { npnr_mistral_resident_v2_create(1, 42, &mut handle) },
        CALL_OK
    );
    assert_eq!(
        unsafe { npnr_mistral_resident_v2_reset(handle, 0, 0) },
        CALL_OK
    );
    // A five-input LUT in the first half of ALM 0, committed.
    let mut held = BelPatchV2 {
        commit: 1,
        ..BelPatchV2::default()
    };
    held.lut.occupied = 1;
    held.lut.input_count = 5;
    held.lut.used_input_count = 5;
    held.lut.bits_count = 32;
    held.lut.mlab_group = -1;
    for (i, net) in held.lut.input_net.iter_mut().take(5).enumerate() {
        *net = 100 + i as u32;
    }
    held.alm_inputs = 5;
    // The candidate: another five-input LUT sharing nothing, so ALM 0's other half refuses it
    // (ten inputs) and any empty ALM takes it.
    let mut candidate = BelPatchV2 {
        lut: held.lut,
        ..BelPatchV2::default()
    };
    for (i, net) in candidate.lut.input_net.iter_mut().take(5).enumerate() {
        *net = 200 + i as u32;
    }
    let order = [1u8, 6, 12]; // ALM 0 half 1, ALM 1 half 0, ALM 2 half 0
    let mut first = 7u32;
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_scan(
                handle,
                0,
                &held,
                1,
                &candidate,
                order.as_ptr(),
                order.len() as u32,
                0,
                &mut first,
            )
        },
        CALL_OK
    );
    assert_eq!(
        first, 1,
        "the second bel of the order is the first legal one"
    );
    // Only the refused bel: nothing is legal.
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_scan(
                handle,
                0,
                std::ptr::null(),
                0,
                &candidate,
                order.as_ptr(),
                1,
                0,
                &mut first,
            )
        },
        CALL_OK
    );
    assert_eq!(first, RESIDENT_SCAN_NONE);
    // The scan applied nothing: the per-bel evaluation of the refused half agrees, and the
    // occupied bel is not a bel to scan.
    let mut trial = candidate;
    trial.alm = 0;
    trial.slot = 1;
    trial.alm_inputs = 10;
    let mut verdict = MaybeUninit::<LabVerdictV2>::uninit();
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_evaluate(
                handle,
                0,
                &trial,
                1,
                QUERY_COMB_BEL,
                0,
                0,
                verdict.as_mut_ptr(),
            )
        },
        CALL_OK
    );
    assert_eq!(unsafe { verdict.assume_init() }.status, LAB_ILLEGAL);
    let occupied = [0u8];
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_scan(
                handle,
                0,
                std::ptr::null(),
                0,
                &candidate,
                occupied.as_ptr(),
                1,
                0,
                &mut first,
            )
        },
        CALL_BAD_SNAPSHOT
    );
    assert_eq!(first, RESIDENT_SCAN_NONE);
    let register_bel = [2u8]; // a register bel for a LUT candidate
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_scan(
                handle,
                0,
                std::ptr::null(),
                0,
                &candidate,
                register_bel.as_ptr(),
                1,
                0,
                &mut first,
            )
        },
        CALL_BAD_SNAPSHOT
    );
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_scan(
                handle,
                0,
                std::ptr::null(),
                0,
                std::ptr::null(),
                order.as_ptr(),
                3,
                0,
                &mut first,
            )
        },
        CALL_NULL
    );
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_scan(
                handle,
                0,
                std::ptr::null(),
                0,
                &candidate,
                order.as_ptr(),
                61,
                0,
                &mut first,
            )
        },
        CALL_BAD_COUNT
    );
    assert_eq!(
        unsafe {
            npnr_mistral_resident_v2_scan(
                handle,
                9,
                std::ptr::null(),
                0,
                &candidate,
                order.as_ptr(),
                3,
                0,
                &mut first,
            )
        },
        CALL_BAD_RANGE
    );
    unsafe { npnr_mistral_resident_v2_destroy(handle) };
}

mod monitor_tests {
    use crate::*;
    use std::mem::MaybeUninit;

    fn string(text: &str) -> NpnrMonitorStringV1 {
        NpnrMonitorStringV1 {
            ptr: text.as_ptr(),
            len: text.len(),
        }
    }

    fn snapshot(phase: u32) -> NpnrMonitorSnapshotV1 {
        NpnrMonitorSnapshotV1 {
            phase,
            columns: 80,
            rows: 40,
            checksum: 0xbb18_ede9,
            run_seconds: 22.0,
            phase_seconds: [0.0, 0.5, 0.8, 13.9, 0.2, 6.6, 0.0, 0.0, 0.0],
            legality: [5_570_583, 1_156_345, 4_414_238, 0, 0, 0, 0],
            resident: [5_570_583, 4_191, 4_699_227, 1_053_914, 4_940_434],
            controls: [0; 7],
            cells: 12_169,
            nets: 13_396,
        }
    }

    #[test]
    fn monitor_round_trip_renders_the_frame_into_the_caller_buffer() {
        let options = [
            string("lab-legality rust   sa-seam on"),
            string("row-cost 0"),
        ];
        let config = NpnrMonitorConfigV1 {
            design: string("f386_exec_probe_nodsp"),
            device: string("5CSEBA6U23I7"),
            log_path: string("/tmp/run.log"),
            legality_mode: string("rust"),
            controls_mode: string("legacy"),
            options: options.as_ptr(),
            option_count: options.len(),
        };
        let mut handle: *mut NpnrMonitor = std::ptr::null_mut();
        assert_eq!(
            unsafe { npnr_mistral_monitor_create(&config, MONITOR_DRY, &mut handle) },
            CALL_OK
        );
        assert!(!handle.is_null());
        let line = b"Info:     iter=3 wires=455000 overused=12 overuse=20 archfail=NA\n";
        assert_eq!(
            unsafe { npnr_mistral_monitor_log(handle, line.as_ptr(), line.len()) },
            CALL_OK
        );
        assert_eq!(
            unsafe { npnr_mistral_monitor_log(handle, std::ptr::null(), 0) },
            CALL_OK
        );
        let mut buffer = vec![0u8; 16 * 1024];
        let mut len = 0usize;
        let wire = snapshot(5);
        assert_eq!(
            unsafe {
                npnr_mistral_monitor_render(
                    handle,
                    &wire,
                    0,
                    buffer.as_mut_ptr(),
                    buffer.len(),
                    &mut len,
                )
            },
            CALL_OK
        );
        let frame = String::from_utf8_lossy(&buffer[..len]).into_owned();
        assert!(frame.contains("phase: ROUTE"));
        assert!(frame.contains("LAB LEGALITY (rust)   evaluations 5,570,583"));
        assert!(frame.contains("ROUTER  #3  wires 455,000  overused 12"));
        assert!(frame.contains("lab-legality rust   sa-seam on"));
        assert!(frame.contains("Info:     iter=3 wires=455000"));
        for line in frame.lines() {
            assert_eq!(line.chars().count(), 80, "{line:?}");
        }
        // A truncating buffer copies what fits and says so.
        let mut small = [0u8; 10];
        let mut small_len = 0usize;
        assert_eq!(
            unsafe {
                npnr_mistral_monitor_render(
                    handle,
                    &wire,
                    0,
                    small.as_mut_ptr(),
                    10,
                    &mut small_len,
                )
            },
            CALL_OK
        );
        assert_eq!(small_len, 10);
        assert_eq!(&small, b"+---------");
        // Envelope errors.
        assert_eq!(
            unsafe {
                npnr_mistral_monitor_render(
                    handle,
                    &snapshot(99),
                    0,
                    std::ptr::null_mut(),
                    0,
                    std::ptr::null_mut(),
                )
            },
            CALL_BAD_RANGE
        );
        assert_eq!(
            unsafe {
                npnr_mistral_monitor_render(
                    std::ptr::null_mut(),
                    &wire,
                    0,
                    std::ptr::null_mut(),
                    0,
                    std::ptr::null_mut(),
                )
            },
            CALL_NULL
        );
        assert_eq!(
            unsafe { npnr_mistral_monitor_log(handle, std::ptr::null(), 4) },
            CALL_NULL
        );
        unsafe { npnr_mistral_monitor_destroy(handle) };
        unsafe { npnr_mistral_monitor_destroy(std::ptr::null_mut()) };
    }

    #[test]
    fn monitor_create_rejects_bad_envelopes() {
        let mut handle: *mut NpnrMonitor = std::ptr::null_mut();
        assert_eq!(
            unsafe { npnr_mistral_monitor_create(std::ptr::null(), MONITOR_DRY, &mut handle) },
            CALL_NULL
        );
        let config = NpnrMonitorConfigV1 {
            design: string(""),
            device: string(""),
            log_path: string(""),
            legality_mode: string(""),
            controls_mode: string(""),
            options: std::ptr::null(),
            option_count: MAX_OPTION_LINES + 1,
        };
        assert_eq!(
            unsafe { npnr_mistral_monitor_create(&config, MONITOR_DRY, &mut handle) },
            CALL_BAD_COUNT
        );
        let config = NpnrMonitorConfigV1 {
            options: std::ptr::null(),
            option_count: 1,
            ..config
        };
        assert_eq!(
            unsafe { npnr_mistral_monitor_create(&config, MONITOR_DRY, &mut handle) },
            CALL_NULL
        );
        assert!(handle.is_null());
        let mut slot = MaybeUninit::<*mut NpnrMonitor>::uninit();
        assert_eq!(
            unsafe {
                npnr_mistral_monitor_create(
                    &config,
                    MONITOR_DRY,
                    slot.as_mut_ptr().cast::<u8>().add(1).cast(),
                )
            },
            CALL_MISALIGNED
        );
    }
}
