// SPDX-License-Identifier: ISC
//! The live monitor of a nextpnr-mistral run (`--monitor`): a frame of panels
//! rendered from a snapshot of the counters the arch already keeps and from the
//! log's tail.
//!
//! Pure: a snapshot in, lines of text out, so every panel is testable line by
//! line. The arch feeds each log line through [`Monitor::note_log_line`], which
//! keeps the tail and reads the placer's and router's progress lines; every tick
//! it passes a [`Snapshot`] and takes the frame from [`Monitor::frame`]. Nothing
//! here touches a terminal; the FFI writes the frame.
#![forbid(unsafe_code)]
#![deny(
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::panic,
    clippy::unreachable
)]
#![cfg_attr(test, allow(clippy::unwrap_used, clippy::expect_used, clippy::panic))]

use std::collections::VecDeque;
use std::fmt::Write as _;

/// The run's phases in order; the arch reports the one it is in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Init,
    Load,
    Pack,
    Place,
    RoutePrepare,
    Route,
    Signoff,
    Done,
    Failed,
}

impl Phase {
    pub const COUNT: usize = 9;
    const ALL: [Phase; Phase::COUNT] = [
        Phase::Init,
        Phase::Load,
        Phase::Pack,
        Phase::Place,
        Phase::RoutePrepare,
        Phase::Route,
        Phase::Signoff,
        Phase::Done,
        Phase::Failed,
    ];

    pub fn from_index(index: u32) -> Option<Phase> {
        Phase::ALL.get(index as usize).copied()
    }

    pub fn index(self) -> usize {
        Phase::ALL.iter().position(|p| *p == self).unwrap_or(0)
    }

    pub fn label(self) -> &'static str {
        match self {
            Phase::Init => "INIT",
            Phase::Load => "LOAD",
            Phase::Pack => "PACK",
            Phase::Place => "PLACE",
            Phase::RoutePrepare => "ROUTE PREP",
            Phase::Route => "ROUTE",
            Phase::Signoff => "SIGNOFF",
            Phase::Done => "DONE",
            Phase::Failed => "FAILED",
        }
    }

    /// The character the phase bar draws for the phase's share of the run.
    fn glyph(self) -> char {
        match self {
            Phase::Init | Phase::Load => '.',
            Phase::Pack => '-',
            Phase::Place => '#',
            Phase::RoutePrepare => ':',
            Phase::Route => '=',
            Phase::Signoff => '*',
            Phase::Done | Phase::Failed => ' ',
        }
    }
}

/// The LAB legality counters (`LabLegalityStats`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Legality {
    pub evaluations: u64,
    pub legal: u64,
    pub illegal: u64,
    pub errors: u64,
    pub mismatches: u64,
    pub stale_cache: u64,
    pub stale_revision: u64,
}

/// The resident session's counters (`ResidentLabLegality`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Resident {
    pub evaluations: u64,
    pub resets: u64,
    pub trials: u64,
    pub commits: u64,
    pub restored: u64,
}

/// The control-set counters (`LabControlStats`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Controls {
    pub evaluations: u64,
    pub preparation: u64,
    pub legal: u64,
    pub illegal: u64,
    pub errors: u64,
    pub mismatches: u64,
    pub fallbacks: u64,
}

/// What the arch passes per tick: where the run is, how large the terminal is,
/// and the counters as they stand.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Snapshot {
    pub phase: Phase,
    pub columns: u32,
    pub rows: u32,
    /// The design checksum the log printed last, or 0 before any.
    pub checksum: u32,
    pub run_seconds: f64,
    /// Seconds spent in each phase so far, indexed by [`Phase::index`].
    pub phase_seconds: [f64; Phase::COUNT],
    pub legality: Legality,
    pub resident: Resident,
    pub controls: Controls,
    pub cells: u64,
    pub nets: u64,
}

impl Default for Snapshot {
    fn default() -> Self {
        Snapshot {
            phase: Phase::Init,
            columns: 80,
            rows: 24,
            checksum: 0,
            run_seconds: 0.0,
            phase_seconds: [0.0; Phase::COUNT],
            legality: Legality::default(),
            resident: Resident::default(),
            controls: Controls::default(),
            cells: 0,
            nets: 0,
        }
    }
}

/// What the arch passes once: the names and the option lines it formatted.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Config {
    pub design: String,
    pub device: String,
    /// The `--log` path, or empty when the log is not kept.
    pub log_path: String,
    /// The options that shaped the run, one line each, rendered verbatim.
    pub options: Vec<String>,
    /// The legality and control modes, for the panel titles.
    pub legality_mode: String,
    pub controls_mode: String,
}

