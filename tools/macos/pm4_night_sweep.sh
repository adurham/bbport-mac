#!/usr/bin/env bash
# tools/macos/pm4_night_sweep.sh: Phase 1 of the 2026-10-19 campaign (docs/PM4_PREREG_2026-10-19.md).
# Blocks of four hidden runs in randomized order - old / old+D* / fixed / fixed+D* - appended to the
# run record, so machine load and drift over the night hit all four arms alike.
#
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 BB_WARM_SOURCE_USER=~/repos/bbport-mac-native/user \
#   DSTAR=500 PM4_BLOCKS=25 bash tools/macos/pm4_night_sweep.sh
#
# 25 blocks = 100 runs (~8.5 h at PM4_SECS=240). It waits (never kills) while any bb-probe runs,
# never shows a window (pm4_repro.sh -> warm_route.sh sets BB_WARM_ONLY=1), and ends by appending
# 'SWEEPDONE <n> runs' to the JSONL. It does not restart itself: run it again by hand if needed.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
: "${BB_GAME_DIR:?set BB_GAME_DIR}"
: "${BB_WARM_SOURCE_USER:?set BB_WARM_SOURCE_USER to a user dir with a save past the title}"
: "${DSTAR:?set DSTAR to the delay Phase 0 picked (BB_SUBMIT_DELAY_US)}"
[[ $DSTAR =~ ^[0-9]+$ ]] || { echo "DSTAR must be a number of microseconds" >&2; exit 2; }
export BB_GAME_DIR BB_WARM_SOURCE_USER BB_WARM_ONLY=1
blocks=${PM4_BLOCKS:-25}
secs=${PM4_SECS:-240}
route=${PM4_ROUTE:-attack}
jsonl=${PM4_JSONL:-docs/pm4-results/$(date +%Y-%m-%d).jsonl}
log=out/pm4_repro/sweep.log
mkdir -p docs/pm4-results out/pm4_repro
runs=0

echo "$(date '+%H:%M:%S') sweep: $blocks blocks x 4 arms, ${secs}s, route $route, D*=$DSTAR us" | tee -a "$log"
echo "$(date '+%H:%M:%S') sweep: record $jsonl" | tee -a "$log"

for ((block = 1; block <= blocks; block++)); do
    # Fisher-Yates: the four arms of this block in a random order.
    arms=(old old-delay fixed fixed-delay)
    for ((i = 3; i > 0; i--)); do
        j=$((RANDOM % (i + 1)))
        tmp=${arms[i]}
        arms[i]=${arms[j]}
        arms[j]=$tmp
    done
    for arm in "${arms[@]}"; do
        case $arm in
            old)        label=$(printf 's-b%02d-old' "$block");     extra=(BB_SUBMIT_ASYNC=1); delay=0; async=--async ;;
            old-delay)  label=$(printf 's-b%02d-old-d' "$block");   extra=(BB_SUBMIT_ASYNC=1 BB_SUBMIT_DELAY_US="$DSTAR"); delay=$DSTAR; async=--async ;;
            fixed)      label=$(printf 's-b%02d-fixed' "$block");   extra=(); delay=0; async= ;;
            fixed-delay) label=$(printf 's-b%02d-fixed-d' "$block"); extra=(BB_SUBMIT_DELAY_US="$DSTAR"); delay=$DSTAR; async= ;;
        esac
        # Never start while the owner (or a leftover run) has a bb-probe up.
        while pgrep -x bb-probe >/dev/null; do
            echo "$(date '+%H:%M:%S') another bb-probe is running; waiting" >>"$log"
            sleep 30
        done
        echo "$(date '+%H:%M:%S') block $block/$blocks: $label (${extra[*]:-defaults})" | tee -a "$log"
        PM4_ROUTES=$route PM4_SECS=$secs \
            bash tools/macos/pm4_repro.sh "$label" 1 ${extra[@]+"${extra[@]}"} >>"$log" 2>&1
        python3 tools/macos/pm4_record.py --label "$label" --arm "sweep-$arm" \
            --run "$block" --route "$route" --delay "$delay" $async --secs "$secs" \
            --log "out/pm4_repro/$label/${route}-1.log" --jsonl "$jsonl" >>"$log" 2>&1
        runs=$((runs + 1))
        tail -1 "$jsonl" | cut -c1-160 | tee -a "$log"
    done
    echo "$(date '+%H:%M:%S') block $block done: $runs runs so far" | tee -a "$log"
done

echo "SWEEPDONE $runs runs" | tee -a "$log" >>"$jsonl"
echo "$(date '+%H:%M:%S') sweep done: $runs runs" | tee -a "$log"
