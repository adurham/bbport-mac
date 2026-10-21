#!/usr/bin/env bash
# tools/macos/curve3440.sh [TAG ...]: the 3440x1440 FSR 3.1 preset curve — one hidden standing
# run per tag in THIS checkout, serially, with a footprint watchdog. Standing route: the camera
# rests where the save drops the player, so every arm is the same spot and the frame log is
# comparable.
#
#   SECS=300 bash tools/macos/curve3440.sh curve-p4 curve-p4m0 curve-p3 curve-p2 curve-p1 curve-off
#
# Tag -> settings (any tag order):
#   curve-p4 : fsr3 preset 4 (today's baseline)     curve-p4m0 : p4 with effect_motion_blur=0
#   curve-p3 : fsr3 preset 3                        curve-p2   : fsr3 preset 2
#   curve-p1 : fsr3 preset 1                        curve-off  : upscaler off (3440x1440 raster)
#
# Everything else is the same for every arm: BB_NATIVE_GPU=1 BB_FRAME_STATS=1, frame_generation=0
# (the checkout ini), no BB_VRAM_LIMIT_MB, gpu_ab_run.sh's own BB_FRAME_LOG/BB_ASYNC_STATIC_VI.
# The settings never touch the installed app's data dir: this checkout's own out/, user/ and ini
# are the sandbox, and BB_WARM_USER points at out/curve3440/user (reset once, from out/warm_user,
# so the arms load the exact spot this afternoon's baseline runs loaded).
#
# Per-run artifacts under out/: frames-<tag>.csv, route-<tag>.txt, run-<tag>.log,
# overlap-<tag>.txt (curve3440/foot-<tag>.txt samples + curve3440/<tag>.done marker).
# A run is marked done only if: 1 'macOS run' header, gameplay reached (max draws/frame >= 500),
# both the FSR context line and a Scene-resolution raster line present, overlap file empty and
# no known-assert crash in its log. Failed runs are retried once (attempts.jsonl keeps the count)
# and a run that died on an assert is never a number.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
[[ "$(basename "$here")" == bbport-mac-native ]] || { echo "STOP: not the native checkout: $here" >&2; exit 1; }
head=$(git rev-parse --short HEAD)
echo "curve3440: checkout $here @ $head"
secs=${SECS:-300}
res=$here/out/curve3440
mkdir -p "$res"
mblur_ini=$res/bbport-mblur0.ini

# Fresh variant ini per series invocation, from the checkout's CURRENT ini (only the one key).
sed 's/^effect_motion_blur=.*/effect_motion_blur=0/' "$here/bbport.ini" > "$mblur_ini"

mk_env() {
    local tag=$1
    case "$tag" in
        curve-p4)   echo "BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4" ;;
        curve-p4m0) echo "BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_CONFIG=$mblur_ini" ;;
        curve-p3)   echo "BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=3" ;;
        curve-p2)   echo "BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=2" ;;
        curve-p1)   echo "BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=1" ;;
        curve-off)  echo "BB_UPSCALER=off" ;;
        *)          echo "" ;;
    esac
}

max_draws() {  # max draws/frame a run log shows (0 when none)
    grep -oE '[0-9]+ draws/frame' "$1" 2>/dev/null | awk '{print $1}' | sort -n | tail -1
}

watchdog() {  # $1 tag while the run is alive: sample bb-gpu footprint + vm_stat, kill at the ceiling
    local tag=$1 samp foot vmstat free inactive pids cl out
    while kill -0 "$route" 2>/dev/null; do
        page=$(vm_stat | awk '/page size of/{gsub(/\(|\)/,""); for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/){print $i; exit}}')
        free=$(vm_stat | awk '/Pages free:/{print $6}')
        inactive=$(vm_stat | awk '/Pages inactive:/{print $6}')
        vmstat=$(( (free + inactive) * page / 1048576 ))
        foot=0; pids=$(pgrep -x bb-gpu 2>/dev/null || true)
        for p in $pids; do
            cl=$(ps -o command= -p "$p" 2>/dev/null)
            case "$cl" in *"$here"*)
                foot=$(/usr/bin/footprint -p "$p" 2>/dev/null | awk '/Footprint:/{for(i=1;i<=NF;i++) if($i=="Footprint:"){v=$(i+1); u=$(i+2)}; if(u~/GB/){v*=1024} else if(u~/KB/){v/=1024} if(u~/MB/){} printf "%.0f", v; exit}')
                break ;;
            esac
        done
        echo "$(date +%s) foot_mib=${foot:-0} freepages_mib=${vmstat}" >> "$res/foot-$tag.txt"
        if [[ ${foot:-0} -gt 19456 ]]; then
            echo "$(date) WATCHDOG: bb-gpu footprint ${foot} MiB > 19456; killing the run" >> "$res/foot-$tag.warnings"
            for g in $(pgrep -x bb-gpu); do ps -o command= -p "$g" | grep -q "$here" && kill "$g" 2>/dev/null; done
            for g in $(pgrep -x bb-probe); do ps -o command= -p "$g" | grep -q "$here" && kill "$g" 2>/dev/null; done
        fi
        if [[ $vmstat -lt 4096 ]]; then
            echo "$(date) WATCHDOG: free+inactive ${vmstat} MiB < 4096; killing the run" >> "$res/foot-$tag.warnings"
            for g in $(pgrep -x bb-gpu); do ps -o command= -p "$g" | grep -q "$here" && kill "$g" 2>/dev/null; done
            for g in $(pgrep -x bb-probe); do ps -o command= -p "$g" | grep -q "$here" && kill "$g" 2>/dev/null; done
        fi
        sleep 10
    done
}

