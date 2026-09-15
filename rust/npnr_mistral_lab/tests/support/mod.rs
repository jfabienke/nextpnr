// SPDX-License-Identifier: ISC
// JSON belongs to test/replay tooling, outside the evaluation path.
use npnr_mistral_lab::wire::*;
use serde_json::{Value, json};

fn u32_field(value: &Value, field: &str) -> Result<u32, String> {
    value
        .get(field)
        .and_then(Value::as_f64)
        .filter(|n| n.is_finite() && *n >= 0.0 && *n <= u32::MAX as f64 && n.fract() == 0.0)
        .map(|n| n as u32)
        .ok_or_else(|| format!("invalid u32 field {field}"))
}

fn u64_field(value: &Value, field: &str) -> Result<u64, String> {
    let text = value
        .get(field)
        .and_then(Value::as_str)
        .ok_or_else(|| format!("invalid string field {field}"))?;
    if text.is_empty() || !text.bytes().all(|b| b.is_ascii_digit()) {
        return Err(format!("invalid u64 field {field}"));
    }
    text.parse().map_err(|_| format!("u64 overflow in {field}"))
}

pub fn read_signal(value: &Value) -> Result<ControlSignalV1, String> {
    Ok(ControlSignalV1 {
        net_id: u32_field(value, "net_id")?,
        flags: u32_field(value, "flags")?,
    })
}

pub fn read_input(record: &Value) -> Result<LabControlsV1, String> {
    if u32_field(record, "schema")? != 1 {
        return Err("unsupported replay schema".into());
    }
    let value = &record["input"];
    let mut input = LabControlsV1 {
        abi_version: u32_field(value, "abi_version")?,
        struct_size: u32_field(value, "struct_size")?,
        rules_version: u32_field(value, "rules_version")?,
        net_count: u32_field(value, "net_count")?,
        request_id: u64_field(value, "request_id")?,
        snapshot_epoch: u64_field(value, "snapshot_epoch")?,
        ff: [LabFfV1::default(); FF_COUNT],
    };
    let ffs = value["ff"]
        .as_array()
        .filter(|ffs| ffs.len() == FF_COUNT)
        .ok_or("expected forty FF records")?;
    for (out, ff) in input.ff.iter_mut().zip(ffs) {
        out.occupied = u32_field(ff, "occupied")?;
        out.reserved = u32_field(ff, "reserved")?;
        let controls = ff["control"]
            .as_array()
            .filter(|c| c.len() == CONTROL_COUNT)
            .ok_or("expected five controls")?;
        for (out, signal) in out.control.iter_mut().zip(controls) {
            *out = read_signal(signal)?;
        }
    }
    Ok(input)
}

pub fn result_json(result: &LabControlResultV1) -> Value {
    let signal = |s: &ControlSignalV1| json!({"net_id": s.net_id, "flags": s.flags});
    json!({
        "abi_version": result.abi_version, "struct_size": result.struct_size,
        "status": result.status, "reason": result.reason,
        "request_id": result.request_id.to_string(), "snapshot_epoch": result.snapshot_epoch.to_string(),
        "allocation": result.allocation.iter().map(signal).collect::<Vec<_>>(),
        "control_kind": result.control_kind, "ff_slot": result.ff_slot,
        "resource_mask": result.resource_mask, "reserved": result.reserved,
        "incoming": signal(&result.incoming),
        "blockers": result.blockers.iter().map(|b| json!({"signal": signal(&b.signal), "ff_slot": b.ff_slot, "reserved": b.reserved})).collect::<Vec<_>>()
    })
}

pub fn layout_json() -> Value {
    use std::mem::{align_of, offset_of, size_of};
    macro_rules! layout {
        ($ty:ty, $($field:ident),+ $(,)?) => { json!({
            "size": size_of::<$ty>(), "align": align_of::<$ty>(),
            "offsets": { $(stringify!($field): offset_of!($ty, $field)),+ }
        }) };
    }
    json!({
        "signal": layout!(ControlSignalV1, net_id, flags),
        "ff": layout!(LabFfV1, occupied, reserved, control),
        "input": layout!(LabControlsV1, abi_version, struct_size, rules_version, net_count, request_id, snapshot_epoch, ff),
        "blocker": layout!(LabControlBlockerV1, signal, ff_slot, reserved),
        "result": layout!(LabControlResultV1, abi_version, struct_size, status, reason, request_id, snapshot_epoch, allocation, control_kind, ff_slot, resource_mask, reserved, incoming, blockers)
    })
}
