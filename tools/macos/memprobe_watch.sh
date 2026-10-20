#!/usr/bin/env bash
# tools/macos/memprobe_watch.sh: the watchdog and sampler for a BB_PROBE_CYCLE run.
#
# A Bloodborne port run has four times in three days pushed this Mac into jetsam, twice taking
# WindowServer and every GUI app with it (docs/DISPLAY_TRANSITION_RULE_2026-10-19.md). So every run
# of the memory probe is bounded by this script, not by hope:
#
#   - samples every 3 s: bb-gpu's pid, `ps -o rss`, `footprint -p` (phys_footprint, the metric
#     jetsam and this watchdog actually act on — RSS is not it) and vm_stat's free+inactive pages;
#   - SIGTERMs bb-gpu at once if its footprint passes BB_WATCH_FOOTPRINT_MIB (default 9216 = 9 GiB)
#     or if the Mac's free+inactive memory falls under BB_WATCH_FREE_MIB (default 2048 = 2 GiB),
#     then stops the run;
#   - writes one CSV line per sample, so the numbers survive whatever the run does.
#
# It is bash, not sh: the background shells on this machine are zsh, where an unmatched glob aborts
# the line and `for x in $VAR` does not word-split.
#
#   bash tools/macos/memprobe_watch.sh <csv> <run-pid> [secs]
#
# <run-pid> is the process whose whole tree is the run (warm_route.sh): when it is gone, the
# sampler stops. The watchdog does not start a run and does not decide what the run does.
set -uo pipefail

csv=${1:?usage: memprobe_watch.sh CSV RUN_PID [SECS]}
run_pid=${2:?usage: memprobe_watch.sh CSV RUN_PID [SECS]}
secs=${3:-3}
footprint_limit=${BB_WATCH_FOOTPRINT_MIB:-9216}
free_limit=${BB_WATCH_FREE_MIB:-2048}
# Optional one-shot full `footprint -p` dump at this many seconds in: the category table says where
# a footprint that is far above RSS is actually held (mapped pool, dirty pages, the ledger).
dump_at=${BB_WATCH_DUMP_AT:-}

# 1024-based: the thresholds are GiB, and ps/footprint report KiB.
mib() { awk -v b="$1" 'BEGIN { printf "%.0f", b / 1048576 }'; }

page_size=$(vm_stat | sed -n '1s/.*page size of \([0-9]*\) bytes.*/\1/p')
: "${page_size:=16384}"

# MiB of free + inactive: what the machine can hand out without evicting anything in use.
free_inactive_mib() {
    vm_stat | awk -v ps="$page_size" '
        /^Pages free/     { gsub(/\./, "", $3); free = $3 }
        /^Pages inactive/ { gsub(/\./, "", $3); inactive = $3 }
        END { printf "%.0f", (free + inactive) * ps / 1048576 }
    '
}

# phys_footprint, from /usr/bin/footprint's own header:
#   bb-gpu [1234]: 64-bit    Footprint: 2576 KB (16384 bytes per page)
footprint_mib() {
    local pid=$1 out value unit
    out=$(/usr/bin/footprint -p "$pid" 2>/dev/null) || { echo ""; return; }
    value=$(printf '%s\n' "$out" | sed -n 's/.*Footprint: \([0-9.][0-9.]*\) \([KMG]B\).*/\1/p' | head -1)
    unit=$(printf '%s\n' "$out" | sed -n 's/.*Footprint: \([0-9.][0-9.]*\) \([KMG]B\).*/\2/p' | head -1)
    [[ -n $value ]] || { echo ""; return; }
    case $unit in
        KB) mib "$(awk -v v="$value" 'BEGIN { printf "%.0f", v * 1024 }')" ;;
        MB) awk -v v="$value" 'BEGIN { printf "%.0f", v * 1000000 / 1048576 }' ;;
        GB) awk -v v="$value" 'BEGIN { printf "%.0f", v * 1000000000 / 1048576 }' ;;
        *) echo "" ;;
    esac
}

gpu_pid() {
    local pid
    pid=$(pgrep -x bb-gpu 2>/dev/null | head -1)
    [[ -n $pid ]] || pid=$(pgrep -f 'out/gpu-arm64/bb-gpu' 2>/dev/null | head -1)
    printf '%s' "$pid"
}

printf 'elapsed_s,pid,rss_kib,footprint_mib,free_inactive_mib,verdict\n' >"$csv"
start=$SECONDS
status=running
dumped=0
while :; do
    kill -0 "$run_pid" 2>/dev/null || { status=run-ended; break; }
    pid=$(gpu_pid)
    rss=""
    fp=""
    if [[ -n $pid ]]; then
        rss=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
        fp=$(footprint_mib "$pid")
    fi
    av=$(free_inactive_mib)
    printf '%s,%s,%s,%s,%s,%s\n' "$((SECONDS - start))" "${pid:--}" "${rss:--}" "${fp:--}" "$av" \
        "$status" >>"$csv"

    if [[ -n $dump_at && -n $pid && $dumped == 0 ]] && ((SECONDS - start >= dump_at)); then
        /usr/bin/footprint -p "$pid" >"${csv%.csv}-footprint-${dump_at}s.txt" 2>/dev/null
        dumped=1
    fi

    # The two tripwires. Footprint first: it is the number jetsam uses, and the one the owner's
    # 12-16 GiB sessions crossed.
    if [[ -n $fp ]] && ((fp > footprint_limit)); then
        printf 'memprobe: WATCHDOG footprint %s MiB > %s MiB — TERM bb-gpu (pid %s), stopping the run\n' \
            "$fp" "$footprint_limit" "$pid" >&2
        kill -TERM "$pid" 2>/dev/null
        status=wdt-footprint
        printf '%s,%s,%s,%s,%s,%s\n' "$((SECONDS - start))" "$pid" "${rss:--}" "$fp" "$av" "$status" >>"$csv"
        break
    fi
    if ((av < free_limit)); then
        printf 'memprobe: WATCHDOG free+inactive %s MiB < %s MiB — TERM bb-gpu (pid %s), stopping the run\n' \
            "$av" "$free_limit" "${pid:-?}" >&2
        [[ -n $pid ]] && kill -TERM "$pid" 2>/dev/null
        status=wdt-lowmem
        printf '%s,%s,%s,%s,%s,%s\n' "$((SECONDS - start))" "${pid:--}" "${rss:--}" "${fp:--}" "$av" "$status" >>"$csv"
        break
    fi
    sleep "$secs"
done

# The run is over or tripped: nothing of it is left behind. The game wrapper is SIGTERMed, never
# SIGKILLed, so it can still write its own log and quit the guest.
if [[ $status == wdt-* ]]; then
    kill -TERM "$run_pid" 2>/dev/null
fi
printf 'memprobe: sampler stopped (%s), %s samples in %s s\n' "$status" \
    "$(($(wc -l <"$csv") - 1))" "$((SECONDS - start))" >&2
