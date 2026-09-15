// SPDX-License-Identifier: ISC
use core::num::NonZeroU16;

use crate::wire::{self, CONTROL_COUNT, FF_COUNT, LabControlsV1, MAX_NETS};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum BoundaryError {
    BadAbi = 100,
    BadSize = 101,
    UnsupportedRules = 102,
    BadNetCount = 103,
    BadOccupancy = 104,
    BadReserved = 105,
    BadSignal = 106,
    InconsistentGlobal = 107,
    SparseNetIds = 108,
}

impl core::fmt::Display for BoundaryError {
    fn fmt(&self, out: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(out, "invalid LAB control snapshot: {self:?}")
    }
}
impl core::error::Error for BoundaryError {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ControlKind {
    Clock = 0,
    Sload = 1,
    Sclr = 2,
    Aclr = 3,
    Ena = 4,
}

impl ControlKind {
    pub(crate) const ALL: [Self; CONTROL_COUNT] =
        [Self::Clock, Self::Sload, Self::Sclr, Self::Aclr, Self::Ena];
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum RejectionReason {
    ClockConflict = 1,
    SloadConflict = 2,
    SclrConflict = 3,
    AclrCapacity = 4,
    EnaCapacity = 5,
    DatainConflict = 6,
}

// IDs never leave the typed snapshot/assessment implementation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct NetId(NonZeroU16);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct FfSlot(u8);
impl FfSlot {
    pub(crate) fn from_index(index: usize) -> Self {
        // The only caller enumerates the fixed forty-slot snapshot.
        debug_assert!(index < FF_COUNT);
        Self(index as u8)
    }
    pub(crate) fn index(self) -> u32 {
        u32::from(self.0)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Polarity {
    Normal,
    Inverted,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ControlSignal {
    net: Option<NetId>,
    polarity: Polarity,
}

impl ControlSignal {
    pub(crate) const EMPTY: Self = Self {
        net: None,
        polarity: Polarity::Normal,
    };
    pub(crate) fn connected(self) -> bool {
        self.net.is_some()
    }

    pub(crate) fn to_wire(self, snapshot: &ControlLabSnapshot) -> wire::ControlSignalV1 {
        wire::ControlSignalV1 {
            net_id: self.net.map_or(0, |id| u32::from(id.0.get())),
            flags: if self.polarity == Polarity::Inverted {
                wire::INVERTED
            } else {
                0
            } | if snapshot.is_global(self) {
                wire::GLOBAL
            } else {
                0
            },
        }
    }
}

/// Validated, owned input. Construction checks bounds, flags, identity density,
/// and consistent net properties. No C++ references survive decoding.
#[derive(Debug)]
pub struct ControlLabSnapshot {
    pub(crate) request_id: u64,
    pub(crate) snapshot_epoch: u64,
    pub(crate) ffs: [Option<[ControlSignal; CONTROL_COUNT]>; FF_COUNT],
    // Zero is unseen, one is local, and two is global. Retaining the validated
    // class avoids constructing a second temporary array during decoding.
    classes: [u8; MAX_NETS + 1],
}

impl ControlLabSnapshot {
    pub(crate) fn is_global(&self, signal: ControlSignal) -> bool {
        signal
            .net
            .is_some_and(|id| self.classes[usize::from(id.0.get())] == 2)
    }
}

impl TryFrom<&LabControlsV1> for ControlLabSnapshot {
    type Error = BoundaryError;

    fn try_from(input: &LabControlsV1) -> Result<Self, Self::Error> {
        use BoundaryError::*;
        if input.abi_version != wire::ABI_VERSION {
            return Err(BadAbi);
        }
        if input.struct_size != core::mem::size_of::<LabControlsV1>() as u32 {
            return Err(BadSize);
        }
        if input.rules_version != wire::RULES_VERSION {
            return Err(UnsupportedRules);
        }
        if input.net_count > MAX_NETS as u32 {
            return Err(BadNetCount);
        }

        // Preserve the C++ validator's first-error order, including unused records.
        let mut classes = [0_u8; MAX_NETS + 1]; // 0 unseen, 1 local, 2 global
        let mut ffs = [None; FF_COUNT];
        for (slot, ff) in input.ff.iter().enumerate() {
            if ff.occupied > 1 {
                return Err(BadOccupancy);
            }
            if ff.reserved != 0 {
                return Err(BadReserved);
            }
            let mut controls = [ControlSignal::EMPTY; CONTROL_COUNT];
            for (kind, raw) in ff.control.iter().enumerate() {
                if (ff.occupied == 0 && (raw.net_id != 0 || raw.flags != 0))
                    || raw.net_id > input.net_count
                    || raw.flags & !(wire::INVERTED | wire::GLOBAL) != 0
                    || (raw.net_id == 0 && raw.flags & wire::GLOBAL != 0)
                {
                    return Err(BadSignal);
                }
                if raw.net_id != 0 {
                    let class = if raw.flags & wire::GLOBAL != 0 { 2 } else { 1 };
                    let previous = &mut classes[raw.net_id as usize];
                    if *previous != 0 && *previous != class {
                        return Err(InconsistentGlobal);
                    }
                    *previous = class;
                }
                // The range check above makes the narrowing conversion lossless.
                controls[kind] = ControlSignal {
                    net: NonZeroU16::new(raw.net_id as u16).map(NetId),
                    polarity: if raw.flags & wire::INVERTED != 0 {
                        Polarity::Inverted
                    } else {
                        Polarity::Normal
                    },
                };
            }
            if ff.occupied != 0 {
                ffs[slot] = Some(controls);
            }
        }
        if classes[1..=input.net_count as usize].contains(&0) {
            return Err(SparseNetIds);
        }
        Ok(Self {
            request_id: input.request_id,
            snapshot_epoch: input.snapshot_epoch,
            ffs,
            classes,
        })
    }
}
