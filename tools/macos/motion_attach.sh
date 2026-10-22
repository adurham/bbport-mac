#!/usr/bin/env bash
# tools/macos/motion_attach.sh TAG [TAG ...]: hidden standing runs pricing the object-motion
# attachment (bench/motion-attachment). Same route and settings as vertex_ceiling.sh /
# motion_fast_ab.sh (fsr3 preset 4, 3440x1440, frame_generation=0, sandbox user dir
# out/curve3440/user, no BB_VRAM_LIMIT_MB). Every run: Metal System Trace (TRACE_S s, TRACE_AFTER s
# into gameplay) with BB_PASS_TRACE armed on the same frames (lines carry mv=/ms= motion draws),
# and a BB_DUMP_TRIGGER dump (motion / objects / depth on the scaled path) DUMP_AT s into gameplay.
#   ctrl-N   : gate off
#   load-N   : BB_MOTION_ATTACH_PROBE=load  (no reload of the motion attachment; timing only)
#   store-N  : BB_MOTION_ATTACH_PROBE=store (no store of it; timing only)
#   both-N   : BB_MOTION_ATTACH_PROBE=both
#   motoff-N : BB_OBJECT_MOTION=0 (ceiling)
#   gate-N   : BB_MOTION_ATTACH=1 (TASK 3 gate, only if built)
# Outputs: out/frames-TAG.csv, out/run-TAG.log, out/mtl-TAG-gpu.xml, out/pass-TAG.txt,
# out/dump-TAG/, out/mattach/{attempts.jsonl,series.log,wrapper-*,foot-*}.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
[[ "$(basename "$here")" == bbport-motionattach-build ]] || { echo "STOP: not the motion-attachment checkout: $here" >&2; exit 1; }
res=$here/out/mattach
mkdir -p "$res"
exec >>"$res/series.log" 2>&1
secs=${SECS:-260}
echo "motion_attach: checkout $here @ $(git rev-parse --short HEAD)$(git diff --quiet || echo +dirty) ($(date +%T)) secs=$secs"
grep -q '^frame_generation=0' "$here/bbport.ini" || { echo "STOP: checkout ini lacks frame_generation=0"; exit 1; }

watchdog() {
    local tag=$1 page free inactive mib foot pids cl
    while kill -0 "$route" 2>/dev/null; do
        page=$(vm_stat | awk '/page size of/{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/){print $i; exit}}')
        free=$(vm_stat | awk '/Pages free:/{v=$3; gsub(/\./,"",v); print v}')
        inactive=$(vm_stat | awk '/Pages inactive:/{v=$3; gsub(/\./,"",v); print v}')
        mib=$(( (free + inactive) * page / 1048576 ))
        foot=0; pids=$(pgrep -x bb-gpu 2>/dev/null || true)
        for p in $pids; do
            cl=$(ps -o command= -p "$p" 2>/dev/null)
            case "$cl" in *"$here"*)
                foot=$(/usr/bin/footprint -p "$p" 2>/dev/null | awk '/Footprint:/{for(i=1;i<=NF;i++) if($i=="Footprint:"){v=$(i+1); u=$(i+2)}; if(u~/GB/){v*=1024} else if(u~/KB/){v/=1024} printf "%.0f", v; exit}')
                break ;;
            esac
        done
        echo "$(date +%s) foot_mib=${foot:-0} freeinactive_mib=$mib" >> "$res/foot-$tag.txt"
        if [[ ${foot:-0} -gt 19456 || ( $mib -lt 4096 && -n $pids ) ]]; then
            echo "$(date) WATCHDOG: foot=${foot} MiB free+inactive=${mib} MiB; killing" >> "$res/foot-$tag.warnings"
            for g in $(pgrep -x bb-gpu) $(pgrep -x bb-probe); do
                ps -o command= -p "$g" | grep -q "$here" && kill "$g" 2>/dev/null
            done
        fi
        sleep 10
    done
}

# The pass trace's window covers the same frames as the Metal trace.
trigger_watch() {
    local trig=$1 tdir=$here/out/mtl-$tag.trace
    while kill -0 "$route" 2>/dev/null; do
        if [[ -d $tdir ]] || pgrep -f "output out/mtl-$tag.trace" >/dev/null 2>&1; then
            : > "$trig"
            echo "motion_attach: pass-trace trigger touched $(date +%T)" >> "$res/wrapper-$tag-$attempt.txt"
            return
        fi
        sleep 0.2
    done
}

# The dump trigger: DUMP_AT s after the first 'Frame stats' line with >= 500 draws/frame.
dump_watch() {
    local trig=$1 start=""
    while kill -0 "$route" 2>/dev/null; do
        if [[ -z $start ]] && grep '^Frame stats' "$here/out/last-run.log" 2>/dev/null |
                grep -qE '([5-9][0-9]{2}|[0-9]{4,}) draws/frame'; then
            start=$(date +%s)
        fi
        if [[ -n $start && $(( $(date +%s) - start )) -ge ${DUMP_AT:-120} ]]; then
            : > "$trig"
            echo "motion_attach: dump trigger touched $(date +%T)" >> "$res/wrapper-$tag-$attempt.txt"
            return
        fi
        sleep 1
    done
}

