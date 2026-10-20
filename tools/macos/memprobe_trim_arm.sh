#!/usr/bin/env bash
# tools/macos/memprobe_trim_arm.sh TAG [ENV=VALUE ...]: one bounded hidden arm of the
# BB_KK_TRIM_POOL measurement.
#
# Runs the same warm route the owner's own runs use (title -> Continue -> walk/turn loop) for
# BB_ARM_SECS seconds, hidden (BB_WARM_ONLY=1 is set inside warm_route.sh and is not overridable),
# at his settings from the sandbox bbport.ini, with the 3 s sampler watching and the two tripwires
# this machine needs: footprint over BB_WATCH_FOOTPRINT_MIB (default 19456 = 19 GiB — bb-gpu is
# structurally ~15 GiB here, so a 9 GiB tripwire kills healthy runs during an area load) or
# free+inactive under 2 GiB.
#
# Everything written goes to out/memprobe-trim/<tag>.*: the sampler CSV, the run log, the watchdog
# log, and a meta file. The owner's ~/Library/Application Support/bbport/bbport.ini is never
# touched: BB_DATA_DIR is the sandbox.
#
# bash, not sh: the background shells on this machine are zsh, where an unmatched glob aborts the
# line and `for x in $VAR` does not word-split.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."

tag=${1:?usage: memprobe_trim_arm.sh TAG [ENV=VALUE ...]}
shift
secs=${BB_ARM_SECS:-420}
out=out/memprobe-trim
mkdir -p "$out"

sandbox=$PWD/out/memprobe-data
warm_user=$PWD/out/warm_user
log=$PWD/out/last-run.log

free_mib() {
    vm_stat | awk '
        /page size of/ { ps = $8 }
        /^Pages free/ { gsub(/\./, "", $3); free = $3 }
        /^Pages inactive/ { gsub(/\./, "", $3); inactive = $3 }
        END { printf "%.1f", (free + inactive) * ps / 1048576 / 1024 }'
}

: "${BB_GAME_DIR:=$HOME/Games/Bloodborne/CUSA03173}"
export BB_GAME_DIR
free_before=$(free_mib)
{
    printf 'arm:       %s\n' "$tag"
    printf 'run start: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')"
    printf 'sandbox:   %s\n' "$sandbox"
    printf 'watchdog:  footprint > %s MiB or free+inactive < %s MiB\n' \
        "${BB_WATCH_FOOTPRINT_MIB:-19456}" "${BB_WATCH_FREE_MIB:-2048}"
    printf 'gate:      BB_KK_TRIM_POOL=%s\n' "${BB_KK_TRIM_POOL:-<unset>}"
    printf 'extra env: %s\n' "$*"
    printf 'free+inactive before: %s GiB\n' "$free_before"
} | tee "$out/$tag-meta.txt"

csv=$out/$tag-samples.csv
watchdog_log=$out/$tag-watchdog.log

env \
    BB_NATIVE_GPU=1 \
    BB_KK_STATS=1 \
    BB_VRAM_LIMIT_MB=6144 \
    BB_PROBE_CYCLE=0 \
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
# The sampler stops on its own when the run's pid goes away; give it a beat, then make sure.
for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$sampler" 2>/dev/null || break
    sleep 1
done
kill -0 "$sampler" 2>/dev/null && kill "$sampler" 2>/dev/null

cp -f "$log" "$out/$tag.log" 2>/dev/null || true
free_after=$(free_mib)
{
    printf 'run end: %s status %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$status"
    printf 'free+inactive after: %s GiB\n' "$free_after"
} | tee -a "$out/$tag-meta.txt"

echo "--- $tag: game evidence from the run's own log"
grep -oE '[0-9]+ draws/frame' "$out/$tag.log" 2>/dev/null | awk '{print $1}' |
    sort -n | awk 'END { if (NR) printf "draws/frame max %s over %s samples\n", $1, NR }'
grep -E '^Frame stats' "$out/$tag.log" 2>/dev/null | tail -2
echo "--- $tag: KosmicKrisp counters, first/middle/last"
grep -h 'command allocators' "$out/$tag.log" 2>/dev/null |
    awk '{ n++; line[n] = $0 } END {
        if (!n) { print "none"; exit }
        print "n=" n
        print "first: " line[1]
        print "mid:   " line[int(n/2)+1]
        print "last:  " line[n]
    }'
echo "--- $tag: trim lines"
grep -h 'PROBE TRIM' "$out/$tag.log" 2>/dev/null | tail -5
printf '%s\n' "$(grep -c 'PROBE TRIM' "$out/$tag.log" 2>/dev/null || echo 0)" >"$out/$tag-trimcount.txt"
echo "trim lines: $(cat "$out/$tag-trimcount.txt")"
echo "--- $tag: footprint series (sampler)"
awk -F, 'NR > 1 && $4 != "-" { fp[++n] = $4; if ($4 + 0 > m) m = $4 + 0 } END {
    if (!n) { print "no samples"; exit }
    printf "n=%d first=%s mid=%s last=%s max=%d MiB\n", n, fp[1], fp[int(n/2)+1], fp[n], m
}' "$csv" 2>/dev/null
