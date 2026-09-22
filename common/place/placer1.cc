/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Claire Xenia Wolf <claire@yosyshq.com>
 *  Copyright (C) 2018  gatecat <gatecat@ds0.me>
 *
 *  Simulated annealing implementation based on arachne-pnr
 *  Copyright (C) 2015-2018 Cotton Seed
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

#include "placer1.h"
#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_set>
#include <vector>
#include "fast_bels.h"
#include "log.h"
#include "place_common.h"
#include "placement_pool.h"
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

class SAPlacer
{
  private:
    struct BoundingBox
    {
        // Actual bounding box
        int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        // Number of cells at each extremity
        int nx0 = 0, nx1 = 0, ny0 = 0, ny1 = 0;
        wirelen_t hpwl(const Placer1Cfg &cfg) const
        {
            return wirelen_t(cfg.hpwl_scale_x * (x1 - x0) + cfg.hpwl_scale_y * (y1 - y0));
        }
    };

    // Position overlay for detached swap evaluation: cost computations see the
    // moved cells at their proposed BELs instead of their live bindings. The
    // live paths pass the empty `no_overlay`, so they are unchanged.
    using BelOverlayList = std::vector<std::pair<const CellInfo *, BelId>>;
    const BelOverlayList no_overlay;
    static inline BelId overlay_bel(const BelOverlayList &overlay, const CellInfo *cell)
    {
        for (const auto &entry : overlay)
            if (entry.first == cell)
                return entry.second;
        return cell->bel;
    }
    inline Loc overlay_loc(const BelOverlayList &overlay, const CellInfo *cell) const
    {
        if (!overlay.empty() && !cell->isPseudo())
            return ctx->getBelLocation(overlay_bel(overlay, cell));
        return cell->getLocation();
    }
    inline delay_t predict_arc_delay(const BelOverlayList &overlay, const NetInfo *net, const PortRef &sink) const
    {
        if (overlay.empty())
            return ctx->predictArcDelay(net, sink);
        // Same selection as Context::predictArcDelay, with overlay BELs.
        const BelId driver_bel = overlay_bel(overlay, net->driver.cell), sink_bel = overlay_bel(overlay, sink.cell);
        if (net->driver.cell == nullptr || driver_bel == BelId() || sink_bel == BelId())
            return 0;
        IdString driver_pin, sink_pin;
        for (auto pin : ctx->getBelPinsForCellPin(net->driver.cell, net->driver.port)) {
            driver_pin = pin;
            break;
        }
        for (auto pin : ctx->getBelPinsForCellPin(sink.cell, sink.port)) {
            sink_pin = pin;
            break;
        }
        if (driver_pin == IdString() || sink_pin == IdString())
            return 0;
        return ctx->predictDelay(driver_bel, driver_pin, sink_bel, sink_pin);
    }

