/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_V2_REPLAY_H
#define MISTRAL_LAB_V2_REPLAY_H

#include <iosfwd>
#include <string>
#include "lab_v2.h"

NEXTPNR_NAMESPACE_BEGIN

// Named-field JSON diagnostic transport. No native structure bytes are serialized.
bool write_lab_v2_replay(std::ostream &out, const NpnrLabFactsV2 &input, const NpnrLabAssessmentV2 &expected,
                         const std::string &provenance);
bool read_lab_v2_replay(const std::string &text, NpnrLabFactsV2 &input, NpnrLabAssessmentV2 &expected,
                        std::string &error);

NEXTPNR_NAMESPACE_END

#endif
