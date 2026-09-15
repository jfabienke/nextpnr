// SPDX-License-Identifier: ISC
//! Immutable, bounded evaluation of `LegacyControlRulesV1`.
//!
//! Evaluation uses fixed storage, without backend callbacks or a live-design handle. A
//! successful assessment covers FF controls only, not a complete LAB or move.
//!
//! ```
//! use npnr_mistral_lab::{ControlLabSnapshot, evaluate, wire::LabControlsV1};
//! let raw = LabControlsV1::default();
//! let snapshot = ControlLabSnapshot::try_from(&raw)?;
//! let assessment = evaluate(&snapshot);
//! assert_eq!(assessment.to_wire().status, npnr_mistral_lab::wire::LEGAL);
//! # Ok::<(), npnr_mistral_lab::BoundaryError>(())
//! ```
//!
//! Raw records cannot bypass validation:
//! ```compile_fail
//! use npnr_mistral_lab::{evaluate, wire::LabControlsV1};
//! evaluate(&LabControlsV1::default());
//! ```
//!
//! Snapshot contents cannot be changed through the public interface:
//! ```compile_fail
//! use npnr_mistral_lab::{ControlLabSnapshot, wire::LabControlsV1};
//! let mut snapshot = ControlLabSnapshot::try_from(&LabControlsV1::default()).unwrap();
//! snapshot.ffs[0] = None;
//! ```
//!
//! A legal allocation cannot be constructed by callers:
//! ```compile_fail
//! use npnr_mistral_lab::{ControlAllocation, ControlLabSnapshot, wire::LabControlsV1};
//! let snapshot = ControlLabSnapshot::try_from(&LabControlsV1::default()).unwrap();
//! let forged = ControlAllocation { snapshot: &snapshot, signals: Default::default() };
//! ```
//!
//! An assessment cannot outlive or consume its source snapshot:
//! ```compile_fail
//! use npnr_mistral_lab::{ControlLabSnapshot, evaluate, wire::LabControlsV1};
//! let snapshot = ControlLabSnapshot::try_from(&LabControlsV1::default()).unwrap();
//! let assessment = evaluate(&snapshot);
//! drop(snapshot);
//! assessment.to_wire();
//! ```

#![forbid(unsafe_code)]

mod model;
mod rules;
mod v2;
pub mod v2_wire;
pub mod wire;

pub use model::{BoundaryError, ControlKind, ControlLabSnapshot, RejectionReason};
pub use rules::{ControlAllocation, ControlAssessment, ControlConflict, evaluate};
pub use v2::{
    LabV2BoundaryError, ValidatedLabSnapshotV2, evaluate_lab_v2, evaluate_lab_v2_wire_into,
};

/// Decode and evaluate one owned-value transport record. No pointers are accepted.
/// Boundary errors remain distinct from hardware-rule rejection.
pub fn evaluate_wire(input: &wire::LabControlsV1) -> wire::LabControlResultV1 {
    let mut result = wire::LabControlResultV1::default();
    evaluate_wire_into(input, &mut result);
    result
}

/// Decode and evaluate directly into caller-owned initialized result storage.
pub fn evaluate_wire_into(input: &wire::LabControlsV1, result: &mut wire::LabControlResultV1) {
    match ControlLabSnapshot::try_from(input) {
        Ok(snapshot) => evaluate(&snapshot).write_wire(result),
        Err(error) => {
            *result = wire::LabControlResultV1::empty(
                input.request_id,
                input.snapshot_epoch,
                if error == BoundaryError::UnsupportedRules {
                    wire::UNSUPPORTED_RULES
                } else {
                    wire::BAD_SNAPSHOT
                },
            );
            result.reason = error as u32;
        }
    }
}

#[cfg(test)]
mod tests;
