/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_V2_H
#define MISTRAL_LAB_V2_H

#include <atomic>
#include "lab_v2_abi.h"
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct Arch;

enum class LabLegalityMode
{
    Legacy,
    Shadow,
    Verify,
    Rust
};

struct LabLegalityStats
{
    std::atomic<uint64_t> evaluations{0}, legal{0}, illegal{0}, errors{0}, mismatches{0}, stale_cache{0},
            stale_revision{0}, diagnostics{0};
};

int resolved_lab_input_limit();
NpnrLabFactsV2 capture_lab_v2(const Arch &arch, uint32_t lab, NpnrLabQueryV2 query, uint32_t query_alm,
                              uint64_t request_id = 0, uint64_t epoch = 0);
NpnrLabAssessmentV2 evaluate_lab_v2_cpp(const NpnrLabFactsV2 &input);
bool lab_v2_result_valid(const NpnrLabFactsV2 &input, const NpnrLabAssessmentV2 &result);
bool lab_v2_results_match(const NpnrLabAssessmentV2 &a, const NpnrLabAssessmentV2 &b);
const char *lab_legality_mode_name(LabLegalityMode mode);
void require_lab_legality_mode(LabLegalityMode mode);
bool dispatch_lab_legality(const Arch &arch, uint32_t lab, NpnrLabQueryV2 query, uint32_t query_alm);
void report_lab_legality_stats(const Arch &arch);

NEXTPNR_NAMESPACE_END

#endif
