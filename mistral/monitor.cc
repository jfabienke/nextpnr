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

#include "monitor.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <cstddef>
#include <exception>
#include <iostream>

#include "lab_control_abi.h"
#include "lab_dispatch.h"
#include "lab_resident.h"
#include "lab_v2.h"
#include "log.h"

#ifdef NO_RUST
// Without Rust there is no renderer: the session is never started, and these keep the file linking.
extern "C" uint32_t npnr_mistral_monitor_create(const NpnrMonitorConfigV1 *, uint32_t, NpnrMonitor **)
{
    return NPNR_LAB_CALL_NULL;
}
extern "C" uint32_t npnr_mistral_monitor_log(NpnrMonitor *, const uint8_t *, size_t) { return NPNR_LAB_CALL_NULL; }
extern "C" uint32_t npnr_mistral_monitor_render(NpnrMonitor *, const NpnrMonitorSnapshotV1 *, uint32_t, uint8_t *,
                                                size_t, size_t *)
{
    return NPNR_LAB_CALL_NULL;
}
extern "C" void npnr_mistral_monitor_destroy(NpnrMonitor *) {}
#endif

NEXTPNR_NAMESPACE_BEGIN

static_assert(sizeof(NpnrMonitorSnapshotV1) == 264, "monitor snapshot layout");
static_assert(offsetof(NpnrMonitorSnapshotV1, run_seconds) == 16, "monitor snapshot layout");
static_assert(offsetof(NpnrMonitorSnapshotV1, phase_seconds) == 24, "monitor snapshot layout");
static_assert(offsetof(NpnrMonitorSnapshotV1, legality) == 96, "monitor snapshot layout");
static_assert(offsetof(NpnrMonitorSnapshotV1, resident) == 152, "monitor snapshot layout");
static_assert(offsetof(NpnrMonitorSnapshotV1, controls) == 192, "monitor snapshot layout");
static_assert(offsetof(NpnrMonitorSnapshotV1, cells) == 248, "monitor snapshot layout");
static_assert(offsetof(NpnrMonitorSnapshotV1, nets) == 256, "monitor snapshot layout");
static_assert(sizeof(NpnrMonitorConfigV1) == 96, "monitor config layout");

void split_log_lines(std::string &pending, const std::string &chunk,
                     const std::function<void(const std::string &)> &emit)
{
    pending += chunk;
    size_t start = 0;
    for (;;) {
        const size_t nl = pending.find('\n', start);
        if (nl == std::string::npos)
            break;
        emit(pending.substr(start, nl - start));
        start = nl + 1;
    }
    pending.erase(0, start);
}

std::vector<std::string> monitor_option_lines(const ArchArgs &a)
{
    const char *seam = a.sa_seam == SwapSeamMode::Off ? "off" : a.sa_seam == SwapSeamMode::Shadow ? "shadow" : "on";
    auto onoff = [](bool b) { return b ? "on" : "off"; };
    return {
            stringf("lab-controls %s  lab-legality %s  sa-seam %s  sa-batch %d  lookahead %d  route-history %g",
                    lab_control_mode_name(a.lab_controls), lab_legality_mode_name(a.lab_legality), seam,
                    int(a.sa_batch), int(a.placer_lookahead), double(a.reuse_routes_history)),
            stringf("alm-pairing %d  spread-demand %d  spread-congestion %s  register-packing %s  row-cost %g",
                    int(a.alm_pairing), int(a.spread_demand), onoff(a.spread_congestion), onoff(a.register_packing),
                    double(a.row_cost)),
            stringf("router2 unit-cost %s  crit-cost %s  reroute %d%s", onoff(a.router2_unit_cost),
                    onoff(a.router2_crit_cost), int(a.router2_reroute),
                    a.router2_reroute_contested ? " (contested only)" : ""),
    };
}

namespace {
NpnrMonitorStringV1 borrow(const std::string &text)
{
    return NpnrMonitorStringV1{reinterpret_cast<const uint8_t *>(text.data()), text.size()};
}

void terminal_size(uint32_t &columns, uint32_t &rows)
{
    columns = 80;
    rows = 24;
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        columns = ws.ws_col;
        rows = ws.ws_row;
    }
}
} // namespace

