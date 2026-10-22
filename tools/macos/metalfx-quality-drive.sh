#!/usr/bin/env bash
# tools/macos/metalfx-quality-drive.sh TAG SECS: a minimal hidden run driver for the MetalFX quality
# captures. It starts the port's own run.sh (hidden: BB_WARM_ONLY=1), presses the title's Continue
# itself, holds a pad state from ROUTE once gameplay draws flow, arms BB_DUMP_TRIGGER after
# DUMP_AT seconds and ends the run with a hard kill — no warm-route state machine in the way (that
# one has hung past its own time cap on this machine).
#
#   TAG    the arm: st (stand still), mv (turn), nat (upscaler off, native 3440)
#   SECS   the run's length in seconds (default 170)
#
# Env: QROOT (the built checkout), DUMP_AT (12 s after the first >=500 draws/frame), FRAMES (10),
# ROUTE (the pad state held from gameplay on), BB_WARM_USER, RESET_WARM_USER, BB_WARM_SOURCE_USER.
set -uo pipefail
tag=${1:?usage: metalfx-quality-drive.sh TAG [SECS]}
secs=${2:-170}
spike=$(cd -- "$(dirname -- "$0")/../.." && pwd)
qroot=${QROOT:-$HOME/repos/bbport-mfxq-build}
res=$spike/out/quality
mkdir -p "$res"
exec >>"$res/capture-$tag.txt" 2>&1
echo "=== metalfx-quality-drive: $tag $(date +%T) qroot=$qroot secs=$secs frames=${FRAMES:-10} dump_at=${DUMP_AT:-12}"

case $tag in
    st)  arm_upscaler=fsr3; route=${ROUTE:-ly=128,rx=128} ;;
    mv|mv2) arm_upscaler=fsr3; route=${ROUTE:-ly=128,rx=94} ;;
    nat) arm_upscaler=off;  route=${ROUTE:-ly=128,rx=128} ;;
    *) echo "unknown arm $tag"; exit 1 ;;
esac

# Every earlier run has to be GONE: a surviving run.sh keeps its tee open on last-run.log and a
# second game's frames land in the same file (and the same 12 GiB of guest memory).
for p in $(pgrep -f 'warm_route.sh|gpu_ab_run.sh|tools/macos/run.sh'); do kill "$p" 2>/dev/null; done
for g in $(pgrep -x bb-probe) $(pgrep -x bb-gpu); do kill "$g" 2>/dev/null; done
for _ in $(seq 1 20); do
    pgrep -x bb-probe >/dev/null || pgrep -x bb-gpu >/dev/null || break
    sleep 1
done
pkill -9 -x bb-probe 2>/dev/null; pkill -9 -x bb-gpu 2>/dev/null; sleep 1

warm_user=${BB_WARM_USER:-$qroot/out/curve3440/user}
source_user=${BB_WARM_SOURCE_USER:-$HOME/repos/bbport-mac/user}
if [[ ${RESET_WARM_USER:-0} == 1 || ! -d $warm_user ]]; then
    rm -rf "$warm_user"
    mkdir -p "$(dirname "$warm_user")"
    cp -cR "$source_user" "$warm_user" 2>/dev/null || cp -aR "$source_user" "$warm_user"
fi
pad=$qroot/out/mfxq_pad
: > "$pad"
log=$qroot/out/last-run.log
rm -rf "$res/dump-$tag"; mkdir -p "$res/dump-$tag"
rm -f "$res/frames-$tag.csv"
dtrig=$res/dump_trigger_$tag
rm -f "$dtrig" "$dtrig"_final

cd "$qroot" || exit 1
export BB_GAME_DIR=${BB_GAME_DIR:-$HOME/Games/Bloodborne/CUSA03173}
[[ -d $BB_GAME_DIR ]] || { echo "STOP: no game folder at $BB_GAME_DIR"; exit 1; }
BB_WARM_ONLY=1 BB_WARM_QUIET=0 BB_WARM_SECONDS=$((secs + 40)) \
    BB_USER_DIR="$warm_user" BB_PAD_FILE="$pad" BB_FRAME_STATS=1 \
    BB_UPSCALER=$arm_upscaler BB_UPSCALE_PRESET=4 BB_OUTPUT_RES=3440x1440 BB_FRAME_GEN=0 \
    BB_DUMP_TRIGGER="$dtrig" BB_DUMP_DIR="$res/dump-$tag" BB_DUMP_FRAMES=${FRAMES:-10} \
    BB_FINAL_DUMP_TRIGGER="${dtrig}_final" \
    BB_FRAME_LOG="$res/frames-$tag.csv" \
    bash tools/macos/run.sh > "$res/route-$tag.txt" 2>&1 < /dev/null &
game=$!
echo "drive: run.sh pid $game"
# run.sh rotates last-run.log itself: give it a moment, so the draw counts below are this run's
# and not the previous one's (that is how a dump got armed at the title).
sleep 4

# The frame log is this run's own (run.sh truncates it at start, and the CSV path is deleted
# below): the port's last-run.log still holds the previous run until run.sh rotates it, which is
# how an earlier version armed a dump at the title screen.
maxdraws() { tail -1 "$res/frames-$tag.csv" 2>/dev/null | awk -F, '{printf "%d", $16}'; }

# 1. the title's Continue, until the log shows an area
start=$SECONDS
hits=0
while (( SECONDS - start < 120 )); do
    d=$(maxdraws)
    if [[ -n ${d:-} && $d -ge 500 ]]; then
        hits=$((hits + 1))
        (( hits >= 3 )) && break
    else
        hits=0
    fi
    printf 'cross\n' > "$pad"; sleep 1.0; : > "$pad"; sleep 2.0
    kill -0 $game 2>/dev/null || break
done
echo "drive: gameplay at ${d:-0} draws/frame after $((SECONDS - start))s of presses"

# 2. hold the route, then arm the dumps
printf '%s\n' "$route" > "$pad"
sleep "${DUMP_AT:-12}"
: > "$dtrig"; : > "${dtrig}_final"
echo "drive: dump armed $(date +%T)"
for _ in $(seq 1 40); do
    n=$(ls "$res/dump-$tag" 2>/dev/null | wc -l | tr -d ' ')
    (( n > 0 )) && break
    sleep 1
done
echo "drive: $n dump files"
sleep 4
# 3. the run's end: a hard kill, and no leftover processes from it
end=$((SECONDS + secs))
while (( SECONDS < end )); do kill -0 $game 2>/dev/null || break; sleep 2; done
for g in $(pgrep -x bb-probe) $(pgrep -x bb-gpu); do
    ps -o command= -p "$g" 2>/dev/null | grep -q "$qroot" && kill "$g" 2>/dev/null
done
sleep 3
for g in $(pgrep -x bb-probe) $(pgrep -x bb-gpu); do kill -9 "$g" 2>/dev/null; done
cp -f "$log" "$res/run-$tag.log" 2>/dev/null
d=$(maxdraws); cam=$(grep -c '^Dump camera:' "$log" 2>/dev/null || true)
gcn=$(grep -c 'gcn_to_vk.cpp:422' "$log" 2>/dev/null || true)
pm4=$(grep -cE 'cp_graphics.cpp:203|Bad PM4' "$log" 2>/dev/null || true)
n=$(ls "$res/dump-$tag" 2>/dev/null | wc -l | tr -d ' ')
echo "tag=$tag max_draws=${d:-0} dump_files=$n camera_lines=${cam:-0} gcn422=${gcn:-0} pm4_203=${pm4:-0}"
echo "=== done $tag $(date +%T)"
