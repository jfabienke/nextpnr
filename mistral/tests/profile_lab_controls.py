#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Capture live LAB inputs, benchmark components, and run paired serial full P&R.

All measured P&R runs use the same binary, without corpus capture. One warmup
pair is excluded. Alternating pair order reduces time/order bias; it cannot
eliminate other activity, thermal variation, or shared-library cache effects.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import tempfile

from validate_lab_controls_live import ROOT, digest, require, run


def distribution(values):
    ordered = sorted(values)
    quartiles = statistics.quantiles(ordered, n=4, method="inclusive") if len(values) > 1 else [values[0]] * 3
    return {"median": statistics.median(values), "min": ordered[0], "max": ordered[-1],
            "q1": quartiles[0], "q3": quartiles[2]}


def summarize(manifest, output):
    measured = [r for r in manifest["runs"] if not r["warmup"]]
    summary = {"full_pnr": {}, "components": {}, "components_by_reason": {}}
    for mode in ("legacy", "rust"):
        runs = [r for r in measured if r["mode"] == mode]
        summary["full_pnr"][mode] = {key: distribution([r[key] for r in runs]) for key in
                                         ("wall_seconds", "user_seconds", "system_seconds", "peak_rss_bytes")}
    pairs = [{r["mode"]: r for r in measured if r["pair"] == pair} for pair in sorted({r["pair"] for r in measured})]
    ratios = [p["rust"]["wall_seconds"] / p["legacy"]["wall_seconds"] for p in pairs]
    rss_deltas = [p["rust"]["peak_rss_bytes"] - p["legacy"]["peak_rss_bytes"] for p in pairs]
    rng = random.Random(0x4c4142)
    bootstrap = sorted(statistics.median(rng.choices(ratios, k=len(ratios))) for _ in range(10000))
    summary["paired"] = {"rust_over_legacy_wall_ratios": ratios, "median_wall_ratio": statistics.median(ratios),
                         "median_ratio_bootstrap_95_percent": [bootstrap[250], bootstrap[9749]],
                         "rss_delta_bytes": rss_deltas, "median_rss_delta_bytes": statistics.median(rss_deltas)}
    for language in ("cpp", "rust"):
        rows = list(csv.DictReader((output / f"{language}.csv").open()))
        for phase in sorted({r["phase"] for r in rows}):
            selected = [r for r in rows if r["phase"] == phase]
            values = []
            for round_id in sorted({r["round"] for r in selected}):
                group = [r for r in selected if r["round"] == round_id]
                values.append(sum(int(r["nanoseconds"]) for r in group) / sum(int(r["calls"]) for r in group))
            summary["components"][phase] = {"ns_per_call": distribution(values),
                "calls": sum(int(r["calls"]) for r in selected),
                "allocation_calls": sum(int(r.get("cpp_new_calls", r.get("rust_alloc_calls"))) for r in selected),
                "requested_bytes": sum(int(r.get("cpp_new_bytes", r.get("rust_alloc_bytes"))) for r in selected),
                "allocator_domain": "C++ operator new" if language == "cpp" else "Rust GlobalAlloc"}
            summary["components_by_reason"][phase] = {}
            for reason in sorted({r["reason"] for r in selected}):
                group = [r for r in selected if r["reason"] == reason]
                summary["components_by_reason"][phase][reason] = distribution(
                    [int(r["nanoseconds"]) / int(r["calls"]) for r in group])
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "cpp-bench", "rust-bench", "input", "qsf"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    parser.add_argument("--device", default="5CSEBA6U23I7")
    parser.add_argument("--pairs", type=int, default=7)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--repeats", type=int, default=128)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    require(args.pairs >= 2 and args.rounds >= 2 and args.repeats > 0, "need >=2 pairs/rounds and positive repeats")
    # The reconstructed live microbenchmark uses this device's LAB0.
    require(args.device == "5CSEBA6U23I7", "C++ microbenchmark currently uses device 5CSEBA6U23I7")
    paths = {key: getattr(args, key).resolve() for key in ("binary", "cpp_bench", "rust_bench", "input", "qsf")}
    for key, path in paths.items():
        require(path.is_file(), f"missing {key}: {path}")
    if args.output_dir:
        output = args.output_dir.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="lab-profile-", dir=ROOT / "build"))
    manifest = {"paths": {k: str(v) for k, v in paths.items()}, "sha256": {k: digest(v) for k, v in paths.items()},
                "platform": platform.platform(), "machine": platform.machine(), "cpu_count": os.cpu_count(),
                "environment": {k: os.environ.get(k) for k in ("MISTRAL_LAB_INPUT_LIMIT", "MISTRAL_HEAP_BETA")},
                "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "pairs": args.pairs, "rounds": args.rounds, "repeats": args.repeats, "runs": []}
    sources = [ROOT / "CMakeLists.txt", ROOT / "mistral/CMakeLists.txt", ROOT / "rust/Cargo.toml", ROOT / "rust/Cargo.lock"]
    for suffix in ("*.cc", "*.h", "*.py"):
        sources += list((ROOT / "mistral").rglob(suffix))
    for crate in ("npnr_mistral_lab", "npnr_mistral_lab_ffi"):
        sources += list((ROOT / "rust" / crate).rglob("*.rs")) + [ROOT / "rust" / crate / "Cargo.toml"]
    manifest["source_sha256"] = {str(p.relative_to(ROOT)): digest(p) for p in sorted(sources)}
    (output / "working-tree.patch").write_bytes(subprocess.check_output(["git", "diff", "--binary"], cwd=ROOT))

    def save():
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    def pnr(label, mode, extra=()):
        design, report, log = (output / f"{label}{suffix}" for suffix in (".json", ".report.json", ".log"))
        command = [str(paths["binary"]), "--device", args.device, "--json", str(paths["input"]), "--qsf", str(paths["qsf"]),
                   "--seed", "1", "--threads", "1", "--placer", "heap", "--router", "router2", "--freq", "12",
                   "--router2-max-iter", "100", "--lab-controls", mode, "--write", str(design), "--report", str(report), *extra]
        print(f"Running {label}...", flush=True)
        result = run(command, log)
        require(result["exit_code"] == 0, f"{label} failed: {log}")
        result.update(label=label, mode=mode, routed_sha256=digest(design))
        data = json.loads(report.read_text())
        result["report"] = {key: data[key] for key in ("fmax", "utilization")}
        if mode == "rust":
            match = re.search(r"LAB controls rust: ([^\n]+)", log.read_text())
            require(match is not None, "missing Rust counters")
            result["counters"] = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", match.group(1))}
            require(all(result["counters"][key] == 0 for key in ("errors", "mismatches", "fallbacks", "diagnostics")),
                    "Rust errors/fallbacks in measured run")
        print(f"{label}: {result['wall_seconds']:.3f}s, {result['peak_rss_bytes']/1048576:.2f} MiB", flush=True)
        return result

    save()
    print(f"Profiling artifacts: {output}", flush=True)
    corpus = output / "corpus.jsonl"
    manifest["capture"] = pnr("capture", "legacy", ["--lab-controls-profile", str(corpus)])
    records = [json.loads(line) for line in corpus.read_text().splitlines()]
    manifest["corpus"] = {"sha256": digest(corpus), "records": len(records),
                          "preparation_records": sum("preparation" in r["provenance"] for r in records),
                          "legal_records": sum(r["expected"]["status"] == 0 for r in records)}
    save()
    for language in ("cpp", "rust"):
        command = [str(paths[f"{language}_bench"]), str(corpus), str(output / f"{language}.csv"), str(args.rounds), str(args.repeats)]
        print(f"Running {language} component benchmark...", flush=True)
        manifest[f"{language}_microbenchmark"] = run(command, output / f"{language}.log")
        save()
        require(manifest[f"{language}_microbenchmark"]["exit_code"] == 0, f"{language} benchmark failed; see log")
    for pair in range(args.pairs + 1):
        for mode in (("legacy", "rust") if pair % 2 == 0 else ("rust", "legacy")):
            result = pnr(f"pair-{pair:02d}-{mode}", mode)
            result.update(pair=pair, warmup=pair == 0)
            manifest["runs"].append(result)
            save()
            require(result["routed_sha256"] == manifest["capture"]["routed_sha256"], "routed output changed")
            require(result["report"] == manifest["capture"]["report"], "timing/utilization changed")
    manifest["passed"] = True
    save()
    print(json.dumps(summarize(manifest, output)["paired"], indent=2), flush=True)
    print(f"Completed profiling: {output / 'summary.json'}", flush=True)


if __name__ == "__main__":
    main()
