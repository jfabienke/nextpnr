/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_MODEL_H
#define MISTRAL_LAB_MODEL_H

#include <string>
#include "lab_control_abi.h"
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

NpnrLabControlsV1 empty_lab_controls(uint64_t request_id = 0, uint64_t snapshot_epoch = 0);
NpnrLabControlResultV1 lab_control_result(const NpnrLabControlsV1 &input, uint32_t status);

// Pure, bounded, allocation-free evaluation. Malformed data is distinct from illegality.
NpnrLabControlResultV1 evaluate_lab_controls_cpp(const NpnrLabControlsV1 &input);

// The legacy worker has no conflict diagnostics. Compare verdicts and complete legal plans only.
bool lab_control_results_match(const NpnrLabControlResultV1 &reference, const NpnrLabControlResultV1 &candidate);

// Complete comparison for detached C++/Rust parity, including error diagnostics.
// Empty means equal; otherwise names the first differing field.
std::string lab_control_first_difference(const NpnrLabControlResultV1 &a, const NpnrLabControlResultV1 &b);

NEXTPNR_NAMESPACE_END
#endif
