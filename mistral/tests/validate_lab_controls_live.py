#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Compare serial full P&R across Rust-off legacy and all Rust-on LAB modes.

Uses caller-provided synthesized JSON/QSF (for example the Fabi386 execution
slice). Requires POSIX wait4 for per-run peak RSS. Writes only to a fresh run
directory. One run per mode establishes parity, not a performance benchmark.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def run(command, log):
    start = time.monotonic()
    with log.open("wb") as stream:
        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
    return {"command": command, "exit_code": process.returncode,
            "wall_seconds": time.monotonic() - start,
            "user_seconds": usage.ru_utime, "system_seconds": usage.ru_stime,
            "peak_rss_bytes": int(usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024))}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--off-binary", type=Path, required=True)
    parser.add_argument("--rust-binary", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--qsf", type=Path, required=True)
    parser.add_argument("--device", default="5CSEBA6U23I7")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    paths = {name: getattr(args, name).resolve() for name in ("off_binary", "rust_binary", "input", "qsf")}
    for name, path in paths.items():
        require(path.is_file(), f"missing {name}: {path}")
    if args.output_dir:
        output = args.output_dir.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="lab-live-", dir=ROOT / "build"))
    manifest = {"paths": {name: str(path) for name, path in paths.items()},
                "sha256": {name: digest(path) for name, path in paths.items()},
                "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "environment": {key: os.environ.get(key) for key in
                                ("MISTRAL_LAB_INPUT_LIMIT", "MISTRAL_HEAP_BETA")},
                "runs": [], "configuration_checks": []}
    (output / "working-tree.patch").write_bytes(subprocess.check_output(["git", "diff", "--binary"], cwd=ROOT))
    sources = [ROOT / "CMakeLists.txt", ROOT / "rust/Cargo.toml", ROOT / "rust/Cargo.lock"]
    sources += list((ROOT / "mistral").rglob("*.cc")) + list((ROOT / "mistral").rglob("*.h"))
    sources += list((ROOT / "mistral").rglob("*.py")) + [ROOT / "mistral/CMakeLists.txt"]
    for crate in ("npnr_mistral_lab", "npnr_mistral_lab_ffi"):
        sources += list((ROOT / "rust" / crate).rglob("*.rs")) + [ROOT / "rust" / crate / "Cargo.toml"]
    manifest["source_sha256"] = {str(path.relative_to(ROOT)): digest(path) for path in sorted(sources)}
    manifest_path = output / "manifest.json"

    def save():
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    save()
    print(f"Validation artifacts: {output}", flush=True)
    for mode in ("shadow", "verify", "rust"):
        check = run([str(paths["off_binary"]), "--device", args.device, "--lab-controls", mode], output / f"off-{mode}.log")
        require(check["exit_code"] != 0 and "requires BUILD_RUST=ON" in (output / f"off-{mode}.log").read_text(),
                f"Rust-off binary did not reject {mode}")
        manifest["configuration_checks"].append(check)
    save()
    routed_hashes, reports, counters = {}, {}, {}
    for label, binary, mode in [("off-legacy", paths["off_binary"], "legacy")] + [
            (f"on-{mode}", paths["rust_binary"], mode) for mode in ("legacy", "shadow", "verify", "rust")]:
        design, report, log = (output / f"{label}{suffix}" for suffix in (".json", ".report.json", ".log"))
        command = [str(binary), "--device", args.device, "--json", str(paths["input"]), "--qsf", str(paths["qsf"]),
                   "--seed", "1", "--threads", "1", "--placer", "heap", "--router", "router2", "--freq", "12",
                   "--router2-max-iter", "100", "--lab-controls", mode, "--write", str(design), "--report", str(report)]
        print(f"Running {label}...", flush=True)
        result = run(command, log)
        result.update(label=label, mode=mode)
        manifest["runs"].append(result)
        save()
        require(result["exit_code"] == 0, f"{label} failed; see {log}")
        routed_hashes[label] = digest(design)
        data = json.loads(report.read_text())
        reports[label] = {key: data[key] for key in ("fmax", "utilization")}
        if mode != "legacy":
            match = re.search(r"LAB controls " + mode + r": ([^\n]+)", log.read_text())
            require(match is not None, f"missing {mode} counters")
            counts = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", match.group(1))}
            require(counts["evaluations"] > counts["preparation"] > 0, "placement/preparation were not both checked")
            require(all(counts[key] == 0 for key in ("errors", "mismatches", "fallbacks", "diagnostics")),
                    f"{mode} encountered errors, mismatches, or fallback")
            require(counts["legal"] + counts["illegal"] == counts["evaluations"], "outcomes do not sum to evaluations")
            counters[label] = counts
        print(f"{label}: {result['wall_seconds']:.2f}s, peak RSS {result['peak_rss_bytes'] / 1048576:.1f} MiB", flush=True)

    manifest.update(routed_sha256=routed_hashes, reports=reports, counters=counters,
                    routed_equal=len(set(routed_hashes.values())) == 1,
                    timing_utilization_equal=all(value == reports["off-legacy"] for value in reports.values()))
    save()
    require(manifest["routed_equal"], "routed JSON differs between modes")
    require(manifest["timing_utilization_equal"], "timing/utilization differs between modes")
    print(f"Passed all five P&R runs, exact routed output and timing/utilization parity: {manifest_path}", flush=True)


if __name__ == "__main__":
    main()