/// HeAP's per-iteration line.
#[derive(Clone, Debug, PartialEq, Eq)]
struct HeapProgress {
    iteration: u64,
    cell_type: String,
    solved: u64,
    spread: u64,
    legal: u64,
}

/// The annealer's per-iteration line.
#[derive(Clone, Debug, PartialEq)]
struct AnnealProgress {
    iteration: u64,
    temperature: f64,
    wirelen: u64,
}

/// router2's per-iteration line.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct RouterIteration {
    iteration: u64,
    wires: u64,
    overused: u64,
}

pub const LOG_CAPACITY: usize = 256;
const MIN_COLUMNS: u32 = 72;
const MAX_COLUMNS: u32 = 140;
const MIN_ROWS: u32 = 16;
const MAX_ROWS: u32 = 80;
const MIN_LOG_LINES: usize = 3;
const SPARK_WIDTH: usize = 40;

/// The monitor's state between ticks: the log tail, the progress read from it,
/// and the previous tick's query count for the rate.
#[derive(Clone, Debug)]
pub struct Monitor {
    config: Config,
    log: VecDeque<String>,
    heap: Option<HeapProgress>,
    anneal: Option<AnnealProgress>,
    router: Vec<RouterIteration>,
    rate_anchor: Option<(f64, u64)>,
    rate: f64,
}

impl Monitor {
    pub fn new(config: Config) -> Monitor {
        Monitor {
            config,
            log: VecDeque::with_capacity(LOG_CAPACITY),
            heap: None,
            anneal: None,
            router: Vec::new(),
            rate_anchor: None,
            rate: 0.0,
        }
    }

    /// One complete log line (without its newline): kept in the tail and read
    /// for the placer's and router's progress.
    pub fn note_log_line(&mut self, line: &str) {
        let line = line.trim_end_matches(['\r', '\n']);
        let body = line.strip_prefix("Info: ").unwrap_or(line);
        if let Some(progress) = parse_router(body) {
            self.router.push(progress);
        } else if let Some(progress) = parse_heap(body) {
            self.heap = Some(progress);
            self.anneal = None;
        } else if let Some(progress) = parse_anneal(body) {
            self.anneal = Some(progress);
        }
        if self.log.len() == LOG_CAPACITY {
            self.log.pop_front();
        }
        self.log.push_back(line.to_string());
    }

    /// The last `count` log lines, oldest first.
    pub fn log_tail(&self, count: usize) -> impl Iterator<Item = &str> {
        let skip = self.log.len().saturating_sub(count);
        self.log.iter().skip(skip).map(String::as_str)
    }

    /// The frame for this tick as lines of exactly `columns` characters (clamped
    /// to what the layout can draw), `rows` lines at most.
    pub fn frame(&mut self, snapshot: &Snapshot) -> Vec<String> {
        let columns = snapshot.columns.clamp(MIN_COLUMNS, MAX_COLUMNS) as usize;
        let rows = snapshot.rows.clamp(MIN_ROWS, MAX_ROWS) as usize;
        self.update_rate(snapshot);
        let inner = columns - 2;
        let border = format!("+{}+", "-".repeat(inner));
        // Fixed rows: top border, two header lines, a rule, the log title, a rule, the
        // footer, and the bottom border.
        let fixed = 8;
        let mut budget = rows.saturating_sub(fixed + MIN_LOG_LINES);
        let mut panels: Vec<Vec<String>> = Vec::new();
        let candidates: [(usize, Vec<String>); 6] = [
            (0, self.legality_panel(snapshot, inner)),
            (1, self.progress_panel(snapshot, inner)),
            (2, self.phases_panel(snapshot, inner)),
            (3, self.resident_panel(snapshot, inner)),
            (4, self.options_panel(inner)),
            (5, self.controls_panel(snapshot, inner)),
        ];
        // Chosen by priority, drawn in layout order.
        let mut chosen = [false; 6];
        for (priority, lines) in &candidates {
            let cost = lines.len() + 1;
            if cost <= budget {
                budget -= cost;
                chosen[*priority] = true;
            }
        }
        for index in [4usize, 2, 1, 0, 3, 5] {
            if chosen[index] {
                panels.push(candidates[index].1.clone());
            }
        }
        let used = fixed + panels.iter().map(|p| p.len() + 1).sum::<usize>();
        let log_lines = rows.saturating_sub(used).max(MIN_LOG_LINES);

        let mut out = Vec::with_capacity(rows);
        out.push(border.clone());
        for line in self.header(snapshot, inner) {
            out.push(boxed(&line, inner));
        }
        out.push(border.clone());
        for panel in &panels {
            for line in panel {
                out.push(boxed(line, inner));
            }
            out.push(border.clone());
        }
        out.push(boxed(" LOG", inner));
        let tail: Vec<&str> = self.log_tail(log_lines).collect();
        for index in 0..log_lines {
            let text = tail.get(index).copied().unwrap_or("");
            out.push(boxed(&format!("  {text}"), inner));
        }
        out.push(border.clone());
        out.push(boxed(&self.footer(inner), inner));
        out.push(border);
        out
    }

