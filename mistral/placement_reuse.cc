/* SPDX-License-Identifier: ISC */
#include "placement_reuse.h"

#include <cinttypes>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

using json11::Json;

struct PreviousCell
{
    std::string type;
    std::string bel;
    // Sorted so that dict iteration order in the current design does not matter.
    std::map<std::string, std::string> params;
    std::map<std::string, std::string> attrs;
    std::map<std::string, std::string> nets; // port -> net name, route-throughs folded out
};

bool ends_with(const std::string &s, const char *suffix)
{
    const std::string t(suffix);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

bool is_placement_attr(const std::string &name)
{
    return name == "NEXTPNR_BEL" || name == "BEL_STRENGTH" || name == "BEL" || name == "ROUTING";
}

std::string json_scalar(const Json &value)
{
    if (value.is_string())
        return value.string_value();
    if (value.is_number())
        return std::to_string(int64_t(value.number_value()));
    if (value.is_bool())
        return value.bool_value() ? "1" : "0";
    return value.dump();
}

// Loads the previous output JSON into name-keyed records.
std::unordered_map<std::string, PreviousCell> load_previous(const std::string &path, PlacementReuseReport &report)
{
    std::ifstream in(path);
    if (!in)
        log_error("Cannot open previous placement '%s'.\n", path.c_str());
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string error;
    const Json root = Json::parse(buffer.str(), error);
    if (!error.empty())
        log_error("Previous placement '%s' is not valid JSON: %s\n", path.c_str(), error.c_str());
    const Json &modules = root["modules"];
    if (!modules.is_object() || modules.object_items().empty())
        log_error("Previous placement '%s' has no modules.\n", path.c_str());
    // nextpnr writes exactly one flattened module; take the largest defensively.
    const Json *module = nullptr;
    size_t best = 0;
    for (const auto &entry : modules.object_items()) {
        const size_t count = entry.second["cells"].object_items().size();
        if (module == nullptr || count > best) {
            module = &entry.second;
            best = count;
        }
    }

    std::unordered_map<int64_t, std::string> bit_names;
    for (const auto &net : (*module)["netnames"].object_items())
        for (const auto &bit : net.second["bits"].array_items())
            if (bit.is_number())
                bit_names.emplace(int64_t(bit.number_value()), net.first);

    // Route-through buffers were generated during the previous run's LAB
    // preparation; they are absent at placement time now. Fold their output
    // nets back to their input nets so consumers match their pre-route state.
    std::unordered_map<std::string, std::string> routethru;
    const auto &cells = (*module)["cells"].object_items();
    for (const auto &cell : cells) {
        if (cell.second["type"].string_value() != "MISTRAL_BUF" || !ends_with(cell.first, "$ROUTETHRU"))
            continue;
        const auto &conn = cell.second["connections"];
        const auto &q = conn["Q"].array_items();
        const auto &a = conn["A"].array_items();
        if (q.size() == 1 && a.size() == 1 && q[0].is_number() && a[0].is_number()) {
            auto out = bit_names.find(int64_t(q[0].number_value()));
            auto inp = bit_names.find(int64_t(a[0].number_value()));
            if (out != bit_names.end() && inp != bit_names.end())
                routethru[out->second] = inp->second;
        }
        ++report.previous_routethru;
    }

    std::unordered_map<std::string, PreviousCell> previous;
    previous.reserve(cells.size());
    for (const auto &cell : cells) {
        if (cell.second["type"].string_value() == "MISTRAL_BUF" && ends_with(cell.first, "$ROUTETHRU"))
            continue;
        PreviousCell record;
        record.type = cell.second["type"].string_value();
        for (const auto &attr : cell.second["attributes"].object_items()) {
            if (attr.first == "NEXTPNR_BEL")
                record.bel = attr.second.string_value();
            else if (!is_placement_attr(attr.first))
                record.attrs.emplace(attr.first, json_scalar(attr.second));
        }
        for (const auto &param : cell.second["parameters"].object_items())
            record.params.emplace(param.first, json_scalar(param.second));
        for (const auto &conn : cell.second["connections"].object_items()) {
            const auto &bits = conn.second.array_items();
            if (bits.size() != 1 || !bits[0].is_number())
                continue; // multi-bit or constant: the current design's port compares as absent
            auto name = bit_names.find(int64_t(bits[0].number_value()));
            if (name == bit_names.end())
                continue;
            std::string net = name->second;
            for (unsigned hops = 0; hops < 4; ++hops) {
                auto folded = routethru.find(net);
                if (folded == routethru.end())
                    break;
                net = folded->second;
            }
            record.nets.emplace(conn.first, net);
        }
        previous.emplace(cell.first, std::move(record));
        ++report.previous_cells;
    }
    return previous;
}

bool signature_matches(const Context &ctx, const CellInfo &cell, const PreviousCell &previous)
{
    if (cell.type.str(&ctx) != previous.type)
        return false;
    std::map<std::string, std::string> params, attrs, nets;
    for (const auto &param : cell.params)
        params.emplace(param.first.str(&ctx), param.second.to_string());
    for (const auto &attr : cell.attrs) {
        const std::string name = attr.first.str(&ctx);
        if (!is_placement_attr(name))
            attrs.emplace(name, attr.second.to_string());
    }
    for (const auto &port : cell.ports)
        if (port.second.net != nullptr)
            nets.emplace(port.first.str(&ctx), port.second.net->name.str(&ctx));
    return params == previous.params && attrs == previous.attrs && nets == previous.nets;
}

// Why a signature differs, for the plan's reason field.
std::string signature_difference(const Context &ctx, const CellInfo &cell, const PreviousCell &previous)
{
    if (cell.type.str(&ctx) != previous.type)
        return "type differs (" + previous.type + " before, " + cell.type.str(&ctx) + " now)";
    std::map<std::string, std::string> params, attrs, nets;
    for (const auto &param : cell.params)
        params.emplace(param.first.str(&ctx), param.second.to_string());
    for (const auto &attr : cell.attrs) {
        const std::string name = attr.first.str(&ctx);
        if (!is_placement_attr(name))
            attrs.emplace(name, attr.second.to_string());
    }
    for (const auto &port : cell.ports)
        if (port.second.net != nullptr)
            nets.emplace(port.first.str(&ctx), port.second.net->name.str(&ctx));
    auto first_diff = [](const std::map<std::string, std::string> &a, const std::map<std::string, std::string> &b) {
        for (const auto &kv : a) {
            auto it = b.find(kv.first);
            if (it == b.end() || it->second != kv.second)
                return kv.first;
        }
        for (const auto &kv : b)
            if (!a.count(kv.first))
                return kv.first;
        return std::string();
    };
    if (params != previous.params)
        return "parameter " + first_diff(params, previous.params) + " differs";
    if (attrs != previous.attrs)
        return "attribute " + first_diff(attrs, previous.attrs) + " differs";
    if (nets != previous.nets)
        return "connectivity of port " + first_diff(nets, previous.nets) + " differs";
    return "signature differs";
}

std::unordered_map<std::string, PreviousCell> g_previous; // of the last plan, for the region release below
} // namespace

