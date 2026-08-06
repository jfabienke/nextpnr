#!/usr/bin/env bash
# pnrcal.sh — the placer/router calibration matrix (referenced by MISTRAL_GAPS.md; recreated —
# the original scratchpad copy was never committed. This is the harness that must gate ANY
# placer/router change: three P&R changes in the 07-24/25 session were judged by reasoning plus
# one run, and all three needed correcting. Change one term -> measure ROUTED outcomes, across
# designs, in a table.)
#
# Runs every design × every config, tabulating: routed? | router iters | final overuse |
# routed Fmax (worst clock) | wall time. Judge by the TABLE, never one cell.
#
# Usage: pnrcal.sh designs.tsv configs.tsv
#   designs.tsv: <name> <json> <device> [qsf]        (tab- or space-separated, # comments ok)
#   configs.tsv: <label> [flags...]                  (label "default" = no flags)
# Env: NEXTPNR (default sibling build/nextpnr-mistral), PNRCAL_FREQ (default 50),
#      MISTRAL_HEAP_BETA (forwarded if set), PNRCAL_KEEP_LOGS=dir (keep logs for inspection).
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NEXTPNR="${NEXTPNR:-$HERE/../../build/nextpnr-mistral}"
FREQ="${PNRCAL_FREQ:-50}"
DESIGNS="${1:?usage: pnrcal.sh designs.tsv configs.tsv}"
CONFIGS="${2:?configs.tsv required}"
[ -x "$NEXTPNR" ] || { echo "nextpnr-mistral not executable: $NEXTPNR" >&2; exit 2; }

LOGDIR="${PNRCAL_KEEP_LOGS:-$(mktemp -d)}"
mkdir -p "$LOGDIR"

printf '%-12s %-22s %7s %8s %8s %10s %8s\n' design config routed iters overuse fmax_mhz time_s
printf '%-12s %-22s %7s %8s %8s %10s %8s\n' ------ ------ ------ ----- ------- -------- ------

grep -v '^\s*#' "$DESIGNS" | grep -v '^\s*$' | while read -r NAME JSON DEVICE QSF; do
  grep -v '^\s*#' "$CONFIGS" | grep -v '^\s*$' | while read -r LABEL FLAGS; do
    LOG="$LOGDIR/${NAME}_${LABEL}.log"
    RBF="$LOGDIR/${NAME}_${LABEL}.rbf"
    QARG=(); [ -n "${QSF:-}" ] && QARG=(--qsf "$QSF")
    CFG=(); [ "$LABEL" != "default" ] && read -ra CFG <<< "$FLAGS"
    T0=$(date +%s)
    "$NEXTPNR" --device "$DEVICE" --json "$JSON" "${QARG[@]}" --rbf "$RBF" \
               --freq "$FREQ" "${CFG[@]}" > "$LOG" 2>&1
    ST=$?
    T=$(( $(date +%s) - T0 ))
    ITERS=$(grep -o 'iter=[0-9]*' "$LOG" | tail -1 | cut -d= -f2)
    OVER=$(grep -o 'overuse=[0-9]*' "$LOG" | tail -1 | cut -d= -f2)
    FMAX=$(grep -o "Max frequency for clock[^:]*: *[0-9.]*" "$LOG" | grep -o '[0-9.]*$' | sort -n | head -1)
    ROUTED=no; [ $ST -eq 0 ] && [ -s "$RBF" ] && ROUTED=yes
    printf '%-12s %-22s %7s %8s %8s %10s %8s\n' \
      "$NAME" "$LABEL" "$ROUTED" "${ITERS:--}" "${OVER:--}" "${FMAX:--}" "$T"
  done
done
echo
echo "logs: $LOGDIR"
