// SPDX-License-Identifier: ISC
//! Validated complete-LAB V2 evaluation.

use crate::evaluate_wire;
use crate::v2_wire::*;
use crate::wire::{CONTROL_COUNT, ControlSignalV1, FF_COUNT, LabControlsV1};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LabV2BoundaryError {
    Header,
    Query,
    Shape,
}

#[derive(Clone, Copy)]
pub struct ValidatedLabSnapshotV2 {
    facts: LabFactsV2,
}

fn signal_valid(signal: ControlSignalV1, net_count: u32) -> bool {
    signal.net_id <= net_count
        && signal.flags & !3 == 0
        && (signal.net_id != 0 || signal.flags & crate::wire::GLOBAL == 0)
}

impl TryFrom<&LabFactsV2> for ValidatedLabSnapshotV2 {
    type Error = LabV2BoundaryError;

    fn try_from(input: &LabFactsV2) -> Result<Self, Self::Error> {
        if input.abi_version != ABI_VERSION_V2
            || input.struct_size != size_of::<LabFactsV2>() as u32
        {
            return Err(LabV2BoundaryError::Header);
        }
        if input.query > QUERY_WHOLE_LAB
            || (input.query != QUERY_WHOLE_LAB && input.query_alm as usize >= ALMS)
            || (input.query == QUERY_WHOLE_LAB && input.query_alm != u32::MAX)
        {
            return Err(LabV2BoundaryError::Query);
        }
        if input.net_count as usize > MAX_NETS_V2 || input.reserved != 0 || input.is_mlab > 1 {
            return Err(LabV2BoundaryError::Shape);
        }
        for alm in &input.alm {
            if alm.reserved != 0 {
                return Err(LabV2BoundaryError::Shape);
            }
            for lut in &alm.lut {
                if lut.occupied > 1
                    || lut.is_carry > 1
                    || lut.input_count as usize > LUT_INPUTS
                    || lut.used_input_count > lut.input_count
                    || lut.chain_shared_input_count < 0
                    || lut.chain_shared_input_count > lut.used_input_count as i32
                    || lut.mlab_group < -1
                    || lut.comb_out_net > input.net_count
                    || !signal_valid(lut.wclk, input.net_count)
                    || !signal_valid(lut.we, input.net_count)
                {
                    return Err(LabV2BoundaryError::Shape);
                }
                let mut used = 0;
                for (pin, &net) in lut.input_net.iter().enumerate() {
                    if net > input.net_count || (pin >= lut.input_count as usize && net != 0) {
                        return Err(LabV2BoundaryError::Shape);
                    }
                    used += u32::from(net != 0);
                }
                if lut.occupied != 0 && used != lut.used_input_count {
                    return Err(LabV2BoundaryError::Shape);
                }
                if lut.occupied == 0
                    && (lut.input_count != 0
                        || lut.used_input_count != 0
                        || lut.bits_count != 0
                        || lut.chain_shared_input_count != 0
                        || lut.mlab_group != 0
                        || lut.constr_z != 0
                        || lut.is_carry != 0
                        || lut.comb_out_net != 0
                        || lut.wclk != ControlSignalV1::default()
                        || lut.we != ControlSignalV1::default())
                {
                    return Err(LabV2BoundaryError::Shape);
                }
            }
            for ff in &alm.ff {
                if ff.occupied > 1
                    || ff.datain_net > input.net_count
                    || ff.sdata_net > input.net_count
                    || ff
                        .control
                        .iter()
                        .any(|&s| !signal_valid(s, input.net_count))
                    || (ff.occupied == 0
                        && (ff.datain_net != 0
                            || ff.sdata_net != 0
                            || ff.control.iter().any(|&s| s != ControlSignalV1::default())))
                {
                    return Err(LabV2BoundaryError::Shape);
                }
            }
        }
        Ok(Self { facts: *input })
    }
}

