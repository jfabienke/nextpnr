// SPDX-License-Identifier: ISC
use super::*;

fn config() -> Config {
    Config {
        design: "f386_exec_probe_nodsp".into(),
        device: "5CSEBA6U23I7".into(),
        log_path: "/tmp/run.log".into(),
        options: vec![
            "lab-controls legacy   lab-legality rust   sa-seam on".into(),
            "alm-pairing 0   register-packing off   row-cost 0".into(),
        ],
        legality_mode: "rust".into(),
        controls_mode: "legacy".into(),
    }
}

fn probe_snapshot() -> Snapshot {
    let mut s = Snapshot {
        phase: Phase::Route,
        columns: 80,
        rows: 40,
        checksum: 0xbb18_ede9,
        run_seconds: 22.1,
        cells: 12_169,
        nets: 13_396,
        ..Snapshot::default()
    };
    s.phase_seconds[Phase::Pack.index()] = 0.8;
    s.phase_seconds[Phase::Place.index()] = 13.9;
    s.phase_seconds[Phase::Route.index()] = 7.4;
    s.legality = Legality {
        evaluations: 5_570_583,
        legal: 1_156_345,
        illegal: 4_414_238,
        ..Legality::default()
    };
    s.resident = Resident {
        evaluations: 5_570_583,
        resets: 4_191,
        trials: 4_699_227,
        commits: 1_053_914,
        restored: 4_940_434,
    };
    s
}

#[test]
fn every_frame_line_is_exactly_the_width_and_the_frame_fits_the_rows() {
    let mut monitor = Monitor::new(config());
    for (columns, rows) in [(80u32, 40u32), (72, 16), (140, 80), (20, 5), (500, 500)] {
        let snapshot = Snapshot {
            columns,
            rows,
            ..probe_snapshot()
        };
        let frame = monitor.frame(&snapshot);
        let width = columns.clamp(MIN_COLUMNS, MAX_COLUMNS) as usize;
        let height = rows.clamp(MIN_ROWS, MAX_ROWS) as usize;
        assert!(
            frame.len() <= height,
            "{columns}x{rows}: {} lines",
            frame.len()
        );
        for line in &frame {
            assert_eq!(line.chars().count(), width, "{columns}x{rows}: {line:?}");
        }
        assert!(frame.first().unwrap().starts_with("+--"));
        assert!(frame.last().unwrap().starts_with("+--"));
    }
}

#[test]
fn the_panels_carry_the_snapshot() {
    let mut monitor = Monitor::new(config());
    let frame = monitor.frame(&probe_snapshot()).join("\n");
    assert!(frame.contains("f386_exec_probe_nodsp @ 5CSEBA6U23I7"));
    assert!(frame.contains("phase: ROUTE 00:00:07"));
    assert!(frame.contains("checksum 0xbb18ede9"));
    assert!(frame.contains("cells 12,169   nets 13,396"));
    assert!(frame.contains("LAB LEGALITY (rust)   evaluations 5,570,583"));
    assert!(frame.contains("legal       1,156,345  20.8%  |"));
    assert!(frame.contains("illegal     4,414,238  79.2%  |"));
    assert!(frame.contains("trials      4,699,227   0.84  |"));
    assert!(frame.contains("commits     1,053,914   0.19  |"));
    assert!(frame.contains("* signoff"));
    assert!(frame.contains("fallbacks 0"));
    assert!(frame.contains("place 13.9 s"));
    assert!(frame.contains("lab-controls legacy"));
    assert!(frame.contains("log: /tmp/run.log"));
    assert!(frame.contains("00:00:22 "));
}

#[test]
fn small_terminals_keep_the_legality_panel_and_drop_the_rest() {
    let mut monitor = Monitor::new(config());
    let frame = monitor
        .frame(&Snapshot {
            rows: 16,
            ..probe_snapshot()
        })
        .join("\n");
    assert!(frame.contains("LAB LEGALITY"));
    assert!(!frame.contains("RESIDENT PROTOCOL"));
    assert!(!frame.contains("OPTIONS"));
    assert!(frame.contains(" LOG"));
}

#[test]
fn bars_are_proportional_and_bounded() {
    assert_eq!(bar(0.0, 10), "|          |");
    assert_eq!(bar(0.5, 10), "|#####     |");
    assert_eq!(bar(1.0, 10), "|##########|");
    assert_eq!(bar(7.0, 10), "|##########|");
    assert_eq!(bar(-1.0, 10), "|          |");
}

