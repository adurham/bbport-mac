#!/usr/bin/env bash
# Arm B: the same runs on the FIXED build (bbport-pm4-build, fix/pm4-crash at 1a773b6), binaries
# already built, hash instrumentation off, copy path on so the wait is actually in play.
set -uo pipefail
cd -- "$HOME/repos/bbport-pm4-build"
export BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173
export BB_WARM_SOURCE_USER=$HOME/repos/bbport-mac-native/user
export BB_SUBMIT_CHECK=0
export BB_COPY_GPU_BUFFERS=1
export PM4_SECS=${PM4_SECS:-180}

echo "$(date '+%H:%M:%S') === arm B series: 3 rounds x 4 routes ==="
bash tools/macos/pm4_repro.sh prefix-fix 3 2>&1 | tee -a "$HOME/repos/bbport-prefix-build/out/pm4_prefix/series-b.log"
echo "$(date '+%H:%M:%S') === arm B supplement: attack, combat, interact ==="
PM4_ROUTES=attack,combat,interact bash tools/macos/pm4_repro.sh prefix-fix2 1 2>&1 | tee -a "$HOME/repos/bbport-prefix-build/out/pm4_prefix/series-b.log"
echo "$(date '+%H:%M:%S') === arm B done ==="
