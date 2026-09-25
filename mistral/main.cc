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
#include <filesystem>
#include <fstream>
#include "build_state.h"
#include "command.h"
#include "design_utils.h"
#include "jsonwrite.h"
#include "log.h"
#include "monitor.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

// The complete LAB evaluator's default authority: Rust where it is built (promoted 2026-09-19 at
// the user's direction; tracker entry "Promotion"), the legacy C++ rules otherwise;
// `--lab-legality legacy` selects the C++ rules in any build. The Stage 1 control-plan authority
// (`--lab-controls`) stays legacy: the complete evaluator owns the control check, and the two
// Rust authorities cannot be combined.
#ifdef NO_RUST
static const char *const kDefaultEvaluator = "legacy";
#else
static const char *const kDefaultEvaluator = "rust";
#endif

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
                           "FF-control preparation evaluator: legacy (default; the complete LAB evaluator owns "
                           "the control check), shadow, verify, or rust (with --lab-legality legacy only)");
    specific.add_options()("lab-controls-profile", po::value<std::string>(),
                           "write a bounded sample of live FF-control queries as JSONL for profiling");
    specific.add_options()("lab-legality", po::value<std::string>()->default_value(kDefaultEvaluator),
                           "complete LAB evaluator: legacy, shadow, verify, or rust (default rust with BUILD_RUST, "
                           "legacy otherwise)");
    specific.add_options()(
            "lab-reuse", po::value<std::string>()->default_value("off"),
            "same-session reuse of LAB-level legality sub-results: off, shadow, on, or content (experimental)");
    specific.add_options()("sa-seam", po::value<std::string>()->default_value("on"),
                           "annealer swap evaluation: off (live bind/check/revert), shadow (detached assessment "
                           "compared against live), or on (detached decides; identical results, no provisional "
                           "binding; the default)");
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
    specific.add_options()("no-lab-tile-scan",
                           "ask the LAB legality authority per bel and per frozen cluster candidate even where "
                           "its batch forms apply (the tile scan and the cluster edits are on in the rust, shadow, "
                           "and verify modes; results are identical either way)");
    specific.add_options()("monitor", "show a live dashboard of the run in the terminal (phases, LAB legality and "
                                      "resident counters, placer and router progress, log tail); the log text "
                                      "goes to --log");
    specific.add_options()("telemetry", po::value<std::string>(),
                           "write the run's counters (LAB legality, resident session, control sets), options, "
                           "checksum, and phase times as JSON to this file after placement and after routing");
    specific.add_options()("row-cost", po::value<float>(),
                           "weight of a vertical tile against a horizontal one in placement (solver, spreader, "
                           "legaliser); the fabric enters LABs through row wires (off by default; experimental)");
    specific.add_options()("sa-timing-lambda", po::value<float>(),
                           "the annealer's timing share of a move's cost, 0 to 1 (default 0.5)");
    specific.add_options()("sa-crit-exp", po::value<float>(), "the annealer's criticality exponent (default 8)");
    specific.add_options()("placement-delay", po::value<std::string>(),
                           "placement delay model: linear (75 ps per column, 200 per row; default) or table (the "
                           "median routed delay by span, design 19.8)");
    specific.add_options()("sa-row-weight", po::value<int>(),
                           "annealer cost per distinct row a net touches beyond the first, in horizontal tiles "
                           "(the fabric enters LABs by row wires; off by default; experimental)");
    specific.add_options()("sa-entry-weight", po::value<int>(),
                           "annealer cost per tile a net's sinks occupy other than the driver's, in horizontal "
                           "tiles (off by default; experimental)");
    specific.add_options()("heap-lab-affinity", po::value<float>(),
                           "HeAP's legaliser first tries the tiles of a cell's placed neighbours, scored by distance "
                           "plus this weight per net that would enter a new LAB (off by default; experimental)");
    specific.add_options()("heap-crit-exp", po::value<float>(),
                           "HeAP's criticality exponent: an arc's solver weight is 1 + timingWeight * crit^e "
                           "(default 7)");
    specific.add_options()("heap-lab-reach", po::value<float>(),
                           "how far from the solver's position (in weighted tiles) a neighbour's tile is tried "
                           "(default 2.5)");
    specific.add_options()("router2-reroute", po::value<int>(),
                           "rip up and re-route every arc every N router2 iterations, not only the arcs on overused "
                           "wires (off by default; experimental)");
    specific.add_options()("router2-reroute-contested",
                           "with --router2-reroute, queue only the nets that use a wire with accumulated history");
    specific.add_options()("router2-unit-cost",
                           "router2 costs every wire one unit instead of its delay (fewer wires, slower paths; "
                           "off by default; experimental)");
    specific.add_options()("router2-crit-cost",
                           "router2 costs a wire by blending one unit and its delay by the arc's criticality "
                           "(replaces --router2-unit-cost; off by default; experimental)");
    specific.add_options()("lab-hint", po::value<std::string>(),
                           "a file of `cell x y` lines: HeAP's legaliser tries each cell's LAB first (design 20.0; "
                           "experimental)");
    specific.add_options()("no-sa-refine", "skip HeAP's annealing refinement (design 20.0; experimental)");
    specific.add_options()("lab-global-clocks",
                           "clocks the global network carries take no LAB DATAIN line in the control model, freeing "
                           "it for a third enable (off by default; experimental)");
    specific.add_options()("lab-input-model", po::value<std::string>(),
                           "LAB input limit counts count (each ALM's unique inputs, summed; default) or nets (the "
                           "distinct nets a LAB needs lines for; experimental, --lab-legality legacy only)");
    specific.add_options()("router2-repair-rounds", po::value<int>(),
                           "after router2 converges, re-route the critical arcs by delay over unused wires, this "
                           "many rounds (off by default; experimental)");
    specific.add_options()("router2-repair-crit", po::value<float>(),
                           "with --router2-repair-rounds, the criticality an arc needs to be repaired (default 0.9)");
    specific.add_options()("router2-crit-threshold", po::value<float>(),
                           "with --router2-crit-cost, arcs below this criticality (0 to 1) keep the unit cost "
                           "(default 0)");
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
    ctx->monitor_phase(NPNR_MONITOR_PHASE_SIGNOFF);
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
    // --verify-lab-controls is the C++-only check: it selects the legacy evaluator unless a mode
    // was named on the command line.
    const auto mode = chipArgs.verify_lab_controls && vm["lab-controls"].defaulted()
                              ? std::string("legacy")
                              : vm["lab-controls"].as<std::string>();
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
    if (vm.count("lab-input-model")) {
        const auto model = vm["lab-input-model"].as<std::string>();
        if (model != "count" && model != "nets")
            log_error("--lab-input-model must be count or nets, not '%s'\n", model.c_str());
        chipArgs.lab_input_nets = model == "nets";
        // Design 19.10, phase A: the C++ rules only; the Rust evaluator still counts per ALM.
        if (chipArgs.lab_input_nets && chipArgs.lab_legality != LabLegalityMode::Legacy)
            log_error("--lab-input-model nets needs --lab-legality legacy until the Rust rules have it.\n");
    }
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
    chipArgs.router2_crit_cost = vm.count("router2-crit-cost") != 0;
    chipArgs.lab_global_clocks = vm.count("lab-global-clocks") != 0;
    if (vm.count("lab-hint"))
        chipArgs.lab_hint_path = vm["lab-hint"].as<std::string>();
    chipArgs.no_sa_refine = vm.count("no-sa-refine") != 0;
    if (vm.count("router2-repair-rounds"))
        chipArgs.router2_repair_rounds = std::max(0, vm["router2-repair-rounds"].as<int>());
    if (vm.count("router2-repair-crit"))
        chipArgs.router2_repair_crit = vm["router2-repair-crit"].as<float>();
    if (vm.count("router2-crit-threshold"))
        chipArgs.router2_crit_threshold = vm["router2-crit-threshold"].as<float>();
    chipArgs.register_packing = vm.count("register-packing") != 0;
    if (vm.count("telemetry"))
        chipArgs.telemetry_path = vm["telemetry"].as<std::string>();
    chipArgs.lab_tile_scan = vm.count("no-lab-tile-scan") == 0;
    if (vm.count("heap-lab-affinity"))
        chipArgs.heap_lab_affinity = std::max(0.0f, vm["heap-lab-affinity"].as<float>());
    if (vm.count("heap-crit-exp"))
        chipArgs.heap_crit_exp = std::max(0.0f, vm["heap-crit-exp"].as<float>());
    if (vm.count("heap-lab-reach"))
        chipArgs.heap_lab_reach = std::max(0.0f, vm["heap-lab-reach"].as<float>());
    if (vm.count("sa-timing-lambda"))
        chipArgs.sa_timing_lambda = std::min(1.0f, std::max(0.0f, vm["sa-timing-lambda"].as<float>()));
    if (vm.count("sa-crit-exp"))
        chipArgs.sa_crit_exp = std::max(0.0f, vm["sa-crit-exp"].as<float>());
    if (vm.count("placement-delay")) {
        const std::string model = vm["placement-delay"].as<std::string>();
        if (model != "linear" && model != "table")
            log_error("--placement-delay must be linear or table, not '%s'\n", model.c_str());
        chipArgs.placement_delay_table = model == "table";
    }
    if (vm.count("sa-row-weight"))
        chipArgs.sa_row_weight = std::max(0, vm["sa-row-weight"].as<int>());
    if (vm.count("sa-entry-weight"))
        chipArgs.sa_entry_weight = std::max(0, vm["sa-entry-weight"].as<int>());
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
    if (vm.count("monitor")) {
        const char *source = vm.count("json") ? "json" : vm.count("resume") ? "resume" : nullptr;
        const std::string design =
                source ? std::filesystem::path(vm[source].as<std::string>()).stem().string() : std::string();
        const std::string log_path = vm.count("log") ? vm["log"].as<std::string>() : std::string();
        ctx->monitor = MonitorSession::start(*ctx, design, log_path);
    }
}

int main(int argc, char *argv[])
{
    MistralCommandHandler handler(argc, argv);
    return handler.exec();
}