void plan_placement_reuse(Context &ctx, const std::string &path, ReusePlan &plan)
{
    PlacementReuseReport counts;
    auto previous = load_previous(path, counts);
    plan.previous_path = path;
    plan.previous_cells = counts.previous_cells;
    plan.previous_routethru = counts.previous_routethru;
    plan.cells.clear();
    const IdString id_bel = ctx.id("BEL");
    pool<std::string> seen;
    for (auto &entry : ctx.cells) {
        CellInfo *cell = entry.second.get();
        const std::string name = cell->name.str(&ctx);
        CellReuseDecision d;
        d.cell = name;
        auto found = previous.find(name);
        if (found == previous.end()) {
            d.decision = ReuseDecision::Added;
            d.reason = "no previous cell of this name";
            plan.cells.push_back(std::move(d));
            continue;
        }
        seen.insert(name);
        d.bel = found->second.bel;
        if (cell->bel != BelId() || cell->attrs.count(id_bel)) {
            // Pin constraints bind IO cells during packing and user BEL attributes are
            // hard constraints; both win over the previous placement.
            d.decision = ReuseDecision::UserConstrained;
            d.reason = cell->bel != BelId() ? "bound before placement (pin constraint)" : "user BEL attribute";
        } else if (!signature_matches(ctx, *cell, found->second)) {
            d.decision = ReuseDecision::Changed;
            d.reason = signature_difference(ctx, *cell, found->second);
        } else {
            BelId bel;
            try {
                if (!found->second.bel.empty())
                    bel = ctx.getBelByNameStr(found->second.bel);
            } catch (const std::exception &) {
                bel = BelId(); // unknown coordinates or a type mismatch on this device
            }
            if (bel == BelId()) {
                d.decision = ReuseDecision::MissingBel;
                d.reason = "previous BEL does not resolve on this device";
            } else {
                d.decision = ReuseDecision::Reuse;
                d.reason = "same name and signature";
            }
        }
        plan.cells.push_back(std::move(d));
    }
    plan.removed_cells = 0;
    for (const auto &entry : previous)
        if (!seen.count(entry.first))
            ++plan.removed_cells;
    g_previous = std::move(previous);
}