#[test]
fn numbers_and_clocks_format() {
    assert_eq!(group(0), "0");
    assert_eq!(group(999), "999");
    assert_eq!(group(1_000), "1,000");
    assert_eq!(group(5_570_583), "5,570,583");
    assert_eq!(clock(0.0), "00:00:00");
    assert_eq!(clock(3_725.9), "01:02:05");
    assert_eq!(clock(-3.0), "00:00:00");
}

#[test]
fn progress_lines_are_read_from_the_log() {
    let mut monitor = Monitor::new(config());
    monitor.note_log_line(
        "Info:     at iteration #3, type MISTRAL_COMB: wirelen solved = 36585, spread = 89707, legal = 91601; time = 0.12s\n",
    );
    assert_eq!(
        monitor.heap,
        Some(HeapProgress {
            iteration: 3,
            cell_type: "MISTRAL_COMB".into(),
            solved: 36585,
            spread: 89707,
            legal: 91601
        })
    );
    monitor.note_log_line(
        "Info:   at iteration #12: temp = 0.001234, timing cost = 567, wirelen = 123456",
    );
    assert_eq!(monitor.anneal.as_ref().map(|a| a.iteration), Some(12));
    assert_eq!(monitor.anneal.as_ref().map(|a| a.wirelen), Some(123_456));
    monitor.note_log_line("Info:     iter=1 wires=456789 overused=1234 overuse=2345 archfail=NA");
    monitor.note_log_line(
        "Info:     iter=2 wires=456000 overused=600 overuse=900 resources=7 overused=1 overuse=1 archfail=NA",
    );
    monitor.note_log_line("Info:     iter=3 wires=455000 overused=0 overuse=0 archfail=0");
    assert_eq!(
        monitor.router,
        vec![
            RouterIteration {
                iteration: 1,
                wires: 456_789,
                overused: 1234
            },
            RouterIteration {
                iteration: 2,
                wires: 456_000,
                overused: 600
            },
            RouterIteration {
                iteration: 3,
                wires: 455_000,
                overused: 0
            },
        ]
    );
    assert_eq!(monitor.sparkline(10), "@=_");
    let frame = monitor.frame(&probe_snapshot()).join("\n");
    assert!(frame.contains("ROUTER  #3  wires 455,000  overused 0"));
    assert!(frame.contains("PLACEMENT  refine #12"));
    // An unrelated line is neither progress nor lost.
    monitor.note_log_line("Info: Routing complete.");
    assert_eq!(monitor.router.len(), 3);
    assert_eq!(monitor.log_tail(1).next(), Some("Info: Routing complete."));
}

#[test]
fn the_log_tail_is_bounded_and_ordered() {
    let mut monitor = Monitor::new(config());
    for i in 0..(LOG_CAPACITY + 10) {
        monitor.note_log_line(&format!("line {i}"));
    }
    assert_eq!(monitor.log.len(), LOG_CAPACITY);
    let tail: Vec<&str> = monitor.log_tail(2).collect();
    assert_eq!(
        tail,
        vec![
            format!("line {}", LOG_CAPACITY + 8).as_str(),
            format!("line {}", LOG_CAPACITY + 9).as_str()
        ]
    );
    // Control characters never reach the frame.
    monitor.note_log_line("tab\there\x1b[31m");
    let frame = monitor.frame(&probe_snapshot()).join("\n");
    assert!(!frame.contains('\t'));
    assert!(!frame.contains('\x1b'));
}

#[test]
fn the_rate_is_the_query_delta_over_the_tick() {
    let mut monitor = Monitor::new(config());
    let mut snapshot = probe_snapshot();
    snapshot.run_seconds = 10.0;
    snapshot.legality.evaluations = 1_000;
    monitor.frame(&snapshot);
    assert_eq!(monitor.rate, 0.0);
    snapshot.run_seconds = 10.25; // too soon: the anchor holds
    snapshot.legality.evaluations = 1_500;
    monitor.frame(&snapshot);
    assert_eq!(monitor.rate, 0.0);
    snapshot.run_seconds = 12.0;
    snapshot.legality.evaluations = 5_000;
    monitor.frame(&snapshot);
    assert!((monitor.rate - 2_000.0).abs() < 1e-9);
}

#[test]
fn phases_round_trip_and_the_frame_text_is_one_write() {
    for index in 0..Phase::COUNT as u32 {
        let phase = Phase::from_index(index).unwrap();
        assert_eq!(phase.index(), index as usize);
    }
    assert_eq!(Phase::from_index(Phase::COUNT as u32), None);
    let text = frame_text(&["ab".into(), "cd".into()]);
    assert_eq!(text, "\x1b[Hab\x1b[K\r\ncd\x1b[K\r\n\x1b[J");
}
