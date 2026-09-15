#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Compare the actual C++ and Rust kernels through value-only test processes.

Requires Python 3.9+, a C++17 compiler, and Cargo. No chip database or FFI bridge
is required. JSON allocation/IO belongs to the drivers, not either kernel.
"""

import argparse
from collections import Counter
import copy
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
MAX_U32 = (1 << 32) - 1
MAX_U64 = (1 << 64) - 1


def signal(net_id=0, flags=0):
    return {"net_id": net_id, "flags": flags}


def empty():
    return {
        "abi_version": 1, "struct_size": 1952, "rules_version": 1,
        "net_count": 0, "request_id": "0", "snapshot_epoch": str(MAX_U64),
        "ff": [{"occupied": 0, "reserved": 0, "control": [signal() for _ in range(5)]} for _ in range(40)],
    }


def canonicalize(value):
    ids = {}
    for ff in value["ff"]:
        for control in ff["control"]:
            if control["net_id"]:
                control["net_id"] = ids.setdefault(control["net_id"], len(ids) + 1)
    value["net_count"] = len(ids)
    return value


def random_input(rng, trial):
    value = empty()
    global_flags = [0] + [2 * rng.randrange(2) for _ in range(8)]
    def choose():
        net = rng.randrange(9)
        return signal(net, global_flags[net] | rng.randrange(2))
    base = [choose() for _ in range(5)]
    for ff in value["ff"]:
        if rng.randrange(3) == 0:
            continue
        ff["occupied"] = 1
        ff["control"] = [choose() if trial % 2 == 0 and rng.randrange(8) == 0 else copy.copy(s) for s in base]
    return canonicalize(value)


def cases(count, seed):
    fixtures = sorted((ROOT / "mistral/tests/fixtures").glob("*.json"))
    if not {"greedy-abc.json", "greedy-bca.json"}.issubset(path.name for path in fixtures):
        raise RuntimeError("missing required legacy replay fixtures")
    for path in fixtures:
        fixture = json.loads(path.read_text())
        yield "fixture", fixture["input"], {"golden": fixture["expected"], "fixture": path.name}
    yield "empty", empty(), {}
    value = empty()
    value.update(abi_version=1.0, struct_size=1952.0, rules_version=1.0, net_count=1.0)
    value["ff"][0]["occupied"] = 1.0
    value["ff"][0]["control"][0] = signal(1.0, 2.0)
    yield "numeric_fields", value, {}
    for slot in range(40):
        value = empty()
        value["ff"][slot]["occupied"] = 1
        value["ff"][slot]["control"][0] = signal(0, 1)
        yield "disconnected_slot", value, {}
    value = empty()
    for i, ff in enumerate(value["ff"]):
        ff["occupied"] = 1
        ff["control"] = [signal(5 * i + j + 1, j % 4) for j in range(5)]
    yield "max_nets", canonicalize(value), {}

    for kind in range(5):
        for choices in itertools.product(range(6), repeat=3):
            value = empty()
            for slot, choice in zip([0, 2, 39], choices):
                value["ff"][slot]["occupied"] = 1
                value["ff"][slot]["control"][kind] = signal(choice // 2, choice % 2)
            yield "exhaustive", canonicalize(value), {}

    # Each validation category, including flags on unoccupied/disconnected slots.
    for field, bad in [("abi_version", 2), ("struct_size", 0), ("rules_version", 2), ("net_count", MAX_U32)]:
        value = empty()
        value[field] = bad
        yield "malformed", value, {}
    for field, bad in [("occupied", 2), ("reserved", 1)]:
        value = empty()
        value["ff"][39][field] = bad
        yield "malformed", value, {}
    for occupied, net, flags in [(0, 0, 1), (1, 0, 2), (1, 0, 4), (1, MAX_U32, 0)]:
        value = empty()
        value["ff"][0]["occupied"] = occupied
        value["ff"][0]["control"][0] = signal(net, flags)
        yield "malformed", value, {}
    value = empty()
    value["net_count"] = 1
    yield "malformed", value, {}
    value = empty()
    value["net_count"] = 1
    value["ff"][0]["occupied"] = 1
    value["ff"][0]["control"][:2] = [signal(1), signal(1, 2)]
    yield "malformed", value, {}

    rng = random.Random(seed)
    for trial in range(count):
        value = random_input(rng, trial)
        yield "random", value, {}
        if trial < min(count, 1000):
            renamed = copy.deepcopy(value)
            perm = list(range(1, value["net_count"] + 1))
            rng.shuffle(perm)
            mapping = {i + 1: new for i, new in enumerate(perm)}
            for ff in renamed["ff"]:
                for control in ff["control"]:
                    control["net_id"] = mapping.get(control["net_id"], 0)
            yield "rename", renamed, {"mapping": mapping}
            reordered = copy.deepcopy(value)
            rng.shuffle(reordered["ff"])
            yield "permutation", reordered, {}  # deliberately no invariant-verdict assertion
        if trial < min(count, 2000):
            broken = copy.deepcopy(value)
            ff = rng.choice(broken["ff"])
            control = rng.choice(ff["control"])
            mutation = trial % 5
            if mutation == 0:
                ff["occupied"] = MAX_U32
            elif mutation == 1:
                ff["reserved"] = rng.randrange(1, MAX_U32)
            elif mutation == 2:
                control["net_id"] = MAX_U32
            elif mutation == 3:
                control["flags"] |= 4
            else:
                control["flags"] ^= 2
            yield "mutated", broken, {}


def translate_result(result, mapping):
    result = copy.deepcopy(result)
    for s in result["allocation"] + [result["incoming"]] + [b["signal"] for b in result["blockers"]]:
        s["net_id"] = mapping.get(str(s["net_id"]), 0)
    return result


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases", type=int, default=10000, help="randomized base cases, in addition to fixed/exhaustive cases")
    parser.add_argument("--seed", type=lambda n: int(n, 0), default=0x4C4142)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="C++17 compiler command")
    parser.add_argument("--offline", action="store_true", help="require cached Cargo test dependencies")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/lab-rust-parity")
    parser.add_argument("--ffi-driver", type=Path, help="also compare a CMake-built nextpnr-mistral-lab-ffi-replay executable")
    args = parser.parse_args()
    if args.cases < 0:
        parser.error("--cases must be nonnegative")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    oracle = output / ("cpp-oracle.exe" if os.name == "nt" else "cpp-oracle")
    target = ROOT / "build/lab-rust"
    rust = target / "release/examples" / ("lab_control_replay.exe" if os.name == "nt" else "lab_control_replay")
    sources = [ROOT / "mistral/tests/lab_control_oracle.cc", ROOT / "mistral/lab_model.cc",
               ROOT / "mistral/lab_replay.cc", ROOT / "3rdparty/json11/json11.cpp"]
    command = shlex.split(args.cxx) + ["-std=c++17", "-O2"]
    command += ["-I" + str(ROOT / directory) for directory in ["mistral", "common/kernel", "3rdparty/json11"]]
    command += [str(path) for path in sources] + ["-o", str(oracle)]
    print("Building the C++ reference and Rust replay drivers...", flush=True)
    with (output / "build.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(["cargo", "build", *( ["--offline"] if args.offline else [] ),
                        "--manifest-path", str(ROOT / "rust/Cargo.toml"), "-p", "npnr_mistral_lab",
                        "--release", "--example", "lab_control_replay", "--target-dir", str(target)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)

    layouts = [json.loads(subprocess.check_output([str(exe), "--layout"], text=True)) for exe in [oracle, rust]]
    if layouts[0] != layouts[1]:
        (output / "layout-mismatch.json").write_text(json.dumps(layouts, indent=2) + "\n")
        raise RuntimeError("C++/Rust native layouts differ")

    counts, outcomes = Counter(), Counter()
    with tempfile.TemporaryDirectory(prefix="records-", dir=output) as directory:
        directory = Path(directory)
        inputs = directory / "inputs.jsonl"
        with inputs.open("w") as stream:
            for index, (category, value, metadata) in enumerate(cases(args.cases, args.seed), start=1):
                value = copy.deepcopy(value)
                value["request_id"] = str(index)
                expected = metadata.get("golden", {"status": 0, "allocation": [signal() for _ in range(12)]})
                record = {"schema": 1, "input": value, "expected": expected, "category": category, "test": metadata}
                stream.write(json.dumps(record, separators=(",", ":")) + "\n")
                counts[category] += 1
        print(f"Comparing {sum(counts.values())} records, including complete conflicts and provenance...", flush=True)
        drivers = [("cpp", oracle), ("rust", rust)]
        if args.ffi_driver:
            drivers.append(("ffi", args.ffi_driver.resolve()))
        for name, exe in drivers:
            with inputs.open() as data, (directory / f"{name}.jsonl").open("w") as results, (output / f"{name}.log").open("w") as log:
                subprocess.run([str(exe)], stdin=data, stdout=results, stderr=log, check=True, timeout=180)

        if args.ffi_driver:
            with inputs.open() as data, (directory / "cpp.jsonl").open() as cpp, (directory / "ffi.jsonl").open() as ffi:
                for lines in itertools.zip_longest(data, cpp, ffi):
                    require(None not in lines, "FFI output record count differs from input")
                    record, reference, candidate = map(json.loads, lines)
                    if reference != candidate:
                        (output / "ffi-mismatch.json").write_text(json.dumps({"record": record, "cpp": reference, "ffi": candidate}, indent=2) + "\n")
                        raise RuntimeError(f"FFI parity failure at request {record['input']['request_id']}")

        base_result = None
        with inputs.open() as data, (directory / "cpp.jsonl").open() as cpp, (directory / "rust.jsonl").open() as rust_results:
            for record_line, cpp_line, rust_line in itertools.zip_longest(data, cpp, rust_results):
                if None in (record_line, cpp_line, rust_line):
                    raise RuntimeError("oracle output record count differs from input")
                record, reference, candidate = map(json.loads, (record_line, cpp_line, rust_line))
                if reference != candidate:
                    (output / "mismatch.json").write_text(json.dumps({"record": record, "cpp": reference, "rust": candidate}, indent=2) + "\n")
                    raise RuntimeError(f"parity failure at request {record['input']['request_id']}; see {output / 'mismatch.json'}")
                require(candidate["request_id"] == record["input"]["request_id"], "request identity was not preserved")
                require(candidate["snapshot_epoch"] == record["input"]["snapshot_epoch"], "snapshot epoch was not preserved")
                outcomes[f"{candidate['status']}:{candidate['reason']}"] += 1
                if record["category"] == "random":
                    base_result = reference
                elif record["category"] == "rename":
                    renamed = translate_result(base_result, record["test"]["mapping"])
                    renamed["request_id"] = candidate["request_id"]
                    require(candidate == renamed, "bijective net-ID renaming changed evaluation")
                elif record["category"] == "fixture":
                    require(candidate["status"] == record["expected"]["status"], "golden fixture verdict differs")
                    if candidate["status"] == 0:
                        require(candidate["allocation"] == record["expected"]["allocation"], "golden fixture allocation differs")

        checked_sources = sources + list((ROOT / "rust/npnr_mistral_lab").rglob("*.rs"))
        checked_sources += [ROOT / "mistral/lab_control_abi.h", ROOT / "rust/Cargo.lock", Path(__file__).resolve()]
        checked_sources += list((ROOT / "rust/npnr_mistral_lab_ffi").rglob("*.rs"))
        summary = {"seed": args.seed, "counts": dict(counts), "outcomes_status_reason": dict(sorted(outcomes.items())),
                   "total": sum(counts.values()), "complete_result_parity": True, "native_layout_parity": True,
                   "input_sha256": sha256(inputs), "source_sha256": {str(path.relative_to(ROOT)): sha256(path) for path in sorted(checked_sources)},
                   "rustc": subprocess.check_output(["rustc", "--version"], text=True).strip(),
                   "cxx": command, "layouts": layouts[0]}
        if args.ffi_driver:
            summary.update(ffi_result_parity=True, ffi_driver=str(args.ffi_driver.resolve()),
                           ffi_driver_sha256=sha256(args.ffi_driver))
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"Passed: {summary['total']} records; {counts['rename']} renaming checks; exact native layouts. Summary: {output / 'summary.json'}")


if __name__ == "__main__":
    main()
