// SPDX-License-Identifier: ISC
use crate::{
    BoundaryError, ControlAssessment, ControlLabSnapshot, evaluate, evaluate_wire, wire::*,
};

fn signal(net_id: u32, flags: u32) -> ControlSignalV1 {
    ControlSignalV1 { net_id, flags }
}

#[test]
fn disconnected_polarity_and_provenance_survive_validation() {
    let mut input = LabControlsV1 {
        request_id: u64::MAX,
        snapshot_epoch: u64::MAX - 1,
        ..Default::default()
    };
    input.ff[39].occupied = 1;
    input.ff[39].control[0] = signal(0, INVERTED);
    let before = input;
    let snapshot = ControlLabSnapshot::try_from(&input).unwrap();
    let assessment = evaluate(&snapshot);
    assert!(matches!(assessment, ControlAssessment::Legal(_)));
    let out = assessment.to_wire();
    assert_eq!(out, evaluate_wire(&input));
    assert_eq!(out.request_id, u64::MAX);
    assert_eq!(out.snapshot_epoch, u64::MAX - 1);
    assert_eq!(out.allocation, [signal(0, 0); ALLOCATION_COUNT]);
    assert_eq!(out.ff_slot, u32::MAX);
    assert_eq!(input, before);
    // The owned snapshot is independent of subsequent changes to the transport.
    input.ff[39].occupied = 2;
    assert_eq!(evaluate(&snapshot).to_wire(), out);
    assert_eq!(evaluate_wire(&input).status, BAD_SNAPSHOT);
}

#[test]
fn capacities_and_first_blocker_origins() {
    for (kind, capacity) in [1, 1, 1, 2, 3].into_iter().enumerate() {
        let mut input = LabControlsV1 {
            net_count: capacity + 1,
            ..Default::default()
        };
        for i in 0..=capacity as usize {
            input.ff[i].occupied = 1;
            input.ff[i].control[kind] = signal(i as u32 + 1, 0);
        }
        let out = evaluate_wire(&input);
        assert_eq!(out.status, ILLEGAL);
        assert_eq!(out.reason, kind as u32 + 1);
        assert_eq!(out.control_kind, kind as u32);
        assert_eq!(out.ff_slot, capacity);
        assert_eq!(out.resource_mask, (1 << capacity) - 1);
        assert_eq!(out.allocation, [signal(0, 0); ALLOCATION_COUNT]);
        for i in 0..capacity as usize {
            assert_eq!(out.blockers[i].signal, signal(i as u32 + 1, 0));
            assert_eq!(out.blockers[i].ff_slot, i as u32);
        }
    }
}

#[test]
fn only_global_clocks_avoid_datain() {
    let mut input = LabControlsV1 {
        net_count: 4,
        ..Default::default()
    };
    for i in 0..3 {
        input.ff[i].occupied = 1;
        input.ff[i].control[0] = signal(1, 0);
        input.ff[i].control[4] = signal(i as u32 + 2, GLOBAL); // global enables still consume DATAIN
    }
    let local = evaluate_wire(&input);
    assert_eq!(
        (local.status, local.reason, local.resource_mask),
        (ILLEGAL, 6, 13)
    );
    for ff in &mut input.ff[..3] {
        ff.control[0].flags = GLOBAL;
    }
    let global = evaluate_wire(&input);
    assert_eq!(global.status, LEGAL);
    assert_eq!(global.allocation[8], signal(4, GLOBAL));
    assert_eq!(global.allocation[10], signal(2, GLOBAL));
    assert_eq!(global.allocation[11], signal(3, GLOBAL));
}

#[test]
fn all_slots_and_all_net_ids_are_bounded() {
    let mut input = LabControlsV1 {
        net_count: MAX_NETS as u32,
        ..Default::default()
    };
    for (i, ff) in input.ff.iter_mut().enumerate() {
        ff.occupied = 1;
        for (j, control) in ff.control.iter_mut().enumerate() {
            *control = signal((i * 5 + j + 1) as u32, 0);
        }
    }
    assert!(ControlLabSnapshot::try_from(&input).is_ok());
    assert_eq!(evaluate_wire(&input).status, ILLEGAL);
    for slot in 0..FF_COUNT {
        let mut input = LabControlsV1 {
            net_count: 1,
            ..Default::default()
        };
        input.ff[slot].occupied = 1;
        input.ff[slot].control[0] = signal(1, INVERTED);
        assert_eq!(evaluate_wire(&input).status, LEGAL); // full ALM slot rules are separate
    }
}

