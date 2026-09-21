/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The Mistral LAB work
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "telemetry.h"

#include <cstdio>
#include <fstream>

#include "json11.hpp"
#include "lab_dispatch.h"
#include "lab_resident.h"
#include "lab_v2.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
using json11::Json;

// json11 has no 64-bit integer; the counters are written as numbers up to 2^53 and as strings
// past it, which none of them reach in practice.
Json count(uint64_t value)
{
    if (value < (uint64_t(1) << 53))
        return Json(double(value));
    return Json(std::to_string(value));
}

Json count(const std::atomic<uint64_t> &value) { return count(value.load(std::memory_order_relaxed)); }
} // namespace

void write_mistral_telemetry(const Arch &arch, const std::string &phase)
{
    if (arch.args.telemetry_path.empty())
        return;
    const auto &a = arch.args;
    const auto &lc = arch.lab_control_stats;
    const auto &ll = arch.lab_legality_stats;
    char checksum[16];
    snprintf(checksum, sizeof(checksum), "0x%08x", arch.telemetry_checksum.load());
    Json::object options{
            {"lab_controls", lab_control_mode_name(a.lab_controls)},
            {"lab_legality", lab_legality_mode_name(a.lab_legality)},
            {"sa_seam", a.sa_seam == SwapSeamMode::Off      ? "off"
                        : a.sa_seam == SwapSeamMode::Shadow ? "shadow"
                                                            : "on"},
            {"sa_batch", a.sa_batch},
            {"placer_lookahead", a.placer_lookahead},
            {"alm_pairing", a.alm_pairing},
            {"spread_demand", a.spread_demand},
            {"spread_congestion", a.spread_congestion},
            {"register_packing", a.register_packing},
            {"row_cost", double(a.row_cost)},
            {"router2_unit_cost", a.router2_unit_cost},
            {"router2_reroute", a.router2_reroute},
            {"router2_reroute_contested", a.router2_reroute_contested},
            {"lab_tile_scan", a.lab_tile_scan},
            {"reuse_routes_history", double(a.reuse_routes_history)},
    };
    Json::object legality{
            {"evaluations", count(ll.evaluations)},
            {"legal", count(ll.legal)},
            {"illegal", count(ll.illegal)},
            {"errors", count(ll.errors)},
            {"mismatches", count(ll.mismatches)},
            {"stale_cache", count(ll.stale_cache)},
            {"stale_revision", count(ll.stale_revision)},
    };
    Json::object controls{
            {"evaluations", count(lc.evaluations)},
            {"preparation", count(lc.preparation)},
            {"legal", count(lc.legal)},
            {"illegal", count(lc.illegal)},
            {"errors", count(lc.errors)},
            {"mismatches", count(lc.mismatches)},
            {"fallbacks", count(lc.fallbacks)},
    };
    Json::object resident;
    if (arch.lab_resident) {
        resident = Json::object{
                {"evaluations", count(arch.lab_resident->evaluations)},
                {"resets", count(arch.lab_resident->resets)},
                {"trials", count(arch.lab_resident->trials)},
                {"commits", count(arch.lab_resident->commits)},
                {"restored", count(arch.lab_resident->restored)},
                {"scans", count(arch.lab_resident->scans)},
                {"scan_bels", count(arch.lab_resident->scan_bels)},
                {"scan_hits", count(arch.lab_resident->scan_hits)},
        };
    }
    Json::object phases{
            {"placement_s", arch.telemetry_placement_seconds},
            {"routing_s", arch.telemetry_routing_seconds},
    };
    Json::object root{
            {"phase", phase},
            {"device", a.device},
            {"checksum", checksum},
            {"options", options},
            {"legality", legality},
            {"controls", controls},
            {"resident", resident},
            {"phases", phases},
            {"cells", double(arch.getCtx()->cells.size())},
            {"nets", double(arch.getCtx()->nets.size())},
    };
    std::ofstream out(a.telemetry_path);
    if (!out) {
        log_warning("telemetry: cannot write '%s'.\n", a.telemetry_path.c_str());
        return;
    }
    out << Json(root).dump() << "\n";
    log_info("Wrote telemetry for phase '%s' to '%s'.\n", phase.c_str(), a.telemetry_path.c_str());
}

NEXTPNR_NAMESPACE_END
