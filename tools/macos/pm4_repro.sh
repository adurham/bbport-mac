#!/usr/bin/env bash
# tools/macos/pm4_repro.sh LABEL ROUNDS [ENV=VALUE ...]: the PM4 "type 1 packet" repro loop
# (docs/PM4_TORN_SUBMISSIONS.md). Runs the combat-heavy routes (attack, combat, interact, street)
# hidden, ROUNDS times each in turn, through tools/macos/warm_route.sh, and keeps every run's log
# in out/pm4_repro/LABEL/; a summary line per run goes to out/pm4_repro/LABEL/summary.txt.
#
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/pm4_repro.sh before 4 BB_SUBMIT_ASYNC=1
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/pm4_repro.sh after 5
#
# The run's sandbox (out/pm4_repro/user) is a clone of BB_WARM_SOURCE_USER (a user dir with a save
# past the title) made once. BB_SUBMIT_CHECK=1 is on for every run: each submission's words are
# hashed when the game submits them and again when bb-gpu takes them ("Submit check:" lines).
# A run is never started while another bb-probe runs (the owner may be playing): it waits.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
label=${1:?usage: pm4_repro.sh LABEL ROUNDS [ENV=VALUE ...]}
rounds=${2:?usage: pm4_repro.sh LABEL ROUNDS [ENV=VALUE ...]}
shift 2
for kv in "$@"; do export "${kv?}"; done
: "${BB_GAME_DIR:?set BB_GAME_DIR}"
export BB_SUBMIT_CHECK=${BB_SUBMIT_CHECK:-1}
dir=$PWD/out/pm4_repro/$label
mkdir -p "$dir"
export BB_WARM_USER=$PWD/out/pm4_repro/user
export BB_WARM_SOURCE_USER=${BB_WARM_SOURCE_USER:?set BB_WARM_SOURCE_USER to a user dir with a save}
# PM4_COLD=1: every run starts from the save with no pipeline cache, so it compiles hundreds of
# pipelines while it plays (the fills that crashed did: the GPU process is loaded, the window
# between a submission and bb-gpu taking it is wider).
if [[ ${PM4_COLD:-0} == 1 ]]; then
    cold=$PWD/out/pm4_repro/cold_source
    if [[ ! -d $cold ]]; then
        cp -cR "$BB_WARM_SOURCE_USER" "$cold" 2>/dev/null || cp -aR "$BB_WARM_SOURCE_USER" "$cold"
        rm -rf "$cold/cache"
    fi
    export BB_WARM_SOURCE_USER=$cold RESET_WARM_USER=1
fi

# name|secs|pad (warmfill.sh's built-in routes; street is warmpack's).
routes='attack|300|r1:2 r1:1 r1:1 circle:1.5 ly=0,rx=128:2 r2:2 l1:2 r1:2
combat|360|ly=0,rx=128:4 r1:1 r1:1 r1:1 circle:1.2 ly=0,rx=128:4 r1:1 r2:1.2 l1:1 l1:1 r1:1 ly=0,rx=128:3 r1:1 r1:1 circle:1.2 l2:1 ly=0,rx=128:5 r1:1 r1:1 l1:1 r2:1.2
interact|180|ly=0,rx=128:3 cross:1 ly=0,rx=128:3 cross:1 ly=0,rx=128:4 cross:1 circle:1 rx=60:0.6 rx=128:0.25 ly=0,rx=128:3 cross:1
street|200|ly=0,rx=128:20 rx=60:1.2 rx=128:0.4 ly=0,rx=128:20 rx=60:1.2 rx=128:0.4'
if [[ -n ${PM4_ROUTES:-} ]]; then
    routes=$(printf '%s\n' "$routes" | grep -E "^(${PM4_ROUTES//,/|})\|")
fi

for ((round = 1; round <= rounds; round++)); do
    while IFS='|' read -r name secs pad; do
        while pgrep -x bb-probe >/dev/null; do
            echo "$(date '+%H:%M:%S') another bb-probe is running; waiting"
            sleep 30
        done
        secs=${PM4_SECS:-$secs}
        tag=$name-$round
        echo "$(date '+%H:%M:%S') run $label/$tag (${secs}s)"
        ROUTE=$pad bash tools/macos/warm_route.sh "$secs" >"$dir/$tag.out" 2>&1 || true
        cp out/last-run.log "$dir/$tag.log" 2>/dev/null || true
        max=$(grep -oE '[0-9]+ draws/frame' "$dir/$tag.log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
        bad=$(grep -c 'Bad PM4 packet' "$dir/$tag.log" 2>/dev/null || true)
        stop=$(grep -c 'assert_fail\|GPU library assertion failed\|Unreachable code' "$dir/$tag.log" 2>/dev/null || true)
        compiles=$(grep -oE 'Warm run: [0-9]+ pipelines created' "$dir/$tag.log" 2>/dev/null | tail -1 | awk '{print $3}')
        changed=$(grep -c '^Submit check: submission' "$dir/$tag.log" 2>/dev/null || true)
        line="$label $tag max_draws=${max:-0} bad_pm4=${bad:-0} asserts=${stop:-0} changed_submissions=${changed:-0} created=${compiles:-?}"
        echo "$line" | tee -a "$dir/summary.txt"
    done <<<"$routes"
done
