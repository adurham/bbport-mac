#!/usr/bin/env bash
# tools/macos/pm4_phase0.sh: the positive control of the 2026-10-19 campaign
# (docs/PM4_PREREG_2026-10-19.md): the OLD submit path (BB_SUBMIT_ASYNC=1) at
# BB_SUBMIT_DELAY_US = 0, 500, 5000, 50000, three hidden runs each, and one JSONL line per run.
#
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 \
#   BB_WARM_SOURCE_USER=~/repos/bbport-mac-native/user bash tools/macos/pm4_phase0.sh
#
# Runs that never reach the title's Continue and CSClothThread0-5 are recorded INVALID, not as
# zeros. It waits (never kills) while another bb-probe runs.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
: "${BB_GAME_DIR:?set BB_GAME_DIR}"
: "${BB_WARM_SOURCE_USER:?set BB_WARM_SOURCE_USER to a user dir with a save past the title}"
export BB_GAME_DIR BB_WARM_SOURCE_USER
jsonl=${PM4_JSONL:-docs/pm4-results/$(date +%Y-%m-%d).jsonl}
secs=${PM4_SECS:-240}
route=${PM4_ROUTE:-attack}
mkdir -p docs/pm4-results out/pm4_repro
echo "$(date '+%H:%M:%S') phase 0: delays 0 500 5000 50000 x3 runs, ${secs}s each, route $route"

for delay in ${PM4_DELAYS:-0 500 5000 50000}; do
    for run in 1 2 3; do
        label=phase0-d${delay}-r${run}
        echo "$(date '+%H:%M:%S') $label (old path, +${delay} us)"
        PM4_ROUTES=$route PM4_SECS=$secs \
            bash tools/macos/pm4_repro.sh "$label" 1 \
                BB_SUBMIT_ASYNC=1 BB_SUBMIT_DELAY_US="$delay" \
                >>"out/pm4_repro/phase0.log" 2>&1
        python3 tools/macos/pm4_record.py --label "$label" --arm "p0-old-d${delay}" \
            --run "$run" --route "$route" --delay "$delay" --async --secs "$secs" \
            --log "out/pm4_repro/$label/${route}-1.log" --jsonl "$jsonl" \
            >>"out/pm4_repro/phase0.log" 2>&1
        tail -1 "$jsonl"
    done
done
echo "$(date '+%H:%M:%S') phase 0 done: $(grep -c '^{' "$jsonl" 2>/dev/null || echo 0) runs in $jsonl"
