#!/usr/bin/env bash
# tools/macos/game_lib_smoke.sh: hidden smoke test of the game library plug-in point (BB_GAME_LIB,
# docs/GAME_LIBRARY.md). Builds the no-op example library, runs one hidden warm pass of the real
# game with it (no window, no sound, quits by itself) and checks the log: the library was loaded,
# its init saw the host interface, install_hook refused two invalid requests, and the loader
# accepted the (empty) hook table and entered the game.
#   BB_GAME_DIR=<game> bash tools/macos/game_lib_smoke.sh            # the no-op library
#   BB_GAME_LIB=<lib.dylib> BB_GAME_DIR=<game> bash tools/macos/game_lib_smoke.sh   # any library
# Needs out/bb-probe (tools/macos/build.sh). Skips itself when the game is running.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
if pgrep -x bb-probe >/dev/null; then
    echo "game_lib_smoke: the game is running (pgrep -x bb-probe); not starting" >&2
    exit 9
fi
[[ -x out/bb-probe ]] || { echo "STOP: out/bb-probe missing; run tools/macos/build.sh" >&2; exit 1; }
if [[ -z ${BB_GAME_LIB:-} ]]; then
    bash examples/game_lib/build.sh
    export BB_GAME_LIB=$PWD/out/game_lib/libbbgame-noop.dylib BBGAME_NOOP_SELFTEST=1
    noop=1
else
    noop=0
fi
mkdir -p out
log=out/game_lib_smoke.log
BB_NATIVE_GPU=${BB_NATIVE_GPU:-1} BB_WARM_ONLY=1 bash tools/macos/run.sh >"$log" 2>&1 || true
fail=0
need() { grep -qF -- "$1" "$log" && echo "ok: $1" || { echo "MISSING: $1"; fail=1; }; }
if [[ $noop == 1 ]]; then
    need "bbgame-noop: host interface v1"
    need "short function -> -1, outside the image -> -1"
fi
need "Game library $BB_GAME_LIB: "
grep -qE "^Game library .*: [0-9]+ hooks installed, init returned 0$" "$log" &&
    echo "ok: $(grep -E '^Game library ' "$log")" || { echo "MISSING: init returned 0"; fail=1; }
need "Entering original x86-64 code"
need "Warm run: "
if grep -E "^ERROR:|^STOP:" "$log"; then fail=1; fi
echo "log: $log"
[[ $fail == 0 ]] && echo "game_lib_smoke: PASS" || { echo "game_lib_smoke: FAIL" >&2; exit 1; }