  public:
    SAPlacer(Context *ctx, Placer1Cfg cfg)
            : ctx(ctx), fast_bels(ctx, /*check_bel_available=*/false, cfg.minBelsForGridPick), cfg(cfg), tmg(ctx)
    {
        for (auto bel : ctx->getBels()) {
            Loc loc = ctx->getBelLocation(bel);
            max_x = std::max(max_x, loc.x);
            max_y = std::max(max_y, loc.y);
        }
        diameter = std::max(max_x, max_y) + 1;

        pool<IdString> cell_types_in_use;
        for (auto &cell : ctx->cells) {
            if (cell.second->isPseudo())
                continue;
            IdString cell_type = cell.second->type;
            cell_types_in_use.insert(cell_type);
        }

        for (auto cell_type : cell_types_in_use) {
            fast_bels.addCellType(cell_type);
        }

        net_bounds.resize(ctx->nets.size());
        net_arc_tcost.resize(ctx->nets.size());
        net_arc_crit.resize(ctx->nets.size());
        old_udata.reserve(ctx->nets.size());
        net_by_udata.reserve(ctx->nets.size());
        decltype(NetInfo::udata) n = 0;
        for (auto &net : ctx->nets) {
            old_udata.emplace_back(net.second->udata);
            net_arc_tcost.at(n).resize(net.second->users.capacity());
            net_arc_crit.at(n).resize(net.second->users.capacity());
            net.second->udata = n++;
            net_by_udata.push_back(net.second.get());
        }
        for (auto &region : ctx->region) {
            Region *r = region.second.get();
            BoundingBox bb;
            if (r->constr_bels) {
                bb.x0 = std::numeric_limits<int>::max();
                bb.x1 = std::numeric_limits<int>::min();
                bb.y0 = std::numeric_limits<int>::max();
                bb.y1 = std::numeric_limits<int>::min();
                for (auto bel : r->bels) {
                    Loc loc = ctx->getBelLocation(bel);
                    bb.x0 = std::min(bb.x0, loc.x);
                    bb.x1 = std::max(bb.x1, loc.x);
                    bb.y0 = std::min(bb.y0, loc.y);
                    bb.y1 = std::max(bb.y1, loc.y);
                }
            } else {
                bb.x0 = 0;
                bb.y0 = 0;
                bb.x1 = max_x;
                bb.y1 = max_y;
            }
            region_bounds[r->name] = bb;
        }
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->isPseudo() || ci->cluster == ClusterId())
                continue;
            cluster2cell[ci->cluster].push_back(ci);
        }
    }

    ~SAPlacer()
    {
        for (auto &net : ctx->nets)
            net.second->udata = old_udata[net.second->udata];
    }

    bool place(bool refine = false)
    {
        log_break();

        std::lock_guard<Context> lock{*ctx};

        size_t placed_cells = 0;
        std::vector<CellInfo *> autoplaced;
        std::vector<CellInfo *> chain_basis;
        if (!refine) {
            // Initial constraints placer
            for (auto &cell_entry : ctx->cells) {
                CellInfo *cell = cell_entry.second.get();
                if (cell->isPseudo())
                    continue;
                auto loc = cell->attrs.find(ctx->id("BEL"));
                if (loc != cell->attrs.end()) {
                    std::string loc_name = loc->second.as_string();
                    BelId bel = ctx->getBelByNameStr(loc_name);
                    if (bel == BelId()) {
                        log_error("No Bel named \'%s\' located for "
                                  "this chip (processing BEL attribute on \'%s\')\n",
                                  loc_name.c_str(), cell->name.c_str(ctx));
                    }

                    if (!ctx->isValidBelForCellType(cell->type, bel)) {
                        IdString bel_type = ctx->getBelType(bel);
                        log_error("Bel \'%s\' of type \'%s\' does not match cell "
                                  "\'%s\' of type \'%s\'\n",
                                  loc_name.c_str(), bel_type.c_str(ctx), cell->name.c_str(ctx), cell->type.c_str(ctx));
                    }
                    auto bound_cell = ctx->getBoundBelCell(bel);
                    if (bound_cell) {
                        log_error(
                                "Cell \'%s\' cannot be bound to bel \'%s\' since it is already bound to cell \'%s\'\n",
                                cell->name.c_str(ctx), loc_name.c_str(), bound_cell->name.c_str(ctx));
                    }

                    ctx->bindBel(bel, cell, STRENGTH_USER);
                    if (!ctx->isBelLocationValid(bel, /* explain_invalid */ true)) {
                        IdString bel_type = ctx->getBelType(bel);
                        log_error("Bel \'%s\' of type \'%s\' is not valid for cell "
                                  "\'%s\' of type \'%s\'\n",
                                  loc_name.c_str(), bel_type.c_str(ctx), cell->name.c_str(ctx), cell->type.c_str(ctx));
                    }
                    locked_bels.insert(bel);
                    placed_cells++;
                }
            }
            int constr_placed_cells = placed_cells;
            log_info("Placed %d cells based on constraints.\n", int(placed_cells));
            ctx->yield();

            // Sort to-place cells for deterministic initial placement

            for (auto &cell : ctx->cells) {
                CellInfo *ci = cell.second.get();
                if (!ci->isPseudo() && (ci->bel == BelId())) {
                    autoplaced.push_back(cell.second.get());
                }
            }
            std::sort(autoplaced.begin(), autoplaced.end(), [](CellInfo *a, CellInfo *b) { return a->name < b->name; });
            ctx->shuffle(autoplaced);
            auto iplace_start = std::chrono::high_resolution_clock::now();
            // Place cells randomly initially
            log_info("Creating initial placement for remaining %d cells.\n", int(autoplaced.size()));

            for (auto cell : autoplaced) {
                place_initial(cell);
                placed_cells++;
                if ((placed_cells - constr_placed_cells) % 500 == 0)
                    log_info("  initial placement placed %d/%d cells\n", int(placed_cells - constr_placed_cells),
                             int(autoplaced.size()));
            }
            if ((placed_cells - constr_placed_cells) % 500 != 0)
                log_info("  initial placement placed %d/%d cells\n", int(placed_cells - constr_placed_cells),
                         int(autoplaced.size()));
            ctx->yield();
            auto iplace_end = std::chrono::high_resolution_clock::now();
            log_info("Initial placement time %.02fs\n",
                     std::chrono::duration<float>(iplace_end - iplace_start).count());
            log_info("Running simulated annealing placer.\n");
        } else {
            for (auto &cell : ctx->cells) {
                CellInfo *ci = cell.second.get();
                if (ci->isPseudo() || ci->belStrength > STRENGTH_STRONG) {
                    continue;
                } else if (ci->cluster != ClusterId()) {
                    if (ctx->getClusterRootCell(ci->cluster) == ci)
                        chain_basis.push_back(ci);
                    else
                        continue;
                } else {
                    autoplaced.push_back(ci);
                }
            }
            require_legal = false;
            diameter = 3;
            log_info("Running simulated annealing placer for refinement.\n");
            batch_mode = cfg.assess_swap && cfg.swap_batch > 0 && cfg.netShareWeight <= 0;
        }
        auto saplace_start = std::chrono::high_resolution_clock::now();

        // Invoke timing analysis to obtain criticalities
        tmg.setup_only = true;
        tmg.setup();

        // Calculate costs after initial placement
        setup_costs();
        moveChange.init(this);
        if (batch_mode)
            batch_setup();
        curr_wirelen_cost = total_wirelen_cost();
        curr_timing_cost = total_timing_cost();
        last_wirelen_cost = curr_wirelen_cost;
        last_timing_cost = curr_timing_cost;

        if (cfg.netShareWeight > 0)
            setup_nets_by_tile();

        wirelen_t avg_wirelen = curr_wirelen_cost;
        wirelen_t min_wirelen = curr_wirelen_cost;

        int n_no_progress = 0;
        temp = refine ? 1e-7 : cfg.startTemp;

        // Main simulated annealing loop
        for (int iter = 1;; iter++) {
            n_move = n_accept = 0;
            improved = false;

            if (iter % 5 == 0 || iter == 1)
                log_info("  at iteration #%d: temp = %f, timing cost = "
                         "%.0f, wirelen = %.0f\n",
                         iter, temp, double(curr_timing_cost), double(curr_wirelen_cost));

            for (int m = 0; m < 15; ++m) {
                if (batch_mode) {
                    run_batched_pass(autoplaced);
                } else {
                    // Loop through all automatically placed cells
                    for (auto cell : autoplaced) {
                        // Find another random Bel for this cell
                        BelId try_bel = random_bel_for_cell(cell);
                        // If valid, try and swap to a new position and see if
                        // the new position is valid/worthwhile
                        if (try_bel != BelId() && try_bel != cell->bel)
                            try_swap_position(cell, try_bel);
                    }
                }
                // Also try swapping chains, if applicable
                for (auto cb : chain_basis) {
                    Loc chain_base_loc = ctx->getBelLocation(cb->bel);
                    BelId try_base = random_bel_for_cell(cb, chain_base_loc.z);
                    if (try_base != BelId() && try_base != cb->bel)
                        try_swap_chain(cb, try_base);
                }
            }

            if (ctx->debug) {
                // Verify correctness of incremental wirelen updates
                for (size_t i = 0; i < net_bounds.size(); i++) {
                    auto net = net_by_udata[i];
                    if (ignore_net(no_overlay, net))
                        continue;
                    auto &incr = net_bounds.at(i), gold = get_net_bounds(no_overlay, net);
                    NPNR_ASSERT(incr.x0 == gold.x0);
                    NPNR_ASSERT(incr.x1 == gold.x1);
                    NPNR_ASSERT(incr.y0 == gold.y0);
                    NPNR_ASSERT(incr.y1 == gold.y1);
                    NPNR_ASSERT(incr.nx0 == gold.nx0);
                    NPNR_ASSERT(incr.nx1 == gold.nx1);
                    NPNR_ASSERT(incr.ny0 == gold.ny0);
                    NPNR_ASSERT(incr.ny1 == gold.ny1);
                }
            }

            if (curr_wirelen_cost < min_wirelen) {
                min_wirelen = curr_wirelen_cost;
                improved = true;
            }

            // Heuristic to improve placement on the 8k
            if (improved)
                n_no_progress = 0;
            else
                n_no_progress++;

            if (temp <= 1e-7 && n_no_progress >= (refine ? 1 : 5)) {
                log_info("  at iteration #%d: temp = %f, timing cost = "
                         "%.0f, wirelen = %.0f \n",
                         iter, temp, double(curr_timing_cost), double(curr_wirelen_cost));
                break;
            }

            double Raccept = double(n_accept) / double(n_move);

            int M = std::max(max_x, max_y) + 1;

            if (ctx->verbose)
                log("iter #%d: temp = %f, timing cost = "
                    "%.0f, wirelen = %.0f, dia = %d, Ra = %.02f \n",
                    iter, temp, double(curr_timing_cost), double(curr_wirelen_cost), diameter, Raccept);

            if (curr_wirelen_cost < 0.95 * avg_wirelen && curr_wirelen_cost > 0) {
                avg_wirelen = 0.8 * avg_wirelen + 0.2 * curr_wirelen_cost;
            } else {
                double diam_next = diameter * (1.0 - 0.44 + Raccept);
                diameter = std::max<int>(1, std::min<int>(M, int(diam_next + 0.5)));
                if (Raccept > 0.96) {
                    temp *= 0.5;
                } else if (Raccept > 0.8) {
                    temp *= 0.9;
                } else if (Raccept > 0.15 && diameter > 1) {
                    temp *= 0.95;
                } else {
                    temp *= 0.8;
                }
            }
            // Once cooled below legalise threshold, run legalisation and start requiring
            // legal moves only
            if (diameter < legalise_dia && require_legal) {
                if (legalise_relative_constraints(ctx)) {
                    // Only increase temperature if something was moved
                    autoplaced.clear();
                    chain_basis.clear();
                    for (auto &cell : ctx->cells) {
                        if (cell.second->isPseudo())
                            continue;
                        if (cell.second->belStrength <= STRENGTH_STRONG && cell.second->cluster != ClusterId() &&
                            ctx->getClusterRootCell(cell.second->cluster) == cell.second.get())
                            chain_basis.push_back(cell.second.get());
                        else if (cell.second->belStrength < STRENGTH_STRONG)
                            autoplaced.push_back(cell.second.get());
                    }
                    // temp = post_legalise_temp;
                    // diameter = std::min<int>(M, diameter * post_legalise_dia_scale);
                    ctx->shuffle(autoplaced);
                }
                require_legal = false;
            }

            // Invoke timing analysis to obtain criticalities
            if (cfg.timing_driven)
                tmg.run();
            // Need to rebuild costs after criticalities change
            setup_costs();
            // Reset incremental bounds
            moveChange.reset(this);
            moveChange.new_net_bounds = net_bounds;
            if (batch_mode)
                batch_refresh_scratch();

            // Recalculate total metric entirely to avoid rounding errors
            // accumulating over time
            curr_wirelen_cost = total_wirelen_cost();
            curr_timing_cost = total_timing_cost();
            last_wirelen_cost = curr_wirelen_cost;
            last_timing_cost = curr_timing_cost;
            // Let the UI show visualization updates.
            ctx->yield();
        }

        auto saplace_end = std::chrono::high_resolution_clock::now();
        log_info("SA placement time %.02fs\n", std::chrono::duration<float>(saplace_end - saplace_start).count());
        if (cfg.assess_swap) {
            log_info("  swap seam: assessed=%" PRIu64 ", illegal=%" PRIu64 ", rejected=%" PRIu64 ", committed=%" PRIu64
                     ", unsupported=%" PRIu64 "\n",
                     seam_assessed, seam_illegal, seam_rejected, seam_committed, seam_unsupported);
            if (batch_mode)
                log_info("  batched refinement: batches=%" PRIu64 ", candidates=%" PRIu64 ", skipped=%" PRIu64
                         ", stale=%" PRIu64 ", unsupported=%" PRIu64 ", illegal=%" PRIu64 ", rejected=%" PRIu64
                         ", committed=%" PRIu64 ", delta-mismatches=%" PRIu64 "; pool spin/block wakes=%" PRIu64
                         "/%" PRIu64 "\n",
                         batch_count, batch_candidates, batch_skipped, batch_stale, batch_unsupported, batch_illegal,
                         batch_rejected, batch_committed, batch_delta_mismatches, worker_pool->spin_wakes(),
                         worker_pool->block_wakes());
            if (cfg.netShareWeight <= 0)
                log_info("  chain seam: planned=%" PRIu64 ", failed=%" PRIu64 ", assessed=%" PRIu64 ", illegal=%" PRIu64
                         ", rejected=%" PRIu64 ", committed=%" PRIu64 ", unsupported=%" PRIu64 "\n",
                         chain_planned, chain_failed, chain_assessed, chain_illegal, chain_rejected, chain_committed,
                         chain_unsupported);
            if (cfg.swap_seam_shadow) {
                log_info("  swap seam shadow: checked=%" PRIu64 ", mismatches=%" PRIu64 "\n", seam_shadow_checked,
                         seam_shadow_mismatches);
                log_info("  chain seam shadow: checked=%" PRIu64 ", mismatches=%" PRIu64 "\n", chain_shadow_checked,
                         chain_shadow_mismatches);
                if (seam_shadow_mismatches != 0)
                    log_error("Swap seam shadow found %" PRIu64 " mismatches.\n", seam_shadow_mismatches);
                if (chain_shadow_mismatches != 0)
                    log_error("Chain seam shadow found %" PRIu64 " mismatches.\n", chain_shadow_mismatches);
            }
        }

        // Final post-placement validity check
        ctx->yield();
        for (auto bel : ctx->getBels()) {
            CellInfo *cell = ctx->getBoundBelCell(bel);
            if (!ctx->isBelLocationValid(bel, /* explain_invalid */ true)) {
                std::string cell_text = "no cell";
                if (cell != nullptr)
                    cell_text = std::string("cell '") + ctx->nameOf(cell) + "'";
                if (ctx->force) {
                    log_warning("post-placement validity check failed for Bel '%s' "
                                "(%s)\n",
                                ctx->nameOfBel(bel), cell_text.c_str());
                } else {
                    log_error("post-placement validity check failed for Bel '%s' "
                              "(%s)\n",
                              ctx->nameOfBel(bel), cell_text.c_str());
                }
            }
        }
        timing_analysis(ctx);

        return true;
    }

  private:
    std::vector<BelId> all_bels;
    // Initial random placement
    void place_initial(CellInfo *cell)
    {
        while (cell) {
            CellInfo *ripup_target = nullptr;
            if (cell->bel != BelId()) {
                ctx->unbindBel(cell->bel);
            }
            FastBels::FastBelsData *bel_data;
            auto type_cnt = fast_bels.getBelsForCellType(cell->type, &bel_data);

            while (true) {
                int nx = ctx->rng(max_x + 1), ny = ctx->rng(max_y + 1);
                if (cfg.minBelsForGridPick >= 0 && type_cnt < cfg.minBelsForGridPick)
                    nx = ny = 0;
                if (nx >= int(bel_data->size()))
                    continue;
                if (ny >= int(bel_data->at(nx).size()))
                    continue;
                const auto &fb = bel_data->at(nx).at(ny);
                if (fb.size() == 0)
                    continue;
                BelId bel = fb.at(ctx->rng(int(fb.size())));
                if (cell->region && cell->region->constr_bels && !cell->region->bels.count(bel))
                    continue;
                if (!ctx->isValidBelForCellType(cell->type, bel))
                    continue;
                ripup_target = ctx->getBoundBelCell(bel);
                if (ripup_target != nullptr) {
                    if (ripup_target->belStrength > STRENGTH_STRONG)
                        continue;
                    ctx->unbindBel(bel);
                } else if (!ctx->checkBelAvail(bel)) {
                    continue;
                }
                ctx->bindBel(bel, cell, STRENGTH_WEAK);
                if (!ctx->isBelLocationValid(bel)) {
                    ctx->unbindBel(bel);
                    if (ripup_target)
                        ctx->bindBel(bel, ripup_target, STRENGTH_WEAK);
                    continue;
                }
                break;
            }
            // Back annotate location
            cell->attrs[ctx->id("BEL")] = ctx->getBelName(cell->bel).str(ctx);
            cell = ripup_target;
        }
    }

    // Attempt a SA position swap, return true on success or false on failure
    void shadow_compare(CellInfo *cell, bool live_legal, wirelen_t live_wirelen_delta, double live_timing_delta)
    {
        shadow_pending = false;
        ++seam_shadow_checked;
        if (live_legal != shadow_legal ||
            (live_legal && (live_wirelen_delta != shadow_wirelen_delta || live_timing_delta != shadow_timing_delta))) {
            if (seam_shadow_mismatches++ < 8)
                log_warning("Swap seam shadow mismatch for '%s': live legal=%d wl=%d tmg=%.17g, detached legal=%d "
                            "wl=%d tmg=%.17g\n",
                            ctx->nameOf(cell), int(live_legal), int(live_wirelen_delta), live_timing_delta,
                            int(shadow_legal), int(shadow_wirelen_delta), shadow_timing_delta);
        }
    }

    bool try_swap_position(CellInfo *cell, BelId newBel)
    {
        static const double epsilon = 1e-20;
        moveChange.reset(this);
        if (!require_legal && cell->cluster != ClusterId())
            return false;
        BelId oldBel = cell->bel;
        CellInfo *other_cell = ctx->getBoundBelCell(newBel);
        if (!require_legal && other_cell != nullptr &&
            (other_cell->cluster != ClusterId() || other_cell->belStrength > STRENGTH_WEAK)) {
            return false;
        }
        int old_dist = get_constraints_distance(ctx, cell);
        int new_dist;
        if (other_cell != nullptr)
            old_dist += get_constraints_distance(ctx, other_cell);
        double delta = 0;

        if (!ctx->isValidBelForCellType(cell->type, newBel)) {
            return false;
        }
        if (other_cell != nullptr && !ctx->isValidBelForCellType(other_cell->type, oldBel)) {
            return false;
        }

        int net_delta_score = 0;

        // Detached path: legality from the architecture's frozen assessment, cost
        // delta from the position overlay, RNG drawn in the original order, and
        // bindings touched only for an accepted swap.
        if (cfg.assess_swap && cfg.netShareWeight <= 0 && cell->cluster == ClusterId() &&
            (other_cell == nullptr || other_cell->cluster == ClusterId())) {
            std::vector<Placer1SwapEdit> edits;
            edits.push_back(
                    {newBel, other_cell, other_cell ? other_cell->belStrength : STRENGTH_NONE, cell, STRENGTH_WEAK});
            edits.push_back({oldBel, cell, cell->belStrength, other_cell, other_cell ? STRENGTH_WEAK : STRENGTH_NONE});
            auto assessment = cfg.assess_swap(ctx, edits);
            if (assessment.status == Placer1SwapAssessment::Status::Unsupported) {
                ++seam_unsupported;
            } else {
                ++seam_assessed;
                const bool legal = assessment.status == Placer1SwapAssessment::Status::Legal;
                if (legal) {
                    BelOverlayList overlay;
                    overlay.emplace_back(cell, newBel);
                    if (other_cell != nullptr)
                        overlay.emplace_back(other_cell, oldBel);
                    add_move_cell(moveChange, overlay, cell, oldBel);
                    if (other_cell != nullptr)
                        add_move_cell(moveChange, overlay, other_cell, newBel);
                    compute_cost_changes(moveChange, overlay);
                }
                if (cfg.swap_seam_shadow) {
                    // Remember the detached answer, then let the live path run and compare.
                    shadow_pending = true;
                    shadow_legal = legal;
                    shadow_wirelen_delta = moveChange.wirelen_delta;
                    shadow_timing_delta = moveChange.timing_delta;
                    moveChange.reset(this);
                } else {
                    if (!legal) {
                        ++seam_illegal;
                        return false;
                    }
                    // Both cells are cluster-free, so their constraint distance is zero before and after.
                    new_dist = 0;
                    delta = lambda * (moveChange.timing_delta / std::max<double>(last_timing_cost, epsilon)) +
                            (1 - lambda) *
                                    (double(moveChange.wirelen_delta) / std::max<double>(last_wirelen_cost, epsilon));
                    delta += (cfg.constraintWeight / temp) * (new_dist - old_dist) / last_wirelen_cost;
                    n_move++;
                    if (delta < 0 || (temp > 1e-8 && (ctx->rng() / float(0x3fffffff)) <= std::exp(-delta / temp))) {
                        n_accept++;
                        if (!cfg.commit_swap(ctx, edits, assessment))
                            log_error("Detached swap commit found a changed design for cell '%s'.\n",
                                      ctx->nameOf(cell));
                        ++seam_committed;
                        commit_cost_changes(moveChange);
                        return true;
                    }
                    ++seam_rejected;
                    return false;
                }
            }
        }

        if (cfg.netShareWeight > 0)
            net_delta_score += update_nets_by_tile(cell, ctx->getBelLocation(cell->bel), ctx->getBelLocation(newBel));

        ctx->unbindBel(oldBel);
        if (other_cell != nullptr) {
            ctx->unbindBel(newBel);
        }

        ctx->bindBel(newBel, cell, STRENGTH_WEAK);

        if (other_cell != nullptr) {
            ctx->bindBel(oldBel, other_cell, STRENGTH_WEAK);
            if (cfg.netShareWeight > 0)
                net_delta_score +=
                        update_nets_by_tile(other_cell, ctx->getBelLocation(newBel), ctx->getBelLocation(oldBel));
        }

        add_move_cell(moveChange, no_overlay, cell, oldBel);

        if (other_cell != nullptr) {
            add_move_cell(moveChange, no_overlay, other_cell, newBel);
        }

        // Always check both the new and old locations; as in some cases of dedicated routing ripping up a cell can deny
        // use of a dedicated path and thus make a site illegal
        if (!ctx->isBelLocationValid(newBel) || !ctx->isBelLocationValid(oldBel)) {
            if (shadow_pending)
                shadow_compare(cell, false, 0, 0);
            ctx->unbindBel(newBel);
            if (other_cell != nullptr)
                ctx->unbindBel(oldBel);
            goto swap_fail;
        }

        // Recalculate metrics for all nets touched by the perturbation
        compute_cost_changes(moveChange, no_overlay);
        if (shadow_pending)
            shadow_compare(cell, true, moveChange.wirelen_delta, moveChange.timing_delta);

        new_dist = get_constraints_distance(ctx, cell);
        if (other_cell != nullptr)
            new_dist += get_constraints_distance(ctx, other_cell);
        delta = lambda * (moveChange.timing_delta / std::max<double>(last_timing_cost, epsilon)) +
                (1 - lambda) * (double(moveChange.wirelen_delta) / std::max<double>(last_wirelen_cost, epsilon));
        delta += (cfg.constraintWeight / temp) * (new_dist - old_dist) / last_wirelen_cost;
        if (cfg.netShareWeight > 0)
            delta += -cfg.netShareWeight * (net_delta_score / std::max<double>(total_net_share, epsilon));
        n_move++;
        // SA acceptance criteria
        if (delta < 0 || (temp > 1e-8 && (ctx->rng() / float(0x3fffffff)) <= std::exp(-delta / temp))) {
            n_accept++;
        } else {
            if (other_cell != nullptr)
                ctx->unbindBel(oldBel);
            ctx->unbindBel(newBel);
            goto swap_fail;
        }
        commit_cost_changes(moveChange);
#if 0
        log_info("swap %s -> %s\n", cell->name.c_str(ctx), ctx->nameOfBel(newBel));
        if (other_cell != nullptr)
            log_info("swap %s -> %s\n", other_cell->name.c_str(ctx), ctx->nameOfBel(oldBel));
#endif
        return true;
    swap_fail:
        ctx->bindBel(oldBel, cell, STRENGTH_WEAK);
        if (other_cell != nullptr) {
            ctx->bindBel(newBel, other_cell, STRENGTH_WEAK);
            if (cfg.netShareWeight > 0)
                update_nets_by_tile(other_cell, ctx->getBelLocation(oldBel), ctx->getBelLocation(newBel));
        }
        if (cfg.netShareWeight > 0)
            update_nets_by_tile(cell, ctx->getBelLocation(newBel), ctx->getBelLocation(oldBel));
        return false;
    }

    // Swap the Bel of a cell with another, return the original location
    BelId swap_cell_bels(CellInfo *cell, BelId newBel)
    {
        BelId oldBel = cell->bel;
#if 0
        log_info("%s old: %s new: %s\n", cell->name.c_str(ctx), ctx->nameOfBel(cell->bel), ctx->nameOfBel(newBel));
#endif
        CellInfo *bound = ctx->getBoundBelCell(newBel);
        if (bound != nullptr)
            ctx->unbindBel(newBel);
        ctx->unbindBel(oldBel);
        ctx->bindBel(newBel, cell, (cell->cluster != ClusterId()) ? STRENGTH_STRONG : STRENGTH_WEAK);
        if (bound != nullptr) {
            ctx->bindBel(oldBel, bound, (bound->cluster != ClusterId()) ? STRENGTH_STRONG : STRENGTH_WEAK);
            if (cfg.netShareWeight > 0)
                update_nets_by_tile(bound, ctx->getBelLocation(newBel), ctx->getBelLocation(oldBel));
        }
        if (cfg.netShareWeight > 0)
            update_nets_by_tile(cell, ctx->getBelLocation(oldBel), ctx->getBelLocation(newBel));
        return oldBel;
    }

    // Attempt to swap a chain with a non-chain
    // Design 18.1: a chain move planned on an occupancy overlay instead of bound. The plan walks the
    // live code's queue of displaced clusters in the same order and fails where it would fail, reading
    // at every step the bindings the live code would have made by then.
    struct ChainPlan
    {
        // The cells the walk moves, in the order the live code's moved_cells dict inserts them: the
        // value the dict holds (the bel before the move) and the cell's bel now, BelId() while unbound.
        struct Moved
        {
            CellInfo *cell;
            BelId before, now;
        };
        std::vector<Moved> moved;
        // Bels whose occupant the walk changed, with the occupant now (nullptr: vacated).
        std::vector<std::pair<BelId, CellInfo *>> occupants;
        bool unsupported = false; // the live code would take a path the plan does not model

        int index_of(const CellInfo *cell) const
        {
            for (size_t i = 0; i < moved.size(); ++i)
                if (moved[i].cell == cell)
                    return int(i);
            return -1;
        }
    };

    CellInfo *plan_occupant(const ChainPlan &plan, BelId bel) const
    {
        for (const auto &entry : plan.occupants)
            if (entry.first == bel)
                return entry.second;
        return ctx->getBoundBelCell(bel);
    }
    static BelId plan_bel(const ChainPlan &plan, const CellInfo *cell)
    {
        const int i = plan.index_of(cell);
        return i < 0 ? cell->bel : plan.moved[i].now;
    }
    static void plan_set(ChainPlan &plan, BelId bel, CellInfo *cell)
    {
        for (auto &entry : plan.occupants)
            if (entry.first == bel) {
                entry.second = cell;
                return;
            }
        plan.occupants.emplace_back(bel, cell);
    }
    // moved_cells[cell->name] = bel
    static void plan_note(ChainPlan &plan, CellInfo *cell, BelId bel)
    {
        const int i = plan.index_of(cell);
        if (i < 0)
            plan.moved.push_back({cell, bel, cell->bel});
        else
            plan.moved[i].before = bel;
    }
    // unbindBel of a noted cell's current bel
    static void plan_unbind(ChainPlan &plan, CellInfo *cell)
    {
        auto &m = plan.moved[plan.index_of(cell)];
        if (m.now != BelId())
            plan_set(plan, m.now, nullptr);
        m.now = BelId();
    }
    // bindBel(bel, cell) of a noted cell
    static void plan_bind(ChainPlan &plan, BelId bel, CellInfo *cell)
    {
        plan_set(plan, bel, cell);
        plan.moved[plan.index_of(cell)].now = bel;
    }

    // The walk of try_swap_chain up to its validity checks; false where the live code goes to swap_fail.
    bool plan_chain(CellInfo *cell, BelId newBase, ChainPlan &plan)
    {
        std::queue<std::pair<ClusterId, BelId>> displaced_clusters;
        displaced_clusters.emplace(cell->cluster, newBase);
        while (!displaced_clusters.empty()) {
            std::vector<std::pair<CellInfo *, BelId>> dest_bels;
            auto cursor = displaced_clusters.front();
            displaced_clusters.pop();
            if (!ctx->getClusterPlacement(cursor.first, cursor.second, dest_bels))
                return false;
            for (const auto &db : dest_bels) {
                const BelId bel = plan_bel(plan, db.first);
                if (bel != BelId()) {
                    plan_note(plan, db.first, bel);
                    plan_unbind(plan, db.first);
                }
            }
            for (const auto &db : dest_bels) {
                CellInfo *bound = plan_occupant(plan, db.second);
                const int noted = plan.index_of(db.first);
                if (noted < 0) {
                    plan.unsupported = true; // the live code's moved_cells.at() would throw here
                    return false;
                }
                const BelId old_bel = plan.moved[noted].before;
                if (plan_occupant(plan, old_bel) != nullptr && bound != nullptr)
                    return false;
                if (bound != nullptr) {
                    if (plan.index_of(bound) >= 0) {
                        return false;
                    } else if (bound->belStrength > STRENGTH_STRONG) {
                        return false;
                    } else if (bound->cluster != ClusterId()) {
                        Loc old_loc = ctx->getBelLocation(old_bel);
                        Loc bound_loc = ctx->getBelLocation(plan_bel(plan, bound));
                        Loc root_loc = ctx->getBelLocation(plan_bel(plan, ctx->getClusterRootCell(bound->cluster)));
                        Loc new_loc =
                                Loc(old_loc.x + (root_loc.x - bound_loc.x), old_loc.y + (root_loc.y - bound_loc.y),
                                    old_loc.z + (root_loc.z - bound_loc.z));
                        if (new_loc.x < 0 || new_loc.x >= ctx->getGridDimX())
                            return false;
                        if (new_loc.y < 0 || new_loc.y >= ctx->getGridDimY())
                            return false;
                        BelId new_root = ctx->getBelByLocation(new_loc);
                        if (new_root == BelId())
                            return false;
                        for (auto cluster_cell : cluster2cell.at(bound->cluster)) {
                            plan_note(plan, cluster_cell, plan_bel(plan, cluster_cell));
                            plan_unbind(plan, cluster_cell);
                        }
                        displaced_clusters.emplace(bound->cluster, new_root);
                    } else {
                        plan_note(plan, bound, plan_bel(plan, bound));
                        plan_unbind(plan, bound);
                        plan_bind(plan, old_bel, bound);
                    }
                } else if (plan_occupant(plan, db.second) != nullptr) {
                    return false;
                }
                plan_bind(plan, db.second, db.first);
            }
        }
        return true;
    }

    // What the live revert leaves behind: every cell it moved bound again at its old bel, weakly.
    static void plan_revert_strengths(const ChainPlan &plan)
    {
        for (const auto &m : plan.moved)
            m.cell->belStrength = STRENGTH_WEAK;
    }

    // Chain seam statistics and the detached answer awaiting comparison in shadow mode.
    uint64_t chain_planned = 0, chain_failed = 0, chain_assessed = 0, chain_unsupported = 0, chain_illegal = 0,
             chain_rejected = 0, chain_committed = 0, chain_shadow_checked = 0, chain_shadow_mismatches = 0;
    int chain_shadow_outcome = -1; // 0 walk failed, 1 illegal, 2 legal
    wirelen_t chain_shadow_wirelen_delta = 0;
    double chain_shadow_timing_delta = 0;

    // A chain move without binding unless it is accepted: 1 committed, 0 refused, -1 not assessable
    // (the live path runs; nothing was changed). With `dry`, only the answer is recorded for the
    // shadow comparison: no draw, no commit, no strength change.
    int try_swap_chain_detached(CellInfo *cell, BelId newBase, bool dry)
    {
        ChainPlan plan;
        moveChange.reset(this);
        const bool walked = plan_chain(cell, newBase, plan);
        if (plan.unsupported) {
            ++chain_unsupported;
            return -1;
        }
        ++chain_planned;
        auto refuse = [&](int outcome) {
            if (dry) {
                chain_shadow_outcome = outcome;
                moveChange.reset(this);
            } else {
                plan_revert_strengths(plan);
            }
            return 0;
        };
        if (!walked) {
            ++chain_failed;
            return refuse(0);
        }
        std::vector<Placer1SwapEdit> edits;
        for (const auto &entry : plan.occupants) {
            CellInfo *before = ctx->getBoundBelCell(entry.first);
            if (before == nullptr && entry.second == nullptr)
                continue;
            edits.push_back({entry.first, before, before != nullptr ? before->belStrength : STRENGTH_NONE, entry.second,
                             entry.second != nullptr ? STRENGTH_WEAK : STRENGTH_NONE, entry.second != nullptr});
        }
        const auto assessment = cfg.assess_swap(ctx, edits);
        if (assessment.status == Placer1SwapAssessment::Status::Unsupported) {
            --chain_planned;
            ++chain_unsupported;
            return -1;
        }
        ++chain_assessed;
        bool legal = assessment.status == Placer1SwapAssessment::Status::Legal;
        for (const auto &m : plan.moved)
            legal = legal && m.cell->testRegion(m.now);
        if (!legal) {
            ++chain_illegal;
            return refuse(1);
        }
        // The live code visits moved_cells in the dict's iteration order, newest first.
        BelOverlayList overlay;
        for (auto it = plan.moved.rbegin(); it != plan.moved.rend(); ++it)
            overlay.emplace_back(it->cell, it->now);
        for (auto it = plan.moved.rbegin(); it != plan.moved.rend(); ++it)
            add_move_cell(moveChange, overlay, it->cell, it->before);
        compute_cost_changes(moveChange, overlay);
        if (dry) {
            chain_shadow_outcome = 2;
            chain_shadow_wirelen_delta = moveChange.wirelen_delta;
            chain_shadow_timing_delta = moveChange.timing_delta;
            moveChange.reset(this);
            return 0;
        }
        const double delta = lambda * (moveChange.timing_delta / last_timing_cost) +
                             (1 - lambda) * (double(moveChange.wirelen_delta) / last_wirelen_cost);
        n_move++;
        if (delta < 0 || (temp > 1e-8 && (ctx->rng() / float(0x3fffffff)) <= std::exp(-delta / temp))) {
            n_accept++;
            if (!cfg.commit_swap(ctx, edits, assessment))
                log_error("Detached chain commit found a changed design for cell '%s'.\n", ctx->nameOf(cell));
            ++chain_committed;
            commit_cost_changes(moveChange);
            return 1;
        }
        ++chain_rejected;
        plan_revert_strengths(plan);
        return 0;
    }

    void chain_shadow_compare(CellInfo *cell, int live_outcome, wirelen_t live_wirelen_delta, double live_timing_delta)
    {
        ++chain_shadow_checked;
        const int detached = chain_shadow_outcome;
        chain_shadow_outcome = -1;
        if (live_outcome != detached || (live_outcome == 2 && (live_wirelen_delta != chain_shadow_wirelen_delta ||
                                                               live_timing_delta != chain_shadow_timing_delta))) {
            if (chain_shadow_mismatches++ < 8)
                log_warning("Chain seam shadow mismatch for '%s': live outcome=%d wl=%d tmg=%.17g, detached outcome=%d "
                            "wl=%d tmg=%.17g\n",
                            ctx->nameOf(cell), live_outcome, int(live_wirelen_delta), live_timing_delta, detached,
                            int(chain_shadow_wirelen_delta), chain_shadow_timing_delta);
        }
    }

    bool try_swap_chain(CellInfo *cell, BelId newBase)
    {
        bool shadow = false;
        if (cfg.assess_swap && cfg.netShareWeight <= 0) {
            const int detached = try_swap_chain_detached(cell, newBase, cfg.swap_seam_shadow);
            if (detached >= 0 && !cfg.swap_seam_shadow)
                return detached == 1;
            shadow = detached >= 0;
        }
        int live_outcome = 0; // for the shadow comparison: 0 walk failed, 1 illegal, 2 legal
        std::vector<std::pair<CellInfo *, Loc>> cell_rel;
        dict<IdString, BelId> moved_cells;
        double delta = 0;
        int orig_share_cost = total_net_share;
        moveChange.reset(this);
#if CHAIN_DEBUG
        log_info("finding cells for chain swap %s\n", cell->name.c_str(ctx));
#endif
        std::queue<std::pair<ClusterId, BelId>> displaced_clusters;
        displaced_clusters.emplace(cell->cluster, newBase);
        while (!displaced_clusters.empty()) {
            std::vector<std::pair<CellInfo *, BelId>> dest_bels;
            auto cursor = displaced_clusters.front();
#if CHAIN_DEBUG
            log_info("%d Cluster %s\n", __LINE__, cursor.first.c_str(ctx));
#endif
            displaced_clusters.pop();
            if (!ctx->getClusterPlacement(cursor.first, cursor.second, dest_bels))
                goto swap_fail;
            for (const auto &db : dest_bels) {
                // Ensure the cluster is ripped up
                if (db.first->bel != BelId()) {
                    moved_cells[db.first->name] = db.first->bel;
#if CHAIN_DEBUG
                    log_info("%d unbind %s\n", __LINE__, ctx->nameOfBel(db.first->bel));
#endif
                    ctx->unbindBel(db.first->bel);
                }
            }
            for (const auto &db : dest_bels) {
                CellInfo *bound = ctx->getBoundBelCell(db.second);
                BelId old_bel = moved_cells.at(db.first->name);
                if (!ctx->checkBelAvail(old_bel) && bound != nullptr) {
                    // Simple swap no longer possible
                    goto swap_fail;
                }
                if (bound != nullptr) {
                    if (moved_cells.count(bound->name)) {
                        // Don't move a cell multiple times in the same go
                        goto swap_fail;
                    } else if (bound->belStrength > STRENGTH_STRONG) {
                        goto swap_fail;
                    } else if (bound->cluster != ClusterId()) {
                        // Displace the entire cluster
                        Loc old_loc = ctx->getBelLocation(old_bel);
                        Loc bound_loc = ctx->getBelLocation(bound->bel);
                        Loc root_loc = ctx->getBelLocation(ctx->getClusterRootCell(bound->cluster)->bel);
                        Loc new_loc =
                                Loc(old_loc.x + (root_loc.x - bound_loc.x), old_loc.y + (root_loc.y - bound_loc.y),
                                    old_loc.z + (root_loc.z - bound_loc.z));
                        if (new_loc.x < 0 || new_loc.x >= ctx->getGridDimX())
                            goto swap_fail;
                        if (new_loc.y < 0 || new_loc.y >= ctx->getGridDimY())
                            goto swap_fail;
                        BelId new_root = ctx->getBelByLocation(new_loc);
                        if (new_root == BelId())
                            goto swap_fail;
                        for (auto cluster_cell : cluster2cell.at(bound->cluster)) {
                            moved_cells[cluster_cell->name] = cluster_cell->bel;
#if CHAIN_DEBUG
                            log_info("%d unbind %s\n", __LINE__, ctx->nameOfBel(cluster_cell->bel));
#endif
                            ctx->unbindBel(cluster_cell->bel);
                        }
                        displaced_clusters.emplace(bound->cluster, new_root);
                    } else {
                        // Just a single cell to move
                        moved_cells[bound->name] = bound->bel;
#if CHAIN_DEBUG
                        log_info("%d unbind %s\n", __LINE__, ctx->nameOfBel(bound->bel));
                        log_info("%d bind %s %s\n", __LINE__, ctx->nameOfBel(old_bel), ctx->nameOf(bound));
#endif
                        ctx->unbindBel(bound->bel);
                        ctx->bindBel(old_bel, bound, STRENGTH_WEAK);
                    }
                } else if (!ctx->checkBelAvail(db.second)) {
                    goto swap_fail;
                }
                // All those shenanigans should now mean the target bel is free to use
#if CHAIN_DEBUG
                log_info("%d bind %s %s\n", __LINE__, ctx->nameOfBel(db.second), ctx->nameOf(db.first));
#endif
                ctx->bindBel(db.second, db.first, STRENGTH_WEAK);
            }
        }

        live_outcome = 1;
        for (const auto &mm : moved_cells) {
            CellInfo *cell = ctx->cells.at(mm.first).get();
            add_move_cell(moveChange, no_overlay, cell, moved_cells.at(cell->name));
            if (cfg.netShareWeight > 0)
                update_nets_by_tile(cell, ctx->getBelLocation(moved_cells.at(cell->name)),
                                    ctx->getBelLocation(cell->bel));
            if (!ctx->isBelLocationValid(cell->bel) || !cell->testRegion(cell->bel))
                goto swap_fail;
        }
#if CHAIN_DEBUG
        log_info("legal chain swap %s\n", cell->name.c_str(ctx));
#endif
        compute_cost_changes(moveChange, no_overlay);
        live_outcome = 2;
        if (shadow)
            chain_shadow_compare(cell, 2, moveChange.wirelen_delta, moveChange.timing_delta);
        delta = lambda * (moveChange.timing_delta / last_timing_cost) +
                (1 - lambda) * (double(moveChange.wirelen_delta) / last_wirelen_cost);
        if (cfg.netShareWeight > 0) {
            delta +=
                    cfg.netShareWeight * (orig_share_cost - total_net_share) / std::max<double>(total_net_share, 1e-20);
        }
        n_move++;
        // SA acceptance criteria
        if (delta < 0 || (temp > 1e-8 && (ctx->rng() / float(0x3fffffff)) <= std::exp(-delta / temp))) {
            n_accept++;
#if CHAIN_DEBUG
            log_info("accepted chain swap %s\n", cell->name.c_str(ctx));
#endif
        } else {
            goto swap_fail;
        }
        commit_cost_changes(moveChange);
        return true;
    swap_fail:
#if CHAIN_DEBUG
        log_info("Swap failed\n");
#endif
        if (shadow && live_outcome < 2)
            chain_shadow_compare(cell, live_outcome, 0, 0);
        for (auto cell_pair : moved_cells) {
            CellInfo *cell = ctx->cells.at(cell_pair.first).get();
            if (cell->bel != BelId()) {
#if CHAIN_DEBUG
                log_info("%d unbind %s\n", __LINE__, ctx->nameOfBel(cell->bel));
#endif
                ctx->unbindBel(cell->bel);
            }
        }
        for (auto cell_pair : moved_cells) {
            CellInfo *cell = ctx->cells.at(cell_pair.first).get();
#if CHAIN_DEBUG
            log_info("%d bind %s %s\n", __LINE__, ctx->nameOfBel(cell_pair.second), cell->name.c_str(ctx));
#endif
            ctx->bindBel(cell_pair.second, cell, STRENGTH_WEAK);
        }
        return false;
    }

    // Find a random Bel of the correct type for a cell, within the specified
    // diameter
    BelId random_bel_for_cell(CellInfo *cell, int force_z = -1)
    {
        IdString targetType = cell->type;
        Loc curr_loc = ctx->getBelLocation(cell->bel);

        int dx = diameter, dy = diameter;
        if (cell->region != nullptr && cell->region->constr_bels) {
            dx = std::min(cfg.hpwl_scale_x * diameter,
                          (region_bounds[cell->region->name].x1 - region_bounds[cell->region->name].x0) + 1);
            dy = std::min(cfg.hpwl_scale_y * diameter,
                          (region_bounds[cell->region->name].y1 - region_bounds[cell->region->name].y0) + 1);
            // Clamp location to within bounds
            curr_loc.x = std::max(region_bounds[cell->region->name].x0, curr_loc.x);
            curr_loc.x = std::min(region_bounds[cell->region->name].x1, curr_loc.x);
            curr_loc.y = std::max(region_bounds[cell->region->name].y0, curr_loc.y);
            curr_loc.y = std::min(region_bounds[cell->region->name].y1, curr_loc.y);
        }

        FastBels::FastBelsData *bel_data;
        auto type_cnt = fast_bels.getBelsForCellType(targetType, &bel_data);

        while (true) {
            int nx = ctx->rng(2 * dx + 1) + std::max(curr_loc.x - dx, 0);
            int ny = ctx->rng(2 * dy + 1) + std::max(curr_loc.y - dy, 0);
            if (cfg.minBelsForGridPick >= 0 && type_cnt < cfg.minBelsForGridPick)
                nx = ny = 0;
            if (nx >= int(bel_data->size()))
                continue;
            if (ny >= int(bel_data->at(nx).size()))
                continue;
            const auto &fb = bel_data->at(nx).at(ny);
            if (fb.size() == 0)
                continue;
            BelId bel = fb.at(ctx->rng(int(fb.size())));
            if (force_z != -1) {
                Loc loc = ctx->getBelLocation(bel);
                if (loc.z != force_z)
                    continue;
            }
            if (!cell->testRegion(bel))
                continue;
            if (locked_bels.find(bel) != locked_bels.end())
                continue;
            return bel;
        }
    }

    // Return true if a net is to be entirely ignored
    inline bool ignore_net(const BelOverlayList &overlay, NetInfo *net)
    {
        if (net->driver.cell == nullptr)
            return true;
        const BelId driver_bel = overlay_bel(overlay, net->driver.cell);
        return driver_bel == BelId() || ctx->getBelGlobalBuf(driver_bel);
    }

    // Get the bounding box for a net
    inline BoundingBox get_net_bounds(const BelOverlayList &overlay, NetInfo *net)
    {
        BoundingBox bb;
        NPNR_ASSERT(net->driver.cell != nullptr);
        Loc dloc = overlay_loc(overlay, net->driver.cell);
        bb.x0 = dloc.x;
        bb.x1 = dloc.x;
        bb.y0 = dloc.y;
        bb.y1 = dloc.y;
        bb.nx0 = 1;
        bb.nx1 = 1;
        bb.ny0 = 1;
        bb.ny1 = 1;
        for (auto user : net->users) {
            if (!user.cell->isPseudo() && overlay_bel(overlay, user.cell) == BelId())
                continue;
            Loc uloc = overlay_loc(overlay, user.cell);
            if (bb.x0 == uloc.x)
                ++bb.nx0;
            else if (uloc.x < bb.x0) {
                bb.x0 = uloc.x;
                bb.nx0 = 1;
            }
            if (bb.x1 == uloc.x)
                ++bb.nx1;
            else if (uloc.x > bb.x1) {
                bb.x1 = uloc.x;
                bb.nx1 = 1;
            }
            if (bb.y0 == uloc.y)
                ++bb.ny0;
            else if (uloc.y < bb.y0) {
                bb.y0 = uloc.y;
                bb.ny0 = 1;
            }
            if (bb.y1 == uloc.y)
                ++bb.ny1;
            else if (uloc.y > bb.y1) {
                bb.y1 = uloc.y;
                bb.ny1 = 1;
            }
        }

        return bb;
    }

    // Get the timing cost for an arc of a net. Criticality is read from the
    // per-arc table refreshed by setup_costs() after each timing run: the value
    // is the same float the analyser holds, without a hashed CellPortKey lookup
    // on every changed arc of every swap.
    inline double get_timing_cost(const BelOverlayList &overlay, NetInfo *net, store_index<PortRef> user_idx)
    {
        int cc;
        if (net->driver.cell == nullptr)
            return 0;
        if (ctx->getPortTimingClass(net->driver.cell, net->driver.port, cc) == TMG_IGNORE)
            return 0;

        const PortRef &user = net->users.at(user_idx);
        float crit = net_arc_crit[net->udata][user_idx.idx()];
        double delay = ctx->getDelayNS(predict_arc_delay(overlay, net, user));
        return delay * std::pow(crit, crit_exp);
    }

    // Set up the cost maps
    void setup_costs()
    {
        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            if (ignore_net(no_overlay, ni))
                continue;
            net_bounds[ni->udata] = get_net_bounds(no_overlay, ni);
            if (cfg.timing_driven && int(ni->users.entries()) < cfg.timingFanoutThresh)
                for (auto usr : ni->users.enumerate()) {
                    net_arc_crit[ni->udata][usr.index.idx()] = tmg.get_criticality(CellPortKey(usr.value));
                    net_arc_tcost[ni->udata][usr.index.idx()] = get_timing_cost(no_overlay, ni, usr.index);
                }
        }
    }

    // Get the total wiring cost for the design
    wirelen_t total_wirelen_cost()
    {
        wirelen_t cost = 0;
        for (const auto &net : net_bounds)
            cost += net.hpwl(cfg);
        return cost;
    }

    // Get the total timing cost for the design
    double total_timing_cost()
    {
        double cost = 0;
        for (const auto &net : net_arc_tcost) {
            for (auto arc_cost : net) {
                cost += arc_cost;
            }
        }
        return cost;
    }

    // Cost-change-related data for a move
    struct MoveChangeData
    {

        enum BoundChangeType
        {
            NO_CHANGE,
            CELL_MOVED_INWARDS,
            CELL_MOVED_OUTWARDS,
            FULL_RECOMPUTE
        };

        std::vector<decltype(NetInfo::udata)> bounds_changed_nets_x, bounds_changed_nets_y;
        std::vector<std::pair<decltype(NetInfo::udata), store_index<PortRef>>> changed_arcs;

        std::vector<BoundChangeType> already_bounds_changed_x, already_bounds_changed_y;
        std::vector<std::vector<bool>> already_changed_arcs;

        std::vector<BoundingBox> new_net_bounds;
        std::vector<std::pair<std::pair<decltype(NetInfo::udata), store_index<PortRef>>, double>> new_arc_costs;

        wirelen_t wirelen_delta = 0;
        double timing_delta = 0;

        void init(SAPlacer *p)
        {
            already_bounds_changed_x.resize(p->ctx->nets.size());
            already_bounds_changed_y.resize(p->ctx->nets.size());
            already_changed_arcs.resize(p->ctx->nets.size());
            for (auto &net : p->ctx->nets) {
                already_changed_arcs.at(net.second->udata).resize(net.second->users.capacity());
            }
            new_net_bounds = p->net_bounds;
        }

        void reset(SAPlacer *p)
        {
            for (auto bc : bounds_changed_nets_x) {
                new_net_bounds[bc] = p->net_bounds[bc];
                already_bounds_changed_x[bc] = NO_CHANGE;
            }
            for (auto bc : bounds_changed_nets_y) {
                new_net_bounds[bc] = p->net_bounds[bc];
                already_bounds_changed_y[bc] = NO_CHANGE;
            }
            for (const auto &tc : changed_arcs)
                already_changed_arcs[tc.first][tc.second.idx()] = false;
            bounds_changed_nets_x.clear();
            bounds_changed_nets_y.clear();
            changed_arcs.clear();
            new_arc_costs.clear();
            wirelen_delta = 0;
            timing_delta = 0;
        }

    } moveChange;

    // Seam statistics.
    uint64_t seam_assessed = 0, seam_illegal = 0, seam_rejected = 0, seam_committed = 0, seam_unsupported = 0;
    uint64_t seam_shadow_checked = 0, seam_shadow_mismatches = 0;
    // Shadow: the detached result awaiting comparison with the live path.
    bool shadow_pending = false, shadow_legal = false;
    wirelen_t shadow_wirelen_delta = 0;
    double shadow_timing_delta = 0;

    // ------------------------------------------------------------------
    // Batched refinement (Placer1Cfg::swap_batch). See the header for the policy.
    bool batch_mode = false;
    std::unique_ptr<PlacementWorkerPool> worker_pool;
    std::vector<MoveChangeData> worker_mc; // one per worker; worker 0 is the owner
    DeterministicRNG accept_rng;           // per-candidate acceptance stream
    struct SwapCandidate
    {
        CellInfo *cell = nullptr;
        CellInfo *other = nullptr;
        BelId old_bel, new_bel;
        float accept_u = 0;
        bool skip = false; // failed a pre-filter at generation: the serial path would return without a draw
        Placer1SwapAssessment::Status status = Placer1SwapAssessment::Status::Unsupported;
        wirelen_t wirelen_delta = 0;
        double timing_delta = 0;
    };
    std::vector<SwapCandidate> batch;
    // Footprint of swaps accepted earlier in the current batch.
    std::vector<uint8_t> net_marks;
    std::vector<decltype(NetInfo::udata)> marked_nets;
    std::unordered_set<const CellInfo *> touched_cells;
    pool<BelId> touched_bels;
    std::unordered_set<int> touched_tiles;
    uint64_t batch_count = 0, batch_candidates = 0, batch_skipped = 0, batch_stale = 0, batch_unsupported = 0,
             batch_illegal = 0, batch_rejected = 0, batch_committed = 0, batch_delta_mismatches = 0;

    inline int tile_of(BelId bel) const
    {
        const Loc loc = ctx->getBelLocation(bel);
        return loc.y * (max_x + 1) + loc.x;
    }

    void batch_setup()
    {
        worker_pool = std::make_unique<PlacementWorkerPool>(cfg.threads);
        worker_mc.resize(worker_pool->workers());
        for (auto &mc : worker_mc)
            mc.init(this);
        net_marks.assign(ctx->nets.size(), 0);
        accept_rng.rngstate = ctx->rng64();
        log_info("Batched refinement: %d candidates per batch, %u workers.\n", cfg.swap_batch, worker_pool->workers());
    }

    void batch_refresh_scratch()
    {
        for (auto &mc : worker_mc)
            mc.new_net_bounds = net_bounds;
    }

    // The early returns of try_swap_position that need no assessment.
    bool batch_prefilter(const SwapCandidate &c) const
    {
        if (c.cell->cluster != ClusterId())
            return false;
        if (c.other != nullptr && (c.other->cluster != ClusterId() || c.other->belStrength > STRENGTH_WEAK))
            return false;
        if (!ctx->isValidBelForCellType(c.cell->type, c.new_bel))
            return false;
        if (c.other != nullptr && !ctx->isValidBelForCellType(c.other->type, c.old_bel))
            return false;
        return true;
    }

    static void batch_edits(const SwapCandidate &c, std::vector<Placer1SwapEdit> &edits)
    {
        edits.clear();
        edits.push_back({c.new_bel, c.other, c.other ? c.other->belStrength : STRENGTH_NONE, c.cell, STRENGTH_WEAK});
        edits.push_back({c.old_bel, c.cell, c.cell->belStrength, c.other, c.other ? STRENGTH_WEAK : STRENGTH_NONE});
    }

    // Detached: legality from the architecture, cost delta from the overlay, no mutation.
    void batch_evaluate(SwapCandidate &c, MoveChangeData &mc)
    {
        std::vector<Placer1SwapEdit> edits;
        batch_edits(c, edits);
        const auto assessment = cfg.assess_swap(ctx, edits);
        c.status = assessment.status;
        c.wirelen_delta = 0;
        c.timing_delta = 0;
        if (c.status != Placer1SwapAssessment::Status::Legal)
            return;
        BelOverlayList overlay;
        overlay.emplace_back(c.cell, c.new_bel);
        if (c.other != nullptr)
            overlay.emplace_back(c.other, c.old_bel);
        mc.reset(this);
        add_move_cell(mc, overlay, c.cell, c.old_bel);
        if (c.other != nullptr)
            add_move_cell(mc, overlay, c.other, c.new_bel);
        compute_cost_changes(mc, overlay);
        c.wirelen_delta = mc.wirelen_delta;
        c.timing_delta = mc.timing_delta;
    }

    bool batch_nets_marked(const CellInfo *cell) const
    {
        for (const auto &port : cell->ports)
            if (port.second.net != nullptr && net_marks[port.second.net->udata])
                return true;
        return false;
    }

    void batch_mark(const SwapCandidate &c)
    {
        touched_cells.insert(c.cell);
        if (c.other != nullptr)
            touched_cells.insert(c.other);
        touched_bels.insert(c.old_bel);
        touched_bels.insert(c.new_bel);
        touched_tiles.insert(tile_of(c.old_bel));
        touched_tiles.insert(tile_of(c.new_bel));
        for (const CellInfo *cell : {c.cell, c.other}) {
            if (cell == nullptr)
                continue;
            for (const auto &port : cell->ports)
                if (port.second.net != nullptr && !net_marks[port.second.net->udata]) {
                    net_marks[port.second.net->udata] = 1;
                    marked_nets.push_back(port.second.net->udata);
                }
        }
    }

    void batch_consume(SwapCandidate &c)
    {
        static const double epsilon = 1e-20;
        if (c.skip) {
            ++batch_skipped;
            return;
        }
        const bool stale = touched_cells.count(c.cell) || (c.other != nullptr && touched_cells.count(c.other)) ||
                           touched_bels.count(c.old_bel) || touched_bels.count(c.new_bel) ||
                           touched_tiles.count(tile_of(c.old_bel)) || touched_tiles.count(tile_of(c.new_bel)) ||
                           batch_nets_marked(c.cell) || (c.other != nullptr && batch_nets_marked(c.other));
        if (stale) {
            // Re-derive the premise at this turn and evaluate synchronously.
            ++batch_stale;
            c.old_bel = c.cell->bel;
            if (c.new_bel == c.old_bel)
                return;
            c.other = ctx->getBoundBelCell(c.new_bel);
            if (!batch_prefilter(c))
                return;
            batch_evaluate(c, worker_mc[0]);
        }
        if (c.status == Placer1SwapAssessment::Status::Unsupported) {
            ++batch_unsupported;
            if (try_swap_position(c.cell, c.new_bel))
                batch_mark(c);
            return;
        }
        if (c.status == Placer1SwapAssessment::Status::Illegal) {
            ++batch_illegal;
            return;
        }
        double delta = lambda * (c.timing_delta / std::max<double>(last_timing_cost, epsilon)) +
                       (1 - lambda) * (double(c.wirelen_delta) / std::max<double>(last_wirelen_cost, epsilon));
        n_move++;
        if (!(delta < 0 || (temp > 1e-8 && c.accept_u <= std::exp(-delta / temp)))) {
            ++batch_rejected;
            return;
        }
        // Accepted: assess again with a fresh stamp and recompute the delta on the owner.
        // A different delta means an undetected dependency, which is a bug, not a policy.
        std::vector<Placer1SwapEdit> edits;
        batch_edits(c, edits);
        const auto assessment = cfg.assess_swap(ctx, edits);
        if (assessment.status != Placer1SwapAssessment::Status::Legal)
            log_error("Batched swap for '%s' became illegal at its turn without a tracked dependency.\n",
                      ctx->nameOf(c.cell));
        BelOverlayList overlay;
        overlay.emplace_back(c.cell, c.new_bel);
        if (c.other != nullptr)
            overlay.emplace_back(c.other, c.old_bel);
        moveChange.reset(this);
        add_move_cell(moveChange, overlay, c.cell, c.old_bel);
        if (c.other != nullptr)
            add_move_cell(moveChange, overlay, c.other, c.new_bel);
        compute_cost_changes(moveChange, overlay);
        if (moveChange.wirelen_delta != c.wirelen_delta || moveChange.timing_delta != c.timing_delta) {
            ++batch_delta_mismatches;
            log_warning("Batched swap mismatch: cell '%s' other '%s' old %s new %s; recomputed wl %d tmg %.9g, "
                        "speculative wl %d tmg %.9g\n",
                        ctx->nameOf(c.cell), c.other ? ctx->nameOf(c.other) : "-", ctx->nameOfBel(c.old_bel),
                        ctx->nameOfBel(c.new_bel), int(moveChange.wirelen_delta), moveChange.timing_delta,
                        int(c.wirelen_delta), c.timing_delta);
            for (const auto &bc : moveChange.bounds_changed_nets_x)
                log_warning("  x-net '%s' marked=%d kind=%d old [%d..%d] new [%d..%d]\n", ctx->nameOf(net_by_udata[bc]),
                            int(net_marks[bc]), int(moveChange.already_bounds_changed_x[bc]), net_bounds[bc].x0,
                            net_bounds[bc].x1, moveChange.new_net_bounds[bc].x0, moveChange.new_net_bounds[bc].x1);
            for (const auto &bc : moveChange.bounds_changed_nets_y)
                log_warning("  y-net '%s' marked=%d kind=%d old [%d..%d] new [%d..%d]\n", ctx->nameOf(net_by_udata[bc]),
                            int(net_marks[bc]), int(moveChange.already_bounds_changed_y[bc]), net_bounds[bc].y0,
                            net_bounds[bc].y1, moveChange.new_net_bounds[bc].y0, moveChange.new_net_bounds[bc].y1);
            log_error("Batched swap for '%s' changed cost at its turn without a tracked dependency.\n",
                      ctx->nameOf(c.cell));
        }
        if (!cfg.commit_swap(ctx, edits, assessment))
            log_error("Batched swap commit found a changed design for cell '%s'.\n", ctx->nameOf(c.cell));
        commit_cost_changes(moveChange);
        batch_mark(c);
        n_accept++;
        ++batch_committed;
    }

    void run_batched_pass(const std::vector<CellInfo *> &autoplaced)
    {
        const size_t limit = size_t(cfg.swap_batch);
        size_t index = 0;
        while (index < autoplaced.size()) {
            batch.clear();
            // 1. Generate from the state at batch start, drawing the location RNG in order.
            for (; index < autoplaced.size() && batch.size() < limit; ++index) {
                CellInfo *cell = autoplaced[index];
                BelId try_bel = random_bel_for_cell(cell);
                if (try_bel == BelId() || try_bel == cell->bel)
                    continue;
                SwapCandidate c;
                c.cell = cell;
                c.old_bel = cell->bel;
                c.new_bel = try_bel;
                c.other = ctx->getBoundBelCell(try_bel);
                c.accept_u = accept_rng.rng() / float(0x3fffffff);
                c.skip = !batch_prefilter(c);
                batch.push_back(c);
            }
            if (batch.empty())
                continue;
            ++batch_count;
            batch_candidates += batch.size();
            // 2. Evaluate detached. The owner is blocked here, so the design is immutable for workers.
            const auto failure = worker_pool->run(batch.size(), [&](unsigned worker, size_t i) {
                if (!batch[i].skip)
                    batch_evaluate(batch[i], worker_mc.at(worker));
            });
            if (!failure.empty())
                log_error("A refinement worker failed: %s\n", failure.c_str());
            // 3. Consume in order.
            for (auto n : marked_nets)
                net_marks[n] = 0;
            marked_nets.clear();
            touched_cells.clear();
            touched_bels.clear();
            touched_tiles.clear();
            for (auto &c : batch)
                batch_consume(c);
        }
    }

    void add_move_cell(MoveChangeData &mc, const BelOverlayList &overlay, CellInfo *cell, BelId old_bel)
    {
        Loc curr_loc = overlay_loc(overlay, cell);
        Loc old_loc = ctx->getBelLocation(old_bel);
        // Check net bounds
        for (const auto &port : cell->ports) {
            NetInfo *pn = port.second.net;
            if (pn == nullptr)
                continue;
            if (ignore_net(overlay, pn))
                continue;
            BoundingBox &curr_bounds = mc.new_net_bounds[pn->udata];
            // Incremental bounding box updates
            // Note that everything other than full updates are applied immediately rather than being queued,
            // so further updates to the same net in the same move are dealt with correctly.
            // If a full update is already queued, this can be considered a no-op
            if (mc.already_bounds_changed_x[pn->udata] != MoveChangeData::FULL_RECOMPUTE) {
                // Bounds x0
                if (curr_loc.x < curr_bounds.x0) {
                    // Further out than current bounds x0
                    curr_bounds.x0 = curr_loc.x;
                    curr_bounds.nx0 = 1;
                    if (mc.already_bounds_changed_x[pn->udata] == MoveChangeData::NO_CHANGE) {
                        // Checking already_bounds_changed_x ensures that each net is only added once
                        // to bounds_changed_nets, lest we add its HPWL change multiple times skewing the
                        // overall cost change
                        mc.already_bounds_changed_x[pn->udata] = MoveChangeData::CELL_MOVED_OUTWARDS;
                        mc.bounds_changed_nets_x.push_back(pn->udata);
                    }
                } else if (curr_loc.x == curr_bounds.x0 && old_loc.x > curr_bounds.x0) {
                    curr_bounds.nx0++;
                    if (mc.already_bounds_changed_x[pn->udata] == MoveChangeData::NO_CHANGE) {
                        mc.already_bounds_changed_x[pn->udata] = MoveChangeData::CELL_MOVED_OUTWARDS;
                        mc.bounds_changed_nets_x.push_back(pn->udata);
                    }
                } else if (old_loc.x == curr_bounds.x0 && curr_loc.x > curr_bounds.x0) {
                    if (mc.already_bounds_changed_x[pn->udata] == MoveChangeData::NO_CHANGE)
                        mc.bounds_changed_nets_x.push_back(pn->udata);
                    if (curr_bounds.nx0 == 1) {
                        mc.already_bounds_changed_x[pn->udata] = MoveChangeData::FULL_RECOMPUTE;
                    } else {
                        curr_bounds.nx0--;
                        if (mc.already_bounds_changed_x[pn->udata] == MoveChangeData::NO_CHANGE)
                            mc.already_bounds_changed_x[pn->udata] = MoveChangeData::CELL_MOVED_INWARDS;
                    }
                }

                // Bounds x1
                if (curr_loc.x > curr_bounds.x1) {
                    // Further out than current bounds x1
                    curr_bounds.x1 = curr_loc.x;
                    curr_bounds.nx1 = 1;
                    if (mc.already_bounds_changed_x[pn->udata] == MoveChangeData::NO_CHANGE) {
                        // Checking already_bounds_changed_x ensures that each net is only added once
                        // to bounds_changed_nets, lest we add its HPWL change multiple times skewing the
                        // overall cost change
                        mc.already_bounds_changed_x[pn->udata] = MoveChangeData::CELL_MOVED_OUTWARDS;
                        mc.bounds_changed_nets_x.push_back(pn->udata);
                    }
                } else if (curr_loc.x == curr_bounds.x1 && old_loc.x < curr_bounds.x1) {
                    curr_bounds.nx1++;
                    if (mc.already_bounds_changed_x[pn->udata] == MoveChangeData::NO_CHANGE) {
                        mc.already_bounds_changed_x[pn->udata] = MoveChangeData::CELL_MOVED_OUTWARDS;
                        mc.bounds_changed_nets_x.push_back(pn->udata);
                    }
                } else if (old_loc.x == curr_bounds.x1 && curr_loc.x < curr_bounds.x1) {
                    if (mc.already_bounds_changed_x[pn->udata] == MoveChangeData::NO_CHANGE)
                        mc.bounds_changed_nets_x.push_back(pn->udata);
                    if (curr_bounds.nx1 == 1) {
                        mc.already_bounds_changed_x[pn->udata] = MoveChangeData::FULL_RECOMPUTE;
                    } else {
                        curr_bounds.nx1--;
                        if (mc.already_bounds_changed_x[pn->udata] == MoveChangeData::NO_CHANGE)
                            mc.already_bounds_changed_x[pn->udata] = MoveChangeData::CELL_MOVED_INWARDS;
                    }
                }
            }
            if (mc.already_bounds_changed_y[pn->udata] != MoveChangeData::FULL_RECOMPUTE) {
                // Bounds y0
                if (curr_loc.y < curr_bounds.y0) {
                    // Further out than current bounds y0
                    curr_bounds.y0 = curr_loc.y;
                    curr_bounds.ny0 = 1;
                    if (mc.already_bounds_changed_y[pn->udata] == MoveChangeData::NO_CHANGE) {
                        mc.already_bounds_changed_y[pn->udata] = MoveChangeData::CELL_MOVED_OUTWARDS;
                        mc.bounds_changed_nets_y.push_back(pn->udata);
                    }
                } else if (curr_loc.y == curr_bounds.y0 && old_loc.y > curr_bounds.y0) {
                    curr_bounds.ny0++;
                    if (mc.already_bounds_changed_y[pn->udata] == MoveChangeData::NO_CHANGE) {
                        mc.already_bounds_changed_y[pn->udata] = MoveChangeData::CELL_MOVED_OUTWARDS;
                        mc.bounds_changed_nets_y.push_back(pn->udata);
                    }
                } else if (old_loc.y == curr_bounds.y0 && curr_loc.y > curr_bounds.y0) {
                    if (mc.already_bounds_changed_y[pn->udata] == MoveChangeData::NO_CHANGE)
                        mc.bounds_changed_nets_y.push_back(pn->udata);
                    if (curr_bounds.ny0 == 1) {
                        mc.already_bounds_changed_y[pn->udata] = MoveChangeData::FULL_RECOMPUTE;
                    } else {
                        curr_bounds.ny0--;
                        if (mc.already_bounds_changed_y[pn->udata] == MoveChangeData::NO_CHANGE)
                            mc.already_bounds_changed_y[pn->udata] = MoveChangeData::CELL_MOVED_INWARDS;
                    }
                }

                // Bounds y1
                if (curr_loc.y > curr_bounds.y1) {
                    // Further out than current bounds y1
                    curr_bounds.y1 = curr_loc.y;
                    curr_bounds.ny1 = 1;
                    if (mc.already_bounds_changed_y[pn->udata] == MoveChangeData::NO_CHANGE) {
                        mc.already_bounds_changed_y[pn->udata] = MoveChangeData::CELL_MOVED_OUTWARDS;
                        mc.bounds_changed_nets_y.push_back(pn->udata);
                    }
                } else if (curr_loc.y == curr_bounds.y1 && old_loc.y < curr_bounds.y1) {
                    curr_bounds.ny1++;
                    if (mc.already_bounds_changed_y[pn->udata] == MoveChangeData::NO_CHANGE) {
                        mc.already_bounds_changed_y[pn->udata] = MoveChangeData::CELL_MOVED_OUTWARDS;
                        mc.bounds_changed_nets_y.push_back(pn->udata);
                    }
                } else if (old_loc.y == curr_bounds.y1 && curr_loc.y < curr_bounds.y1) {
                    if (mc.already_bounds_changed_y[pn->udata] == MoveChangeData::NO_CHANGE)
                        mc.bounds_changed_nets_y.push_back(pn->udata);
                    if (curr_bounds.ny1 == 1) {
                        mc.already_bounds_changed_y[pn->udata] = MoveChangeData::FULL_RECOMPUTE;
                    } else {
                        curr_bounds.ny1--;
                        if (mc.already_bounds_changed_y[pn->udata] == MoveChangeData::NO_CHANGE)
                            mc.already_bounds_changed_y[pn->udata] = MoveChangeData::CELL_MOVED_INWARDS;
                    }
                }
            }

            if (cfg.timing_driven && int(pn->users.entries()) < cfg.timingFanoutThresh) {
                // Output ports - all arcs change timing
                if (port.second.type == PORT_OUT) {
                    int cc;
                    TimingPortClass cls = ctx->getPortTimingClass(cell, port.first, cc);
                    if (cls != TMG_IGNORE)
                        for (auto usr : pn->users.enumerate())
                            if (!mc.already_changed_arcs[pn->udata][usr.index.idx()]) {
                                mc.changed_arcs.emplace_back(std::make_pair(pn->udata, usr.index));
                                mc.already_changed_arcs[pn->udata][usr.index.idx()] = true;
                            }
                } else if (port.second.type == PORT_IN) {
                    auto usr_idx = port.second.user_idx;
                    if (!mc.already_changed_arcs[pn->udata][usr_idx.idx()]) {
                        mc.changed_arcs.emplace_back(std::make_pair(pn->udata, usr_idx));
                        mc.already_changed_arcs[pn->udata][usr_idx.idx()] = true;
                    }
                }
            }
        }
    }

    void compute_cost_changes(MoveChangeData &md, const BelOverlayList &overlay)
    {
        for (const auto &bc : md.bounds_changed_nets_x) {
            if (md.already_bounds_changed_x[bc] == MoveChangeData::FULL_RECOMPUTE)
                md.new_net_bounds[bc] = get_net_bounds(overlay, net_by_udata[bc]);
        }
        for (const auto &bc : md.bounds_changed_nets_y) {
            if (md.already_bounds_changed_x[bc] != MoveChangeData::FULL_RECOMPUTE &&
                md.already_bounds_changed_y[bc] == MoveChangeData::FULL_RECOMPUTE)
                md.new_net_bounds[bc] = get_net_bounds(overlay, net_by_udata[bc]);
        }

        for (const auto &bc : md.bounds_changed_nets_x)
            md.wirelen_delta += md.new_net_bounds[bc].hpwl(cfg) - net_bounds[bc].hpwl(cfg);
        for (const auto &bc : md.bounds_changed_nets_y)
            if (md.already_bounds_changed_x[bc] == MoveChangeData::NO_CHANGE)
                md.wirelen_delta += md.new_net_bounds[bc].hpwl(cfg) - net_bounds[bc].hpwl(cfg);

        if (cfg.timing_driven) {
            for (const auto &tc : md.changed_arcs) {
                double old_cost = net_arc_tcost.at(tc.first).at(tc.second.idx());
                double new_cost = get_timing_cost(overlay, net_by_udata.at(tc.first), tc.second);
                md.new_arc_costs.emplace_back(std::make_pair(tc, new_cost));
                md.timing_delta += (new_cost - old_cost);
                md.already_changed_arcs[tc.first][tc.second.idx()] = false;
            }
        }
    }

    void commit_cost_changes(MoveChangeData &md)
    {
        for (const auto &bc : md.bounds_changed_nets_x)
            net_bounds[bc] = md.new_net_bounds[bc];
        for (const auto &bc : md.bounds_changed_nets_y)
            net_bounds[bc] = md.new_net_bounds[bc];
        for (const auto &tc : md.new_arc_costs)
            net_arc_tcost[tc.first.first].at(tc.first.second.idx()) = tc.second;
        curr_wirelen_cost += md.wirelen_delta;
        curr_timing_cost += md.timing_delta;
        // Every commit, whichever path made it (batched, live for unsupported swaps, or chain
        // swaps), must reach the workers' scratch copies of the bounds, or their next
        // speculative delta reads stale values.
        if (batch_mode) {
            for (auto &mc : worker_mc) {
                for (const auto &bc : md.bounds_changed_nets_x)
                    mc.new_net_bounds[bc] = net_bounds[bc];
                for (const auto &bc : md.bounds_changed_nets_y)
                    mc.new_net_bounds[bc] = net_bounds[bc];
            }
        }
    }

    // Simple routeability driven placement
    const int large_cell_thresh = 50;
    int total_net_share = 0;
    std::vector<std::vector<dict<IdString, int>>> nets_by_tile;
    void setup_nets_by_tile()
    {
        total_net_share = 0;
        nets_by_tile.resize(max_x + 1, std::vector<dict<IdString, int>>(max_y + 1));
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->isPseudo() || (int(ci->ports.size()) > large_cell_thresh))
                continue;
            Loc loc = ctx->getBelLocation(ci->bel);
            auto &nbt = nets_by_tile.at(loc.x).at(loc.y);
            for (const auto &port : ci->ports) {
                if (port.second.net == nullptr)
                    continue;
                if (port.second.net->driver.cell == nullptr || ctx->getBelGlobalBuf(port.second.net->driver.cell->bel))
                    continue;
                int &s = nbt[port.second.net->name];
                if (s > 0)
                    ++total_net_share;
                ++s;
            }
        }
    }

    int update_nets_by_tile(CellInfo *ci, Loc old_loc, Loc new_loc)
    {
        if (int(ci->ports.size()) > large_cell_thresh)
            return 0;
        int loss = 0, gain = 0;
        auto &nbt_old = nets_by_tile.at(old_loc.x).at(old_loc.y);
        auto &nbt_new = nets_by_tile.at(new_loc.x).at(new_loc.y);

        for (const auto &port : ci->ports) {
            if (port.second.net == nullptr)
                continue;
            if (port.second.net->driver.cell == nullptr || ctx->getBelGlobalBuf(port.second.net->driver.cell->bel))
                continue;
            int &o = nbt_old[port.second.net->name];
            --o;
            NPNR_ASSERT(o >= 0);
            if (o > 0)
                ++loss;
            int &n = nbt_new[port.second.net->name];
            if (n > 0)
                ++gain;
            ++n;
        }
        int delta = gain - loss;
        total_net_share += delta;
        return delta;
    }

    // Get the combined wirelen/timing metric
    inline double curr_metric()
    {
        return lambda * curr_timing_cost + (1 - lambda) * curr_wirelen_cost - cfg.netShareWeight * total_net_share;
    }

    // Map nets to their bounding box (so we can skip recompute for moves that do not exceed the bounds
    std::vector<BoundingBox> net_bounds;
    // Map net arcs to their timing cost (criticality * delay ns)
    std::vector<std::vector<double>> net_arc_tcost;
    std::vector<std::vector<float>> net_arc_crit; // criticality per arc, refreshed in setup_costs()

    // Fast lookup for cell to clusters
    dict<ClusterId, std::vector<CellInfo *>> cluster2cell;

    // Wirelength and timing cost at last and current iteration
    wirelen_t last_wirelen_cost, curr_wirelen_cost;
    double last_timing_cost, curr_timing_cost;

    Context *ctx;
    float temp = 10;
    float crit_exp = 8;
    float lambda = 0.5;
    bool improved = false;
    int n_move, n_accept;
    int diameter = 35, max_x = 1, max_y = 1;
    dict<IdString, std::tuple<int, int>> bel_types;
    dict<IdString, BoundingBox> region_bounds;
    FastBels fast_bels;
    pool<BelId> locked_bels;
    std::vector<NetInfo *> net_by_udata;
    std::vector<decltype(NetInfo::udata)> old_udata;
    bool require_legal = true;
    const int legalise_dia = 4;
    Placer1Cfg cfg;

    TimingAnalyser tmg;
};

