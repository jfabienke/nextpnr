/* SPDX-License-Identifier: ISC */
#include "alm_pairing.h"
#include "pack_admission.h"

#include <algorithm>
#include <cinttypes>
#include <unordered_map>
#include <unordered_set>

#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

const IdString lut_ports[] = {id_A, id_B, id_C, id_D, id_E};

std::vector<const NetInfo *> input_nets(const CellInfo *ci)
{
    std::vector<const NetInfo *> nets;
    for (IdString port : lut_ports) {
        const NetInfo *net = ci->getPort(port);
        if (net != nullptr && std::find(nets.begin(), nets.end(), net) == nets.end())
            nets.push_back(net);
    }
    return nets;
}

int shared_inputs(const CellInfo *a, const CellInfo *b)
{
    auto na = input_nets(a), nb = input_nets(b);
    int shared = 0;
    for (const NetInfo *n : na)
        if (std::find(nb.begin(), nb.end(), n) != nb.end())
            ++shared;
    return shared;
}

bool eligible(const CellInfo *ci)
{
    return alm_pair_lut_inputs(ci->type) >= 0 && ci->cluster == ClusterId() && ci->constr_children.empty() &&
           ci->bel == BelId() && !ci->attrs.count(id_BEL);
}

} // namespace

int alm_pair_lut_inputs(IdString type)
{
    if (type == id_MISTRAL_ALUT5)
        return 5;
    if (type == id_MISTRAL_ALUT4)
        return 4;
    if (type == id_MISTRAL_ALUT3)
        return 3;
    if (type == id_MISTRAL_ALUT2)
        return 2;
    if (type == id_MISTRAL_NOT)
        return 1;
    return -1;
}

bool alm_pair_compatible(const CellInfo *a, const CellInfo *b)
{
    const int na = alm_pair_lut_inputs(a->type), nb = alm_pair_lut_inputs(b->type);
    if (na < 0 || nb < 0)
        return false;
    if ((1 << na) + (1 << nb) > 64)
        return false;
    // Each half has three exclusive input lines; what a LUT needs beyond three must sit on the
    // two shared lines A and B, and both halves' shared-line nets together must fit in two.
    const int shared_a = std::max(0, na - 3), shared_b = std::max(0, nb - 3);
    const int common = std::min(shared_inputs(a, b), std::min(shared_a, shared_b));
    return shared_a + shared_b - common <= 2;
}

bool is_alm_pair_root(const CellInfo *root)
{
    if (root->constr_children.size() != 1 || root->constr_abs_z || root->constr_z != 0 || root->cluster != root->name)
        return false;
    const CellInfo *child = root->constr_children.front();
    return alm_pair_lut_inputs(root->type) >= 0 && alm_pair_lut_inputs(child->type) >= 0 && !child->constr_abs_z &&
           child->constr_x == 0 && child->constr_y == 0 && child->constr_z == 1 && child->cluster == root->name;
}

