/* SPDX-License-Identifier: ISC */
#include "checkpoint.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"
#include "version.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
using json11::Json;

constexpr int SCHEMA = 1;
constexpr int BACKEND_STATE_VERSION = 1;

int lookup_index(const BaseCtx *ctx, const std::string &name)
{
    auto it = ctx->idstring_str_to_idx->find(name);
    return it == ctx->idstring_str_to_idx->end() ? -1 : it->second;
}

// Names in the checkpoint must already be interned: the table was replayed
// before the netlist was imported, so an unknown name is corrupt data, and
// interning it here would silently shift every later index.
IdString known_id(const Context *ctx, const std::string &name, const char *what)
{
    int index = lookup_index(ctx, name);
    if (index < 0)
        log_error("checkpoint: %s '%s' is not in the IdString table.\n", what, name.c_str());
    IdString id;
    id.index = index;
    return id;
}

CellInfo *known_cell(Context *ctx, const std::string &name, const char *where)
{
    int index = lookup_index(ctx, name);
    if (index >= 0) {
        IdString id;
        id.index = index;
        auto it = ctx->cells.find(id);
        if (it != ctx->cells.end())
            return it->second.get();
    }
    log_error("checkpoint: %s names unknown cell '%s'.\n", where, name.c_str());
}

NetInfo *known_net(Context *ctx, const std::string &name, const char *where)
{
    int index = lookup_index(ctx, name);
    if (index >= 0) {
        IdString id;
        id.index = index;
        auto it = ctx->nets.find(id);
        if (it != ctx->nets.end())
            return it->second.get();
    }
    log_error("checkpoint: %s names unknown net '%s'.\n", where, name.c_str());
}

Json property_json(const Property &p)
{
    if (p.is_string)
        return Json::object{{"str", p.as_string()}};
    return Json::object{{"bits", p.to_string()}};
}

Property property_from_json(const Json &j, const char *where)
{
    if (j["str"].is_string())
        return Property(j["str"].string_value());
    if (j["bits"].is_string())
        return Property::from_string(j["bits"].string_value());
    log_error("checkpoint: %s carries a malformed property value.\n", where);
}

template <typename T> Json dict_order(const Context *ctx, const dict<IdString, T> &d)
{
    Json::array order;
    order.reserve(d.size());
    for (auto &entry : d)
        order.push_back(entry.first.str(ctx));
    return order;
}

// Rebuild `d` so that iterating it visits `order` in sequence. nextpnr's dict
// iterates entries newest first, so the entries are reinserted in reverse.
// Values are moved, never copied: pointers into them stay valid.
// With `allow_extra`, entries the list does not name are kept and iterate
// after the recorded ones (settings: an option the writing run did not have,
// such as --uncompressed-rbf, must not block a resume).
template <typename T>
void restore_dict_order(Context *ctx, dict<IdString, T> &d, const Json &order, const std::string &where,
                        bool allow_extra = false)
{
    if (!order.is_array() ||
        (allow_extra ? order.array_items().size() > d.size() : order.array_items().size() != d.size()))
        log_error("checkpoint: order list for %s has %zu entries, the design has %zu.\n", where.c_str(),
                  order.is_array() ? order.array_items().size() : size_t(0), size_t(d.size()));
    std::unordered_map<int, T> moved;
    moved.reserve(d.size());
    std::vector<std::pair<IdString, T>> extra; // in current iteration order
    if (allow_extra) {
        std::unordered_set<int> named;
        for (const auto &item : order.array_items()) {
            int index = lookup_index(ctx, item.string_value());
            if (index >= 0)
                named.insert(index);
        }
        for (auto &entry : d)
            if (!named.count(entry.first.index))
                extra.emplace_back(entry.first, std::move(entry.second));
    }
    for (auto &entry : d)
        moved.emplace(entry.first.index, std::move(entry.second));
    d.clear();
    // Extras first: the dict iterates newest first, so they land after the recorded entries.
    for (auto it = extra.rbegin(); it != extra.rend(); ++it) {
        moved.erase(it->first.index);
        d[it->first] = std::move(it->second);
    }
    const auto &items = order.array_items();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        int index = lookup_index(ctx, it->string_value());
        auto m = index < 0 ? moved.end() : moved.find(index);
        if (m == moved.end())
            log_error("checkpoint: order list for %s names unknown or repeated entry '%s'.\n", where.c_str(),
                      it->string_value().c_str());
        IdString key;
        key.index = index;
        d[key] = std::move(m->second);
        moved.erase(m);
    }
}

template <typename T> Json dict_order_indices(const dict<IdString, T> &d)
{
    Json::array order;
    order.reserve(d.size());
    for (auto &entry : d)
        order.push_back(entry.first.index);
    return order;
}

// Index-keyed variant of restore_dict_order for the per-object lists.
template <typename T>
void restore_dict_order_indices(dict<IdString, T> &d, const Json &order, const char *what, const IdString owner,
                                const Context *ctx)
{
    if (!order.is_array() || order.array_items().size() != d.size())
        log_error("checkpoint: %s order for %s has %zu entries, the design has %zu.\n", what, owner.c_str(ctx),
                  order.is_array() ? order.array_items().size() : size_t(0), size_t(d.size()));
    std::unordered_map<int, T> moved;
    moved.reserve(d.size());
    for (auto &entry : d)
        moved.emplace(entry.first.index, std::move(entry.second));
    d.clear();
    const auto &items = order.array_items();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        auto m = moved.find(it->int_value());
        if (m == moved.end())
            log_error("checkpoint: %s order for %s names unknown or repeated entry %d.\n", what, owner.c_str(ctx),
                      it->int_value());
        IdString key;
        key.index = it->int_value();
        d[key] = std::move(m->second);
        moved.erase(m);
    }
}