for tag in "$@"; do
    arm=()
    case $tag in
        ctrl-*)   arm=() ;;
        load-*)   arm=(BB_MOTION_ATTACH_PROBE=load) ;;
        store-*)  arm=(BB_MOTION_ATTACH_PROBE=store) ;;
        both-*)   arm=(BB_MOTION_ATTACH_PROBE=both) ;;
        motoff-*) arm=(BB_OBJECT_MOTION=0) ;;
        gate-*)   arm=(BB_MOTION_ATTACH=1) ;;
        *) echo "unknown tag $tag"; continue ;;
    esac
    [[ -f $res/$tag.done ]] && { echo "$tag done - skip"; continue; }
    trace=(TRACE=1 TRACE_SIGINT_S=${TRACE_S:-3} TRACE_AFTER=${TRACE_AFTER:-200})
    for attempt in 1 2 3; do
        while pgrep -x bb-probe >/dev/null; do echo "$(date +%T) foreign bb-probe up; waiting"; sleep 10; done
        ptrigger=$here/out/passtrace_trigger_mattach
        dtrig=$here/out/dump_trigger_mattach
        rm -f "$ptrigger" "$dtrig" "$here/out/pass-$tag.txt"
        rm -rf "$here/out/dump-$tag"; mkdir -p "$here/out/dump-$tag"
        echo "motion_attach: ---- $tag attempt $attempt ($(date +%T)) arm=${arm[*]-off}"
        env "${trace[@]}" \
            BB_PASS_TRACE="$here/out/pass-$tag.txt" BB_PASS_TRACE_TRIGGER="$ptrigger" \
            BB_PASS_TRACE_FRAMES="${PASS_FRAMES:-400}" \
            BB_DUMP_TRIGGER="$dtrig" BB_DUMP_DIR="$here/out/dump-$tag" BB_DUMP_FRAMES=${DUMP_FRAMES:-2} \
            BB_WARM_USER="$here/out/curve3440/user" ROUTE='ly=128,rx=128:5' \
            BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_OUTPUT_RES=3440x1440 \
            ${arm[@]+"${arm[@]}"} \
            bash tools/macos/gpu_ab_run.sh "$tag" "$secs" > "$res/wrapper-$tag-$attempt.txt" 2>&1 &
        route=$!
        watchdog "$tag" & wd=$!
        trigger_watch "$ptrigger" & tw=$!
        dump_watch "$dtrig" & dw=$!
        wait $route; kill $wd $tw $dw 2>/dev/null; wait $wd $tw $dw 2>/dev/null
        log=$here/out/run-$tag.log
        cp -f "$log" "$res/run-$tag-$attempt.log" 2>/dev/null
        gcn=$(grep -c 'gcn_to_vk.cpp:422' "$log" 2>/dev/null || true); gcn=${gcn:-0}
        pm4=$(grep -cE 'cp_graphics.cpp:203|Bad PM4' "$log" 2>/dev/null || true); pm4=${pm4:-0}
        asserts=$(grep -E 'gcn_to_vk|Bad PM4|cp_graphics|Unreachable code|died mid-run|Assertion|ASSERT|ended by signal' "$log" 2>/dev/null | head -1 | tr -d '"\\')
        maxd=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
        ov=$(wc -l < "$here/out/overlap-$tag.txt" 2>/dev/null | tr -d ' ')
        ptl=$(grep -c '^PT ' "$here/out/pass-$tag.txt" 2>/dev/null || true); ptl=${ptl:-0}
        wd_kill=0; [[ -s $res/foot-$tag.warnings ]] && wd_kill=1
        xml=0; [[ -s $here/out/mtl-$tag-gpu.xml ]] && xml=1
        probe=$(grep -c '^Motion attach' "$log" 2>/dev/null || true); probe=${probe:-0}
        nulls=$(grep -c 'Null fetch shader:' "$log" 2>/dev/null || true); nulls=${nulls:-0}
        dumps=$(ls "$here/out/dump-$tag" 2>/dev/null | wc -l | tr -d ' ')
        echo "{\"tag\":\"$tag\",\"attempt\":$attempt,\"max_draws\":${maxd:-0},\"overlap\":${ov:-0},\"pass_lines\":$ptl,\"gcn422\":$gcn,\"pm4_203\":$pm4,\"watchdog\":$wd_kill,\"xml\":$xml,\"probe_lines\":$probe,\"null_fetch\":$nulls,\"dumps\":$dumps,\"assert\":\"$asserts\"}" >> "$res/attempts.jsonl"
        if [[ -z $asserts && ${maxd:-0} -ge 500 && ${ov:-0} -eq 0 && $wd_kill -eq 0 && $xml -eq 1 && $ptl -ge 1000 ]]; then
            echo ok > "$res/$tag.done"
            echo "motion_attach: $tag OK (draws $maxd, pass lines $ptl, probe lines $probe, dumps $dumps)"
            break
        fi
        echo "motion_attach: $tag attempt $attempt failed (draws=${maxd:-0} overlap=${ov:-0} watchdog=$wd_kill xml=$xml pass_lines=$ptl assert=$asserts)"
        rm -f "$res/foot-$tag.warnings"
    done
done
echo "motion_attach: ALLDONE ($(date +%T))"