AlmPairingReport pair_alm_luts(Context &ctx, int level)
{
    AlmPairingReport report;
    // Deterministic order: largest LUTs first (they have the fewest partners), then by name.
    std::vector<CellInfo *> luts;
    for (auto &cell : ctx.cells)
        if (eligible(cell.second.get()))
            luts.push_back(cell.second.get());
    std::sort(luts.begin(), luts.end(), [](const CellInfo *a, const CellInfo *b) {
        const int na = alm_pair_lut_inputs(a->type), nb = alm_pair_lut_inputs(b->type);
        return na != nb ? na > nb : a->name.index < b->name.index;
    });
    report.eligible = luts.size();
    std::unordered_map<const NetInfo *, std::vector<CellInfo *>> users; // input net -> eligible LUTs
    for (CellInfo *ci : luts)
        for (const NetInfo *net : input_nets(ci))
            users[net].push_back(ci);
    std::unordered_set<const CellInfo *> paired;
    // The rule above is the search's filter; what admits a pair is the authority the placer will
    // ask, on a clean ALM, before the pair is committed (design section 13).
    PackAdmission gate(ctx, /*assign_facts=*/true);

    struct Constraint
    {
        ClusterId cluster;
        int x, y, z;
        bool abs_z;
    };
    auto constraint_of = [](const CellInfo *c) {
        return Constraint{c->cluster, c->constr_x, c->constr_y, c->constr_z, c->constr_abs_z};
    };
    auto restore = [](CellInfo *c, const Constraint &was) {
        c->cluster = was.cluster;
        c->constr_x = was.x;
        c->constr_y = was.y;
        c->constr_z = was.z;
        c->constr_abs_z = was.abs_z;
    };

    auto pair_up = [&](CellInfo *a, CellInfo *b, uint64_t &bucket) -> bool {
        CellInfo *root = a, *child = b;
        if (alm_pair_lut_inputs(b->type) > alm_pair_lut_inputs(a->type) ||
            (alm_pair_lut_inputs(b->type) == alm_pair_lut_inputs(a->type) && b->name.index < a->name.index))
            std::swap(root, child);
        const Constraint root_was = constraint_of(root), child_was = constraint_of(child);
        root->cluster = root->name;
        root->constr_abs_z = false;
        root->constr_z = 0;
        child->cluster = root->name;
        child->constr_x = 0;
        child->constr_y = 0;
        child->constr_z = 1;
        child->constr_abs_z = false;
        root->constr_children.push_back(child);
        if (!gate.admits(root)) {
            root->constr_children.pop_back();
            restore(root, root_was);
            restore(child, child_was);
            report.refused++;
            return false;
        }
        paired.insert(a);
        paired.insert(b);
        report.pairs++;
        bucket++;
        report.shared_nets += uint64_t(shared_inputs(a, b));
        return true;
    };

    // Level 1: partners that share input nets, most shared first, then larger partners first.
    for (CellInfo *ci : luts) {
        if (paired.count(ci))
            continue;
        CellInfo *best = nullptr;
        int best_score = -1;
        for (const NetInfo *net : input_nets(ci)) {
            for (CellInfo *other : users.at(net)) {
                if (other == ci || paired.count(other) || !alm_pair_compatible(ci, other))
                    continue;
                const int score = shared_inputs(ci, other) * 16 + alm_pair_lut_inputs(other->type);
                if (score > best_score || (score == best_score && other->name.index < best->name.index)) {
                    best = other;
                    best_score = score;
                }
            }
        }
        if (best != nullptr)
            pair_up(ci, best, report.by_shared);
    }
    // Level 2: a LUT and one it drives or is driven by.
    if (level >= 2) {
        for (CellInfo *ci : luts) {
            if (paired.count(ci))
                continue;
            std::vector<CellInfo *> linked;
            for (const NetInfo *net : input_nets(ci))
                if (net->driver.cell != nullptr && eligible(net->driver.cell) && !paired.count(net->driver.cell))
                    linked.push_back(net->driver.cell);
            if (const NetInfo *q = ci->getPort(id_Q))
                for (auto &usr : q->users)
                    if (usr.cell != ci && eligible(usr.cell) && !paired.count(usr.cell))
                        linked.push_back(usr.cell);
            std::sort(linked.begin(), linked.end(),
                      [](const CellInfo *a, const CellInfo *b) { return a->name.index < b->name.index; });
            for (CellInfo *other : linked)
                if (alm_pair_compatible(ci, other) && pair_up(ci, other, report.by_link))
                    break;
        }
    }
    // Level 3: anything compatible, in order.
    if (level >= 3) {
        std::vector<CellInfo *> left;
        for (CellInfo *ci : luts)
            if (!paired.count(ci))
                left.push_back(ci);
        for (size_t i = 0; i < left.size(); i++) {
            if (paired.count(left[i]))
                continue;
            for (size_t j = i + 1; j < left.size(); j++) {
                if (!paired.count(left[j]) && alm_pair_compatible(left[i], left[j]) &&
                    pair_up(left[i], left[j], report.by_any))
                    break;
            }
        }
    }
    report.unpaired = report.eligible - 2 * report.pairs;
    report_pack_admission(ctx, gate, "alm-pairing");
    return report;
}

void report_alm_pairing(const AlmPairingReport &r)
{
    log_info("ALM pairing: %" PRIu64 " pairable LUTs, %" PRIu64 " pairs (%" PRIu64 " sharing inputs, %" PRIu64
             " linked, %" PRIu64 " unrelated), %" PRIu64 " left single, %" PRIu64 " input nets shared inside pairs.\n",
             r.eligible, r.pairs, r.by_shared, r.by_link, r.by_any, r.unpaired, r.shared_nets);
}

NEXTPNR_NAMESPACE_END
