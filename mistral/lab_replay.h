/* SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LAB_REPLAY_H
#define MISTRAL_LAB_REPLAY_H

#include <iosfwd>
#include <string>
#include "lab_model.h"

NEXTPNR_NAMESPACE_BEGIN

// Named-field JSON, with decimal strings for 64-bit provenance. These diagnostic APIs allocate.
bool write_lab_control_replay(std::ostream &out, const NpnrLabControlsV1 &input,
                              const NpnrLabControlResultV1 &reference, const std::string &provenance,
                              const NpnrLabControlResultV1 *candidate = nullptr, const std::string &difference = "");
bool read_lab_control_replay(const std::string &text, NpnrLabControlsV1 &input, NpnrLabControlResultV1 &reference,
                             std::string &error);

NEXTPNR_NAMESPACE_END
#endif