fn reject(
    result: &mut LabAssessmentV2,
    reason: u32,
    alm: u32,
    slot: u32,
    observed: i32,
    limit: i32,
) -> bool {
    result.status = LAB_ILLEGAL;
    result.reason = reason;
    result.failing_alm = alm;
    result.failing_slot = slot;
    result.observed = observed;
    result.limit = limit;
    false
}

fn same_ctrlset(a: &LabFfV2, b: &LabFfV2) -> bool {
    a.control == b.control
}

fn check_alm(input: &LabFactsV2, alm_index: usize, result: &mut LabAssessmentV2) -> bool {
    let alm = &input.alm[alm_index];
    let mut bits = 0i32;
    let mut inputs = 0i32;
    for lut in alm.lut.iter().filter(|lut| lut.occupied != 0) {
        inputs += lut.input_count as i32;
        bits += lut.bits_count as i32;
    }
    if bits > 64 {
        return reject(result, ALM_BITS, alm_index as u32, u32::MAX, bits, 64);
    }
    if inputs > 8 {
        let mut shared = 0;
        for &net in &alm.lut[1].input_net[..alm.lut[1].input_count as usize] {
            if alm.lut[0].input_net[..alm.lut[0].input_count as usize].contains(&net) {
                shared += 1;
            }
        }
        if inputs - shared > 8 {
            return reject(
                result,
                ALM_INPUTS,
                alm_index as u32,
                u32::MAX,
                inputs - shared,
                8,
            );
        }
    }
    let carry = (alm.lut[0].occupied != 0 && alm.lut[0].is_carry != 0)
        || (alm.lut[1].occupied != 0 && alm.lut[1].is_carry != 0);
    if alm.lut[0].occupied != 0
        && alm.lut[1].occupied != 0
        && alm.lut[0].is_carry != alm.lut[1].is_carry
    {
        return reject(result, CARRY_MIX, alm_index as u32, u32::MAX, 0, 0);
    }
    for half in 0..2 {
        let mut route_thru = alm.lut[half].occupied == 0 && !carry && inputs < 8 && bits < 64;
        let mut ef_available =
            alm.lut[1 - half].occupied == 0 || alm.lut[1 - half].used_input_count <= 2;
        let mut first: Option<&LabFfV2> = None;
        for j in 0..2 {
            let slot = 2 * half + j;
            let ff = &alm.ff[slot];
            if ff.occupied == 0 {
                continue;
            }
            if j == 1 {
                return reject(result, ODD_FF, alm_index as u32, slot as u32, 0, 0);
            }
            if first.is_some_and(|old| !same_ctrlset(old, ff)) {
                return reject(result, FF_CONTROL, alm_index as u32, slot as u32, 0, 0);
            }
            first.get_or_insert(ff);
            if ff.sdata_net != 0 {
                if !ef_available {
                    return reject(result, SDATA_PATH, alm_index as u32, slot as u32, 0, 0);
                }
                ef_available = false;
            }
            if ff.datain_net != 0
                && (alm.lut[half].occupied == 0 || ff.datain_net != alm.lut[half].comb_out_net)
            {
                if route_thru {
                    route_thru = false;
                } else if ef_available {
                    ef_available = false;
                } else {
                    return reject(result, DATAIN_PATH, alm_index as u32, slot as u32, 0, 0);
                }
            }
        }
    }
    true
}

fn recompute_inputs(alm: &AlmFactsV2) -> i32 {
    let mut lut_inputs = 0;
    for lut in alm.lut.iter().filter(|lut| lut.occupied != 0) {
        if lut.mlab_group != -1 && lut.constr_z > 2 {
            return 0;
        }
        lut_inputs += lut.used_input_count as i32 - lut.chain_shared_input_count;
    }
    let mut shared = 0;
    if alm.lut.iter().all(|lut| lut.occupied != 0) {
        for &net in &alm.lut[1].input_net[..alm.lut[1].input_count as usize] {
            if net == 0 {
                continue;
            }
            if alm.lut[0].input_net[..alm.lut[0].input_count as usize].contains(&net) {
                shared += 1;
            }
            if shared >= 2 && alm.lut[0].mlab_group == -1 {
                break;
            }
        }
    }
    let mut total = 0.max(lut_inputs - shared);
    for (slot, ff) in alm.ff.iter().enumerate().filter(|(_, ff)| ff.occupied != 0) {
        total += i32::from(ff.sdata_net != 0);
        total += i32::from(
            ff.datain_net != 0
                && (alm.lut[slot / 2].occupied == 0
                    || ff.datain_net != alm.lut[slot / 2].comb_out_net),
        );
    }
    total
}

