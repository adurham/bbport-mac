#!/usr/bin/env bash
# tools/macos/capture_condition.sh LABEL: one attribution sample of a VISIBLE run (title-screen
# artifact vs Direct scanout, docs/macos-title-artifact.md). Run it from a second terminal (or over
# ssh) while the game shows the artifact. It takes, within about a second:
#   1. a real screen capture of the game's display (screencapture -x: what the WindowServer scanned
#      out, so a present/scanout tear is in it; several frames, since a tear moves),
#   2. the port's own frame capture (the Ctrl+Shift+C path, via BB_BUGREPORT_TRIGGER): the frame the
#      port drew BEFORE present — a tear is never in it,
#   3. the run's state: the present mode, the layer dumps and checks, the display mode and its
#      scanout verdict, the capture's state line — appended to a report file.
# Compare 1 and 2: artifact in both = the frame itself (rendering); only in 1 = present/scanout
# (tearing, the drawable path) — the matrix in the doc says what each condition means.
#
#   LABEL e.g. opaque1-fifo, opaque1-immediate, opaquekeep-fifo, opaquekeep-immediate,
#   opaque1-fifo-native3440.
# Environment: BB_DATA_DIR (where the run writes out/: the app's ~/Library/Application
# Support/bbport by default, or the checkout), BB_BUGREPORT_TRIGGER (default
# $BB_DATA_DIR/out/bugreport_trigger: the run must have been started with the same value — run.sh
# exports it), SHOTS (screen captures, default 3), DISPLAY_INDEX (screencapture -D, default: the
# main display).
#
# screencapture of the whole screen works for the account running it; capturing other apps'
# windows may ask for the Screen Recording permission (System Settings > Privacy & Security) for
# the terminal app the first time. A black image means that permission is missing.
set -uo pipefail
label=${1:?usage: tools/macos/capture_condition.sh LABEL (e.g. opaque1-fifo)}
label=${label//[^A-Za-z0-9._-]/_}
here=$(cd -- "$(dirname -- "$0")/../.." && pwd)
if [[ -n ${BB_DATA_DIR:-} ]]; then
    data=$BB_DATA_DIR
elif [[ -f $HOME/Library/Application\ Support/bbport/out/last-run.log &&
        ( ! -f $here/out/last-run.log ||
          $HOME/Library/Application\ Support/bbport/out/last-run.log -nt $here/out/last-run.log ) ]]; then
    data=$HOME/Library/Application\ Support/bbport
else
    data=$here
fi
log=$data/out/last-run.log
trigger=${BB_BUGREPORT_TRIGGER:-$data/out/bugreport_trigger}
stamp=$(date +%Y%m%d-%H%M%S)
dir=$data/out/attribution/$stamp-$label
mkdir -p "$dir"
report=$data/out/attribution/report.md
shots=${SHOTS:-3}

[[ -f $log ]] || { echo "STOP: no $log (is a run up? set BB_DATA_DIR)" >&2; exit 1; }
pgrep -x bb-gpu >/dev/null || echo "warning: no bb-gpu process: the game is not running" >&2

# 1 + 2 at the same moment: the port's capture first (it writes on its next frame), the screen
# right after, then a few more screen frames ~100 ms apart (a tear wanders, a rendering bug stays).
before_lines=$(wc -l < "$log")
: > "$trigger"
screen_args=(-x -t png)
[[ -n ${DISPLAY_INDEX:-} ]] && screen_args+=(-D "$DISPLAY_INDEX")
for ((i = 1; i <= shots; i++)); do
    screencapture "${screen_args[@]}" "$dir/screen-$i.png" 2>>"$dir/screencapture.err" ||
        echo "screencapture $i failed (see $dir/screencapture.err)" >&2
    sleep 0.1
done
# The port's PNG: the "Capture: wrote ..." line of this request (the encode runs off-thread).
port_png=""
for _ in $(seq 1 40); do
    port_png=$(tail -n +"$((before_lines + 1))" "$log" | sed -n 's/^Capture: wrote \(.*\.png\) (.*/\1/p' | tail -1)
    [[ -n $port_png ]] && break
    sleep 0.25
done
if [[ -f $trigger ]]; then
    rm -f "$trigger"
    echo "warning: the run did not take the trigger: start it with BB_BUGREPORT_TRIGGER=$trigger (run.sh exports it by default)" >&2
fi
if [[ -n $port_png ]]; then
    if [[ $port_png != /* ]]; then
        # Relative to the GPU process's working directory (the checkout, or the app's Resources).
        gpu_cwd=$(lsof -a -p "$(pgrep -x bb-gpu | head -1)" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p' | head -1)
        for base in "$gpu_cwd" "$data" "$here"; do
            [[ -n $base && -f $base/$port_png ]] && { port_png=$base/$port_png; break; }
        done
    fi
    cp -f "$port_png" "$dir/port-frame.png" 2>/dev/null || echo "warning: cannot copy $port_png" >&2
fi
new=$(tail -n +"$((before_lines + 1))" "$log")

# 3: the state. The display as CoreGraphics has it now (external to the port), then the log's
# lines that set the condition.
display_now=$(system_profiler SPDisplaysDataType 2>/dev/null | sed -n '/Displays:/,$p' | grep -E '^\s+[A-Za-z].*:|Resolution|UI Looks like|Main Display' | sed 's/^ *//')
{
    echo
    echo "## $stamp $label"
    echo
    echo "- screen captures (post-scanout): $(ls "$dir"/screen-*.png 2>/dev/null | wc -l | tr -d ' ') in $dir"
    echo "- port frame (pre-present): ${port_png:-NONE (the run did not answer the trigger)}"
    echo "- verdict (fill in): artifact in screen-*.png [ ] yes [ ] no; in port-frame.png [ ] yes [ ] no; HUD said [ ] Direct [ ] Composited"
    echo
    echo '```'
    echo "# environment of this condition (from the log)"
    grep -m1 '^macOS run:' "$log"
    grep -E '^Settings: |^Present mode: |Present mode .* not supported|^Layer: override|^Layer dump: CAMetalLayer|^Layer dump: display .* mode|^Layer dump: scanout|^Layer check|^Display mode' "$log" | tail -24
    echo "# this sample"
    printf '%s\n' "$new" | grep -E '^Capture: |^Display mode \(capture\)' | tail -6
    echo "# displays now (system_profiler)"
    printf '%s\n' "$display_now"
    echo '```'
} >> "$report"
cp -f "$log" "$dir/last-run.log"
echo "Sample $label: $dir"
echo "Report: $report"
[[ -n $port_png ]] || echo "No port frame: was the run started by run.sh from this build (BB_BUGREPORT_TRIGGER)?" >&2