Placer1Cfg::Placer1Cfg(Context *ctx)
{
    constraintWeight = ctx->setting<float>("placer1/constraintWeight", 10);
    netShareWeight = ctx->setting<float>("placer1/netShareWeight", 0);
    minBelsForGridPick = ctx->setting<int>("placer1/minBelsForGridPick", 64);
    startTemp = ctx->setting<float>("placer1/startTemp", 1);
    timingFanoutThresh = std::numeric_limits<int>::max();
    timing_driven = ctx->setting<bool>("timing_driven");
    hpwl_scale_x = 1;
    hpwl_scale_y = 1;
}

bool placer1(Context *ctx, Placer1Cfg cfg)
{
    try {
        SAPlacer placer(ctx, cfg);
        placer.place();
        log_info("Checksum: 0x%08x\n", ctx->checksum());
#ifndef NDEBUG
        ctx->lock();
        ctx->check();
        ctx->unlock();
#endif
        return true;
    } catch (log_execution_error_exception) {
#ifndef NDEBUG
        ctx->lock();
        ctx->check();
        ctx->unlock();
#endif
        return false;
    }
}

bool placer1_refine(Context *ctx, Placer1Cfg cfg)
{
    try {
        SAPlacer placer(ctx, cfg);
        placer.place(true);
        log_info("Checksum: 0x%08x\n", ctx->checksum());
#ifndef NDEBUG
        ctx->lock();
        ctx->check();
        ctx->unlock();
#endif
        return true;
    } catch (log_execution_error_exception) {
#ifndef NDEBUG
        ctx->lock();
        ctx->check();
        ctx->unlock();
#endif
        return false;
    }
}

NEXTPNR_NAMESPACE_END
