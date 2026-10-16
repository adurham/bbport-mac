#!/usr/bin/env bash
# tools/macos/warm_route.sh SECS [ENV=VALUE ...]: fills the pipeline cache from a gameplay area,
# in a hidden run, by driving the game there with a scripted pad.
#
# Why it exists: a pipeline is created at the first DRAW with its state key (shader + render state +
# vertex layout + render targets), not when an area loads, so the pipelines an area needs can only be
# made by getting the game to draw it once. A plain warm run (BB_WARM_ONLY) never gets there: it quits
# on the first lull in the compiles, and the title screen is such a lull — it preloads what the cache
# already holds and compiles nothing new (presenter.cpp, BB_WARM_QUIET). This script presses the
# title's Continue and then walks and turns, with the early quit off, so the area's own pipelines
# are compiled and written to <user>/cache/<serial>/.
#
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/warm_route.sh 300
#
# The player's save is never touched: <checkout>/user is copied once to out/warm_user and BB_USER_DIR
# points the run at it (the pipeline cache comes with it, so the run preloads what earlier runs
# compiled and adds to it). RESET_WARM_USER=1 copies it afresh.
#
# Env: TITLE_WAIT (s from the start to the title, default 45), LOAD_WAIT (s after the cross, default
# 75: the load), ROUTE (the walk loop, default below), plus everything the run itself reads
# (BB_NATIVE_GPU, BB_FRAME_STATS, BB_WARM_SECONDS ...).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
secs=${1:?usage: tools/macos/warm_route.sh SECS [ENV=VALUE ...]}
shift
: "${BB_GAME_DIR:?set BB_GAME_DIR to the merged 1.09 game folder}"
export BB_GAME_DIR

# The sandbox. cp -c clones on APFS: the 14 MB save and the cache cost nothing.
if [[ ${RESET_WARM_USER:-0} == 1 || ! -d out/warm_user ]]; then
    rm -rf out/warm_user
    if [[ -d user ]]; then
        cp -ac user out/warm_user
    else
        # No checkout user/ to copy: warm a cold cache (the run builds the store from nothing).
        mkdir -p out/warm_user
    fi
fi
export BB_USER_DIR=$PWD/out/warm_user

pad=$PWD/out/warm_pad
: >"$pad"
export BB_PAD_FILE=$pad
log=$PWD/out/last-run.log
pad_write() { printf '%s\n' "$1" >"$pad"; }   # the file holds the state, not a sequence

title_wait=${TITLE_WAIT:-45}
load_wait=${LOAD_WAIT:-75}
# The walk: forward, a turn, forward, a turn the other way. ly=0 is forward and ly=255 back (the
# sticks run 0..255 with 128 at rest), rx is the look. An entry is "<pad file contents>:<seconds>",
# the comma standing in for the space the pad file wants between tokens.
default_route='ly=0,rx=128:4 ly=0,rx=40:0.5 ly=0,rx=128:4 ly=0,rx=215:0.5 ly=255,rx=160:3'
route=${ROUTE:-$default_route}
echo "warm route: ${secs}s; title at ${title_wait}s, cross, load ${load_wait}s, then loop: $route"

BB_NATIVE_GPU=${BB_NATIVE_GPU:-1} BB_FRAME_STATS=${BB_FRAME_STATS:-1} BB_WARM_QUIET=${BB_WARM_QUIET:-0} \
    BB_WARM_SECONDS=${BB_WARM_SECONDS:-$secs} \
    bash tools/macos/run.sh >/dev/null 2>&1 </dev/null &
game=$!

sleep "$title_wait"
pad_write cross; sleep 0.4; pad_write ""

(
    sleep "$load_wait"
    deadline=$((SECONDS + secs - load_wait - title_wait - 15))
    while ((SECONDS < deadline)); do
        for entry in $route; do
            ((SECONDS < deadline)) || break
            state=${entry%:*}
            pad_write "${state//,/ }"
            sleep "${entry##*:}"
        done
    done
    pad_write ""
) &
driver=$!

# The run ends on its own (BB_WARM_SECONDS is the warm cap); this only stops it if it does not.
for ((i = 0; i < secs + 180; i++)); do
    sleep 1
    kill -0 "$game" 2>/dev/null || break
done
kill -0 "$game" 2>/dev/null && kill "$game" 2>/dev/null
wait "$driver" 2>/dev/null || true
pad_write ""
echo "--- the run's own numbers ($log)"
grep -E '^Warm run:|cached pipelines were preloaded|skipping preload|pad file|pad opened' "$log" | tail -8 || true
grep '^Frame stats' "$log" | tail -3 || true
