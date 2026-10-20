#!/usr/bin/env bash
# tools/macos/memprobe_arm.sh TAG [ENV=VALUE ...]: one bounded hidden arm of a memory-lever
# measurement, at the owner's real settings.
#
# This is memprobe_trim_arm.sh without the probe runs' BB_VRAM_LIMIT_MB=6144: these arms measure
# the configuration the owner actually plays, where the renderer's own share is not held down by a
# 6 GiB cap, so the budget formula is the thing under test. Nothing else about the run differs:
# the same warm route (title -> Continue -> walk/turn loop), hidden (BB_WARM_ONLY=1 is set inside
# warm_route.sh), the sandbox bbport.ini, a 3 s sampler, and the two tripwires this machine needs
# (footprint over BB_WATCH_FOOTPRINT_MIB, default 19456 = 19 GiB — bb-gpu is structurally ~15 GiB
# here, so a 9 GiB tripwire kills healthy runs during an area load — or free+inactive under 2 GiB).
#
# Everything written goes to out/memprobe-arms/<tag>.*: sampler CSV, run log, watchdog log, meta,
# and the metrics JSON the report script writes. ~/Library/Application Support/bbport/bbport.ini is
# never touched: BB_DATA_DIR is the sandbox.
#
# bash, not sh: the background shells on this machine are zsh, where an unmatched glob aborts the
# line and `for x in $VAR` does not word-split.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."

tag=${1:?usage: memprobe_arm.sh TAG [ENV=VALUE ...]}
shift
secs=${BB_ARM_SECS:-420}
out=out/memprobe-arms
mkdir -p "$out"

sandbox=$PWD/out/memprobe-data
warm_user=$PWD/out/warm_user
log=$PWD/out/last-run.log

free_gib() {
    vm_stat | awk '
        /page size of/ { ps = $8 }
        /^Pages free/ { gsub(/\./, "", $3); free = $3 }
        /^Pages inactive/ { gsub(/\./, "", $3); inactive = $3 }
        END { printf "%.1f", (free + inactive) * ps / 1048576 / 1024 }'
}

: "${BB_GAME_DIR:=$HOME/Games/Bloodborne/CUSA03173}"
export BB_GAME_DIR
free_before=$(free_gib)
{
    printf 'arm:       %s\n' "$tag"
    printf 'run start: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')"
    printf 'sandbox:   %s\n' "$sandbox"
    printf 'seconds:   %s\n' "$secs"
    printf 'watchdog:  footprint > %s MiB or free+inactive < %s MiB\n' \
        "${BB_WATCH_FOOTPRINT_MIB:-19456}" "${BB_WATCH_FREE_MIB:-2048}"
    printf 'gates:     BB_KK_TRIM_POOL=%s BB_KK_TRIM_LOADONLY=%s BB_MEM_BUDGET_RESERVE=%s\n' \
        "${BB_KK_TRIM_POOL:-<unset>}" "${BB_KK_TRIM_LOADONLY:-<unset>}" \
        "${BB_MEM_BUDGET_RESERVE:-<unset>}"
    printf 'vram cap:  BB_VRAM_LIMIT_MB=%s (must be unset in this batch)\n' \
        "${BB_VRAM_LIMIT_MB:-<unset>}"
    printf 'extra env: %s\n' "$*"
    printf 'free+inactive before: %s GiB\n' "$free_before"
} | tee "$out/$tag-meta.txt"

csv=$out/$tag-samples.csv
watchdog_log=$out/$tag-watchdog.log

env \
    BB_NATIVE_GPU=1 \
    BB_KK_STATS=1 \
    BB_DATA_DIR="$sandbox" \
    BB_WARM_USER="$warm_user" \
    BB_WARM_SOURCE_USER="$warm_user" \
    "$@" \
    bash tools/macos/warm_route.sh "$secs" >"$out/$tag-warmroute.log" 2>&1 </dev/null &
run=$!
printf 'warm_route.sh pid %s\n' "$run" | tee -a "$out/$tag-meta.txt"

BB_WATCH_FOOTPRINT_MIB=${BB_WATCH_FOOTPRINT_MIB:-19456} \
    BB_WATCH_FREE_MIB=${BB_WATCH_FREE_MIB:-2048} \
    BB_WATCH_DUMP_AT=${BB_WATCH_DUMP_AT:-360} \
    bash tools/macos/memprobe_watch.sh "$csv" "$run" 3 2>"$watchdog_log" &
sampler=$!

wait "$run"
status=$?
for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$sampler" 2>/dev/null || break
    sleep 1
done
kill -0 "$sampler" 2>/dev/null && kill "$sampler" 2>/dev/null

cp -f "$log" "$out/$tag.log" 2>/dev/null || true
free_after=$(free_gib)
{
    printf 'run end: %s status %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$status"
    printf 'free+inactive after: %s GiB\n' "$free_after"
} | tee -a "$out/$tag-meta.txt"

python3 tools/macos/memprobe_arm_report.py "$tag"