// Mirrors Arch::assign_default_pinmap: the entry it would create for this
// port, which the checkpoint therefore need not carry.
bool pin_is_default(const Arch &arch, const CellInfo *ci, IdString port, const ArchPinInfo &pin)
{
    if (ci->type == id_MISTRAL_M10K || ci->type == id_MISTRAL_M10K_DC)
        return false;
    if (pin.state != PIN_SIG || pin.bel_pins.size() != 1 || !ci->ports.count(port))
        return false;
    IdString expected = port;
    if (arch.is_comb_cell(ci->type)) {
        auto it = Arch::comb_pinmap.find(port);
        if (it != Arch::comb_pinmap.end())
            expected = it->second;
    }
    return pin.bel_pins[0] == expected;
}

void restore_users(Context *ctx, NetInfo *net, const Json &users, const std::vector<CellInfo *> &cells_by_position)
{
    const std::string where = "users of net " + net->name.str(ctx);
    if (!users.is_array() || int32_t(users.array_items().size()) != net->users.entries())
        log_error("checkpoint: %s lists %zu entries, the design has %d.\n", where.c_str(),
                  users.is_array() ? users.array_items().size() : size_t(0), int(net->users.entries()));
    for (auto &user : net->users)
        user.cell->ports.at(user.port).user_idx = store_index<PortRef>();
    net->users.clear();
    for (const auto &entry : users.array_items()) {
        if (!entry.is_array() || entry.array_items().size() != 2 || !entry[0].is_number() || !entry[1].is_number())
            log_error("checkpoint: %s carries a malformed entry.\n", where.c_str());
        const int position = entry[0].int_value();
        if (position < 0 || size_t(position) >= cells_by_position.size())
            log_error("checkpoint: %s names cell position %d of %zu.\n", where.c_str(), position,
                      cells_by_position.size());
        CellInfo *cell = cells_by_position[size_t(position)];
        IdString port;
        port.index = entry[1].int_value();
        if (port.index < 0 || size_t(port.index) >= ctx->idstring_idx_to_str->size())
            log_error("checkpoint: %s names IdString %d outside the table.\n", where.c_str(), port.index);
        auto pit = cell->ports.find(port);
        if (pit == cell->ports.end() || pit->second.net != net || pit->second.type == PORT_OUT)
            log_error("checkpoint: %s names %s.%s, which does not sink this net.\n", where.c_str(),
                      cell->name.c_str(ctx), port.c_str(ctx));
        if (pit->second.user_idx)
            log_error("checkpoint: %s names %s.%s twice.\n", where.c_str(), cell->name.c_str(ctx), port.c_str(ctx));
        PortRef ref;
        ref.cell = cell;
        ref.port = port;
        pit->second.user_idx = net->users.add(ref);
    }
}

bool has_cluster_state(const CellInfo *ci)
{
    return ci->cluster != ClusterId() || !ci->constr_children.empty() || ci->constr_x != 0 || ci->constr_y != 0 ||
           ci->constr_z != 0 || ci->constr_abs_z;
}

uint64_t parse_u64(const std::string &text, const char *what)
{
    errno = 0;
    char *end = nullptr;
    unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (text.empty() || end == nullptr || *end != '\0' || errno != 0)
        log_error("checkpoint: %s '%s' is not an unsigned decimal.\n", what, text.c_str());
    return uint64_t(value);
}

int require_int(const Json &j, const char *what)
{
    if (!j.is_number())
        log_error("checkpoint: manifest field %s is missing or not a number.\n", what);
    return j.int_value();
}

std::string require_string(const Json &j, const char *what)
{
    if (!j.is_string())
        log_error("checkpoint: manifest field %s is missing or not a string.\n", what);
    return j.string_value();
}

