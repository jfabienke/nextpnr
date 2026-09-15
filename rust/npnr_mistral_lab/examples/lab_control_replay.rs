// SPDX-License-Identifier: ISC
//! Test driver: one named-field replay JSON record per line; one complete result per line.
#![forbid(unsafe_code)]

#[path = "../tests/support/mod.rs"]
mod support;

use std::io::{self, BufRead, Write};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args().skip(1).collect();
    if args == ["--layout"] {
        println!("{}", support::layout_json());
        return Ok(());
    }
    if !args.is_empty() {
        return Err("usage: lab_control_replay [--layout]".into());
    }
    let mut out = io::BufWriter::new(io::stdout().lock());
    for (index, line) in io::stdin().lock().lines().enumerate() {
        let record = serde_json::from_str(&line?)?;
        let input =
            support::read_input(&record).map_err(|e| format!("record {}: {e}", index + 1))?;
        let result = npnr_mistral_lab::evaluate_wire(&input);
        writeln!(out, "{}", support::result_json(&result))?;
    }
    out.flush()?;
    Ok(())
}