    fn update_rate(&mut self, snapshot: &Snapshot) {
        let now = (snapshot.run_seconds, snapshot.legality.evaluations);
        if let Some((then_s, then_n)) = self.rate_anchor {
            let dt = now.0 - then_s;
            if dt >= 0.5 {
                self.rate = (now.1.saturating_sub(then_n)) as f64 / dt;
                self.rate_anchor = Some(now);
            }
        } else {
            self.rate_anchor = Some(now);
        }
    }

    fn header(&self, snapshot: &Snapshot, inner: usize) -> [String; 2] {
        let left = if self.config.design.is_empty() {
            format!(" nextpnr-mistral monitor   {}", self.config.device)
        } else {
            format!(
                " nextpnr-mistral monitor   {} @ {}",
                self.config.design, self.config.device
            )
        };
        let right = format!("{} ", clock(snapshot.run_seconds));
        let phase_time = snapshot.phase_seconds[snapshot.phase.index()];
        let checksum = if snapshot.checksum == 0 {
            "-".to_string()
        } else {
            format!("0x{:08x}", snapshot.checksum)
        };
        let second = format!(
            " phase: {} {}   checksum {}   cells {}   nets {}",
            snapshot.phase.label(),
            clock(phase_time),
            checksum,
            group(snapshot.cells),
            group(snapshot.nets)
        );
        [justify(&left, &right, inner), second]
    }

    fn footer(&self, inner: usize) -> String {
        let left = if self.config.log_path.is_empty() {
            " log: not kept (add --log)".to_string()
        } else {
            format!(" log: {}", self.config.log_path)
        };
        justify(&left, "Ctrl-C aborts the run ", inner)
    }

    fn options_panel(&self, _inner: usize) -> Vec<String> {
        let mut lines = vec![" OPTIONS".to_string()];
        for option in &self.config.options {
            lines.push(format!("  {option}"));
        }
        lines
    }

    fn phases_panel(&self, snapshot: &Snapshot, inner: usize) -> Vec<String> {
        let cell = |phase: Phase| {
            let s = snapshot.phase_seconds[phase.index()];
            if s > 0.0 {
                format!("{s:.1} s")
            } else {
                "-".to_string()
            }
        };
        let title = format!(
            " PHASES   pack {}   place {}   route prep {}   route {}   signoff {}",
            cell(Phase::Pack),
            cell(Phase::Place),
            cell(Phase::RoutePrepare),
            cell(Phase::Route),
            cell(Phase::Signoff)
        );
        const LEGEND: &str = "  . load - pack # place : prep = route * signoff";
        let width = inner.saturating_sub(LEGEND.len() + 4).clamp(10, 60);
        let total: f64 = snapshot.phase_seconds.iter().sum();
        let mut bar = String::with_capacity(width);
        if total > 0.0 {
            for phase in Phase::ALL {
                let share = snapshot.phase_seconds[phase.index()] / total;
                let cells = (share * width as f64).round() as usize;
                for _ in 0..cells {
                    if bar.len() < width {
                        bar.push(phase.glyph());
                    }
                }
            }
        }
        while bar.len() < width {
            bar.push(' ');
        }
        vec![title, format!("  [{bar}]{LEGEND}")]
    }

    fn progress_panel(&self, snapshot: &Snapshot, inner: usize) -> Vec<String> {
        let placement = if let Some(anneal) = &self.anneal {
            format!(
                " PLACEMENT  refine #{}  temp {:.4}  wirelen {}",
                anneal.iteration,
                anneal.temperature,
                group(anneal.wirelen)
            )
        } else if let Some(heap) = &self.heap {
            format!(
                " PLACEMENT  HeAP #{} {}  solved {}  spread {}  legal {}",
                heap.iteration,
                heap.cell_type,
                group(heap.solved),
                group(heap.spread),
                group(heap.legal)
            )
        } else {
            " PLACEMENT  waiting for the placer".to_string()
        };
        let queries = format!(
            "  queries {}   {}/s",
            group(snapshot.legality.evaluations),
            group(self.rate.round() as u64)
        );
        let router = if let Some(last) = self.router.last() {
            let text = format!(
                " ROUTER  #{}  wires {}  overused {}  ",
                last.iteration,
                group(last.wires),
                group(last.overused)
            );
            let spark_width = inner.saturating_sub(text.len() + 1).min(SPARK_WIDTH);
            format!("{text}{}", self.sparkline(spark_width))
        } else {
            " ROUTER  waiting for the router".to_string()
        };
        vec![placement, queries, router]
    }

