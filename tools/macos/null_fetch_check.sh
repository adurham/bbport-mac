#!/usr/bin/env bash
# tools/macos/null_fetch_check.sh TAG [TAG ...]: hidden title->gameplay runs for the null
# fetch-shader guard (fix/null-fetch-shader). Same standing route and settings as the vertex /
# motion series (fsr3 preset 4, 3440x1440, frame_generation=0, sandbox user dir
# out/curve3440/user, no BB_VRAM_LIMIT_MB). Counts 'Null fetch shader:' lines per run.
#   ctrl-N   : defaults (the guard must never fire: 0 lines)
#   motoff-N : BB_OBJECT_MOTION=0 (the path the original crash was seen on)
# Outputs: out/nullfetch/{series.log,attempts.jsonl,run-TAG-N.log,foot-TAG.txt}.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
[[ "$(basename "$here")" == bbport-nullfetch-build ]] || { echo "STOP: not the null-fetch checkout: $here" >&2; exit 1; }
res=$here/out/nullfetch
mkdir -p "$res"
exec >>"$res/series.log" 2>&1
echo "null_fetch_check: checkout $here @ $(git rev-parse --short HEAD)$(git diff --quiet || echo +dirty) ($(date +%T))"
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

for tag in "$@"; do
    case $tag in
        ctrl-*)   arm=() ;;
        motoff-*) arm=(BB_OBJECT_MOTION=0) ;;
        *) echo "unknown tag $tag"; continue ;;
    esac
    [[ -f $res/$tag.done ]] && { echo "$tag done - skip"; continue; }
    while pgrep -x bb-probe >/dev/null; do echo "$(date +%T) foreign bb-probe up; waiting"; sleep 10; done
    echo "null_fetch_check: ---- $tag ($(date +%T)) arm=${arm[*]-default} secs=${SECS:-200}"
    env BB_WARM_USER="$here/out/curve3440/user" ROUTE='ly=128,rx=128:5' \
        BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_OUTPUT_RES=3440x1440 \
        ${arm[@]+"${arm[@]}"} \
        bash tools/macos/gpu_ab_run.sh "$tag" "${SECS:-200}" > "$res/wrapper-$tag.txt" 2>&1 &
    route=$!
    watchdog "$tag" & wd=$!
    wait $route; kill $wd 2>/dev/null; wait $wd 2>/dev/null
    log=$here/out/run-$tag.log
    cp -f "$log" "$res/run-$tag.log" 2>/dev/null
    nulls=$(grep -c 'Null fetch shader:' "$log" 2>/dev/null || true); nulls=${nulls:-0}
    nodecode=$(grep -c 'does not decode' "$log" 2>/dev/null || true); nodecode=${nodecode:-0}
    gcn=$(grep -c 'gcn_to_vk.cpp:422' "$log" 2>/dev/null || true); gcn=${gcn:-0}
    pm4=$(grep -cE 'cp_graphics.cpp:203|Bad PM4' "$log" 2>/dev/null || true); pm4=${pm4:-0}
    asserts=$(grep -E 'gcn_to_vk|Bad PM4|cp_graphics|Unreachable code|died mid-run|Assertion|ASSERT|ended by signal|SIGSEGV' "$log" 2>/dev/null | head -1 | tr -d '"\\')
    maxd=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
    ov=$(wc -l < "$here/out/overlap-$tag.txt" 2>/dev/null | tr -d ' ')
    wd_kill=0; [[ -s $res/foot-$tag.warnings ]] && wd_kill=1
    echo "{\"tag\":\"$tag\",\"max_draws\":${maxd:-0},\"overlap\":${ov:-0},\"null_fetch_lines\":$nulls,\"fetch_no_decode\":$nodecode,\"gcn422\":$gcn,\"pm4_203\":$pm4,\"watchdog\":$wd_kill,\"assert\":\"$asserts\"}" >> "$res/attempts.jsonl"
    [[ -z $asserts && ${maxd:-0} -ge 500 && ${ov:-0} -eq 0 && $wd_kill -eq 0 ]] && echo ok > "$res/$tag.done"
    echo "null_fetch_check: $tag draws=${maxd:-0} null_fetch=$nulls no_decode=$nodecode overlap=${ov:-0} assert=$asserts"
done
echo "null_fetch_check: ALLDONE ($(date +%T))"
