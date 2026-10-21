#!/usr/bin/env bash
# tools/macos/protect_probe.sh TAG [TAG ...]: hidden standing runs (curve3440.sh's route, store and
# watchdog) with the BB_PROTECT_PROBE=1 split of the protect/ipc term (bench/protect-ipc).
#   probe-p1 : fsr3 preset 1 + probe          probe-p4 : fsr3 preset 4 + probe
#   spot-p1  : fsr3 preset 1 + probe, on the earlier campaign's save (USER_SRC, default the
#              metalfx worktree's 2026-10-17 warm_user), copied into out/protect/user-spot
# Outputs: out/frames-TAG.csv, out/protect-TAG.csv (probe), out/run-TAG.log, out/protect/*.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
[[ "$(basename "$here")" == bbport-protect-build ]] || { echo "STOP: not the probe checkout: $here" >&2; exit 1; }
echo "protect_probe: checkout $here @ $(git rev-parse --short HEAD)"
secs=${SECS:-240}
res=$here/out/protect
mkdir -p "$res"
# The script logs itself (SERIES_LOG, default out/protect/series.log), so a launch needs no
# redirect into a directory that may not exist yet (zsh background launches die on that).
exec >>"${SERIES_LOG:-$res/series.log}" 2>&1
user_src=${USER_SRC:-$HOME/repos/bbport-metalfx-build/out/warm_user}

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
        probe-p1) preset=1; user=$here/out/curve3440/user ;;
        probe-p4) preset=4; user=$here/out/curve3440/user ;;
        long-p1)  preset=1; user=$here/out/curve3440/user ;;  # SECS=300: the curve's window
        spot-p1)  preset=1; user=$res/user-spot
                  [[ -d $user ]] || cp -cR "$user_src" "$user" ;;
        *) echo "unknown tag $tag" >&2; continue ;;
    esac
    [[ -f $res/$tag.done ]] && { echo "$tag done — skip"; continue; }
    for attempt in 1 2; do
        while pgrep -x bb-probe >/dev/null; do echo "$(date +%T) foreign bb-probe up; waiting"; sleep 10; done
        echo "protect_probe: ---- $tag attempt $attempt ($(date +%T))"
        env BB_WARM_USER="$user" ROUTE='ly=128,rx=128:5' \
            BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=$preset \
            BB_PROTECT_PROBE=1 BB_PROTECT_PROBE_LOG="$here/out/protect-$tag.csv" \
            bash tools/macos/gpu_ab_run.sh "$tag" "$secs" > "$res/wrapper-$tag-$attempt.txt" 2>&1 &
        route=$!
        watchdog "$tag" &
        wd=$!
        wait $route; kill $wd 2>/dev/null; wait $wd 2>/dev/null
        log=$here/out/run-$tag.log
        cp -f "$log" "$res/run-$tag-$attempt.log" 2>/dev/null
        asserts=$(grep -E 'gcn_to_vk|Bad PM4|cp_graphics|Unreachable code|died mid-run' "$log" 2>/dev/null | head -3)
        maxd=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
        ov=$(wc -l < "$here/out/overlap-$tag.txt" 2>/dev/null | tr -d ' ')
        echo "{\"tag\":\"$tag\",\"attempt\":$attempt,\"max_draws\":${maxd:-0},\"overlap\":${ov:-0},\"assert\":\"$(echo "$asserts" | head -1 | tr -d '\"')\"}" >> "$res/attempts.jsonl"
        if [[ -z $asserts && ${maxd:-0} -ge 500 && ${ov:-0} -eq 0 ]]; then
            echo ok > "$res/$tag.done"; echo "protect_probe: $tag OK (max draws $maxd)"; break
        fi
        echo "protect_probe: $tag attempt $attempt failed (draws=${maxd:-0} overlap=${ov:-0} assert=$asserts)"
    done
done
echo "protect_probe: ALLDONE ($(date +%T))"