    /// The last iterations' overused wires as a bar of glyphs, scaled to the
    /// highest of them.
    fn sparkline(&self, width: usize) -> String {
        const LEVELS: [char; 9] = ['_', '.', ':', '-', '=', '+', '*', '#', '@'];
        if width == 0 {
            return String::new();
        }
        let skip = self.router.len().saturating_sub(width);
        let points: Vec<u64> = self.router.iter().skip(skip).map(|r| r.overused).collect();
        let peak = points.iter().copied().max().unwrap_or(0);
        points
            .iter()
            .map(|&value| {
                if peak == 0 || value == 0 {
                    LEVELS[0]
                } else {
                    let level = ((value as f64 / peak as f64) * (LEVELS.len() - 1) as f64).ceil();
                    LEVELS[(level as usize).clamp(1, LEVELS.len() - 1)]
                }
            })
            .collect()
    }

    fn legality_panel(&self, snapshot: &Snapshot, inner: usize) -> Vec<String> {
        let l = &snapshot.legality;
        let title = justify(
            &format!(
                " LAB LEGALITY ({})   evaluations {}",
                self.config.legality_mode,
                group(l.evaluations)
            ),
            &format!("{}/s ", group(self.rate.round() as u64)),
            inner,
        );
        let width = bar_width(inner);
        vec![
            title,
            counter_bar("legal", l.legal, l.evaluations, width),
            counter_bar("illegal", l.illegal, l.evaluations, width),
            format!(
                "  errors {}   mismatches {}   stale cache {}   stale revision {}",
                group(l.errors),
                group(l.mismatches),
                group(l.stale_cache),
                group(l.stale_revision)
            ),
        ]
    }

    fn resident_panel(&self, snapshot: &Snapshot, inner: usize) -> Vec<String> {
        let r = &snapshot.resident;
        let width = bar_width(inner);
        vec![
            format!(
                " RESIDENT PROTOCOL   evaluations {}   resets {}   per evaluation",
                group(r.evaluations),
                group(r.resets)
            ),
            ratio_bar("trials", r.trials, r.evaluations, width),
            ratio_bar("restored", r.restored, r.evaluations, width),
            ratio_bar("commits", r.commits, r.evaluations, width),
        ]
    }

    fn controls_panel(&self, snapshot: &Snapshot, _inner: usize) -> Vec<String> {
        let c = &snapshot.controls;
        vec![format!(
            " CONTROL SETS ({})   eval {}   prep {}   legal {}   illegal {}   fallbacks {}",
            self.config.controls_mode,
            group(c.evaluations),
            group(c.preparation),
            group(c.legal),
            group(c.illegal),
            group(c.fallbacks)
        )]
    }
}

fn bar_width(inner: usize) -> usize {
    inner.saturating_sub(34).clamp(10, 40)
}

/// `  label  count  share%  |####    |`
fn counter_bar(label: &str, count: u64, total: u64, width: usize) -> String {
    let share = if total == 0 {
        0.0
    } else {
        count as f64 / total as f64
    };
    format!(
        "  {label:<9}{:>12} {:>5.1}%  {}",
        group(count),
        share * 100.0,
        bar(share, width)
    )
}

/// `  label  count  ratio  |####    |` with the ratio per evaluation.
fn ratio_bar(label: &str, count: u64, per: u64, width: usize) -> String {
    let ratio = if per == 0 {
        0.0
    } else {
        count as f64 / per as f64
    };
    format!(
        "  {label:<9}{:>12} {:>6.2}  {}",
        group(count),
        ratio,
        bar(ratio.min(1.0), width)
    )
}

fn bar(fraction: f64, width: usize) -> String {
    let filled = ((fraction.clamp(0.0, 1.0)) * width as f64).round() as usize;
    format!("|{}{}|", "#".repeat(filled), " ".repeat(width - filled))
}

