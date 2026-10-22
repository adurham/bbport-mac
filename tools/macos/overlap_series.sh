#!/usr/bin/env bash
# tools/macos/overlap_series.sh TAG [TAG ...]: hidden 420 s standing runs at 3440x1440 FSR 3.1
# preset 3 (raster 1720x720) for the encoder-overlap work (probe/kk-overlap, docs/KK_OVERLAP.md).
# Same route/save/sandbox as vertex_ceiling.sh, so flat (60-110 s) and late windows compare.
#
#   <arm>-N     : no trace       (arm = ctrl | nodep | lite | vk | ovl)
#   <arm>tr-N   : + Metal System Trace (TRACE_S, after TRACE_AFTER s of gameplay) + CAPTURES port captures
#   ctrl = BB_KK_BARRIER unset (legacy);  nodep/lite/vk = BB_KK_BARRIER=<arm>;  ovl = BB_KK_OVERLAP=1
#
# Before each attempt: waits for bb-probe AND bb-gpu to be absent (10 polls x 60 s), else STOPs.
# Watchdog: footprint > 19 GiB or free+inactive < 4 GiB kills this checkout's game.
# Outputs: out/frames-TAG.csv, out/run-TAG.log, out/cap-TAG-N.png, out/mtl-TAG-gpu.xml,
#          out/overlap/{attempts.jsonl,wrapper-*,foot-*,series.log}
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
[[ "$(basename "$here")" == bbport-overlap-build ]] || { echo "STOP: not the overlap checkout: $here" >&2; exit 1; }
secs=${SECS:-420}
res=$here/out/overlap
mkdir -p "$res"
exec >>"${SERIES_LOG:-$res/series.log}" 2>&1
echo "overlap_series: checkout $here @ $(git rev-parse --short HEAD) ($(date +%T)) tags: $*"
grep -q '^frame_generation=0' "$here/bbport.ini" || { echo "STOP: checkout ini lacks frame_generation=0"; exit 1; }

