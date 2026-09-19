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

#ifndef MISTRAL_MONITOR_H
#define MISTRAL_MONITOR_H

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "monitor_abi.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// `--monitor`: the live dashboard of a run. The session owns the terminal for the run: the log's
// terminal stream is replaced by a hook that feeds the monitor's log tail (the `--log` file keeps
// the text), and a ticker thread renders a frame four times a second from the counters the arch
// keeps as atomics and the phase times the owner thread reports. The frame is Rust
// (`rust/npnr_mistral_monitor`); this side only collects, and reads nothing of the netlist off
// the owner thread.
struct MonitorSession
{
    // Starts the session, or returns null with a warning when stdout is not a terminal or the
    // build has no Rust. `dry` (tests) skips the terminal, the log hook, and the ticker: the
    // session then only collects, and `render_to` gives the frame.
    static std::shared_ptr<MonitorSession> start(const Arch &arch, const std::string &design,
                                                 const std::string &log_path, bool dry = false);
    ~MonitorSession();
    MonitorSession(const MonitorSession &) = delete;
    MonitorSession &operator=(const MonitorSession &) = delete;

    // Owner thread: the run entered `phase` (an NpnrMonitorPhase). Closes the previous phase's
    // time and snapshots the cell and net counts.
    void phase(uint32_t phase);
    // A snapshot of the arch's counters and the phase clock, from any thread.
    void fill(NpnrMonitorSnapshotV1 &out) const;
    // One frame into `frame`, the terminal untouched (tests).
    uint32_t render_to(std::string &frame) const;
    // The last render status; a renderer failure stops the ticker and is reported at the end.
    uint32_t last_status() const { return status_.load(std::memory_order_relaxed); }

  private:
    MonitorSession(const Arch &arch, bool dry);
    bool open(const std::string &design, const std::string &log_path);
    void tick();
    void forward(const std::string &chunk);

    const Arch &arch_;
    const bool dry_;
    NpnrMonitor *handle_ = nullptr;
    std::thread ticker_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> status_{0};
    std::atomic<uint32_t> phase_{NPNR_MONITOR_PHASE_INIT};
    std::atomic<uint64_t> cells_{0}, nets_{0};
    mutable std::mutex times_;
    std::chrono::steady_clock::time_point run_started_, phase_started_;
    std::array<double, NPNR_MONITOR_PHASES> phase_seconds_{};
    std::mutex pending_mutex_;
    std::string pending_;
    std::vector<std::pair<std::ostream *, LogLevel>> saved_streams_;
    log_write_type saved_write_;
    bool hooked_ = false;
};

// Splits `chunk` into complete lines, carrying a partial line over in `pending`.
void split_log_lines(std::string &pending, const std::string &chunk,
                     const std::function<void(const std::string &)> &emit);
// The option lines the frame shows, from the arch's arguments.
std::vector<std::string> monitor_option_lines(const ArchArgs &args);

NEXTPNR_NAMESPACE_END

#endif