/// Digits grouped by thousands.
pub fn group(value: u64) -> String {
    let digits = value.to_string();
    let mut out = String::with_capacity(digits.len() + digits.len() / 3);
    for (index, ch) in digits.chars().enumerate() {
        if index > 0 && (digits.len() - index) % 3 == 0 {
            out.push(',');
        }
        out.push(ch);
    }
    out
}

/// `hh:mm:ss`.
pub fn clock(seconds: f64) -> String {
    let total = seconds.max(0.0) as u64;
    format!(
        "{:02}:{:02}:{:02}",
        total / 3600,
        (total / 60) % 60,
        total % 60
    )
}

/// `left` and `right` at the two ends of a line of `width`, the left text
/// truncated first.
fn justify(left: &str, right: &str, width: usize) -> String {
    let right_len = right.chars().count();
    let room = width.saturating_sub(right_len + 1);
    let left: String = left.chars().take(room).collect();
    let pad = width.saturating_sub(left.chars().count() + right_len);
    format!("{left}{}{right}", " ".repeat(pad))
}

/// `|` + the line padded or truncated to `inner` + `|`.
fn boxed(line: &str, inner: usize) -> String {
    let mut text: String = line
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .take(inner)
        .collect();
    let len = text.chars().count();
    for _ in len..inner {
        text.push(' ');
    }
    let mut out = String::with_capacity(inner + 2);
    out.push('|');
    out.push_str(&text);
    out.push('|');
    out
}

/// The text after `key`, when the line contains it.
fn after<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    line.find(key).map(|at| &line[at + key.len()..])
}

/// The leading unsigned number of `text`.
fn number(text: &str) -> Option<u64> {
    let end = text
        .char_indices()
        .find(|(_, c)| !c.is_ascii_digit())
        .map(|(i, _)| i)
        .unwrap_or(text.len());
    text[..end].parse().ok()
}

/// The leading decimal number of `text`.
fn decimal(text: &str) -> Option<f64> {
    let end = text
        .char_indices()
        .find(|(_, c)| !(c.is_ascii_digit() || *c == '.' || *c == '-' || *c == 'e'))
        .map(|(i, _)| i)
        .unwrap_or(text.len());
    text[..end].parse().ok()
}

/// `    iter=12 wires=456789 overused=1234 overuse=2345 archfail=NA`
fn parse_router(body: &str) -> Option<RouterIteration> {
    let iteration = number(after(body.trim_start(), "iter=")?)?;
    let wires = number(after(body, "wires=")?)?;
    let overused = number(after(body, "overused=")?)?;
    Some(RouterIteration {
        iteration,
        wires,
        overused,
    })
}

/// `    at iteration #3, type MISTRAL_COMB: wirelen solved = 36585, spread = 89707, legal = 91601; time = 0.12s`
fn parse_heap(body: &str) -> Option<HeapProgress> {
    let rest = after(body, "at iteration #")?;
    let iteration = number(rest)?;
    let cell_type = after(rest, "type ")?.split(':').next()?.trim().to_string();
    let solved = number(after(rest, "wirelen solved = ")?)?;
    let spread = number(after(rest, "spread = ")?)?;
    let legal = number(after(rest, "legal = ")?)?;
    Some(HeapProgress {
        iteration,
        cell_type,
        solved,
        spread,
        legal,
    })
}

/// `  at iteration #12: temp = 0.001234, timing cost = 567, wirelen = 123456`
fn parse_anneal(body: &str) -> Option<AnnealProgress> {
    let rest = after(body, "at iteration #")?;
    let iteration = number(rest)?;
    let temperature = decimal(after(rest, "temp = ")?)?;
    let wirelen = number(after(rest, "wirelen = ")?)?;
    Some(AnnealProgress {
        iteration,
        temperature,
        wirelen,
    })
}

/// The frame as one terminal write: cursor home, each line cleared to its end,
/// and everything below cleared.
pub fn frame_text(lines: &[String]) -> String {
    let mut out = String::with_capacity(lines.iter().map(|l| l.len() + 6).sum::<usize>() + 8);
    out.push_str("\x1b[H");
    for line in lines {
        let _ = write!(out, "{line}\x1b[K\r\n");
    }
    out.push_str("\x1b[J");
    out
}

/// What the terminal is sent when the monitor starts: cursor hidden, screen cleared.
pub const TERMINAL_ENTER: &str = "\x1b[?25l\x1b[2J\x1b[H";
/// What the terminal is sent when the monitor ends: cursor shown, below the frame.
pub const TERMINAL_LEAVE: &str = "\x1b[?25h";

#[cfg(test)]
mod tests;
