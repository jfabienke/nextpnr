#!/bin/bash
# The pre-commit gate for the LAB work (CLAUDE.md, Conventions): the Rust checks scoped to the
# two lab crates, both gtest suites, the exec-probe identity on the default path, and the
# whitespace and format checks. Everything it needs is already in the checkout and the two
# configured trees; on this machine export SDKROOT first (CLAUDE.md, Build).
#
#   mistral/tests/gate.sh [--no-build] [--no-probe]
#
# The probe identity is the default path's: a deliberate change to it updates the three constants
# below and the tracker's record in the same commit. Logs go under build/gate/ (never committed).
set -euo pipefail
cd "$(dirname "$0")/../.."

PROBE_PLACED=0xbb18ede9
PROBE_ROUTED=0xbc1365c6
PROBE_REPORT_SHA256=56e3b75e84be78a30659aa5e3860c3899eb7597e375cfa34a90d13d94a8a034a

build=1
probe=1
for arg in "$@"; do
    case "$arg" in
    --no-build) build=0 ;;
    --no-probe) probe=0 ;;
    *)
        echo "gate: unknown option '$arg'" >&2
        exit 2
        ;;
    esac
done
jobs=${GATE_JOBS:-4}
mkdir -p build/gate
started=$SECONDS

step() { printf '\n== %s (%ds)\n' "$1" $((SECONDS - started)); }
fail() {
    echo "gate: FAILED at '$1'; see $2" >&2
    tail -n 40 "$2" >&2
    exit 1
}

step "cargo test (workspace)"
cargo test --manifest-path rust/Cargo.toml --offline --workspace --quiet >build/gate/cargo-test.log 2>&1 ||
    fail "cargo test" build/gate/cargo-test.log
grep -E '^test result' build/gate/cargo-test.log | sort | uniq -c

step "cargo clippy (lab crates, -D warnings)"
cargo clippy --manifest-path rust/Cargo.toml --offline -p npnr_mistral_lab -p npnr_mistral_lab_ffi \
    -p npnr_mistral_monitor --all-targets --quiet -- -D warnings >build/gate/cargo-clippy.log 2>&1 ||
    fail "cargo clippy" build/gate/cargo-clippy.log
echo clean

step "cargo fmt (lab crates)"
cargo fmt --manifest-path rust/Cargo.toml -p npnr_mistral_lab -p npnr_mistral_lab_ffi -p npnr_mistral_monitor -- --check
echo clean

for tree in build/rust-enabled build; do
    if [ ! -f "$tree/CMakeCache.txt" ]; then
        echo "gate: $tree is not configured; skipped" >&2
        continue
    fi
    name=$(basename "$tree")
    if [ "$build" = 1 ]; then
        step "build $tree"
        cmake --build "$tree" --target nextpnr-mistral nextpnr-mistral-test -j "$jobs" >"build/gate/build-$name.log" 2>&1 ||
            fail "build $tree" "build/gate/build-$name.log"
        grep -E '^(mistral|common)/.*warning:|warning: .*(mistral|common)/' "build/gate/build-$name.log" | head -n 5 || true
    fi
    step "gtest $tree"
    "$tree/nextpnr-mistral-test" >"build/gate/gtest-$name.log" 2>&1 || fail "gtest $tree" "build/gate/gtest-$name.log"
    grep -E '^\[  PASSED' "build/gate/gtest-$name.log"
done

if [ "$probe" = 1 ]; then
    step "exec probe identity (default path)"
    if [ ! -f build/fabi386-inputs/f386_exec_probe_nodsp.json ]; then
        echo "gate: build/fabi386-inputs/ is missing (CLAUDE.md, Tests); probe skipped" >&2
    else
        ./build/rust-enabled/nextpnr-mistral --device 5CSEBA6U23I7 \
            --json build/fabi386-inputs/f386_exec_probe_nodsp.json --qsf build/fabi386-inputs/exec_probe.qsf \
            --seed 1 --threads 1 --placer heap --router router2 --freq 12 --router2-max-iter 100 \
            --lab-controls legacy --report build/gate/probe.report.json --log build/gate/probe.log \
            >/dev/null 2>&1 || fail "probe run" build/gate/probe.log
        sums=$(grep -o 'Checksum: 0x[0-9a-f]*' build/gate/probe.log | awk '{print $2}' | tr '\n' ' ')
        sha=$(shasum -a 256 build/gate/probe.report.json | awk '{print $1}')
        echo "checksums: $sums report: ${sha:0:16}"
        if [ "$sums" != "$PROBE_PLACED $PROBE_ROUTED " ] || [ "$sha" != "$PROBE_REPORT_SHA256" ]; then
            echo "gate: the probe identity changed (expected $PROBE_PLACED $PROBE_ROUTED, report $PROBE_REPORT_SHA256)" >&2
            exit 1
        fi
        echo identical
    fi
fi

step "git diff --check"
git diff --check
git diff --cached --check
echo clean

step "clang-format (changed C++ files)"
files=$( (git diff --name-only HEAD; git ls-files --others --exclude-standard) |
    grep -E '^(mistral|common)/.*\.(cc|h)$' | sort -u || true)
if [ -z "$files" ]; then
    echo "no changed C++ files"
else
    # shellcheck disable=SC2086
    clang-format --dry-run --Werror --style=file $files
    echo "$(echo "$files" | wc -l | tr -d ' ') files clean"
fi

step "gate passed"
