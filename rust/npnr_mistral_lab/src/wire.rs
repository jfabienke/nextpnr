// SPDX-License-Identifier: ISC
//! Native value transport matching `mistral/lab_control_abi.h`.
//!
//! All fields permit every integer bit pattern. Validation constructs semantic
//! types separately; these records are neither legality certificates nor a file format.

pub const ABI_VERSION: u32 = 1;
pub const RULES_VERSION: u32 = 1;
pub const FF_COUNT: usize = 40;
pub const CONTROL_COUNT: usize = 5;
pub const MAX_NETS: usize = 200;
pub const ALLOCATION_COUNT: usize = 12;
pub const INVERTED: u32 = 1;
pub const GLOBAL: u32 = 2;

pub const LEGAL: u32 = 0;
pub const ILLEGAL: u32 = 1;
pub const BAD_SNAPSHOT: u32 = 2;
pub const UNSUPPORTED_RULES: u32 = 3;
pub const INTERNAL_ERROR: u32 = 4;
pub const BAD_RULES: u32 = 102;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ControlSignalV1 {
    pub net_id: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LabFfV1 {
    pub occupied: u32,
    pub reserved: u32,
    /// CLK, SLOAD, SCLR, ACLR, ENA, in that order.
    pub control: [ControlSignalV1; CONTROL_COUNT],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LabControlsV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub rules_version: u32,
    pub net_count: u32,
    pub request_id: u64,
    pub snapshot_epoch: u64,
    pub ff: [LabFfV1; FF_COUNT],
}

impl Default for LabControlsV1 {
    fn default() -> Self {
        Self {
            abi_version: ABI_VERSION,
            struct_size: core::mem::size_of::<Self>() as u32,
            rules_version: RULES_VERSION,
            net_count: 0,
            request_id: 0,
            snapshot_epoch: 0,
            ff: [LabFfV1::default(); FF_COUNT],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LabControlBlockerV1 {
    pub signal: ControlSignalV1,
    pub ff_slot: u32,
    pub reserved: u32,
}

impl Default for LabControlBlockerV1 {
    fn default() -> Self {
        Self {
            signal: ControlSignalV1::default(),
            ff_slot: u32::MAX,
            reserved: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LabControlResultV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub status: u32,
    pub reason: u32,
    pub request_id: u64,
    pub snapshot_epoch: u64,
    /// Legal only: clk, sload, sclr, aclr[2], ena[3], datain[4].
    pub allocation: [ControlSignalV1; ALLOCATION_COUNT],
    pub control_kind: u32,
    pub ff_slot: u32,
    pub resource_mask: u32,
    pub reserved: u32,
    pub incoming: ControlSignalV1,
    pub blockers: [LabControlBlockerV1; 4],
}

impl LabControlResultV1 {
    pub(crate) fn empty(request_id: u64, snapshot_epoch: u64, status: u32) -> Self {
        Self {
            abi_version: ABI_VERSION,
            struct_size: core::mem::size_of::<Self>() as u32,
            status,
            reason: 0,
            request_id,
            snapshot_epoch,
            allocation: [ControlSignalV1::default(); ALLOCATION_COUNT],
            control_kind: u32::MAX,
            ff_slot: u32::MAX,
            resource_mask: 0,
            reserved: 0,
            incoming: ControlSignalV1::default(),
            blockers: [LabControlBlockerV1::default(); 4],
        }
    }
}

impl Default for LabControlResultV1 {
    fn default() -> Self {
        Self::empty(0, 0, INTERNAL_ERROR)
    }
}

// Match the C++ assertions without reading or transmuting native struct bytes.
const _: () = {
    use core::mem::{align_of, offset_of, size_of};
    assert!(size_of::<ControlSignalV1>() == 8);
    assert!(offset_of!(ControlSignalV1, flags) == 4);
    assert!(size_of::<LabFfV1>() == 48);
    assert!(offset_of!(LabFfV1, control) == 8);
    assert!(size_of::<LabControlsV1>() == 1952);
    assert!(offset_of!(LabControlsV1, request_id) == 16);
    assert!(offset_of!(LabControlsV1, snapshot_epoch) == 24);
    assert!(offset_of!(LabControlsV1, ff) == 32);
    assert!(size_of::<LabControlBlockerV1>() == 16);
    assert!(size_of::<LabControlResultV1>() == 216);
    assert!(offset_of!(LabControlResultV1, allocation) == 32);
    assert!(offset_of!(LabControlResultV1, control_kind) == 128);
    assert!(offset_of!(LabControlResultV1, incoming) == 144);
    assert!(offset_of!(LabControlResultV1, blockers) == 152);
    assert!(align_of::<LabControlsV1>() == align_of::<u64>());
    assert!(align_of::<LabControlResultV1>() == align_of::<u64>());
};
