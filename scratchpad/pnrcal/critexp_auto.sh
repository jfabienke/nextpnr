#!/usr/bin/env bash
# critexp_auto.sh — G1 conditional applier (see MISTRAL_GAPS.md §G1).
#
# The measured finding: --placer-heap-critexp 2 --placer-heap-timingweight 30 is a CONDITIONAL
# trade, not a uniform win (+12.5% core / -7.2% neutral / +3.7% compact). The predictor is whether
# routing is CONGESTION-BOUND (high router2 iteration count at the default config), not design size.
# This tool automates exactly that decision, honestly:
#
#   1. run the DEFAULT config; parse router iterations + routed Fmax from the log
#   2. if iterations < threshold: NOT congestion-bound -> keep the default, do NOT apply the flags
#   3. else: re-run with critexp 2 / timingweight 30; compare ROUTED Fmax (never post-place —
#      "tighter predicted != better routed"); keep whichever .rbf actually routed faster
#   4. emit a one-line JSON verdict (schema critexp-auto/1) naming both measurements and the choice
#
# The threshold default (200) sits between the measured populations: neutral routed in 81 iters
# (flags hurt), core needed 1312 (flags helped). It is a tunable hypothesis from three data points,
# not a law — override with CRITEXP_ITER_THRESHOLD, and re-derive it via pnrcal.sh as designs accrue.
#
# Usage: critexp_auto.sh <design.json> <device> <out.rbf> [--qsf file.qsf] [-- <extra nextpnr args>]
# Env:   NEXTPNR (binary; default: sibling build/nextpnr-mistral), CRITEXP_ITER_THRESHOLD (default 200),
#        CRITEXP_FREQ (MHz, default 50), MISTRAL_HEAP_BETA (forwarded if set; 0.35 routes dense cores).
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NEXTPNR="${NEXTPNR:-$HERE/../../build/nextpnr-mistral}"
THRESH="${CRITEXP_ITER_THRESHOLD:-200}"
FREQ="${CRITEXP_FREQ:-50}"

JSON="${1:?usage: critexp_auto.sh <design.json> <device> <out.rbf> [--qsf f] [-- extra args]}"
DEVICE="${2:?device required (e.g. 5CSEBA6U23I7)}"
OUT="${3:?output .rbf required}"
shift 3
QSF=()
if [ "${1:-}" = "--qsf" ]; then QSF=(--qsf "$2"); shift 2; fi
[ "${1:-}" = "--" ] && shift
EXTRA=("$@")

[ -x "$NEXTPNR" ] || { echo "nextpnr-mistral not executable: $NEXTPNR" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# run <tag> <rbf-out> [flags...] -> writes $WORK/<tag>.log; echoes "iters fmax" (fmax in MHz, 0 if absent)
run() {
  local tag="$1" rbf="$2"; shift 2
  "$NEXTPNR" --device "$DEVICE" --json "$JSON" "${QSF[@]}" --rbf "$rbf" \
             --freq "$FREQ" "$@" "${EXTRA[@]}" > "$WORK/$tag.log" 2>&1
  local status=$?
  # router2 prints "    iter=N wires=..."; the LAST iter line is the total count.
  local iters fmax
  iters=$(grep -o 'iter=[0-9]*' "$WORK/$tag.log" | tail -1 | cut -d= -f2)
  # timing report: "Max frequency for clock '...': XX.XX MHz" — worst (min) across clocks.
  fmax=$(grep -o "Max frequency for clock[^:]*: *[0-9.]*" "$WORK/$tag.log" \
         | grep -o '[0-9.]*$' | sort -n | head -1)
  echo "${iters:-0} ${fmax:-0} $status"
}

echo "critexp-auto: default run (freq ${FREQ}MHz)..." >&2
read -r D_ITERS D_FMAX D_STATUS <<< "$(run default "$WORK/default.rbf")"
if [ "$D_STATUS" != 0 ]; then
  echo "{\"schema\":\"critexp-auto/1\",\"verdict\":\"error\",\"stage\":\"default\",\"detail\":\"nextpnr exited $D_STATUS; see log\",\"log\":\"$WORK kept? no — rerun manually\"}"
  tail -5 "$WORK/default.log" >&2
  exit 3
fi

if [ "$D_ITERS" -lt "$THRESH" ]; then
  # Not congestion-bound: the measured evidence says the flags HURT here (-7.2% on neutral). Keep default.
  cp "$WORK/default.rbf" "$OUT"
  echo "{\"schema\":\"critexp-auto/1\",\"verdict\":\"keep_default\",\"congestion_bound\":false,\"router_iters\":$D_ITERS,\"iter_threshold\":$THRESH,\"fmax_mhz\":$D_FMAX,\"applied_flags\":null,\"note\":\"below threshold - critexp 2 measured to HURT non-congested designs; not applied\"}"
  exit 0
fi

echo "critexp-auto: congestion-bound ($D_ITERS iters >= $THRESH) — trying critexp 2 / tw 30..." >&2
read -r C_ITERS C_FMAX C_STATUS <<< "$(run critexp2 "$WORK/critexp2.rbf" --placer-heap-critexp 2 --placer-heap-timingweight 30)"

if [ "$C_STATUS" != 0 ] || [ "$(echo "$C_FMAX <= 0" | bc -l)" = 1 ]; then
  cp "$WORK/default.rbf" "$OUT"
  echo "{\"schema\":\"critexp-auto/1\",\"verdict\":\"keep_default\",\"congestion_bound\":true,\"router_iters\":$D_ITERS,\"fmax_mhz\":$D_FMAX,\"candidate\":{\"status\":\"failed\",\"exit\":$C_STATUS},\"note\":\"candidate run failed to route - default kept\"}"
  exit 0
fi

# The decision is by ROUTED Fmax only.
if [ "$(echo "$C_FMAX > $D_FMAX" | bc -l)" = 1 ]; then
  cp "$WORK/critexp2.rbf" "$OUT"; WIN=apply_critexp2
else
  cp "$WORK/default.rbf" "$OUT"; WIN=keep_default
fi
DELTA=$(echo "scale=2; if ($D_FMAX > 0) ($C_FMAX - $D_FMAX) / $D_FMAX * 100 else 0" | bc -l)
echo "{\"schema\":\"critexp-auto/1\",\"verdict\":\"$WIN\",\"congestion_bound\":true,\"iter_threshold\":$THRESH,\"default\":{\"router_iters\":$D_ITERS,\"fmax_mhz\":$D_FMAX},\"critexp2\":{\"router_iters\":$C_ITERS,\"fmax_mhz\":$C_FMAX},\"fmax_delta_pct\":$DELTA,\"applied_flags\":\"--placer-heap-critexp 2 --placer-heap-timingweight 30\",\"note\":\"decided by routed Fmax - tighter predicted is not better routed\"}"
