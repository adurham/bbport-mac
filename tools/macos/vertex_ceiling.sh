#!/usr/bin/env bash
# tools/macos/vertex_ceiling.sh TAG [TAG ...]: hidden standing runs for the heavy-vertex-pass work
# (bench/vertex-passes). Same route, save and settings as null_render.sh (fsr3 preset 4, 3440x1440,
# frame_generation=0, sandbox user dir, no BB_VRAM_LIMIT_MB, 420 s standing), so the flat (60-110 s)
# and late (last 75 s) windows compare with the existing curve/null series.
#
#   ctrl-N   : gate off (control arm)
#   skip-N   : BB_SKIP_VS=<SKIP_VS> - the heavy passes' draws left out (the ceiling arm)
#   trace-N  : gate off + Metal System Trace of bb-gpu (TRACE_SIGINT_S) + BB_PASS_TRACE armed at the
#              same moment, so the trace's per-encoder vertex time and the port's per-encoder identity
#              come from the same frames (tools/macos/mtl_pass_join.py)
#   trace2560-N : trace-N at BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 with OUT_RES=2560x1440
#
# Env: SECS (420), SKIP_VS (the vertex shader hashes, comma separated), TRACE_S (4), TRACE_AFTER (30),
# RES (3440x1440), PASS_FRAMES (240), BB_WARM_USER (out/curve3440/user).
# Outputs: out/frames-TAG.csv, out/run-TAG.log, out/pass-TAG.txt, out/vertex/{attempts.jsonl,
# wrapper-*,foot-*}; report: python3 tools/macos/null_render_report.py TAG...
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
[[ "$(basename "$here")" == bbport-nullrender-build ]] || { echo "STOP: not the probe checkout: $here" >&2; exit 1; }
secs=${SECS:-420}
res=$here/out/vertex
mkdir -p "$res"
exec >>"${SERIES_LOG:-$res/series.log}" 2>&1
echo "vertex_ceiling: checkout $here @ $(git rev-parse --short HEAD) ($(date +%T))"
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

# The pass trace's window has to cover the same frames as the trace: touch its trigger the moment
# xctrace appears, so both see the same ~4 s of gameplay.
trigger_watch() {
    local trig=$1 tdir=$here/out/mtl-$tag.trace
    while kill -0 "$route" 2>/dev/null; do
        if [[ -d $tdir ]] || pgrep -f "xctrace record" >/dev/null 2>&1; then
            : > "$trig"
            echo "vertex_ceiling: pass-trace trigger touched $(date +%T)" >> "$res/wrapper-$tag-$attempt.txt"
            return
        fi
        sleep 0.2
    done
}

for tag in "$@"; do
    trace=(); traceenv=() ; arm=(); outres=${RES:-3440x1440}
    case $tag in
        ctrl-*)   arm=() ;;
        skip-*)   arm=(BB_SKIP_VS="${SKIP_VS:?SKIP_VS not set}") ;;
        trace-*)  trace=(TRACE=1 TRACE_SIGINT_S=${TRACE_S:-4} TRACE_AFTER=${TRACE_AFTER:-30}) ;;
        trace2560-*) trace=(TRACE=1 TRACE_SIGINT_S=${TRACE_S:-4} TRACE_AFTER=${TRACE_AFTER:-30}); outres=2560x1440 ;;
        *) echo "unknown tag $tag"; continue ;;
    esac
    if [[ $tag == trace* ]]; then
        ptrigger=$here/out/passtrace_trigger
        rm -f "$ptrigger"
        traceenv=(BB_PASS_TRACE="$here/out/pass-$tag.txt" BB_PASS_TRACE_TRIGGER="$ptrigger" \
                  BB_PASS_TRACE_FRAMES="${PASS_FRAMES:-240}")
        rm -f "$here/out/pass-$tag.txt"
    fi
    [[ -f $res/$tag.done ]] && { echo "$tag done - skip"; continue; }
    for attempt in 1 2 3; do
        while pgrep -x bb-probe >/dev/null; do echo "$(date +%T) foreign bb-probe up; waiting"; sleep 10; done
        echo "vertex_ceiling: ---- $tag attempt $attempt ($(date +%T)) arm=${arm[*]-off} res=$outres"
        env ${trace[@]+"${trace[@]}"} ${traceenv[@]+"${traceenv[@]}"} \
            BB_WARM_USER="$here/out/curve3440/user" ROUTE='ly=128,rx=128:5' \
            BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_OUTPUT_RES="$outres" \
            ${arm[@]+"${arm[@]}"} \
            bash tools/macos/gpu_ab_run.sh "$tag" "$secs" > "$res/wrapper-$tag-$attempt.txt" 2>&1 &
        route=$!
        watchdog "$tag" & wd=$!
        tw=""
        [[ $tag == trace* ]] && { trigger_watch "$ptrigger" & tw=$!; }
        wait $route; kill $wd 2>/dev/null; wait $wd 2>/dev/null
        [[ -n $tw ]] && { kill $tw 2>/dev/null; wait $tw 2>/dev/null; }
        log=$here/out/run-$tag.log
        cp -f "$log" "$res/run-$tag-$attempt.log" 2>/dev/null
        gcn=$(grep -c 'gcn_to_vk.cpp:422' "$log" 2>/dev/null || true); gcn=${gcn:-0}
        pm4=$(grep -cE 'cp_graphics.cpp:203|Bad PM4' "$log" 2>/dev/null || true); pm4=${pm4:-0}
        asserts=$(grep -E 'gcn_to_vk|Bad PM4|cp_graphics|Unreachable code|died mid-run|Assertion|ASSERT|ended by signal' "$log" 2>/dev/null | head -3)
        maxd=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
        ov=$(wc -l < "$here/out/overlap-$tag.txt" 2>/dev/null | tr -d ' ')
        ptl=$(grep -c '^PT ' "$here/out/pass-$tag.txt" 2>/dev/null || true); ptl=${ptl:-0}
        wd_kill=0; [[ -s $res/foot-$tag.warnings ]] && wd_kill=1
        echo "{\"tag\":\"$tag\",\"attempt\":$attempt,\"max_draws\":${maxd:-0},\"overlap\":${ov:-0},\"pass_lines\":$ptl,\"gcn422\":$gcn,\"pm4_203\":$pm4,\"watchdog\":$wd_kill,\"assert\":\"$(echo "$asserts" | head -1 | tr -d '"\\')\"}" >> "$res/attempts.jsonl"
        if [[ -z $asserts && ${maxd:-0} -ge 500 && ${ov:-0} -eq 0 && $wd_kill -eq 0 ]]; then
            if [[ $tag == trace* && ${ptl:-0} -lt 1000 ]]; then
                echo "vertex_ceiling: $tag pass trace only $ptl lines - retry"
            else
                echo ok > "$res/$tag.done"; echo "vertex_ceiling: $tag OK (draws $maxd, pass lines $ptl)"; break
            fi
        fi
        echo "vertex_ceiling: $tag attempt $attempt failed (draws=${maxd:-0} overlap=${ov:-0} watchdog=$wd_kill pass_lines=$ptl assert=$asserts)"
        rm -f "$res/foot-$tag.warnings"
    done
done
echo "vertex_ceiling: ALLDONE ($(date +%T))"