struct Projection {
    input: LabControlsV1,
    v2_net: [u32; crate::wire::MAX_NETS + 1],
}

fn project_controls(input: &LabFactsV2) -> Projection {
    let mut projection = Projection {
        input: LabControlsV1 {
            request_id: input.request_id,
            snapshot_epoch: input.snapshot_epoch,
            ..LabControlsV1::default()
        },
        v2_net: [0; crate::wire::MAX_NETS + 1],
    };
    let mut local = [0u32; MAX_NETS_V2 + 1];
    for (alm_index, alm) in input.alm.iter().enumerate() {
        for (ff_index, source) in alm.ff.iter().enumerate() {
            if source.occupied == 0 {
                continue;
            }
            let dest = &mut projection.input.ff[4 * alm_index + ff_index];
            dest.occupied = 1;
            for kind in 0..CONTROL_COUNT {
                dest.control[kind].flags = source.control[kind].flags;
                let net = source.control[kind].net_id as usize;
                if net == 0 {
                    continue;
                }
                if local[net] == 0 {
                    projection.input.net_count += 1;
                    local[net] = projection.input.net_count;
                    projection.v2_net[projection.input.net_count as usize] = net as u32;
                }
                dest.control[kind].net_id = local[net];
            }
        }
    }
    projection
}

fn translate_controls(projection: &Projection, result: &mut crate::wire::LabControlResultV1) {
    let translate = |signal: &mut ControlSignalV1| {
        if signal.net_id != 0 {
            signal.net_id = projection.v2_net[signal.net_id as usize];
        }
    };
    result.allocation.iter_mut().for_each(translate);
    translate(&mut result.incoming);
    result
        .blockers
        .iter_mut()
        .for_each(|b| translate(&mut b.signal));
}

fn check_mlab(input: &LabFactsV2, result: &mut LabAssessmentV2) -> bool {
    if input.is_mlab == 0 {
        return true;
    }
    let mut found = -2;
    for (alm_index, alm) in input.alm.iter().enumerate() {
        for lut in alm.lut.iter().filter(|lut| lut.occupied != 0) {
            if found == -2 {
                found = lut.mlab_group;
            } else if found != lut.mlab_group {
                return reject(
                    result,
                    MLAB_GROUP,
                    alm_index as u32,
                    u32::MAX,
                    lut.mlab_group,
                    found,
                );
            }
        }
    }
    if found >= 0 {
        for (alm_index, alm) in input.alm.iter().enumerate() {
            if let Some((slot, _)) = alm.ff.iter().enumerate().find(|(_, ff)| ff.occupied != 0) {
                return reject(result, MLAB_FF, alm_index as u32, slot as u32, 0, 0);
            }
        }
    }
    true
}

