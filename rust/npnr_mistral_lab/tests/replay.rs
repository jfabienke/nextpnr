// SPDX-License-Identifier: ISC
#![forbid(unsafe_code)]
mod support;

use npnr_mistral_lab::{evaluate_wire, wire::*};

#[test]
fn cpp_legacy_golden_fixtures() {
    for fixture in [
        include_str!("../../../mistral/tests/fixtures/greedy-abc.json"),
        include_str!("../../../mistral/tests/fixtures/greedy-bca.json"),
    ] {
        let record: serde_json::Value = serde_json::from_str(fixture).unwrap();
        let input = support::read_input(&record).unwrap();
        let before = input;
        let out = evaluate_wire(&input);
        assert_eq!(
            out.status,
            record["expected"]["status"].as_u64().unwrap() as u32
        );
        if out.status == LEGAL {
            let expected: Vec<_> = record["expected"]["allocation"]
                .as_array()
                .unwrap()
                .iter()
                .map(|s| support::read_signal(s).unwrap())
                .collect();
            assert_eq!(out.allocation.as_slice(), expected);
        }
        assert_eq!(out, evaluate_wire(&input));
        assert_eq!(input, before);
        // Wide provenance is serialized as strings, including output records.
        assert!(support::result_json(&out)["request_id"].is_string());
    }
}

#[test]
fn layout_metadata_covers_native_records() {
    let layout = support::layout_json();
    assert_eq!(layout["input"]["size"], 1952);
    assert_eq!(layout["result"]["size"], 216);
    assert_eq!(layout["result"]["offsets"]["blockers"], 152);
}

#[test]
fn replay_numbers_match_the_cpp_readers_integer_checks() {
    let mut record: serde_json::Value = serde_json::from_str(include_str!(
        "../../../mistral/tests/fixtures/greedy-bca.json"
    ))
    .unwrap();
    record["input"]["abi_version"] = serde_json::json!(1.0);
    assert_eq!(support::read_input(&record).unwrap().abi_version, 1);
    for invalid in [
        serde_json::json!(-1),
        serde_json::json!(1.5),
        serde_json::json!(u32::MAX as u64 + 1),
    ] {
        record["input"]["net_count"] = invalid;
        assert!(support::read_input(&record).is_err());
    }
}
