/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_V2_H
#define MISTRAL_LAB_V2_H

#include <atomic>
#include "archdefs.h"
#include "lab_v2_abi.h"
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct Arch;
struct CellInfo;

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
NpnrLabFactsV2 capture_lab_v2_overlay(const Arch &arch, uint32_t lab, NpnrLabQueryV2 query, uint32_t query_alm,
                                      const dict<BelId, CellInfo *> &occupancy, uint64_t request_id = 0,
                                      uint64_t epoch = 0);
// One bel's facts with net ids as run-stable keys (the net name's index plus one), for the
// resident path: slot 0 or 1 fills `lut`, 2 to 5 fill `ff`; the other half stays zero.
void capture_bel_v2_keyed(const Arch &arch, uint32_t lab, uint8_t alm, uint8_t slot, NpnrBelPatchV2 &out);
// The same facts for a cell that is bound nowhere: the candidate of a tile scan (design 14).
void capture_cell_v2_keyed(const CellInfo &cell, bool is_ff, NpnrBelPatchV2 &out);
// The tile scan for the strict legaliser: for an unclustered `cell` and free bels of one LAB in the
// legaliser's order, the index of the first bel isBelLocationValid would accept with the cell
// bound there, or -1. False when there is no batch answer (legacy mode, no Rust, a cell or bels
// the scan does not cover, or a failed call): the legaliser then asks per bel as before.
bool scan_lab_tile(const Arch &arch, const CellInfo *cell, const std::vector<BelId> &bels, int &first_legal);
// A scan's prediction for a bel against the live check of the same bel.
void note_lab_tile_prediction(const Arch &arch, bool predicted_legal, bool live_legal);
NpnrLabAssessmentV2 evaluate_lab_v2_cpp(const NpnrLabFactsV2 &input);
bool lab_v2_result_valid(const NpnrLabFactsV2 &input, const NpnrLabAssessmentV2 &result);
bool lab_v2_result_valid(uint64_t request_id, uint64_t snapshot_epoch, uint32_t query, uint32_t query_alm,
                         const NpnrLabAssessmentV2 &result);
// A resident verdict's self-consistency for the query it answers, and its agreement with a
// capture-path assessment on every verdict field (the control allocation's net ids are keys on the
// resident path and dense ids on the capture path, so they are not compared).
bool lab_v2_verdict_valid(uint32_t query, const NpnrLabVerdictV2 &verdict);
bool lab_v2_verdict_matches(const NpnrLabAssessmentV2 &assessment, const NpnrLabVerdictV2 &verdict);
bool lab_v2_results_match(const NpnrLabAssessmentV2 &a, const NpnrLabAssessmentV2 &b);
const char *lab_legality_mode_name(LabLegalityMode mode);
void require_lab_legality_mode(LabLegalityMode mode);
bool dispatch_lab_legality(const Arch &arch, uint32_t lab, NpnrLabQueryV2 query, uint32_t query_alm);
void report_lab_legality_stats(const Arch &arch);

NEXTPNR_NAMESPACE_END

#endif
