#!/usr/bin/env bash
# tools/macos/protect_async.sh TAG [TAG ...]: hidden standing runs for the BB_PROTECT_ASYNC=1 A/B
# (bench/protect-async), on protect_probe.sh's route, store, save and watchdog (fsr3 preset 1,
# curve3440's save, SECS=300 so the late phase is reached). Both arms carry BB_PROTECT_PROBE=1.
#   ctrl-N  : the gate off. BB_PROTECT_ASYNC_LOG is still given: the control must not write it.
#   async-N : BB_PROTECT_ASYNC=1.
# Outputs: out/frames-TAG.csv, out/protect-TAG.csv, out/pasync-TAG.csv, out/run-TAG.log,
# out/protect-async/* (attempts.jsonl, wrappers, footprint samples, run logs per attempt).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
[[ "$(basename "$here")" == bbport-protect-build ]] || { echo "STOP: not the probe checkout: $here" >&2; exit 1; }
secs=${SECS:-300}
res=$here/out/protect-async
mkdir -p "$res"
exec >>"${SERIES_LOG:-$res/series.log}" 2>&1
echo "protect_async: checkout $here @ $(git rev-parse --short HEAD) ($(date +%T))"

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

for tag in "$@"; do
    case $tag in
        ctrl-*)  arm=() ;;
        async-*) arm=(BB_PROTECT_ASYNC=1) ;;
        *) echo "unknown tag $tag"; continue ;;
    esac
    user=$here/out/curve3440/user
    [[ -f $res/$tag.done ]] && { echo "$tag done - skip"; continue; }
    for attempt in 1 2 3; do
        while pgrep -x bb-probe >/dev/null; do echo "$(date +%T) foreign bb-probe up; waiting"; sleep 10; done
        echo "protect_async: ---- $tag attempt $attempt ($(date +%T)) arm=${arm[*]-off}"
        rm -f "$here/out/pasync-$tag.csv" "$here/out/protect-$tag.csv"
        env BB_WARM_USER="$user" ROUTE='ly=128,rx=128:5' \
            BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=1 \
            BB_PROTECT_PROBE=1 BB_PROTECT_PROBE_LOG="$here/out/protect-$tag.csv" \
            BB_PROTECT_ASYNC_LOG="$here/out/pasync-$tag.csv" ${arm[@]+"${arm[@]}"} \
            bash tools/macos/gpu_ab_run.sh "$tag" "$secs" > "$res/wrapper-$tag-$attempt.txt" 2>&1 &
        route=$!
        watchdog "$tag" &
        wd=$!
        wait $route; kill $wd 2>/dev/null; wait $wd 2>/dev/null
        log=$here/out/run-$tag.log
        cp -f "$log" "$res/run-$tag-$attempt.log" 2>/dev/null
        asserts=$(grep -E 'gcn_to_vk|Bad PM4|cp_graphics|Unreachable code|died mid-run|Assertion|ASSERT|ended by signal' "$log" 2>/dev/null | head -3)
        maxd=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
        ov=$(wc -l < "$here/out/overlap-$tag.txt" 2>/dev/null | tr -d ' ')
        pa_lines=$(grep -c '^Protect async' "$log" 2>/dev/null || true); pa_lines=${pa_lines:-0}
        pa_csv=0; [[ -e $here/out/pasync-$tag.csv ]] && pa_csv=1
        wd_kill=0; [[ -s $res/foot-$tag.warnings ]] && wd_kill=1
        echo "{\"tag\":\"$tag\",\"attempt\":$attempt,\"max_draws\":${maxd:-0},\"overlap\":${ov:-0},\"pa_lines\":$pa_lines,\"pa_csv\":$pa_csv,\"watchdog\":$wd_kill,\"assert\":\"$(echo "$asserts" | head -1 | tr -d '"\\')\"}" >> "$res/attempts.jsonl"
        if [[ -z $asserts && ${maxd:-0} -ge 500 && ${ov:-0} -eq 0 && $wd_kill -eq 0 ]]; then
            echo ok > "$res/$tag.done"; echo "protect_async: $tag OK (max draws $maxd, async lines $pa_lines, async csv $pa_csv)"; break
        fi
        echo "protect_async: $tag attempt $attempt failed (draws=${maxd:-0} overlap=${ov:-0} watchdog=$wd_kill assert=$asserts)"
        rm -f "$res/foot-$tag.warnings"
    done
done
echo "protect_async: ALLDONE ($(date +%T))"