# Busy = any bb-probe/bb-gpu, or any bench harness of ANOTHER checkout (their series restart the
# game between phases, so a bare pgrep can land in a gap: two attempts were voided that way).
freeinactive_mib() {
    local page free inactive
    page=$(vm_stat | awk '/page size of/{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/){print $i; exit}}')
    free=$(vm_stat | awk '/Pages free:/{v=$3; gsub(/\./,"",v); print v}')
    inactive=$(vm_stat | awk '/Pages inactive:/{v=$3; gsub(/\./,"",v); print v}')
    echo $(( (free + inactive) * page / 1048576 ))
}
busy() {
    # A run needs ~15 GiB of footprint; under 16 GiB free+inactive the watchdog kills it later
    # (another session's 52 GB job voided two attempts that way).
    [[ $(freeinactive_mib) -lt ${MIN_FREE_MIB:-16384} ]] && return 0
    pgrep -x bb-probe >/dev/null && return 0
    pgrep -x bb-gpu >/dev/null && return 0
    pgrep -x xctrace >/dev/null && return 0
    local p c
    for p in $(pgrep -f 'warm_route.sh|gpu_ab_run.sh|sizing_run.sh|warmfill.sh|curve3440.sh|vertex_ceiling.sh|metalfx_matrix.sh' 2>/dev/null); do
        c=$(lsof -a -p "$p" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p')
        [[ $c == "$here"* ]] || return 0
    done
    return 1
}
wait_idle() {
    local i k ok
    for i in $(seq 1 ${IDLE_POLLS:-10}); do
        ok=1
        for k in $(seq 1 12); do busy && { ok=0; break; }; sleep 5; done   # idle for a full 60 s
        [[ $ok == 1 ]] && return 0
        echo "$(date +%T) machine busy (poll $i/10): $(pgrep -lx bb-probe | tr '\n' ' ')$(pgrep -fl 'sizing_run|warm_route|gpu_ab_run|warmfill|curve3440' | cut -c1-60 | tr '\n' ' ')"
        sleep 60
    done
    return 1
}

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
    arm_name=${tag%%-*}; trace=()
    case $arm_name in *tr) arm_name=${arm_name%tr}
        trace=(TRACE=1 TRACE_SIGINT_S=${TRACE_S:-3} TRACE_AFTER=${TRACE_AFTER:-150} CAPTURES=${CAPTURES:-3}) ;;
    esac
    case $arm_name in
        ctrl) arm=() ;;
        nodep|lite|vk|none|free|cfree) arm=(BB_KK_BARRIER=$arm_name) ;;
        ovl) arm=(BB_KK_OVERLAP=1) ;;
        *) echo "unknown tag $tag"; continue ;;
    esac
    [[ -f $res/$tag.done ]] && { echo "$tag done - skip"; continue; }
    for attempt in 1 2 3; do
        if ! wait_idle; then echo "overlap_series: STOP - machine busy for 10 min before $tag"; exit 3; fi
        rm -f "$res/foot-$tag.warnings"
        echo "overlap_series: ---- $tag attempt $attempt ($(date +%T)) arm=${arm[*]-legacy} ${trace[*]-}"
        env ${trace[@]+"${trace[@]}"} \
            BB_WARM_USER="$here/out/curve3440/user" ROUTE='ly=128,rx=128:5' \
            BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=3 BB_OUTPUT_RES=3440x1440 \
            ${arm[@]+"${arm[@]}"} ${EXTRA_ENV:-} \
            bash tools/macos/gpu_ab_run.sh "$tag" "$secs" > "$res/wrapper-$tag-$attempt.txt" 2>&1 &
        route=$!
        watchdog "$tag" & wd=$!
        wait $route; kill $wd 2>/dev/null; wait $wd 2>/dev/null
        log=$here/out/run-$tag.log
        cp -f "$log" "$res/run-$tag-$attempt.log" 2>/dev/null
        gcn=$(grep -c 'gcn_to_vk.cpp:422' "$log" 2>/dev/null || true); gcn=${gcn:-0}
        pm4=$(grep -cE 'cp_graphics.cpp:203|Bad PM4' "$log" 2>/dev/null || true); pm4=${pm4:-0}
        asserts=$(grep -E 'gcn_to_vk|Bad PM4|cp_graphics|Unreachable code|died mid-run|Assertion|ASSERT|ended by signal' "$log" 2>/dev/null | head -3)
        maxd=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
        ov=$(wc -l < "$here/out/overlap-$tag.txt" 2>/dev/null | tr -d ' ')
        mode=$(grep -m1 'end-of-encoder barriers\|BB_KK_OVERLAP' "$log" 2>/dev/null | tr -d '"\\')
        wd_kill=0; [[ -s $res/foot-$tag.warnings ]] && wd_kill=1
        echo "{\"tag\":\"$tag\",\"attempt\":$attempt,\"max_draws\":${maxd:-0},\"overlap\":${ov:-0},\"gcn422\":$gcn,\"pm4_203\":$pm4,\"watchdog\":$wd_kill,\"mode\":\"$mode\",\"assert\":\"$(echo "$asserts" | head -1 | tr -d '"\\')\"}" >> "$res/attempts.jsonl"
        # BB_KK_BARRIER=none renders a black world: the draw gate is the only gameplay gate there.
        if [[ -z $asserts && ${maxd:-0} -ge 500 && ${ov:-0} -eq 0 && $wd_kill -eq 0 ]]; then
            echo ok > "$res/$tag.done"; echo "overlap_series: $tag OK (draws $maxd; $mode)"; break
        fi
        echo "overlap_series: $tag attempt $attempt failed (draws=${maxd:-0} overlap=${ov:-0} watchdog=$wd_kill assert=$asserts)"
    done
done
echo "overlap_series: ALLDONE ($(date +%T))"
