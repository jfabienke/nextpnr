/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  gatecat <gatecat@ds0.me>
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

#include <cerrno>
#include <fstream>
#include "build_state.h"
#include "command.h"
#include "design_utils.h"
#include "jsonwrite.h"
#include "log.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

class MistralCommandHandler : public CommandHandler
{
  public:
    MistralCommandHandler(int argc, char **argv);
    virtual ~MistralCommandHandler() {};
    std::unique_ptr<Context> createContext(dict<std::string, Property> &values) override;
    void setupArchContext(Context *ctx) override {};
    void customBitstream(Context *ctx) override;
    void customAfterLoad(Context *ctx) override;

  protected:
    po::options_description getArchOptions() override;
};

MistralCommandHandler::MistralCommandHandler(int argc, char **argv) : CommandHandler(argc, argv) {}

po::options_description MistralCommandHandler::getArchOptions()
{
    po::options_description specific("Architecture specific options");
    specific.add_options()("device", po::value<std::string>(), "device name (e.g. 5CSEBA6U23I7)");
    specific.add_options()("qsf", po::value<std::string>(), "path to QSF constraints file");
    specific.add_options()("rbf", po::value<std::string>(), "RBF bitstream to write");
    specific.add_options()("uncompressed-rbf",
                           "emit an uncompressed bitstream (default is compressed; uncompressed "
                           "configures the device but may leave IO non-functional on some loaders)");
    specific.add_options()("compress-rbf", "deprecated, no-op: compressed output is now the default");
    specific.add_options()("verify-lab-controls", "verify LAB FF-control checks against the detached C++ evaluator");
    specific.add_options()("lab-controls", po::value<std::string>()->default_value("legacy"),
                           "FF-control evaluator: legacy, shadow, verify, or rust (experimental)");
    specific.add_options()("lab-controls-profile", po::value<std::string>(),
                           "write a bounded sample of live FF-control queries as JSONL for profiling");
    specific.add_options()("lab-legality", po::value<std::string>()->default_value("legacy"),
                           "complete LAB evaluator: legacy, shadow, verify, or rust (experimental)");
    specific.add_options()(
            "lab-reuse", po::value<std::string>()->default_value("off"),
            "same-session reuse of LAB-level legality sub-results: off, shadow, on, or content (experimental)");
    specific.add_options()("sa-seam", po::value<std::string>()->default_value("off"),
                           "annealer swap evaluation: off (live bind/check/revert), shadow (detached assessment "
                           "compared against live), or on (detached decides; identical results, no provisional "
                           "binding) (experimental)");
    specific.add_options()("route-prepare-only",
                           "run LAB and global routing preparation, then stop before the router (for --checkpoint)");
    specific.add_options()("sa-batch", po::value<int>(),
                           "batched annealer refinement: speculate this many swaps per batch, evaluate them "
                           "detached on --threads workers, consume in order (results depend on the seed and "
                           "this value, not on --threads; 0 = serial, default; experimental)");
    specific.add_options()("reuse-routes", po::value<std::string>(),
                           "previous routed output JSON or routed checkpoint whose routes are reused where the "
                           "current design still allows them (Stage 5, 3c)");
    specific.add_options()("reuse-plan-out", po::value<std::string>(),
                           "write the reuse plan (every cell and net decision with its reason) to this JSON file");
    specific.add_options()("reuse-dry-run", "plan placement and route reuse but apply nothing");
    specific.add_options()("alm-pairing", po::value<int>(),
                           "pair plain LUTs into ALMs before placement: 1 pairs LUTs sharing an input net, 2 also "
                           "pairs a LUT with one it drives, 3 pairs whatever is compatible (0 = off, default; "
                           "experimental)");
    specific.add_options()("spread-congestion", "spread cells by a wire-density estimate of the current placement, "
                                                "rebuilt every spreading pass (off by default; experimental)");
    specific.add_options()("spread-demand", "spread comb cells by their unique input count instead of one per bel "
                                            "(routing-demand-aware placement; off by default; experimental)");
    specific.add_options()("register-packing", "pack a register with the LUT that drives it into one ALM before "
                                               "placement (off by default; experimental)");
    specific.add_options()("telemetry", po::value<std::string>(),
                           "write the run's counters (LAB legality, resident session, control sets), options, "
                           "checksum, and phase times as JSON to this file after placement and after routing");
    specific.add_options()("row-cost", po::value<float>(),
                           "weight of a vertical tile against a horizontal one in placement (solver, spreader, "
                           "legaliser); the fabric enters LABs through row wires (off by default; experimental)");
    specific.add_options()("router2-reroute", po::value<int>(),
                           "rip up and re-route every arc every N router2 iterations, not only the arcs on overused "
                           "wires (off by default; experimental)");
    specific.add_options()("router2-reroute-contested",
                           "with --router2-reroute, queue only the nets that use a wire with accumulated history");
    specific.add_options()("router2-unit-cost",
                           "router2 costs every wire one unit instead of its delay (fewer wires, slower paths; "
                           "off by default; experimental)");
    specific.add_options()("reuse-routes-history", po::value<float>(),
                           "seed router2's history cost on every preserved wire so the dirty nets route around "
                           "them from the first iteration (1.0 = off, default; experimental)");
    specific.add_options()("reuse-placement", po::value<std::string>(),
                           "previous nextpnr output JSON; cells with an identical name and signature are "
                           "constrained to their previous BEL, everything else is placed normally (experimental)");
    specific.add_options()("placer-lookahead", po::value<int>(),
                           "speculate this many candidates per clustered HeAP move and evaluate them detached "
                           "on --threads workers; results are identical to the serial search (0 = serial, "
                           "default; experimental)");

    return specific;
}

