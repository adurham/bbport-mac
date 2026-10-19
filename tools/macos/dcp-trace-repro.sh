#!/bin/bash
# dcp-trace-repro.sh — one traced reproduction of the DCPEXT1 axi_rd_err panic.
#
# The three 2026-10-18 panics all lost the tail of the unified log ("logd is
# asynchronous and nothing flushes at panic"), so none of the reports carries a
# trace of the switch that killed the machine. This wraps the minimal reproducer
# in a live `log stream` writing straight to disk, so the next report has the
# seconds before the fault instead of a gap.
#
# Usage:
#   tools/macos/dcp-trace-repro.sh --to 1x              # guarded, refuses on DSC
#   tools/macos/dcp-trace-repro.sh --to 1x --allow-dsc  # the actual experiment
#
# Anything that can panic this machine needs the owner's explicit go-ahead for
# that one run. This script does not ask; it just refuses to run unguarded
# without --allow-dsc.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PROBE="${PROBE:-/tmp/dcp-mode-probe}"
OUTDIR="${OUTDIR:-$HOME/.hermes/cache/scratch/dcp-traces}"
STAMP=$(date +%Y%m%d_%H%M%S)
TRACE="$OUTDIR/trace_$STAMP.log"
MODE_LOG="$OUTDIR/modes_$STAMP.log"
mkdir -p "$OUTDIR"

[ -x "$PROBE" ] || { echo "probe not built: clang -O2 -framework CoreGraphics -framework CoreFoundation -o $PROBE $HERE/dcp-mode-probe.c" >&2; exit 1; }

echo "== before =="
"$PROBE" | tee "$MODE_LOG"
BEFORE_UPTIME=$(sysctl -n kern.boottime | sed 's/.*sec = \([0-9]*\).*/\1/')
PANIC_BEFORE=$(ls -1 /Library/Logs/DiagnosticReports/Retired/panic-*.panic 2>/dev/null | wc -l | tr -d ' ')

# WindowServer's Display:* lines are the record of the set; kernel lines carry the
# DCP messages; keep both, skip the per-frame noise.
PRED='process == "WindowServer" AND eventMessage CONTAINS "Display:"'
log stream --style compact --predicate "$PRED" > "$TRACE" 2>&1 &
STREAM_PID=$!
sleep 2

echo "== switching (this is the part that can reboot the machine) =="
"$PROBE" "$@" | tee -a "$MODE_LOG"
RC=${PIPESTATUS[0]}
echo "probe exit: $RC"

# The 23:26 fault fired 108.6 s AFTER a set that returned success, so a two-minute
# wait is the minimum useful observation window, not a politeness delay.
echo "== watching 130 s for the delayed fault =="
sleep 130
kill "$STREAM_PID" 2>/dev/null
wait "$STREAM_PID" 2>/dev/null

AFTER_UPTIME=$(sysctl -n kern.boottime | sed 's/.*sec = \([0-9]*\).*/\1/')
PANIC_AFTER=$(ls -1 /Library/Logs/DiagnosticReports/Retired/panic-*.panic 2>/dev/null | wc -l | tr -d ' ')

echo "== result =="
if [ "$AFTER_UPTIME" != "$BEFORE_UPTIME" ]; then
    echo "REBOOTED — the machine panicked. New panic reports: $((PANIC_AFTER - PANIC_BEFORE))"
    ls -lt /Library/Logs/DiagnosticReports/Retired/panic-*.panic 2>/dev/null | head -3
else
    echo "still up (no reboot); uptime unchanged"
    echo "after: $("$PROBE" | head -1)"
    echo "DSC:  $(/opt/homebrew/bin/betterdisplaycli get -name=AW3425DW -DSCReport 2>/dev/null | grep -o 'DSC): .*')"
fi
echo
echo "trace:  $TRACE  ($(wc -l < "$TRACE" | tr -d ' ') lines)"
echo "modes:  $MODE_LOG"
echo "-- last 25 trace lines --"
tail -25 "$TRACE"