pub fn evaluate_lab_v2(snapshot: &ValidatedLabSnapshotV2) -> LabAssessmentV2 {
    let input = &snapshot.facts;
    let mut result = LabAssessmentV2::empty(input);
    let mut total = 0;
    for (index, alm) in input.alm.iter().enumerate() {
        result.recomputed_input_count[index] = recompute_inputs(alm);
        result.recomputed_valid_mask |= 1 << index;
        total += result.recomputed_input_count[index];
    }
    if input.query == QUERY_WHOLE_LAB {
        for alm in 0..ALMS {
            if !check_alm(input, alm, &mut result) {
                return result;
            }
        }
    } else if !check_alm(input, input.query_alm as usize, &mut result) {
        return result;
    }
    if total > input.input_limit {
        reject(
            &mut result,
            LAB_INPUT_LIMIT,
            u32::MAX,
            u32::MAX,
            total,
            input.input_limit,
        );
        return result;
    }
    if input.query != QUERY_COMB_BEL {
        let projection = project_controls(input);
        result.control = evaluate_wire(&projection.input);
        translate_controls(&projection, &mut result.control);
        result.control_valid = 1;
        if result.control.status != crate::wire::LEGAL {
            let alm = result.control.ff_slot / 4;
            let slot = result.control.ff_slot % 4;
            let reason = result.control.reason as i32;
            reject(&mut result, CONTROL_CONFLICT, alm, slot, reason, 0);
            return result;
        }
    }
    check_mlab(input, &mut result);
    result
}

pub fn evaluate_lab_v2_wire_into(input: &LabFactsV2, result: &mut LabAssessmentV2) {
    match ValidatedLabSnapshotV2::try_from(input) {
        Ok(snapshot) => *result = evaluate_lab_v2(&snapshot),
        Err(error) => {
            *result = LabAssessmentV2::empty(input);
            result.status = LAB_MALFORMED;
            result.reason = match error {
                LabV2BoundaryError::Header => BAD_HEADER,
                LabV2BoundaryError::Query => BAD_QUERY,
                LabV2BoundaryError::Shape => BAD_SHAPE,
            };
        }
    }
}

const _: () = assert!(FF_COUNT == ALMS * FFS);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn structured_rules_and_boundary_errors_are_distinct() {
        let mut input = LabFactsV2 {
            query: QUERY_WHOLE_LAB,
            query_alm: u32::MAX,
            request_id: u64::MAX,
            snapshot_epoch: u64::MAX - 1,
            ..LabFactsV2::default()
        };
        let mut result = LabAssessmentV2::default();
        evaluate_lab_v2_wire_into(&input, &mut result);
        assert_eq!(result.status, LAB_LEGAL);
        assert_eq!(result.recomputed_valid_mask, 0x3ff);

        input.alm[0].ff[1].occupied = 1;
        evaluate_lab_v2_wire_into(&input, &mut result);
        assert_eq!((result.status, result.reason), (LAB_ILLEGAL, ODD_FF));
        assert_eq!((result.failing_alm, result.failing_slot), (0, 1));

        input.abi_version += 1;
        evaluate_lab_v2_wire_into(&input, &mut result);
        assert_eq!((result.status, result.reason), (LAB_MALFORMED, BAD_HEADER));
        assert_eq!(result.request_id, u64::MAX);
    }

    #[test]
    fn input_sharing_preserves_null_equality_and_non_null_accounting() {
        let mut input = LabFactsV2 {
            query: QUERY_COMB_BEL,
            query_alm: 0,
            net_count: 9,
            ..LabFactsV2::default()
        };
        for half in 0..2 {
            let lut = &mut input.alm[0].lut[half];
            lut.occupied = 1;
            lut.input_count = 5;
            lut.used_input_count = 4;
            lut.bits_count = 32;
            lut.mlab_group = -1;
        }
        input.alm[0].lut[0].input_net[..5].copy_from_slice(&[1, 2, 3, 4, 0]);
        input.alm[0].lut[1].input_net[..5].copy_from_slice(&[5, 6, 7, 8, 0]);
        let snapshot = ValidatedLabSnapshotV2::try_from(&input).unwrap();
        let result = evaluate_lab_v2(&snapshot);
        assert_eq!(result.status, LAB_ILLEGAL);
        assert_eq!(result.reason, ALM_INPUTS);
        assert_eq!(result.observed, 9); // the live ALM rule counts shared null here
        assert_eq!(result.recomputed_input_count[0], 8); // accounting excludes shared null
    }
}
