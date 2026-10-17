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
# One area per invocation, and one route per area. For the whole thing in one command — snapshot,
# several routes, and the merge back into the store the game reads — use tools/macos/warmfill.sh,
# which runs this script as its step and points the sandbox at its own copy with BB_WARM_USER.
#
# The player's save is never touched: the user dir is copied once to $BB_WARM_USER (out/warm_user
# unless BB_WARM_USER says otherwise) and BB_USER_DIR points the run at it (the pipeline cache comes
# with it, so the run preloads what earlier runs compiled and adds to it). RESET_WARM_USER=1 copies
# it afresh; BB_WARM_SOURCE_USER overrides which user dir is copied (default the one this layout
# uses, ${BB_DATA_DIR:-.}/user).
#
# The run is hidden and windowed, always: BB_WARM_ONLY=1 is set below and not overridable
# (gpu/shim/window.cpp creates a hidden window and never switches the display mode for it), so
# nothing appears on the owner's screen and the run ends at BB_WARM_SECONDS rather than by hand.
#
# Env: TITLE_WAIT (s from the start to the title, default 45), LOAD_WAIT (s after the cross, default
# 30: measured — the run's own log puts the first frames of the area five seconds after the cross,
# so the 75 this used to be spent 70 s of every run standing still in an already loaded area; 30 s
# leaves 25 s of margin, and a press into a load is ignored rather than fatal), ROUTE (the walk loop,
# default below), plus everything the run itself reads (BB_NATIVE_GPU, BB_FRAME_STATS,
# BB_WARM_SECONDS ...).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
secs=${1:?usage: tools/macos/warm_route.sh SECS [ENV=VALUE ...]}
shift
: "${BB_GAME_DIR:?set BB_GAME_DIR to the merged 1.09 game folder}"
export BB_GAME_DIR
# Hidden, windowed: the caches are compiled for later sessions and nothing shows on screen.
export BB_WARM_ONLY=1

# The sandbox. cp -c clones on APFS: the saves and the cache cost nothing.
warm_user=${BB_WARM_USER:-$PWD/out/warm_user}
source_user=${BB_WARM_SOURCE_USER:-${BB_DATA_DIR:-.}/user}
if [[ ${RESET_WARM_USER:-0} == 1 || ! -d $warm_user ]]; then
    rm -rf "$warm_user"
    if [[ -d $source_user ]]; then
        mkdir -p "$(dirname -- "$warm_user")"
        cp -cR "$source_user" "$warm_user" 2>/dev/null || cp -aR "$source_user" "$warm_user"
    else
        # No user dir to copy: warm a cold cache (the run builds the store from nothing).
        mkdir -p "$warm_user"
    fi
fi
export BB_USER_DIR=$warm_user

pad=$PWD/out/warm_pad
: >"$pad"
export BB_PAD_FILE=$pad
log=$PWD/out/last-run.log
pad_write() { printf '%s\n' "$1" >"$pad"; }   # the file holds the state, not a sequence

title_wait=${TITLE_WAIT:-45}
load_wait=${LOAD_WAIT:-30}
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
# The title's Continue: press it, and keep pressing until the run's own log shows the game is past
# the title. One press at one fixed moment, held 0.4 s, is what this used to do and it is not
# reliable on this build: the run of 2026-10-16 19:09 has no pad-file line for the press at all (the
# log goes from the empty pad straight to the walk) and the game sat at the title for its whole
# length, drawing 24 frames a second and compiling nothing — a whole route's GPU time for nothing.
# The log is the clock that says whether a press took: the title draws ~24 a frame and an area
# several hundred, so while the highest draws/frame is under BB_WARMFILL_GAMEPLAY_DRAWS (200) the
# press is still the title's; once it is over, another press would be an interaction in the area and
# the presses stop. (The game's output is block-buffered through run.sh's tee, so the log lags a
# window or two: a press or two can still land after the transition, which is why the hold is short
# and the presses stop as soon as there is anything to see. TITLE_PRESS_MAX caps the waiting.)
gameplay_draws=${BB_WARMFILL_GAMEPLAY_DRAWS:-200}
title_in() {
    local max
    max=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
    [[ -n $max ]] && ((max >= gameplay_draws))
}
game_start=$SECONDS
presses=0
press_deadline=$((SECONDS + ${TITLE_PRESS_MAX:-90}))
until title_in; do
    pad_write cross
    sleep 1.2
    pad_write ""
    presses=$((presses + 1))
    ((SECONDS < press_deadline)) || break
    sleep 3
done
if title_in; then
    echo "warm route: the title's Continue took after $presses press(es); the log shows the area"
else
    echo "warm route: the title's Continue did NOT take in ${TITLE_PRESS_MAX:-90}s ($presses press(es)); the log still shows the title — this run will walk nothing"
fi

(
    sleep "$load_wait"
    # The walk runs to the run's own end (BB_WARM_SECONDS, `secs`) less the 15 s warm_route.sh keeps
    # for the run to quit in. Counted from the game's start, so the title presses above (which may
    # have taken several tries) come out of the walk rather than overrunning the cap.
    deadline=$((game_start + secs - 15))
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
grep -E '^Warm run:|hidden window|cached pipelines were preloaded|skipping preload|pad file|pad opened' "$log" | tail -8 || true
grep '^Frame stats' "$log" | tail -3 || true