PlacementReuseReport apply_placement_reuse(Context &ctx, const ReusePlan &plan)
{
    PlacementReuseReport report;
    report.previous_cells = plan.previous_cells;
    report.previous_routethru = plan.previous_routethru;
    report.current_cells = plan.cells.size();
    report.removed = plan.removed_cells;
    const IdString id_bel = ctx.id("BEL");
    for (const auto &d : plan.cells) {
        switch (d.decision) {
        case ReuseDecision::Reuse: {
            auto it = ctx.cells.find(ctx.id(d.cell));
            if (it == ctx.cells.end())
                log_error("placement reuse: plan names unknown cell '%s'.\n", d.cell.c_str());
            it->second->attrs[id_bel] = d.bel;
            ++report.matched;
            break;
        }
        case ReuseDecision::Changed:
            ++report.changed;
            break;
        case ReuseDecision::Added:
            ++report.added;
            break;
        case ReuseDecision::UserConstrained:
            ++report.user_constrained;
            break;
        case ReuseDecision::MissingBel:
            ++report.missing_bel;
            break;
        case ReuseDecision::Released:
            ++report.released;
            break;
        default:
            break;
        }
    }
    return report;
}

PlacementReuseReport apply_placement_reuse(Context &ctx, const std::string &path)
{
    ReusePlan plan;
    plan_placement_reuse(ctx, path, plan);
    return apply_placement_reuse(ctx, plan);
}

unsigned release_placement_region(Context &ctx, ReusePlan &plan, int radius)
{
    const IdString id_bel = ctx.id("BEL");
    auto loc_of = [&](const std::string &bel_name, Loc &loc) {
        try {
            BelId bel = ctx.getBelByNameStr(bel_name);
            if (bel == BelId())
                return false;
            loc = ctx.getBelLocation(bel);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    };
    // Anchors: previous BELs of changed cells; for added cells, the previous
    // BELs of the cells on their nets.
    std::vector<Loc> anchors;
    if (radius >= 0) {
        for (const auto &d : plan.cells) {
            if (d.decision == ReuseDecision::Changed) {
                Loc loc;
                if (loc_of(d.bel, loc))
                    anchors.push_back(loc);
            } else if (d.decision == ReuseDecision::Added) {
                auto it = ctx.cells.find(ctx.id(d.cell));
                if (it == ctx.cells.end())
                    continue;
                for (auto &port : it->second->ports) {
                    NetInfo *net = port.second.net;
                    if (net == nullptr)
                        continue;
                    auto anchor_cell = [&](const CellInfo *other) {
                        if (other == nullptr)
                            return;
                        auto prev = g_previous.find(other->name.str(&ctx));
                        Loc loc;
                        if (prev != g_previous.end() && loc_of(prev->second.bel, loc))
                            anchors.push_back(loc);
                    };
                    anchor_cell(net->driver.cell);
                    for (auto &user : net->users)
                        anchor_cell(user.cell);
                }
            }
        }
    }
    unsigned released = 0;
    for (auto &d : plan.cells) {
        if (d.decision != ReuseDecision::Reuse)
            continue;
        bool release = radius < 0;
        if (!release) {
            Loc loc;
            if (loc_of(d.bel, loc))
                for (const Loc &a : anchors)
                    if (std::abs(a.x - loc.x) + std::abs(a.y - loc.y) <= radius) {
                        release = true;
                        break;
                    }
        }
        if (!release)
            continue;
        d.decision = ReuseDecision::Released;
        d.reason = radius < 0 ? "released: every transplant dropped after repeated placer failures"
                              : "released: within " + std::to_string(radius) +
                                        " tiles of a dirty cell after a placer failure";
        auto it = ctx.cells.find(ctx.id(d.cell));
        if (it != ctx.cells.end())
            it->second->attrs.erase(id_bel);
        ++released;
    }
    plan.released_cells += released;
    if (radius < 0)
        plan.placement_full_fallback = true;
    return released;
}

void report_placement_reuse(const PlacementReuseReport &r)
{
    log_info("Placement reuse: previous=%" PRIu64 " (+%" PRIu64 " route-throughs), current=%" PRIu64 ", reused=%" PRIu64
             " (%.1f%%), changed=%" PRIu64 ", added=%" PRIu64 ", removed=%" PRIu64 ", user-constrained=%" PRIu64
             ", missing-bel=%" PRIu64 ", released=%" PRIu64 "\n",
             r.previous_cells, r.previous_routethru, r.current_cells, r.matched,
             r.current_cells ? 100.0 * double(r.matched) / double(r.current_cells) : 0.0, r.changed, r.added, r.removed,
             r.user_constrained, r.missing_bel, r.released);
}

NEXTPNR_NAMESPACE_END
