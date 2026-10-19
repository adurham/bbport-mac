#!/usr/bin/env bash
# Arm A: 15 hidden warm runs on the PRE-FIX build (47fa499), instrumentation off.
# Harness is byte-identical to the sibling worktree's tools/macos/pm4_repro.sh; only the
# engine binaries differ (this worktree's out/gpu-arm64/bb-gpu).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
export BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173
export BB_WARM_SOURCE_USER=$HOME/repos/bbport-mac-native/user
export BB_SUBMIT_CHECK=0   # the pre-fix build has no hash instrumentation; keep the env harmless
export BB_SUBMIT_ASYNC=1   # pre-fix is async by construction; kept so both arms name the same path
# The historical crash's bad buffer was in bb-gpu's own heap (memory type -1): that is the
# BB_COPY_GPU_BUFFERS copy, and the flag defaults to off. Off, bb-gpu decodes guest memory
# directly and the copy race cannot happen at all, so every arm runs with it on.
export BB_COPY_GPU_BUFFERS=1
export PM4_SECS=${PM4_SECS:-180}

echo "$(date '+%H:%M:%S') === arm A series: 3 rounds x 4 routes ==="
bash tools/macos/pm4_repro.sh prefix-ab 3 2>&1 | tee -a out/pm4_prefix/series.log
echo "$(date '+%H:%M:%S') === arm A supplement: attack, combat, interact ==="
PM4_ROUTES=attack,combat,interact bash tools/macos/pm4_repro.sh prefix-ab2 1 2>&1 | tee -a out/pm4_prefix/series.log
echo "$(date '+%H:%M:%S') === arm A done ==="
