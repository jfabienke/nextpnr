// SPDX-License-Identifier: ISC
//! C-compatible value transport matching `mistral/lab_v2_abi.h`.

use crate::wire::{CONTROL_COUNT, ControlSignalV1, LabControlResultV1};

pub const ABI_VERSION_V2: u32 = 2;
pub const ALMS: usize = 10;
pub const LUTS: usize = 2;
pub const FFS: usize = 4;
pub const LUT_INPUTS: usize = 7;
pub const MAX_NETS_V2: usize = 512;

pub const QUERY_COMB_BEL: u32 = 0;
pub const QUERY_FF_BEL: u32 = 1;
pub const QUERY_WHOLE_LAB: u32 = 2;

pub const LAB_LEGAL: u32 = 0;
pub const LAB_ILLEGAL: u32 = 1;
pub const LAB_MALFORMED: u32 = 2;

pub const OK: u32 = 0;
pub const BAD_HEADER: u32 = 1;
pub const BAD_QUERY: u32 = 2;
pub const BAD_SHAPE: u32 = 3;
pub const ALM_BITS: u32 = 10;
pub const ALM_INPUTS: u32 = 11;
pub const CARRY_MIX: u32 = 12;
pub const ODD_FF: u32 = 13;
pub const FF_CONTROL: u32 = 14;
pub const SDATA_PATH: u32 = 15;
pub const DATAIN_PATH: u32 = 16;
pub const LAB_INPUT_LIMIT: u32 = 20;
pub const CONTROL_CONFLICT: u32 = 21;
pub const MLAB_GROUP: u32 = 22;
pub const MLAB_FF: u32 = 23;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LabLutV2 {
    pub occupied: u32,
    pub input_count: u32,
    pub used_input_count: u32,
    pub bits_count: u32,
    pub chain_shared_input_count: i32,
    pub mlab_group: i32,
    pub constr_z: i32,
    pub is_carry: u32,
    pub input_net: [u32; LUT_INPUTS],
    pub comb_out_net: u32,
    pub wclk: ControlSignalV1,
    pub we: ControlSignalV1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LabFfV2 {
    pub occupied: u32,
    pub datain_net: u32,
    pub sdata_net: u32,
    pub control: [ControlSignalV1; CONTROL_COUNT],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AlmFactsV2 {
    pub lut: [LabLutV2; LUTS],
    pub ff: [LabFfV2; FFS],
    pub cached_input_count: i32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LabFactsV2 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub request_id: u64,
    pub snapshot_epoch: u64,
    pub query: u32,
    pub query_alm: u32,
    pub input_limit: i32,
    pub is_mlab: u32,
    pub net_count: u32,
    pub reserved: u32,
    pub alm: [AlmFactsV2; ALMS],
}

impl Default for LabFactsV2 {
    fn default() -> Self {
        Self {
            abi_version: ABI_VERSION_V2,
            struct_size: size_of::<Self>() as u32,
            request_id: 0,
            snapshot_epoch: 0,
            query: QUERY_COMB_BEL,
            query_alm: 0,
            input_limit: 42,
            is_mlab: 0,
            net_count: 0,
            reserved: 0,
            alm: [AlmFactsV2::default(); ALMS],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LabAssessmentV2 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub request_id: u64,
    pub snapshot_epoch: u64,
    pub status: u32,
    pub reason: u32,
    pub query: u32,
    pub query_alm: u32,
    pub failing_alm: u32,
    pub failing_slot: u32,
    pub observed: i32,
    pub limit: i32,
    pub recomputed_valid_mask: u32,
    pub recomputed_input_count: [i32; ALMS],
    pub control_valid: u32,
    pub reserved: u32,
    pub control: LabControlResultV1,
}

impl LabAssessmentV2 {
    pub(crate) fn empty(input: &LabFactsV2) -> Self {
        let mut control = LabControlResultV1::empty(
            input.request_id,
            input.snapshot_epoch,
            crate::wire::UNSUPPORTED_RULES,
        );
        control.reason = crate::wire::BAD_RULES;
        Self {
            abi_version: ABI_VERSION_V2,
            struct_size: size_of::<Self>() as u32,
            request_id: input.request_id,
            snapshot_epoch: input.snapshot_epoch,
            status: LAB_LEGAL,
            reason: OK,
            query: input.query,
            query_alm: input.query_alm,
            failing_alm: u32::MAX,
            failing_slot: u32::MAX,
            observed: 0,
            limit: 0,
            recomputed_valid_mask: 0,
            recomputed_input_count: [0; ALMS],
            control_valid: 0,
            reserved: 0,
            control,
        }
    }
}

impl Default for LabAssessmentV2 {
    fn default() -> Self {
        Self::empty(&LabFactsV2::default())
    }
}

const _: () = {
    use core::mem::{offset_of, size_of};
    assert!(size_of::<LabLutV2>() == 80);
    assert!(size_of::<LabFfV2>() == 52);
    assert!(size_of::<AlmFactsV2>() == 376);
    assert!(size_of::<LabFactsV2>() == 3808);
    assert!(offset_of!(LabFactsV2, alm) == 48);
    assert!(size_of::<LabAssessmentV2>() == 328);
    assert!(offset_of!(LabAssessmentV2, control) == 112);
};