MonitorSession::MonitorSession(const Arch &arch, bool dry) : arch_(arch), dry_(dry) {}

std::shared_ptr<MonitorSession> MonitorSession::start(const Arch &arch, const std::string &design,
                                                      const std::string &log_path, bool dry)
{
#ifdef NO_RUST
    (void)design;
    (void)log_path;
    (void)dry;
    log_warning("--monitor needs the Rust build; running without the monitor.\n");
    return nullptr;
#else
    if (!dry && !isatty(STDOUT_FILENO)) {
        log_warning("--monitor: stdout is not a terminal; running without the monitor.\n");
        return nullptr;
    }
    std::shared_ptr<MonitorSession> session(new MonitorSession(arch, dry));
    if (!session->open(design, log_path))
        return nullptr;
    return session;
#endif
}

bool MonitorSession::open(const std::string &design, const std::string &log_path)
{
    const std::vector<std::string> options = monitor_option_lines(arch_.args);
    std::vector<NpnrMonitorStringV1> option_strings;
    for (const auto &line : options)
        option_strings.push_back(borrow(line));
    const std::string legality = lab_legality_mode_name(arch_.args.lab_legality);
    const std::string controls = lab_control_mode_name(arch_.args.lab_controls);
    NpnrMonitorConfigV1 config{borrow(design),   borrow(arch_.args.device), borrow(log_path),     borrow(legality),
                               borrow(controls), option_strings.data(),     option_strings.size()};
    const uint32_t status = npnr_mistral_monitor_create(&config, dry_ ? NPNR_MONITOR_DRY : 0u, &handle_);
    if (status != NPNR_LAB_CALL_OK) {
        log_warning("--monitor: the monitor could not be created (status %u); running without it.\n", status);
        handle_ = nullptr;
        return false;
    }
    run_started_ = phase_started_ = std::chrono::steady_clock::now();
    phase(NPNR_MONITOR_PHASE_LOAD);
    if (dry_)
        return true;
    // The terminal is the monitor's now: the log's terminal streams go, the file streams stay,
    // and every message reaches the log tail through the hook.
    saved_streams_ = log_streams;
    saved_write_ = log_write_function;
    log_streams.erase(std::remove_if(log_streams.begin(), log_streams.end(),
                                     [](const std::pair<std::ostream *, LogLevel> &s) {
                                         return s.first == &std::cerr || s.first == &std::cout;
                                     }),
                      log_streams.end());
    log_write_function = [this](std::string chunk) { forward(chunk); };
    hooked_ = true;
    if (log_path.empty())
        log_warning("--monitor without --log: the log text is not kept beyond this tail.\n");
    ticker_ = std::thread([this] { tick(); });
    return true;
}

MonitorSession::~MonitorSession()
{
    if (handle_ == nullptr)
        return;
    phase(std::uncaught_exceptions() > 0 ? NPNR_MONITOR_PHASE_FAILED : NPNR_MONITOR_PHASE_DONE);
    stop_.store(true, std::memory_order_release);
    if (ticker_.joinable())
        ticker_.join();
    if (hooked_) {
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (!pending_.empty()) {
                npnr_mistral_monitor_log(handle_, reinterpret_cast<const uint8_t *>(pending_.data()), pending_.size());
                pending_.clear();
            }
        }
        log_streams = saved_streams_;
        log_write_function = saved_write_;
        hooked_ = false;
    }
    npnr_mistral_monitor_destroy(handle_);
    handle_ = nullptr;
    const uint32_t status = last_status();
    if (status != NPNR_LAB_CALL_OK)
        log_warning("--monitor: the renderer stopped with status %u; the run continued without it.\n", status);
}

