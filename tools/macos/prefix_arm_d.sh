#!/usr/bin/env bash
# Arm D: 6 runs on the PRE-FIX build with BB_SUBMIT_STALL_MS set, so bb-gpu's reader thread goes
# away for 200 ms out of every 500 ms between messages while the guest keeps producing submissions.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
export BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173
export BB_WARM_SOURCE_USER=$HOME/repos/bbport-mac-native/user
export BB_SUBMIT_CHECK=0
export BB_COPY_GPU_BUFFERS=1
export PM4_SECS=${PM4_SECS:-180}
export BB_SUBMIT_STALL_MS=${BB_SUBMIT_STALL_MS:-200}

echo "$(date '+%H:%M:%S') === arm D: 2 rounds x attack,combat,interact, stall=${BB_SUBMIT_STALL_MS}ms/500ms ==="
PM4_ROUTES=attack,combat,interact bash tools/macos/pm4_repro.sh prefix-stall 2 2>&1 | tee -a out/pm4_prefix/series-d.log
echo "$(date '+%H:%M:%S') === arm D done ==="
