// SPDX-License-Identifier: ISC
use crate::model::{ControlKind, ControlLabSnapshot, ControlSignal, FfSlot, RejectionReason};
use crate::wire::{self, ALLOCATION_COUNT, LabControlResultV1};

/// A completed control subcheck. This does not certify an entire LAB or move.
#[derive(Debug)]
pub enum ControlAssessment<'a> {
    Legal(ControlAllocation<'a>),
    Illegal(ControlConflict<'a>),
}

/// An allocation is constructed only by successful evaluation and is tied to
/// the snapshot that supplies its local net identities and provenance.
#[derive(Debug)]
pub struct ControlAllocation<'a> {
    snapshot: &'a ControlLabSnapshot,
    signals: [ControlSignal; ALLOCATION_COUNT],
}

/// The first failed legacy operation, including bounded blocker provenance.
#[derive(Debug)]
pub struct ControlConflict<'a> {
    snapshot: &'a ControlLabSnapshot,
    failure: Failure,
}

impl ControlConflict<'_> {
    pub fn reason(&self) -> RejectionReason {
        self.failure.reason
    }
    pub fn control_kind(&self) -> ControlKind {
        self.failure.kind
    }
}

impl ControlAssessment<'_> {
    fn wire_header(&self) -> (u64, u64, u32) {
        match self {
            Self::Legal(plan) => (
                plan.snapshot.request_id,
                plan.snapshot.snapshot_epoch,
                wire::LEGAL,
            ),
            Self::Illegal(conflict) => (
                conflict.snapshot.request_id,
                conflict.snapshot.snapshot_epoch,
                wire::ILLEGAL,
            ),
        }
    }

    fn populate_wire(&self, result: &mut LabControlResultV1) {
        match self {
            Self::Legal(plan) => {
                for (out, signal) in result.allocation.iter_mut().zip(plan.signals) {
                    *out = signal.to_wire(plan.snapshot);
                }
            }
            Self::Illegal(conflict) => {
                let failure = &conflict.failure;
                result.reason = failure.reason as u32;
                result.control_kind = failure.kind as u32;
                result.ff_slot = failure.incoming.origin.map_or(u32::MAX, FfSlot::index);
                result.incoming = failure.incoming.signal.to_wire(conflict.snapshot);
                result.resource_mask = u32::from(failure.mask);
                for (out, blocker) in result.blockers.iter_mut().zip(failure.blockers) {
                    out.signal = blocker.signal.to_wire(conflict.snapshot);
                    out.ff_slot = blocker.origin.map_or(u32::MAX, FfSlot::index);
                }
            }
        }
    }

    pub(crate) fn write_wire(&self, result: &mut LabControlResultV1) {
        let (request_id, snapshot_epoch, status) = self.wire_header();
        *result = LabControlResultV1::empty(request_id, snapshot_epoch, status);
        self.populate_wire(result);
    }

    /// Copy to the value ABI. No caller-supplied snapshot can be substituted.
    pub fn to_wire(&self) -> LabControlResultV1 {
        let (request_id, snapshot_epoch, status) = self.wire_header();
        let mut result = LabControlResultV1::empty(request_id, snapshot_epoch, status);
        self.populate_wire(&mut result);
        result
    }
}

#[derive(Clone, Copy, Debug)]
struct Assigned {
    signal: ControlSignal,
    origin: Option<FfSlot>,
}
impl Assigned {
    const EMPTY: Self = Self {
        signal: ControlSignal::EMPTY,
        origin: None,
    };
}

#[derive(Debug)]
struct Failure {
    reason: RejectionReason,
    kind: ControlKind,
    incoming: Assigned,
    mask: u8,
    blockers: [Assigned; 4],
}

// These indices are private constants, never decoded from transport integers.
#[derive(Clone, Copy)]
#[repr(usize)]
enum Resource {
    Clock,
    Sload,
    Sclr,
    Aclr0,
    Aclr1,
    Ena0,
    Ena1,
    Ena2,
    Datain0,
    Datain1,
    Datain2,
    Datain3,
}

struct Choice {
    resource: Resource,
    /// Bit/index used to report this slot in a conflict.
    diagnostic: usize,
}

impl Choice {
    const fn new(resource: Resource, diagnostic: usize) -> Self {
        Self {
            resource,
            diagnostic,
        }
    }
}

