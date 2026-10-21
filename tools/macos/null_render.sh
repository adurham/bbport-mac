#!/usr/bin/env bash
# tools/macos/null_render.sh TAG [TAG ...]: hidden standing runs for the BB_NULL_RENDER probe
# (bench/null-render, docs/NULL_RENDER_2026-10-21.md). Route, save and settings = the curve work:
# BB_NATIVE_GPU=1 BB_FRAME_STATS=1 BB_FRAME_LOG (gpu_ab_run.sh), sandbox data (this checkout's own
# out/ user/ bbport.ini: never the app's data dir), no BB_VRAM_LIMIT_MB, fsr3 preset 4,
# frame_generation=0 (checkout ini), standing route, curve3440 save. SECS (420) so the late phase is
# reached. Footprint watchdog: 19 GiB footprint / 4 GiB free+inactive.
#   null-N  : BB_NULL_RENDER=1 (draws + dispatches dropped, blank flips)
#   null2-N : BB_NULL_RENDER=2 (draws dropped, dispatches kept: the fallback if null-N misbehaves)
#   ctrl-N  : gate off (proves inertness: zero "Null render" lines)
#   trace-N : gate off, Metal System Trace of bb-gpu 4 s (SIGINT-finalised) after gameplay+TRACE_AFTER
# SAMPLE=1 also takes a 3 s `sample` of the game process (bb-probe) at gameplay+80 s and in the last
# 75 s: per-thread stacks name GXRenderThread and show whether it runs, sleeps or spins.
# Outputs: out/frames-TAG.csv, out/run-TAG.log, out/null-render/{attempts.jsonl,wrapper-*,foot-*,
# sample-TAG-*.txt}; report: python3 tools/macos/null_render_report.py TAG...
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
[[ "$(basename "$here")" == bbport-nullrender-build ]] || { echo "STOP: not the probe checkout: $here" >&2; exit 1; }
secs=${SECS:-420}
res=$here/out/null-render
mkdir -p "$res"
exec >>"${SERIES_LOG:-$res/series.log}" 2>&1
echo "null_render: checkout $here @ $(git rev-parse --short HEAD) ($(date +%T))"
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

sampler() {  # $1 tag: 3 s `sample` of OUR bb-probe at gameplay+80 s and at secs-60 s of the run
    local tag=$1 start=$SECONDS n g P
    while kill -0 "$route" 2>/dev/null; do
        n=$(grep '^Frame stats' "$here/out/last-run.log" 2>/dev/null | tail -1 | grep -o '[0-9]* draws/frame' | cut -d' ' -f1)
        [[ ${n:-0} -gt 300 ]] && break
        sleep 5
    done
    local gp=$SECONDS
    for at in 80 late; do
        if [[ $at == late ]]; then
            while kill -0 "$route" 2>/dev/null && (( SECONDS - start < secs - 60 )); do sleep 5; done
        else
            while kill -0 "$route" 2>/dev/null && (( SECONDS - gp < 80 )); do sleep 5; done
        fi
        P=""
        for g in $(pgrep -x bb-probe); do
            case "$(ps -o command= -p "$g")" in *"$here"*) P=$g ;; esac
        done
        [[ -n $P ]] || return
        /usr/bin/sample "$P" 3 -mayDie -file "$res/sample-$tag-$at.txt" >/dev/null 2>&1
    done
}

for tag in "$@"; do
    trace=()
    case $tag in
        ctrl-*)  arm=() ;;
        null-*)  arm=(BB_NULL_RENDER=1) ;;
        null2-*) arm=(BB_NULL_RENDER=2) ;;
        trace-*) arm=(); trace=(TRACE=1 TRACE_SIGINT_S=${TRACE_S:-4} TRACE_AFTER=${TRACE_AFTER:-30}) ;;
        *) echo "unknown tag $tag"; continue ;;
    esac
    [[ -f $res/$tag.done ]] && { echo "$tag done - skip"; continue; }
    for attempt in 1 2 3; do
        while pgrep -x bb-probe >/dev/null; do echo "$(date +%T) foreign bb-probe up; waiting"; sleep 10; done
        echo "null_render: ---- $tag attempt $attempt ($(date +%T)) arm=${arm[*]-off}"
        env ${trace[@]+"${trace[@]}"} BB_WARM_USER="$here/out/curve3440/user" ROUTE='ly=128,rx=128:5' \
            BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 ${arm[@]+"${arm[@]}"} \
            bash tools/macos/gpu_ab_run.sh "$tag" "$secs" > "$res/wrapper-$tag-$attempt.txt" 2>&1 &
        route=$!
        watchdog "$tag" & wd=$!
        sp=""
        if [[ ${SAMPLE:-0} == 1 && $tag != trace-* ]]; then sampler "$tag" & sp=$!; fi
        wait $route; kill $wd 2>/dev/null; wait $wd 2>/dev/null
        [[ -n $sp ]] && { kill $sp 2>/dev/null; wait $sp 2>/dev/null; }
        log=$here/out/run-$tag.log
        cp -f "$log" "$res/run-$tag-$attempt.log" 2>/dev/null
        gcn=$(grep -c 'gcn_to_vk.cpp:422' "$log" 2>/dev/null || true); gcn=${gcn:-0}
        pm4=$(grep -cE 'cp_graphics.cpp:203|Bad PM4' "$log" 2>/dev/null || true); pm4=${pm4:-0}
        asserts=$(grep -E 'gcn_to_vk|Bad PM4|cp_graphics|Unreachable code|died mid-run|Assertion|ASSERT|ended by signal' "$log" 2>/dev/null | head -3)
        maxd=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
        ov=$(wc -l < "$here/out/overlap-$tag.txt" 2>/dev/null | tr -d ' ')
        nr=$(grep -c '^Null render' "$log" 2>/dev/null || true); nr=${nr:-0}
        wd_kill=0; [[ -s $res/foot-$tag.warnings ]] && wd_kill=1
        echo "{\"tag\":\"$tag\",\"attempt\":$attempt,\"max_draws\":${maxd:-0},\"overlap\":${ov:-0},\"null_lines\":$nr,\"gcn422\":$gcn,\"pm4_203\":$pm4,\"watchdog\":$wd_kill,\"assert\":\"$(echo "$asserts" | head -1 | tr -d '"\\')\"}" >> "$res/attempts.jsonl"
        # The null arm's draws/frame is the guest's DECODED count (still counted, not recorded).
        if [[ -z $asserts && ${maxd:-0} -ge 500 && ${ov:-0} -eq 0 && $wd_kill -eq 0 ]]; then
            echo ok > "$res/$tag.done"; echo "null_render: $tag OK (max draws $maxd, null lines $nr)"; break
        fi
        echo "null_render: $tag attempt $attempt failed (draws=${maxd:-0} overlap=${ov:-0} watchdog=$wd_kill assert=$asserts)"
        rm -f "$res/foot-$tag.warnings"
    done
done
echo "null_render: ALLDONE ($(date +%T))"