#[test]
fn invalid_transport_is_not_a_placement_rejection() {
    use BoundaryError::*;
    type InvalidCase = (fn(&mut LabControlsV1), BoundaryError);
    let cases: [InvalidCase; 12] = [
        (|s| s.abi_version = 2, BadAbi),
        (|s| s.struct_size -= 1, BadSize),
        (|s| s.rules_version = 2, UnsupportedRules),
        (|s| s.net_count = 201, BadNetCount),
        (|s| s.ff[0].occupied = 2, BadOccupancy),
        (|s| s.ff[39].reserved = 1, BadReserved),
        (|s| s.ff[0].control[0].flags = INVERTED, BadSignal),
        (
            |s| {
                s.ff[0].occupied = 1;
                s.ff[0].control[0].flags = 4;
            },
            BadSignal,
        ),
        (
            |s| {
                s.ff[0].occupied = 1;
                s.ff[0].control[0].flags = GLOBAL;
            },
            BadSignal,
        ),
        (
            |s| {
                s.ff[0].occupied = 1;
                s.ff[0].control[0].net_id = u32::MAX;
            },
            BadSignal,
        ),
        (|s| s.net_count = 1, SparseNetIds),
        (
            |s| {
                s.net_count = 1;
                s.ff[0].occupied = 1;
                s.ff[0].control[0] = signal(1, 0);
                s.ff[0].control[1] = signal(1, GLOBAL);
            },
            InconsistentGlobal,
        ),
    ];
    for (change, error) in cases {
        let mut input = LabControlsV1::default();
        change(&mut input);
        assert_eq!(ControlLabSnapshot::try_from(&input).unwrap_err(), error);
        let out = evaluate_wire(&input);
        assert_eq!(out.reason, error as u32);
        assert_eq!(
            out.status,
            if error == UnsupportedRules {
                UNSUPPORTED_RULES
            } else {
                BAD_SNAPSHOT
            }
        );
        assert_eq!(out.allocation, [signal(0, 0); ALLOCATION_COUNT]);
        assert_eq!(out.blockers, [LabControlBlockerV1::default(); 4]);
    }
}

#[test]
fn frozen_snapshot_supports_independent_workers() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ControlLabSnapshot>();
    assert_send_sync::<ControlAssessment<'static>>();
    let mut input = LabControlsV1 {
        net_count: 1,
        ..Default::default()
    };
    input.ff[39].occupied = 1;
    input.ff[39].control[0] = signal(1, GLOBAL);
    let snapshot = ControlLabSnapshot::try_from(&input).unwrap();
    let expected = evaluate(&snapshot).to_wire();
    std::thread::scope(|scope| {
        for _ in 0..8 {
            let snapshot = &snapshot;
            scope.spawn(move || {
                for _ in 0..1000 {
                    assert_eq!(evaluate(snapshot).to_wire(), expected);
                }
            });
        }
    });
}

#[test]
fn representation_budgets() {
    use core::mem::size_of;
    assert_eq!(size_of::<LabControlsV1>(), 1952);
    assert_eq!(size_of::<LabControlResultV1>(), 216);
    assert!(size_of::<ControlLabSnapshot>() <= 2048);
    assert!(size_of::<crate::rules::Worker>() <= 256);
    assert!(size_of::<ControlAssessment<'static>>() <= 256);
    std::println!(
        "wire_input={} typed_snapshot={} worker={} assessment={} wire_result={}",
        size_of::<LabControlsV1>(),
        size_of::<ControlLabSnapshot>(),
        size_of::<crate::rules::Worker>(),
        size_of::<ControlAssessment<'static>>(),
        size_of::<LabControlResultV1>()
    );
}