use Resource::*;
const CLOCK_POOL: [Choice; 1] = [Choice::new(Clock, 0)];
const SLOAD_POOL: [Choice; 1] = [Choice::new(Sload, 0)];
const SCLR_POOL: [Choice; 1] = [Choice::new(Sclr, 0)];
const ACLR_POOL: [Choice; 2] = [Choice::new(Aclr0, 0), Choice::new(Aclr1, 1)];
const ENA_POOL: [Choice; 3] = [
    Choice::new(Ena0, 0),
    Choice::new(Ena1, 1),
    Choice::new(Ena2, 2),
];
const CLOCK_DATA: [Choice; 1] = [Choice::new(Datain0, 0)];
const SLOAD_DATA: [Choice; 1] = [Choice::new(Datain1, 1)];
const SCLR_DATA: [Choice; 1] = [Choice::new(Datain3, 3)];
const ACLR_DATA: [Choice; 2] = [Choice::new(Datain3, 3), Choice::new(Datain2, 2)];
const ENA_DATA: [Choice; 3] = [
    Choice::new(Datain2, 2),
    Choice::new(Datain3, 3),
    Choice::new(Datain0, 0),
];

pub(crate) struct Worker {
    slots: [Assigned; ALLOCATION_COUNT],
}

impl Worker {
    fn assign(
        &mut self,
        incoming: Assigned,
        kind: ControlKind,
        choices: &[Choice],
        reason: RejectionReason,
    ) -> Result<(), Failure> {
        if !incoming.signal.connected() {
            return Ok(());
        }
        for choice in choices {
            let slot = &mut self.slots[choice.resource as usize];
            if slot.signal == incoming.signal {
                return Ok(());
            }
            if !slot.signal.connected() {
                *slot = incoming;
                return Ok(());
            }
        }
        let mut failure = Failure {
            reason,
            kind,
            incoming,
            mask: 0,
            blockers: [Assigned::EMPTY; 4],
        };
        for choice in choices {
            failure.mask |= 1 << choice.diagnostic;
            failure.blockers[choice.diagnostic] = self.slots[choice.resource as usize];
        }
        Err(failure)
    }

    fn run(&mut self, snapshot: &ControlLabSnapshot) -> Result<(), Failure> {
        // Keep physical FF order and per-FF CLK/SLOAD/SCLR/ACLR/ENA order.
        for (index, ff) in snapshot.ffs.iter().enumerate() {
            let Some(controls) = ff else {
                continue;
            };
            for (kind, signal) in ControlKind::ALL.into_iter().zip(controls) {
                let (choices, reason): (&[Choice], _) = match kind {
                    ControlKind::Clock => (&CLOCK_POOL, RejectionReason::ClockConflict),
                    ControlKind::Sload => (&SLOAD_POOL, RejectionReason::SloadConflict),
                    ControlKind::Sclr => (&SCLR_POOL, RejectionReason::SclrConflict),
                    ControlKind::Aclr => (&ACLR_POOL, RejectionReason::AclrCapacity),
                    ControlKind::Ena => (&ENA_POOL, RejectionReason::EnaCapacity),
                };
                self.assign(
                    Assigned {
                        signal: *signal,
                        origin: Some(FfSlot::from_index(index)),
                    },
                    kind,
                    choices,
                    reason,
                )?;
            }
        }

        let clock = self.slots[Clock as usize];
        if !snapshot.is_global(clock.signal) {
            self.assign(
                clock,
                ControlKind::Clock,
                &CLOCK_DATA,
                RejectionReason::DatainConflict,
            )?;
        }
        self.assign(
            self.slots[Sload as usize],
            ControlKind::Sload,
            &SLOAD_DATA,
            RejectionReason::DatainConflict,
        )?;
        self.assign(
            self.slots[Sclr as usize],
            ControlKind::Sclr,
            &SCLR_DATA,
            RejectionReason::DatainConflict,
        )?;
        for resource in [Aclr0, Aclr1] {
            self.assign(
                self.slots[resource as usize],
                ControlKind::Aclr,
                &ACLR_DATA,
                RejectionReason::DatainConflict,
            )?;
        }
        for resource in [Ena0, Ena1, Ena2] {
            self.assign(
                self.slots[resource as usize],
                ControlKind::Ena,
                &ENA_DATA,
                RejectionReason::DatainConflict,
            )?;
        }
        Ok(())
    }
}

/// Evaluate exactly the existing greedy control rules; never sort FFs, prefer
/// sharing over the first free slot, or exempt global reset/enable signals.
pub fn evaluate(snapshot: &ControlLabSnapshot) -> ControlAssessment<'_> {
    let mut worker = Worker {
        slots: [Assigned::EMPTY; ALLOCATION_COUNT],
    };
    match worker.run(snapshot) {
        Ok(()) => ControlAssessment::Legal(ControlAllocation {
            snapshot,
            signals: worker.slots.map(|slot| slot.signal),
        }),
        Err(failure) => ControlAssessment::Illegal(ControlConflict { snapshot, failure }),
    }
}