void MistralCommandHandler::customBitstream(Context *ctx)
{
    report_lab_control_stats(*ctx);
    report_lab_legality_stats(*ctx);
    write_lab_control_profile(*ctx);
    if (vm.count("rbf")) {
        std::string filename = vm["rbf"].as<std::string>();
        // Stage 5 (3a): only a validated build publishes a bitstream.
        validate_build(Build<BuildPhase::Routed>::adopt(*ctx));
        ctx->build_bitstream();
        std::vector<uint8_t> data;
        ctx->cyclonev->rbf_save(data);

        std::ofstream out(filename, std::ios::binary);
        if (!out.is_open()) {
            log_error("Failed to open RBF file '%s' for writing: %s.\n", filename.c_str(),
                      std::error_code(errno, std::generic_category()).message().c_str());
        }
        out.write(reinterpret_cast<const char *>(data.data()), data.size());
    }
}

std::unique_ptr<Context> MistralCommandHandler::createContext(dict<std::string, Property> &values)
{
    ArchArgs chipArgs;
    if (!vm.count("device")) {
        log_error("device must be specified on the command line (e.g. --device 5CSEBA6U23I7)\n");
    }
    chipArgs.device = vm["device"].as<std::string>();
    chipArgs.verify_lab_controls = vm.count("verify-lab-controls") != 0;
    if (vm.count("lab-controls-profile"))
        chipArgs.lab_control_profile_path = vm["lab-controls-profile"].as<std::string>();
    const auto mode = vm["lab-controls"].as<std::string>();
    if (mode == "legacy")
        chipArgs.lab_controls = LabControlMode::Legacy;
    else if (mode == "shadow")
        chipArgs.lab_controls = LabControlMode::Shadow;
    else if (mode == "verify")
        chipArgs.lab_controls = LabControlMode::Verify;
    else if (mode == "rust")
        chipArgs.lab_controls = LabControlMode::Rust;
    else
        log_error("Unknown --lab-controls mode '%s'; use legacy, shadow, verify, or rust.\n", mode.c_str());
    if (chipArgs.verify_lab_controls && chipArgs.lab_controls != LabControlMode::Legacy)
        log_error("--verify-lab-controls is the C++-only check; select --lab-controls verify for Rust verification.\n");
    require_lab_control_mode(chipArgs.lab_controls);
    const auto legality_mode = vm["lab-legality"].as<std::string>();
    if (legality_mode == "legacy")
        chipArgs.lab_legality = LabLegalityMode::Legacy;
    else if (legality_mode == "shadow")
        chipArgs.lab_legality = LabLegalityMode::Shadow;
    else if (legality_mode == "verify")
        chipArgs.lab_legality = LabLegalityMode::Verify;
    else if (legality_mode == "rust")
        chipArgs.lab_legality = LabLegalityMode::Rust;
    else
        log_error("Unknown --lab-legality mode '%s'; use legacy, shadow, verify, or rust.\n", legality_mode.c_str());
    require_lab_legality_mode(chipArgs.lab_legality);
    const auto reuse_mode = vm["lab-reuse"].as<std::string>();
    if (reuse_mode == "off")
        chipArgs.lab_reuse = LabReuseMode::Off;
    else if (reuse_mode == "shadow")
        chipArgs.lab_reuse = LabReuseMode::Shadow;
    else if (reuse_mode == "on")
        chipArgs.lab_reuse = LabReuseMode::On;
    else if (reuse_mode == "content")
        chipArgs.lab_reuse = LabReuseMode::Content;
    else
        log_error("Unknown --lab-reuse mode '%s'; use off, shadow, on, or content.\n", reuse_mode.c_str());
    const auto seam_mode = vm["sa-seam"].as<std::string>();
    if (seam_mode == "off")
        chipArgs.sa_seam = SwapSeamMode::Off;
    else if (seam_mode == "shadow")
        chipArgs.sa_seam = SwapSeamMode::Shadow;
    else if (seam_mode == "on")
        chipArgs.sa_seam = SwapSeamMode::On;
    else
        log_error("Unknown --sa-seam mode '%s'; use off, shadow, or on.\n", seam_mode.c_str());
    chipArgs.route_prepare_only = vm.count("route-prepare-only") != 0;
    if (vm.count("sa-batch")) {
        chipArgs.sa_batch = vm["sa-batch"].as<int>();
        if (chipArgs.sa_batch < 0)
            log_error("--sa-batch must be zero or positive.\n");
    }
    if (vm.count("reuse-placement"))
        chipArgs.reuse_placement_path = vm["reuse-placement"].as<std::string>();
    if (vm.count("reuse-routes"))
        chipArgs.reuse_routes_path = vm["reuse-routes"].as<std::string>();
    if (vm.count("reuse-plan-out"))
        chipArgs.reuse_plan_path = vm["reuse-plan-out"].as<std::string>();
    chipArgs.reuse_dry_run = vm.count("reuse-dry-run") != 0;
    if (vm.count("alm-pairing")) {
        chipArgs.alm_pairing = vm["alm-pairing"].as<int>();
        if (chipArgs.alm_pairing < 0 || chipArgs.alm_pairing > 3)
            log_error("--alm-pairing must be 0, 1, 2, or 3.\n");
    }
    chipArgs.spread_demand = vm.count("spread-demand") != 0;
    chipArgs.spread_congestion = vm.count("spread-congestion") != 0;
    if (vm.count("router2-reroute")) {
        chipArgs.router2_reroute = vm["router2-reroute"].as<int>();
        if (chipArgs.router2_reroute < 0)
            log_error("--router2-reroute must be 0 or a positive iteration count.\n");
    }
    chipArgs.router2_reroute_contested = vm.count("router2-reroute-contested") != 0;
    chipArgs.router2_unit_cost = vm.count("router2-unit-cost") != 0;
    chipArgs.register_packing = vm.count("register-packing") != 0;
    if (vm.count("telemetry"))
        chipArgs.telemetry_path = vm["telemetry"].as<std::string>();
    if (vm.count("row-cost")) {
        chipArgs.row_cost = vm["row-cost"].as<float>();
        if (chipArgs.row_cost <= 0.0f)
            log_error("--row-cost must be positive.\n");
    }
    if (vm.count("reuse-routes-history")) {
        chipArgs.reuse_routes_history = vm["reuse-routes-history"].as<float>();
        if (chipArgs.reuse_routes_history < 1.0f)
            log_error("--reuse-routes-history must be 1.0 or more.\n");
    }
    if (vm.count("placer-lookahead")) {
        chipArgs.placer_lookahead = vm["placer-lookahead"].as<int>();
        if (chipArgs.placer_lookahead < 0)
            log_error("--placer-lookahead must be zero or positive.\n");
    }
    if (chipArgs.lab_legality != LabLegalityMode::Legacy && chipArgs.lab_controls != LabControlMode::Legacy)
        log_error("--lab-legality owns its internal control check; do not combine it with a non-legacy "
                  "--lab-controls mode.\n");
    auto ctx = std::unique_ptr<Context>(new Context(chipArgs));
    if (vm.count("uncompressed-rbf"))
        ctx->settings[id_uncompressed_rbf] = Property::State::S1;
    if (vm.count("compress-rbf"))
        log_warning("--compress-rbf is deprecated and has no effect; compressed output is now the "
                    "default (pass --uncompressed-rbf to opt out).\n");
    return ctx;
}

void MistralCommandHandler::customAfterLoad(Context *ctx)
{
    if (vm.count("qsf")) {
        std::string filename = vm["qsf"].as<std::string>();
        auto in = open_ifstream_and_log_error(filename, "input QSF file");
        ctx->read_qsf(in);
    }
}

int main(int argc, char *argv[])
{
    MistralCommandHandler handler(argc, argv);
    return handler.exec();
}
