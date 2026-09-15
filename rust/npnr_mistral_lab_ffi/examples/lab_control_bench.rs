// SPDX-License-Identifier: ISC
//! Warm-corpus component profiling. Allocation instrumentation is isolated here.
use npnr_mistral_lab::{ControlLabSnapshot, evaluate, evaluate_wire, wire::*};
use npnr_mistral_lab_ffi::{CALL_OK, npnr_mistral_eval_controls_v1};
use std::alloc::{GlobalAlloc, Layout, System};
use std::hint::black_box;
use std::io::{BufRead, Write};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed};
use std::time::Instant;

#[allow(dead_code)]
#[path = "../../npnr_mistral_lab/tests/support/mod.rs"]
mod support;

struct CountAlloc;
static COUNTING: AtomicBool = AtomicBool::new(false);
static ALLOCATIONS: AtomicU64 = AtomicU64::new(0);
static BYTES: AtomicU64 = AtomicU64::new(0);
fn count(bytes: usize) {
    if COUNTING.load(Relaxed) {
        ALLOCATIONS.fetch_add(1, Relaxed);
        BYTES.fetch_add(bytes as u64, Relaxed);
    }
}
unsafe impl GlobalAlloc for CountAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        count(layout.size());
        unsafe { System.alloc(layout) }
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        count(layout.size());
        unsafe { System.alloc_zeroed(layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        count(size);
        unsafe { System.realloc(ptr, layout, size) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }
}
#[global_allocator]
static ALLOCATOR: CountAlloc = CountAlloc;

#[derive(Clone, Copy, Default)]
struct Measurement {
    nanos: u128,
    calls: u64,
    allocations: u64,
    bytes: u64,
    cases: u64,
}
fn measure(mut operation: impl FnMut(), repeats: u32, records: usize, m: &mut Measurement) {
    operation();
    ALLOCATIONS.store(0, Relaxed);
    BYTES.store(0, Relaxed);
    COUNTING.store(true, Relaxed);
    let start = Instant::now();
    for _ in 0..repeats {
        operation();
    }
    let elapsed = start.elapsed();
    COUNTING.store(false, Relaxed);
    m.nanos += elapsed.as_nanos();
    m.calls += u64::from(repeats) * records as u64;
    m.cases += 1;
    m.allocations += ALLOCATIONS.load(Relaxed);
    m.bytes += BYTES.load(Relaxed);
}
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args().collect();
    if !(3..=5).contains(&args.len()) {
        return Err(
            "usage: lab_control_bench CORPUS.jsonl OUTPUT.csv [rounds=7] [repeats=128]".into(),
        );
    }
    let rounds: usize = args.get(3).map_or(Ok(7), |s| s.parse())?;
    let repeats: u32 = args.get(4).map_or(Ok(128), |s| s.parse())?;
    assert!(rounds > 0 && repeats > 0);
    let mut records = Vec::new();
    for line in std::io::BufReader::new(std::fs::File::open(&args[1])?).lines() {
        let record: serde_json::Value = serde_json::from_str(&line?)?;
        let input = support::read_input(&record)?;
        let result = evaluate_wire(&input);
        let actual = support::result_json(&result);
        assert_eq!(actual["status"], record["expected"]["status"]);
        if result.status == LEGAL {
            assert_eq!(actual["allocation"], record["expected"]["allocation"]);
        } else {
            assert!(
                result
                    .allocation
                    .iter()
                    .all(|s| s.net_id == 0 && s.flags == 0)
            );
        }
        records.push(input);
    }
    assert!(!records.is_empty());
    let phases = [
        "rust_decode",
        "rust_rules",
        "rust_encode",
        "rust_safe_wire",
        "rust_native_ffi",
    ];
    let mut output = std::io::BufWriter::new(std::fs::File::create(&args[2])?);
    writeln!(
        output,
        "round,phase,reason,cases,calls,nanoseconds,rust_alloc_calls,rust_alloc_bytes"
    )?;
    for round in 0..rounds {
        let mut measurements = [[Measurement::default(); 7]; 5];
        for (case, input) in records.iter().enumerate() {
            let typed = ControlLabSnapshot::try_from(input)?;
            let assessment = evaluate(&typed);
            let expected = assessment.to_wire();
            let mut result = LabControlResultV1::default();
            assert_eq!(
                unsafe { npnr_mistral_eval_controls_v1(input, 1, &mut result, 1) },
                CALL_OK
            );
            assert_eq!(
                support::result_json(&result),
                support::result_json(&expected)
            );
            for step in 0..phases.len() {
                let phase = (step + case + round) % phases.len();
                let m = &mut measurements[phase][expected.reason as usize];
                match phase {
                    0 => measure(
                        || {
                            black_box(ControlLabSnapshot::try_from(black_box(input))).unwrap();
                        },
                        repeats,
                        1,
                        m,
                    ),
                    1 => measure(|| _ = black_box(evaluate(black_box(&typed))), repeats, 1, m),
                    2 => measure(
                        || _ = black_box(black_box(&assessment).to_wire()),
                        repeats,
                        1,
                        m,
                    ),
                    3 => measure(
                        || _ = black_box(evaluate_wire(black_box(input))),
                        repeats,
                        1,
                        m,
                    ),
                    4 => measure(
                        || {
                            _ = black_box(unsafe {
                                npnr_mistral_eval_controls_v1(black_box(input), 1, &mut result, 1)
                            });
                            black_box(&result);
                        },
                        repeats,
                        1,
                        m,
                    ),
                    _ => unreachable!(),
                }
            }
        }
        for (phase, groups) in measurements.iter().enumerate() {
            for (reason, m) in groups.iter().enumerate().filter(|(_, m)| m.calls > 0) {
                writeln!(
                    output,
                    "{round},{},{reason},{},{},{},{},{}",
                    phases[phase], m.cases, m.calls, m.nanos, m.allocations, m.bytes
                )?;
            }
        }
        for batch_size in [2usize, 4, 8, 16, 32, 64] {
            let mut m = Measurement::default();
            let mut results = vec![LabControlResultV1::default(); batch_size];
            for inputs in records.chunks_exact(batch_size) {
                measure(
                    || {
                        assert_eq!(
                            unsafe {
                                npnr_mistral_eval_controls_v1(
                                    black_box(inputs.as_ptr()),
                                    batch_size as u32,
                                    results.as_mut_ptr(),
                                    batch_size as u32,
                                )
                            },
                            CALL_OK
                        );
                        black_box(&results);
                    },
                    repeats,
                    batch_size,
                    &mut m,
                );
            }
            writeln!(
                output,
                "{round},rust_native_ffi_batch_{batch_size},0,{},{},{},{},{}",
                m.cases, m.calls, m.nanos, m.allocations, m.bytes
            )?;
        }
        output.flush()?;
        eprintln!("completed Rust microbenchmark round {}/{rounds}", round + 1);
    }
    eprintln!(
        "object bytes: typed_snapshot={} assessment={} input={} result={}",
        size_of::<ControlLabSnapshot>(),
        size_of_val(&evaluate(&ControlLabSnapshot::try_from(&records[0])?)),
        size_of::<LabControlsV1>(),
        size_of::<LabControlResultV1>()
    );
    Ok(())
}