// Compact SHA-256 (FIPS 180-4) for the manifest lineage; no dependency.
struct Sha256
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64];
    size_t block_len = 0;
    uint64_t total = 0;

    static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
    void transform()
    {
        static const uint32_t k[64] = {
                0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[4 * i]) << 24) | (uint32_t(block[4 * i + 1]) << 16) |
                   (uint32_t(block[4 * i + 2]) << 8) | uint32_t(block[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    void update(const uint8_t *data, size_t len)
    {
        total += len;
        while (len > 0) {
            size_t take = std::min(len, size_t(64) - block_len);
            std::memcpy(block + block_len, data, take);
            block_len += take;
            data += take;
            len -= take;
            if (block_len == 64) {
                transform();
                block_len = 0;
            }
        }
    }
    std::string hex()
    {
        const uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (block_len != 56)
            update(&zero, 1);
        uint8_t len_bytes[8];
        for (int i = 0; i < 8; ++i)
            len_bytes[i] = uint8_t(bits >> (56 - 8 * i));
        update(len_bytes, 8);
        char out[65];
        for (int i = 0; i < 8; ++i)
            snprintf(out + 8 * i, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

WireId wire_by_name(Context *ctx, const std::string &name, const char *where)
{
    WireId wire;
    try {
        wire = ctx->getWireByName(IdStringList::parse(ctx, name));
    } catch (const std::exception &) {
        wire = WireId();
    }
    if (wire == WireId())
        log_error("checkpoint: %s names unknown wire '%s'.\n", where, name.c_str());
    return wire;
}

PipId pip_by_name(Context *ctx, const std::string &name, const char *where)
{
    PipId pip;
    try {
        pip = ctx->getPipByName(IdStringList::parse(ctx, name));
    } catch (const std::exception &) {
        pip = PipId();
    }
    if (pip == PipId())
        log_error("checkpoint: %s names unknown pip '%s'.\n", where, name.c_str());
    return pip;
}
} // namespace

// Parsed checkpoint kept between preload (before the netlist import) and
// restore (after it).
struct MistralCheckpoint
{
    Json root;
    std::string phase;
    size_t idstrings = 0;
    Json input;                               // the parent manifest's "input" object, propagated
    dict<IdString, Property> settings_before; // command-line settings before the file's were imported
};

int checkpoint_phase_rank(const std::string &phase)
{
    if (phase.empty())
        return 0;
    if (phase == "packed")
        return 1;
    if (phase == "placed")
        return 2;
    if (phase == "route-prepared")
        return 3;
    if (phase == "routed")
        return 4;
    return -1;
}

bool checkpoint_phase_restorable(const std::string &phase) { return checkpoint_phase_rank(phase) >= 1; }

std::string checkpoint_sha256_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string();
    Sha256 hash;
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        hash.update(reinterpret_cast<const uint8_t *>(buffer.data()), size_t(in.gcount()));
    }
    return hash.hex();
}

std::string Arch::checkpointPhaseAfterRoute() const { return args.route_prepare_only ? "route-prepared" : "routed"; }

bool Arch::writeCheckpoint(std::ostream &out, const std::string &phase) const
{
    const Context *ctx = getCtx();
    const int rank = checkpoint_phase_rank(phase);
    if (rank <= 0)
        log_error("checkpoint: '%s' is not a checkpoint phase.\n", phase.c_str());

    // The IdString table, in index order, including everything the JSON writer
    // interned just before this object. A resuming process replays it before
    // it imports the netlist, so every index it assigns matches ours.
    Json::array idstrings;
    idstrings.reserve(idstring_idx_to_str->size());
    for (const std::string *s : *idstring_idx_to_str)
        idstrings.push_back(*s);

    // Iteration orders the placer and router depend on. Cells and nets are
    // listed by name; per-object lists are aligned with those two lists and
    // hold IdString indices, which the replayed table makes exact.
    Json::array cells_order, nets_order, cell_ports, cell_attrs, cell_params, net_attrs, users;
    std::unordered_map<int, int> cell_position;
    cells_order.reserve(cells.size());
    cell_position.reserve(cells.size());
    for (auto &cell : cells) {
        const CellInfo *ci = cell.second.get();
        cell_position.emplace(ci->name.index, int(cells_order.size()));
        cells_order.push_back(ci->name.str(ctx));
        // Ports carry their direction: the frontend drops disconnected ports
        // on reload (a port exists per connection bit), and the restore must
        // recreate them before it reorders.
        Json::array port_list;
        port_list.reserve(ci->ports.size());
        for (auto &port : ci->ports)
            port_list.push_back(Json::array{port.first.index, int(port.second.type)});
        cell_ports.push_back(std::move(port_list));
        cell_attrs.push_back(dict_order_indices(ci->attrs));
        cell_params.push_back(dict_order_indices(ci->params));
    }
    nets_order.reserve(nets.size());
    for (auto &net : nets) {
        const NetInfo *ni = net.second.get();
        nets_order.push_back(ni->name.str(ctx));
        Json::array net_users;
        net_users.reserve(size_t(ni->users.entries()));
        for (auto &user : ni->users)
            net_users.push_back(Json::array{cell_position.at(user.cell->name.index), user.port.index});
        users.push_back(std::move(net_users));
        net_attrs.push_back(dict_order_indices(ni->attrs));
    }
    Json order = Json::object{
            {"cells", cells_order},
            {"nets", nets_order},
            {"settings", dict_order(ctx, settings)},
            {"attrs", dict_order(ctx, attrs)},
            {"cell_ports", cell_ports},
            {"cell_attrs", cell_attrs},
            {"cell_params", cell_params},
            {"net_attrs", net_attrs},
            {"users", users},
    };

    // Netlist state the JSON writer keeps but a reload of a nextpnr-written
    // file does not rebuild: the top-level port table (the frontend skips
    // IO buffer creation for files marked "synth").
    Json::array top_ports;
    for (auto &port : ports)
        top_ports.push_back(Json::object{{"name", port.first.str(ctx)},
                                         {"net", port.second.net ? port.second.net->name.str(ctx) : std::string()},
                                         {"type", int(port.second.type)}});
    // Undriven nets keep the port name of a driver the packer removed
    // (disconnectPort clears the cell, not the port). Nothing reads it, but
    // Context::checksum() hashes it, and a resumed context is meant to be
    // the writer's context.
    Json::array stale_driver_ports;
    for (auto &net : nets) {
        const NetInfo *ni = net.second.get();
        if (ni->driver.cell == nullptr && ni->driver.port != IdString())
            stale_driver_ports.push_back(Json::array{ni->name.str(ctx), ni->driver.port.str(ctx)});
    }
    Json netlist = Json::object{{"top_ports", top_ports}, {"stale_driver_ports", stale_driver_ports}};

    // Packing: cluster geometry, pin maps, and the QSF-derived IO attributes.
    // Pin entries that assign_default_pinmap() would recreate are omitted;
    // assignArchInfo() regenerates them on restore.
    Json::array cluster_cells, pins, io_attrs;
    size_t pin_entries = 0;
    for (auto &cell : cells) {
        const CellInfo *ci = cell.second.get();
        if (has_cluster_state(ci)) {
            Json::array children;
            for (const CellInfo *child : ci->constr_children)
                children.push_back(child->name.str(ctx));
            cluster_cells.push_back(Json::object{
                    {"cell", ci->name.str(ctx)},
                    {"cluster", ci->cluster == ClusterId() ? std::string() : ci->cluster.str(ctx)},
                    {"x", ci->constr_x},
                    {"y", ci->constr_y},
                    {"z", ci->constr_z},
                    {"abs_z", ci->constr_abs_z},
                    {"children", children},
            });
        }
        Json::array cell_pins;
        for (auto &pin : ci->pin_data) {
            if (pin_is_default(*this, ci, pin.first, pin.second))
                continue;
            Json::array bel_pins;
            for (IdString bp : pin.second.bel_pins)
                bel_pins.push_back(bp.str(ctx));
            cell_pins.push_back(Json::object{
                    {"port", pin.first.str(ctx)}, {"state", int(pin.second.state)}, {"bel_pins", bel_pins}});
            ++pin_entries;
        }
        if (!cell_pins.empty())
            pins.push_back(Json::object{{"cell", ci->name.str(ctx)}, {"ports", cell_pins}});
    }
    for (auto &port : io_attr) {
        Json::array entries;
        for (auto &attr : port.second)
            entries.push_back(Json::object{{"name", attr.first.str(ctx)}, {"value", property_json(attr.second)}});
        io_attrs.push_back(Json::object{{"port", port.first.str(ctx)}, {"attrs", entries}});
    }
    Json packing = Json::object{{"cluster_cells", cluster_cells}, {"pins", pins}, {"io_attr", io_attrs}};

    // Physical state: every binding (the packer already binds QSF-located IO
    // and PLL cells, so a packed checkpoint carries bindings too) and the
    // placer's PLL clock selections. BELs are named; the restore resolves
    // names through the BEL list rather than the parser, which would intern.
    Json::array bindings, pllclk;
    for (auto &cell : cells) {
        const CellInfo *ci = cell.second.get();
        if (ci->bel == BelId())
            continue;
        bindings.push_back(Json::object{{"cell", ci->name.str(ctx)},
                                        {"bel", getBelName(ci->bel).str(ctx)},
                                        {"strength", int(ci->belStrength)}});
    }
    for (auto &kv : pllclk_sel_map)
        pllclk.push_back(Json::object{{"key", std::to_string(kv.first)}, {"sel", int(kv.second)}});
    Json::object physical_obj{{"bindings", bindings}, {"pllclk_sel", pllclk}};
    size_t reserved_count = 0, routed_nets = 0;
    if (rank >= 3) {
        // Routing preparation (2b): LAB control allocation and ALM modes per
        // LAB, the control-wire reservations, and every bound route (globals
        // at route-prepared; everything at routed), in wires-map order.
        Json::array labs_json;
        labs_json.reserve(labs.size());
        for (const LABInfo &lab_data : labs) {
            Json::array alms;
            for (const ALMInfo &alm : lab_data.alms)
                alms.push_back(Json::array{alm.clk_ena_idx[0], alm.clk_ena_idx[1], alm.aclr_idx[0], alm.aclr_idx[1],
                                           alm.l6_mode, alm.carry_mode});
            labs_json.push_back(Json::array{alms, Json::array{lab_data.aclr_used[0], lab_data.aclr_used[1]}});
        }
        Json::array reserved;
        for (auto &wire : wires) {
            if ((wire.second.flags & WireInfo::RESERVED_ROUTE) == 0)
                continue;
            reserved.push_back(Json::array{getWireName(wire.first).str(ctx), std::to_string(wire.second.flags)});
        }
        reserved_count = reserved.size();
        Json::array routes;
        for (auto &net : nets) {
            const NetInfo *ni = net.second.get();
            if (ni->wires.empty())
                continue;
            Json::array entries;
            entries.reserve(ni->wires.size());
            for (auto &wire : ni->wires)
                entries.push_back(
                        Json::array{getWireName(wire.first).str(ctx),
                                    wire.second.pip == PipId() ? std::string() : getPipName(wire.second.pip).str(ctx),
                                    int(wire.second.strength)});
            routes.push_back(Json::array{ni->name.str(ctx), entries});
            ++routed_nets;
        }
        physical_obj["labs"] = labs_json;
        physical_obj["reserved_wires"] = reserved;
        physical_obj["routes"] = routes;
    }
    Json physical = physical_obj;

    // Non-interning lookup: the table above must stay complete.
    uint64_t seed = 0;
    const int seed_index = lookup_index(ctx, "seed");
    if (seed_index >= 0) {
        IdString seed_key;
        seed_key.index = seed_index;
        auto seed_it = settings.find(seed_key);
        if (seed_it != settings.end())
            seed = uint64_t(seed_it->second.as_int64());
    }
    // Lineage: the Yosys netlist this design came from (propagated through
    // every checkpoint) and the checkpoint this run resumed from, if any.
    Json input, parent;
    if (design_source_is_checkpoint) {
        if (!restored_input_json_.empty()) {
            std::string err;
            input = Json::parse(restored_input_json_, err);
        }
        parent = Json::object{{"path", design_source_path},
                              {"phase", checkpoint_phase_},
                              {"sha256", checkpoint_sha256_file(design_source_path)}};
    } else if (!design_source_path.empty()) {
        input = Json::object{{"path", design_source_path}, {"sha256", checkpoint_sha256_file(design_source_path)}};
    }
    Json manifest = Json::object{
            {"schema", SCHEMA},
            {"backend", "mistral"},
            {"backend_state_version", BACKEND_STATE_VERSION},
            {"phase", phase},
            {"device", args.device},
            {"nextpnr", GIT_DESCRIBE_STR},
            {"seed", std::to_string(seed)},
            {"rng_state", std::to_string(ctx->rngstate)},
            {"idstrings", int(idstrings.size())},
            {"input", input},
            {"parent", parent},
    };
    Json checkpoint = Json::object{{"manifest", manifest}, {"idstrings", idstrings}, {"order", order},
                                   {"netlist", netlist},   {"packing", packing},     {"physical", physical}};
    std::string text;
    checkpoint.dump(text);
    out << text;
    log_info("Checkpoint: wrote phase '%s' (%zu idstrings, %zu cells, %zu nets, %zu top ports, %zu cluster cells, "
             "%zu non-default pin entries, %zu io_attr ports, %zu bindings, %zu pllclk selections, %zu LABs, %zu "
             "reserved wires, %zu routed nets, rng_state %" PRIu64 ").\n",
             phase.c_str(), idstrings.size(), size_t(cells.size()), size_t(nets.size()), top_ports.size(),
             cluster_cells.size(), pin_entries, io_attrs.size(), bindings.size(), pllclk.size(),
             rank >= 3 ? labs.size() : size_t(0), reserved_count, routed_nets, ctx->rngstate);
    return true;
}

bool Arch::checkpointPreload(const std::string &checkpoint_json)
{
    std::string error;
    Json root = Json::parse(checkpoint_json, error);
    if (!error.empty())
        log_error("checkpoint: not valid JSON: %s\n", error.c_str());
    const Json &manifest = root["manifest"];
    if (!manifest.is_object())
        log_error("checkpoint: no manifest.\n");
    if (require_int(manifest["schema"], "schema") != SCHEMA)
        log_error("checkpoint: schema %d, this build reads schema %d.\n", manifest["schema"].int_value(), SCHEMA);
    if (require_string(manifest["backend"], "backend") != "mistral")
        log_error("checkpoint: written by backend '%s', not mistral.\n", manifest["backend"].string_value().c_str());
    if (require_int(manifest["backend_state_version"], "backend_state_version") != BACKEND_STATE_VERSION)
        log_error("checkpoint: backend state version %d, this build restores version %d.\n",
                  manifest["backend_state_version"].int_value(), BACKEND_STATE_VERSION);
    const std::string device = require_string(manifest["device"], "device");
    if (device != args.device)
        log_error("checkpoint: written for device %s, this run targets %s.\n", device.c_str(), args.device.c_str());
    const std::string phase = require_string(manifest["phase"], "phase");
    if (!checkpoint_phase_restorable(phase))
        log_error("checkpoint: phase '%s' cannot be resumed by this build (packed, placed, route-prepared, and routed "
                  "are supported).\n",
                  phase.c_str());
    const std::string version = require_string(manifest["nextpnr"], "nextpnr");
    if (version != GIT_DESCRIBE_STR)
        log_warning("checkpoint: written by nextpnr %s, this build is %s; identity with the writing run is not "
                    "guaranteed.\n",
                    version.c_str(), GIT_DESCRIBE_STR);

    // Replay the IdString table. Everything this process has interned so far
    // (arch constants, chip data, settings keys) must be a prefix of the
    // recorded table; the rest is interned in recorded order.
    const Json &idstrings = root["idstrings"];
    if (!idstrings.is_array())
        log_error("checkpoint: no idstrings table.\n");
    const auto &items = idstrings.array_items();
    const size_t have = idstring_idx_to_str->size();
    if (items.size() < have)
        log_error("checkpoint: IdString table has %zu entries, this process already interned %zu; the checkpoint was "
                  "written by a different build or with different options.\n",
                  items.size(), have);
    for (size_t i = 0; i < items.size(); ++i) {
        const std::string &s = items[i].string_value();
        if (i < have) {
            if (*(*idstring_idx_to_str)[i] != s)
                log_error("checkpoint: IdString %zu is '%s' here but '%s' in the checkpoint; the checkpoint was "
                          "written by a different build or with different options.\n",
                          i, (*idstring_idx_to_str)[i]->c_str(), s.c_str());
            continue;
        }
        IdString interned = id(s);
        if (size_t(interned.index) != i)
            log_error("checkpoint: IdString '%s' interned at %d, expected %zu.\n", s.c_str(), interned.index, i);
    }

    auto pending = std::make_shared<MistralCheckpoint>();
    pending->phase = phase;
    pending->idstrings = items.size();
    pending->input = root["manifest"]["input"];
    pending->settings_before = settings;
    pending->root = std::move(root);
    pending_checkpoint = std::move(pending);
    log_info("Checkpoint: phase '%s' for %s, %zu idstrings replayed (%zu were already interned).\n", phase.c_str(),
             device.c_str(), items.size(), have);
    return true;
}

bool Arch::checkpointRestore()
{
    if (!pending_checkpoint)
        log_error("checkpoint: restore without a preloaded checkpoint.\n");
    Context *ctx = getCtx();
    const Json &root = pending_checkpoint->root;
    const std::string phase = pending_checkpoint->phase; // a copy: the pending object is released below
    const int rank = checkpoint_phase_rank(phase);

    if (idstring_idx_to_str->size() != pending_checkpoint->idstrings)
        log_error("checkpoint: the netlist import interned %zu IdStrings beyond the recorded table; the checkpoint "
                  "does not belong to this netlist.\n",
                  idstring_idx_to_str->size() - pending_checkpoint->idstrings);

    // Iteration orders first: everything below addresses objects by name.
    const Json &order = root["order"];
    if (!order.is_object())
        log_error("checkpoint: no order section.\n");
    restore_dict_order(ctx, cells, order["cells"], "cells");
    restore_dict_order(ctx, nets, order["nets"], "nets");
    restore_dict_order(ctx, settings, order["settings"], "settings", /*allow_extra=*/true);
    restore_dict_order(ctx, attrs, order["attrs"], "attributes");

    // The frontend imported the file's settings over the command line's. A
    // differing seed is a contradiction (the manifest's RNG state is what
    // runs); any other difference is reported so nobody believes an option
    // the checkpoint overrode.
    for (auto &before : pending_checkpoint->settings_before) {
        auto now = settings.find(before.first);
        if (now == settings.end())
            continue;
        const bool same = now->second.is_string == before.second.is_string &&
                          now->second.to_string() == before.second.to_string();
        if (same)
            continue;
        if (before.first.str(ctx) == "seed")
            log_error("checkpoint: --seed on the command line (%s) differs from the checkpoint's (%s); resume with the "
                      "checkpoint's seed or omit the option.\n",
                      before.second.to_string().c_str(), now->second.to_string().c_str());
        log_warning("checkpoint: setting %s is %s in the checkpoint and %s on the command line; the checkpoint "
                    "wins.\n",
                    before.first.c_str(ctx), now->second.to_string().c_str(), before.second.to_string().c_str());
    }
    std::vector<CellInfo *> cells_by_position;
    cells_by_position.reserve(cells.size());
    for (auto &cell : cells)
        cells_by_position.push_back(cell.second.get());
    const auto &cell_ports = order["cell_ports"].array_items();
    const auto &cell_attrs = order["cell_attrs"].array_items();
    const auto &cell_params = order["cell_params"].array_items();
    if (cell_ports.size() != cells.size() || cell_attrs.size() != cells.size() || cell_params.size() != cells.size())
        log_error("checkpoint: per-cell order lists do not match the %zu cells.\n", size_t(cells.size()));
    for (size_t i = 0; i < cells_by_position.size(); ++i) {
        CellInfo *ci = cells_by_position[i];
        Json::array port_indices;
        for (const auto &entry : cell_ports[i].array_items()) {
            if (!entry.is_array() || entry.array_items().size() != 2)
                log_error("checkpoint: malformed port entry for %s.\n", ci->name.c_str(ctx));
            IdString port;
            port.index = entry[0].int_value();
            if (port.index < 0 || size_t(port.index) >= idstring_idx_to_str->size())
                log_error("checkpoint: port entry for %s names IdString %d outside the table.\n", ci->name.c_str(ctx),
                          port.index);
            const PortType type = PortType(entry[1].int_value());
            auto pit = ci->ports.find(port);
            if (pit == ci->ports.end()) {
                // Disconnected in the checkpoint; the reload had nothing to create it from.
                PortInfo &recreated = ci->ports[port];
                recreated.name = port;
                recreated.net = nullptr;
                recreated.type = type;
            } else if (pit->second.type != type) {
                log_error("checkpoint: port %s.%s is %d in the checkpoint but %d after reload.\n", ci->name.c_str(ctx),
                          port.c_str(ctx), int(type), int(pit->second.type));
            }
            port_indices.push_back(port.index);
        }
        restore_dict_order_indices(ci->ports, Json(port_indices), "port", ci->name, ctx);
        restore_dict_order_indices(ci->attrs, cell_attrs[i], "attribute", ci->name, ctx);
        restore_dict_order_indices(ci->params, cell_params[i], "parameter", ci->name, ctx);
    }
    const auto &users = order["users"].array_items();
    const auto &net_attrs = order["net_attrs"].array_items();
    if (users.size() != nets.size() || net_attrs.size() != nets.size())
        log_error("checkpoint: per-net order lists do not match the %zu nets.\n", size_t(nets.size()));
    {
        size_t i = 0;
        for (auto &net : nets) {
            NetInfo *ni = net.second.get();
            restore_users(ctx, ni, users[i], cells_by_position);
            restore_dict_order_indices(ni->attrs, net_attrs[i], "attribute", ni->name, ctx);
            ++i;
        }
    }

    // Netlist state the reload dropped: the top-level port table, in its
    // recorded iteration order (newest entry first, so inserted in reverse).
    const auto &top_ports = root["netlist"]["top_ports"].array_items();
    if (!ports.empty())
        log_error("checkpoint: the reload created %zu top-level ports; expected none for a nextpnr-written file.\n",
                  size_t(ports.size()));
    for (auto it = top_ports.rbegin(); it != top_ports.rend(); ++it) {
        PortInfo pinfo;
        pinfo.name = known_id(ctx, (*it)["name"].string_value(), "top-level port");
        const std::string net_name = (*it)["net"].string_value();
        pinfo.net = net_name.empty() ? nullptr : known_net(ctx, net_name, "netlist.top_ports");
        pinfo.type = PortType((*it)["type"].int_value());
        ports[pinfo.name] = pinfo;
    }

    for (const auto &entry : root["netlist"]["stale_driver_ports"].array_items()) {
        NetInfo *ni = known_net(ctx, entry[0].string_value(), "netlist.stale_driver_ports");
        if (ni->driver.cell != nullptr)
            log_error("checkpoint: net %s is driven after reload but was undriven in the checkpoint.\n",
                      ni->name.c_str(ctx));
        ni->driver.port = known_id(ctx, entry[1].string_value(), "stale driver port");
    }

    // Packing.
    const Json &packing = root["packing"];
    if (!packing.is_object())
        log_error("checkpoint: no packing section.\n");
    size_t cluster_cells = 0, pin_entries = 0;
    for (const auto &entry : packing["cluster_cells"].array_items()) {
        CellInfo *ci = known_cell(ctx, entry["cell"].string_value(), "packing.cluster_cells");
        const std::string cluster = entry["cluster"].string_value();
        ci->cluster = cluster.empty() ? ClusterId() : known_id(ctx, cluster, "cluster");
        ci->constr_x = entry["x"].int_value();
        ci->constr_y = entry["y"].int_value();
        ci->constr_z = entry["z"].int_value();
        ci->constr_abs_z = entry["abs_z"].bool_value();
        ci->constr_children.clear();
        for (const auto &child : entry["children"].array_items())
            ci->constr_children.push_back(known_cell(ctx, child.string_value(), "packing.cluster_cells.children"));
        ++cluster_cells;
    }
    io_attr.clear();
    {
        // Both levels are dicts: reinsert in reverse so iteration matches the writer.
        const auto &io_entries = packing["io_attr"].array_items();
        for (auto it = io_entries.rbegin(); it != io_entries.rend(); ++it) {
            auto &attrs_for_port = io_attr[known_id(ctx, (*it)["port"].string_value(), "io_attr port")];
            const auto &attr_entries = (*it)["attrs"].array_items();
            for (auto ait = attr_entries.rbegin(); ait != attr_entries.rend(); ++ait)
                attrs_for_port[known_id(ctx, (*ait)["name"].string_value(), "io_attr name")] =
                        property_from_json((*ait)["value"], "packing.io_attr");
        }
    }

    // Recorded pin entries are applied twice around assignArchInfo(). The
    // states must be in place before it: assign_ff_info reads PIN_INV to set
    // the control-set inversions the bitstream programs. The bel-pin lists
    // must win after it: routing preparation leaves constant and unused LUT
    // inputs with an empty list, compute_lut_mask keys on that emptiness,
    // and the default pass (the same one pack() ends with) refills every
    // empty list it finds.
    struct RecordedPin
    {
        CellInfo *cell;
        IdString port;
        CellPinState state;
        std::vector<IdString> bel_pins;
    };
    std::vector<RecordedPin> recorded_pins;
    for (const auto &entry : packing["pins"].array_items()) {
        CellInfo *ci = known_cell(ctx, entry["cell"].string_value(), "packing.pins");
        for (const auto &pin : entry["ports"].array_items()) {
            RecordedPin rec;
            rec.cell = ci;
            rec.port = known_id(ctx, pin["port"].string_value(), "pin port");
            const int state = pin["state"].int_value();
            if (state < PIN_SIG || state > PIN_INV)
                log_error("checkpoint: packing.pins carries pin state %d for %s.\n", state, ci->name.c_str(ctx));
            rec.state = CellPinState(state);
            for (const auto &bp : pin["bel_pins"].array_items())
                rec.bel_pins.push_back(known_id(ctx, bp.string_value(), "bel pin"));
            recorded_pins.push_back(std::move(rec));
            ++pin_entries;
        }
    }
    for (const RecordedPin &rec : recorded_pins) {
        auto &pd = rec.cell->pin_data[rec.port];
        pd.state = rec.state;
        pd.bel_pins = rec.bel_pins;
    }
    assignArchInfo();
    for (const RecordedPin &rec : recorded_pins)
        rec.cell->pin_data[rec.port].bel_pins = rec.bel_pins;

    io_attr.clear();
    {
        // Both levels are dicts: reinsert in reverse so iteration matches the writer.
        const auto &io_entries = packing["io_attr"].array_items();
        for (auto it = io_entries.rbegin(); it != io_entries.rend(); ++it) {
            auto &attrs_for_port = io_attr[known_id(ctx, (*it)["port"].string_value(), "io_attr port")];
            const auto &attr_entries = (*it)["attrs"].array_items();
            for (auto ait = attr_entries.rbegin(); ait != attr_entries.rend(); ++ait)
                attrs_for_port[known_id(ctx, (*ait)["name"].string_value(), "io_attr name")] =
                        property_from_json((*ait)["value"], "packing.io_attr");
        }
    }

    // The same call pack() ends with: comb/FF facts and default pin maps from
    // the restored netlist, pin states, and clusters. The recorded pin
    // entries are applied afterwards so that they win over the defaults:
    // routing preparation leaves constant and unused LUT inputs with an
    // empty bel-pin list, the LUT mask keys on that emptiness, and a default
    // pass run after them would refill it.
    assignArchInfo();
    for (const auto &entry : packing["pins"].array_items()) {
        CellInfo *ci = known_cell(ctx, entry["cell"].string_value(), "packing.pins");
        for (const auto &pin : entry["ports"].array_items()) {
            auto &pd = ci->pin_data[known_id(ctx, pin["port"].string_value(), "pin port")];
            pd.bel_pins.clear();
            const int state = pin["state"].int_value();
            if (state < PIN_SIG || state > PIN_INV)
                log_error("checkpoint: packing.pins carries pin state %d for %s.\n", state, ci->name.c_str(ctx));
            pd.state = CellPinState(state);
            for (const auto &bp : pin["bel_pins"].array_items())
                pd.bel_pins.push_back(known_id(ctx, bp.string_value(), "bel pin"));
            ++pin_entries;
        }
    }

    // Physical state: bindings at every phase, PLL clock selections, then the
    // legality sweep. Nothing may certify a placement it did not check.
    const auto &bindings = root["physical"]["bindings"].array_items();
    size_t bound = 0;
    if (!bindings.empty()) {
        std::unordered_map<std::string, BelId> bel_by_name;
        bel_by_name.reserve(getBels().size());
        for (BelId bel : getBels())
            bel_by_name.emplace(getBelName(bel).str(ctx), bel);
        for (const auto &entry : bindings) {
            CellInfo *ci = known_cell(ctx, entry["cell"].string_value(), "physical.bindings");
            const std::string bel_name = entry["bel"].string_value();
            auto bit = bel_by_name.find(bel_name);
            if (bit == bel_by_name.end())
                log_error("checkpoint: cell %s is bound to unknown BEL '%s'.\n", ci->name.c_str(ctx), bel_name.c_str());
            if (ci->bel != BelId())
                log_error("checkpoint: cell %s is already bound at %s.\n", ci->name.c_str(ctx),
                          ctx->nameOfBel(ci->bel));
            if (ctx->getBoundBelCell(bit->second) != nullptr)
                log_error("checkpoint: BEL %s is claimed twice (by %s and %s).\n", bel_name.c_str(),
                          ctx->getBoundBelCell(bit->second)->name.c_str(ctx), ci->name.c_str(ctx));
            ctx->bindBel(bit->second, ci, PlaceStrength(entry["strength"].int_value()));
            ++bound;
        }
    }
    pllclk_sel_map.clear();
    for (const auto &entry : root["physical"]["pllclk_sel"].array_items())
        pllclk_sel_map[parse_u64(entry["key"].string_value(), "pllclk_sel key")] = uint8_t(entry["sel"].int_value());
    // The placement rules certify a packed or placed restore. After routing
    // preparation they no longer apply: lab_pre_route binds MISTRAL_BUF
    // route-throughs onto LUT BELs outside both isValidBelForCellType and the
    // ALM sharing rules, so for those phases the writer's own preparation is
    // the certification and only the structural checks above run.
    if (rank <= 2) {
        for (auto &cell : cells) {
            const CellInfo *ci = cell.second.get();
            if (ci->bel == BelId())
                continue;
            if (!ctx->isValidBelForCellType(ci->type, ci->bel))
                log_error("checkpoint: cell %s of type %s cannot sit at %s.\n", ci->name.c_str(ctx),
                          ci->type.c_str(ctx), ctx->nameOfBel(ci->bel));
            if (!ctx->isBelLocationValid(ci->bel))
                log_error("checkpoint: restored placement is illegal at %s (cell %s).\n", ctx->nameOfBel(ci->bel),
                          ci->name.c_str(ctx));
        }
    }

    size_t reserved_count = 0, routed_nets = 0;
    if (rank >= 3) {
        const auto &labs_json = root["physical"]["labs"].array_items();
        if (labs_json.size() != labs.size())
            log_error("checkpoint: physical.labs has %zu entries, the device has %zu LABs.\n", labs_json.size(),
                      labs.size());
        for (size_t lab = 0; lab < labs.size(); ++lab) {
            LABInfo &lab_data = labs[lab];
            const auto &alms = labs_json[lab][0].array_items();
            if (alms.size() != lab_data.alms.size())
                log_error("checkpoint: physical.labs[%zu] has %zu ALMs.\n", lab, alms.size());
            for (size_t alm = 0; alm < alms.size(); ++alm) {
                const auto &v = alms[alm].array_items();
                if (v.size() != 6)
                    log_error("checkpoint: physical.labs[%zu] ALM %zu is malformed.\n", lab, alm);
                ALMInfo &alm_data = lab_data.alms[alm];
                alm_data.clk_ena_idx = {v[0].int_value(), v[1].int_value()};
                alm_data.aclr_idx = {v[2].int_value(), v[3].int_value()};
                alm_data.l6_mode = v[4].bool_value();
                alm_data.carry_mode = v[5].bool_value();
            }
            const auto &used = labs_json[lab][1].array_items();
            if (used.size() != 2)
                log_error("checkpoint: physical.labs[%zu] aclr_used is malformed.\n", lab);
            lab_data.aclr_used = {used[0].bool_value(), used[1].bool_value()};
        }
        for (const auto &entry : root["physical"]["reserved_wires"].array_items()) {
            WireId wire = wire_by_name(ctx, entry[0].string_value(), "physical.reserved_wires");
            wires.at(wire).flags = parse_u64(entry[1].string_value(), "reserved wire flags");
            ++reserved_count;
        }
        for (const auto &route : root["physical"]["routes"].array_items()) {
            NetInfo *ni = known_net(ctx, route[0].string_value(), "physical.routes");
            if (!ni->wires.empty())
                log_error("checkpoint: net %s is already routed before its route is restored.\n", ni->name.c_str(ctx));
            const auto &entries = route[1].array_items();
            // The wires map iterates newest first; bind in reverse to reproduce it.
            for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                const auto &v = it->array_items();
                if (v.size() != 3)
                    log_error("checkpoint: route of %s is malformed.\n", ni->name.c_str(ctx));
                WireId wire = wire_by_name(ctx, v[0].string_value(), "physical.routes");
                const PlaceStrength strength = PlaceStrength(v[2].int_value());
                if (ctx->getBoundWireNet(wire) != nullptr)
                    log_error("checkpoint: wire %s of net %s is already bound to %s.\n", v[0].string_value().c_str(),
                              ni->name.c_str(ctx), ctx->getBoundWireNet(wire)->name.c_str(ctx));
                if (v[1].string_value().empty()) {
                    ctx->bindWire(wire, ni, strength);
                } else {
                    PipId pip = pip_by_name(ctx, v[1].string_value(), "physical.routes");
                    if (ctx->getPipDstWire(pip) != wire)
                        log_error("checkpoint: pip %s does not drive wire %s (net %s).\n", v[1].string_value().c_str(),
                                  v[0].string_value().c_str(), ni->name.c_str(ctx));
                    ctx->bindPip(pip, ni, strength);
                }
            }
            ++routed_nets;
        }
        if (rank >= 4)
            note_routing_complete();
    }

    if (idstring_idx_to_str->size() != pending_checkpoint->idstrings)
        log_error("checkpoint: the restore interned %zu IdStrings; the checkpoint does not match this build.\n",
                  idstring_idx_to_str->size() - pending_checkpoint->idstrings);

    ctx->rngstate = parse_u64(root["manifest"]["rng_state"].string_value(), "rng_state");
    checkpoint_phase_ = phase;
    restored_input_json_ = pending_checkpoint->input.is_null() ? std::string() : pending_checkpoint->input.dump();
    pending_checkpoint.reset();
    log_info("Checkpoint: restored phase '%s' (%zu cells, %zu nets, %zu top ports, %zu cluster cells, %zu non-default "
             "pin entries, %zu io_attr ports, %zu bound cells, %zu reserved wires, %zu routed nets, rng_state %" PRIu64
             ").\n",
             phase.c_str(), size_t(cells.size()), size_t(nets.size()), size_t(ports.size()), cluster_cells, pin_entries,
             size_t(io_attr.size()), bound, reserved_count, routed_nets, ctx->rngstate);
    return true;
}

std::string Arch::checkpointPhase() const { return checkpoint_phase_; }

NEXTPNR_NAMESPACE_END