any_tag=0
for tag in "$@"; do
    [[ -n $(mk_env "$tag") ]] || { echo "curve3440: unknown tag $tag — skip" >&2; continue; }
    any_tag=1
    if [[ -f $res/$tag.done ]]; then
        echo "curve3440: $tag already done — skip"
        continue
    fi
    if [[ ${RESET_CURVE_USER:-0} == 1 && ! -f $res/.user_reset ]]; then
        rm -rf "$res/user"
    fi
    echo "curve3440: ---- $tag ($(date +%T)), SECS=$secs"
    wait=0
    attempts=0
    while [[ $attempts -lt 2 ]]; do
        attempts=$((attempts + 1))
        while pgrep -x bb-probe >/dev/null; do
            wait=$((wait + 10))
            if ((wait >= 1800)); then echo "curve3440: STOP: a foreign bb-probe held the machine 30 min — tag $tag left undone" >&2; break 2; fi
            echo "curve3440: $(date +%T) a bb-probe is up; waiting 10 s"
            sleep 10
        done
        env BB_WARM_USER=$res/user BB_WARM_SOURCE_USER=$here/out/warm_user \
            ROUTE='ly=128,rx=128:5' $(mk_env "$tag") \
            bash tools/macos/gpu_ab_run.sh "$tag" "$secs" > "$res/wrapper-$tag-$attempts.txt" 2>&1 &
        route=$!
        watchdog "$tag" &
        wd=$!
        wait $route
        kill "$wd" 2>/dev/null || true
        wait $wd 2>/dev/null || true
        # Classify the attempt.
        log=$here/out/run-"$tag".log
        status=""
        note=""
        n_head=$(grep -c '^macOS run:' "$log" 2>/dev/null || true); n_head=${n_head:-0}
        n_max=$(max_draws "$log" 2>/dev/null || true); n_max=${n_max:-0}
        n_fsr=$(grep -c 'Upscaler: FSR 3.1 available' "$log" 2>/dev/null || true)
        n_raster=$(grep -c 'Scene resolution: raster' "$log" 2>/dev/null || true)
        n_overlap=$(wc -l < "$here/out/overlap-$tag".txt 2>/dev/null || true); n_overlap=${n_overlap:-0}
        grep -E 'gcn_to_vk|Bad PM4|cp_graphics.*203|Unreachable code|died mid-run' "$log" 2>/dev/null | sed -n '1,3p' > "$res/$tag.assert-$attempts.txt" || true
        if [[ -s $res/$tag.assert-$attempts.txt ]]; then
            status=assert
        elif [[ $n_head -ne 1 ]]; then
            status=void; note="macOS run headers: $n_head"
        elif [[ $n_overlap -ne 0 ]]; then
            status=overlapped; note="foreign bb-probe seen during the run"
        elif [[ ${n_max:-0} -lt 500 ]]; then
            status=nogameplay; note="max draws/frame=$n_max"
        elif [[ ${n_fsr:-0} -lt 1 || ${n_raster:-0} -lt 1 ]]; then
            status=nocontext; note="FSR ctx lines=$n_fsr raster lines=$n_raster"
        else
            status=ok; note="max draws/frame=$n_max"
        fi
        if [[ $status == assert ]]; then
            note=$(head -1 "$res/$tag.assert-$attempts.txt" | tr -d '"')
        fi
        echo "{\"tag\":\"$tag\",\"attempt\":$attempts,\"status\":\"$status\",\"max_draws\":${n_max:-0},\"note\":\"$note\",\"t_end\":\"$(date +%T)\"}" >> "$res/attempts.jsonl"
        grep 'Frame stats' "$log" 2>/dev/null | tail -2 >> "$res/attempts.jsonl.log" 2>/dev/null || true
        if [[ $status == ok ]]; then
            echo "$status" > "$res/$tag.done"
            echo "curve3440: $tag OK (attempt $attempts, max draws/frame=$n_max)"
            break
        fi
        echo "curve3440: $tag attempt $attempts -> $status ($note)"
        # A crashed run can clear its sandbox store: if it looks emptied, drop it so the next
        # attempt re-copies it from out/warm_user (the crash-damage behaviour, never merged from).
        blobs=$(du -s "$res/user/cache" 2>/dev/null | awk '{print $1}')
        if [[ ${blobs:-0} -lt 1000 ]]; then
            echo "curve3440: store looks emptied by a crash (${blobs:-0} KB); dropping it for a fresh copy"
            rm -rf "$res/user"
        fi
    done
    [[ $attempts -lt 2 || $status == ok ]] || echo "curve3440: $tag FAILED both attempts ($status)" >&2
done
[[ $any_tag == 1 ]] || { echo "usage: curve3440.sh TAG [TAG ...] (tags: curve-p4 curve-p4m0 curve-p3 curve-p2 curve-p1 curve-off)" >&2; exit 1; }
echo "curve3440: ALLDONE ($(date +%T))"