void MonitorSession::phase(uint32_t phase)
{
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(times_);
        const uint32_t previous = phase_.load(std::memory_order_relaxed);
        if (previous < NPNR_MONITOR_PHASES)
            phase_seconds_[previous] += std::chrono::duration<double>(now - phase_started_).count();
        phase_started_ = now;
        phase_.store(phase, std::memory_order_relaxed);
    }
    cells_.store(arch_.getCtx()->cells.size(), std::memory_order_relaxed);
    nets_.store(arch_.getCtx()->nets.size(), std::memory_order_relaxed);
}

void MonitorSession::fill(NpnrMonitorSnapshotV1 &out) const
{
    out = NpnrMonitorSnapshotV1{};
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(times_);
        out.phase = phase_.load(std::memory_order_relaxed);
        out.run_seconds = std::chrono::duration<double>(now - run_started_).count();
        for (size_t i = 0; i < NPNR_MONITOR_PHASES; ++i)
            out.phase_seconds[i] = phase_seconds_[i];
        if (out.phase < NPNR_MONITOR_PHASES)
            out.phase_seconds[out.phase] += std::chrono::duration<double>(now - phase_started_).count();
    }
    if (dry_) {
        out.columns = 80;
        out.rows = 40;
    } else {
        terminal_size(out.columns, out.rows);
    }
    out.checksum = arch_.telemetry_checksum.load(std::memory_order_relaxed);
    auto get = [](const std::atomic<uint64_t> &c) { return c.load(std::memory_order_relaxed); };
    const LabLegalityStats &l = arch_.lab_legality_stats;
    out.legality[0] = get(l.evaluations);
    out.legality[1] = get(l.legal);
    out.legality[2] = get(l.illegal);
    out.legality[3] = get(l.errors);
    out.legality[4] = get(l.mismatches);
    out.legality[5] = get(l.stale_cache);
    out.legality[6] = get(l.stale_revision);
    if (auto resident = std::atomic_load(&arch_.lab_resident)) {
        out.resident[0] = get(resident->evaluations);
        out.resident[1] = get(resident->resets);
        out.resident[2] = get(resident->trials);
        out.resident[3] = get(resident->commits);
        out.resident[4] = get(resident->restored);
    }
    const LabControlStats &c = arch_.lab_control_stats;
    out.controls[0] = get(c.evaluations);
    out.controls[1] = get(c.preparation);
    out.controls[2] = get(c.legal);
    out.controls[3] = get(c.illegal);
    out.controls[4] = get(c.errors);
    out.controls[5] = get(c.mismatches);
    out.controls[6] = get(c.fallbacks);
    out.cells = cells_.load(std::memory_order_relaxed);
    out.nets = nets_.load(std::memory_order_relaxed);
}

uint32_t MonitorSession::render_to(std::string &frame) const
{
    if (handle_ == nullptr)
        return NPNR_LAB_CALL_NULL;
    NpnrMonitorSnapshotV1 snapshot;
    fill(snapshot);
    std::string buffer(64 * 1024, '\0');
    size_t len = 0;
    const uint32_t status = npnr_mistral_monitor_render(handle_, &snapshot, NPNR_MONITOR_DRY,
                                                        reinterpret_cast<uint8_t *>(&buffer[0]), buffer.size(), &len);
    frame.assign(buffer, 0, len);
    return status;
}

void MonitorSession::tick()
{
    NpnrMonitorSnapshotV1 snapshot;
    for (;;) {
        fill(snapshot);
        const uint32_t status = npnr_mistral_monitor_render(handle_, &snapshot, 0u, nullptr, 0, nullptr);
        if (status != NPNR_LAB_CALL_OK) {
            status_.store(status, std::memory_order_relaxed);
            return;
        }
        if (stop_.load(std::memory_order_acquire))
            return; // the frame just drawn carries the final phase
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void MonitorSession::forward(const std::string &chunk)
{
    std::lock_guard<std::mutex> lock(pending_mutex_);
    split_log_lines(pending_, chunk, [this](const std::string &line) {
        npnr_mistral_monitor_log(handle_, reinterpret_cast<const uint8_t *>(line.data()), line.size());
    });
}

void Arch::monitor_phase(uint32_t phase) const
{
    if (monitor)
        monitor->phase(phase);
}

NEXTPNR_NAMESPACE_END
