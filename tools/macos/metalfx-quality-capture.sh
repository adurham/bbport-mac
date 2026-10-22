#!/usr/bin/env bash
# tools/macos/metalfx-quality-capture.sh TAG [SECS]: one hidden standing/turning run that dumps the
# upscaler's real inputs for the MetalFX-vs-FSR sharpness comparison (docs/METALFX_QUALITY.md).
#
# It runs an ALREADY BUILT port checkout (QROOT: the run needs the port's own binaries, and only
# spike/metalfx-tail carries the MetalFX code; the capture gates themselves are in dd9705b, so
# ~/repos/bbport-motionfast-build has them) through tools/macos/gpu_ab_run.sh, with the same sandbox
# and watchdog shape as motion_fast_ab.sh, and arms BB_DUMP_TRIGGER once gameplay draws flow.
#
#   TAG    = the arm: st (fsr3 standing), mv (fsr3 turning), nat (upscaler off, native 3440)
#   SECS   = run length (default 200)
#
# Outputs, under <spike checkout>/out/quality/:
#   dump-TAG/        fNNN_{input,depth,motion,output,objects}_WxH_fmt.raw (the upscaler's inputs,
#                    its display-resolution output, and the per-frame camera/jitter lines are in
#                    run-TAG.log)
#   run-TAG.log      the port's log (parsed for 'Dump camera:' jitter and camera state)
#   frames-TAG.csv   the frame log
#   cap-TAG-N.png    the port's own presented-frame capture (BB_BUGREPORT_TRIGGER) where CAPTURES>0
#   capture-TAG.txt  the arm's environment and the watchdog's footprint trace
#
# Env: QROOT, DUMP_AT (20 s after the first >=500 draws/frame line), FRAMES (14 dumped frames),
# ROUTE, CAPTURES (0), RESET_WARM_USER (0: reuse the checkpoint's out/curve3440/user save+cache).
set -uo pipefail
tag=${1:?usage: metalfx-quality-capture.sh TAG [SECS]}
secs=${2:-200}
spike=$(cd -- "$(dirname -- "$0")/../.." && pwd)
qroot=${QROOT:-$HOME/repos/bbport-motionfast-build}
[[ -f $qroot/gpu/upscale/vk_temporal_upscaler.cpp ]] || { echo "STOP: no port checkout at $qroot" >&2; exit 1; }
[[ -x $qroot/out/bb-probe ]] || { echo "STOP: $qroot is not built (out/bb-probe)" >&2; exit 1; }
res=$spike/out/quality
mkdir -p "$res"
exec >>"$res/capture-$tag.txt" 2>&1
echo "=== metalfx-quality-capture: $tag $(date +%T) qroot=$qroot @ $(git -C "$qroot" rev-parse --short HEAD)$(git -C "$qroot" diff --quiet || echo +dirty) secs=$secs"

case $tag in
    st)  arm=(BB_UPSCALER=fsr3 ROUTE='ly=128,rx=128:5' CAPTURES=${CAPTURES:-2}) ;;
    mv)  arm=(BB_UPSCALER=fsr3 ROUTE='ly=128,rx=128:1.5 ly=128,rx=94:18 ly=128,rx=128:1.5 ly=128,rx=162:18' CAPTURES=0) ;;
    # mv3: no standing pauses at all, so whenever the dump lands the camera is turning.
    mv3) arm=(BB_UPSCALER=fsr3 ROUTE='ly=128,rx=94:45 ly=128,rx=162:45' CAPTURES=0) ;;
    # mv4: the character walks (full forward) while the camera turns: a held look stick alone left
    # the view matrix identical across the dumped frames, a walk cannot.
    mv4) arm=(BB_UPSCALER=fsr3 ROUTE='ly=0,rx=94:45 ly=0,rx=162:45' CAPTURES=0) ;;
    nat) arm=(BB_UPSCALER=off ROUTE='ly=128,rx=128:5' CAPTURES=${CAPTURES:-2}) ;;
    *) echo "unknown arm $tag"; exit 1 ;;
esac

watchdog() {
    local page free inactive mib foot pids cl
    while kill -0 "$route" 2>/dev/null; do
        page=$(vm_stat | awk '/page size of/{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/){print $i; exit}}')
        free=$(vm_stat | awk '/Pages free:/{v=$3; gsub(/\./,"",v); print v}')
        inactive=$(vm_stat | awk '/Pages inactive:/{v=$3; gsub(/\./,"",v); print v}')
        mib=$(( (free + inactive) * page / 1048576 ))
        foot=0; pids=$(pgrep -x bb-gpu 2>/dev/null || true)
        for p in $pids; do
            cl=$(ps -o command= -p "$p" 2>/dev/null)
            case "$cl" in *"$qroot"*)
                foot=$(/usr/bin/footprint -p "$p" 2>/dev/null | awk '/Footprint:/{for(i=1;i<=NF;i++) if($i=="Footprint:"){v=$(i+1); u=$(i+2)}; if(u~/GB/){v*=1024} else if(u~/KB/){v/=1024} printf "%.0f", v; exit}')
                break ;;
            esac
        done
        echo "$(date +%s) foot_mib=${foot:-0} freeinactive_mib=$mib"
        if [[ ${foot:-0} -gt 19456 || ( $mib -lt 4096 && -n $pids ) ]]; then
            echo "$(date) WATCHDOG: foot=${foot} MiB free+inactive=${mib} MiB; killing"
            for g in $(pgrep -x bb-gpu) $(pgrep -x bb-probe); do
                ps -o command= -p "$g" 2>/dev/null | grep -q "$qroot" && kill "$g" 2>/dev/null
            done
        fi
        sleep 10
    done
}

dump_watch() {
    local trig=$1 start=""
    while kill -0 "$route" 2>/dev/null; do
        if [[ -z $start ]] && grep '^Frame stats' "$qroot/out/last-run.log" 2>/dev/null |
                grep -qE '([5-9][0-9]{2}|[0-9]{4,}) draws/frame'; then
            start=$(date +%s)
            echo "dump_watch: gameplay seen $(date +%T)"
        fi
        if [[ -n $start && $(( $(date +%s) - start )) -ge ${DUMP_AT:-20} ]]; then
            : > "$trig"
            # BB_FINAL_DUMP_TRIGGER: the frame as presented (display res, SDR, with the UI), one
            # frame per touch. Also armed on the upscaler-off arm, which has no upscale dispatch
            # to dump: that frame is the native 3440x1440 sharpness reference.
            : > "${trig}_final" 2>/dev/null || true
            echo "dump_watch: trigger touched $(date +%T)"
            return
        fi
        sleep 1
    done
}

while pgrep -x bb-probe >/dev/null; do echo "$(date +%T) foreign bb-probe up; waiting"; sleep 10; done
# A run this script started and then lost (watchdog kill) leaves its route writing the same pad
# file and stealing the same checkout: clear those before starting another.
for p in $(pgrep -f 'warm_route.sh|gpu_ab_run.sh'); do kill "$p" 2>/dev/null; done
sleep 1
dtrig=$spike/out/quality/dump_trigger_$tag
rm -f "$dtrig"; rm -rf "$res/dump-$tag"; mkdir -p "$res/dump-$tag"
env BB_WARM_USER="$qroot/out/curve3440/user" RESET_WARM_USER=${RESET_WARM_USER:-0} \
    BB_UPSCALE_PRESET=4 BB_OUTPUT_RES=3440x1440 BB_FRAME_GEN=0 \
    BB_DUMP_TRIGGER="$dtrig" BB_DUMP_DIR="$res/dump-$tag" BB_DUMP_FRAMES=${FRAMES:-14} \
    BB_FINAL_DUMP_TRIGGER="${dtrig}_final" \
    "${arm[@]}" \
    bash "$qroot/tools/macos/gpu_ab_run.sh" "$tag" "$secs" > "$res/wrapper-$tag.txt" 2>&1 &
route=$!
watchdog & wd=$!
dump_watch "$dtrig" & dw=$!
wait $route; kill $wd $dw 2>/dev/null; wait $wd $dw 2>/dev/null
cp -f "$qroot/out/last-run.log" "$res/run-$tag.log" 2>/dev/null
cp -f "$qroot/out/frames-$tag.csv" "$res/frames-$tag.csv" 2>/dev/null
for f in "$qroot"/out/cap-"$tag"-*.png; do [[ -f $f ]] && cp -f "$f" "$res/"; done
log=$res/run-$tag.log
gcn=$(grep -c 'gcn_to_vk.cpp:422' "$log" 2>/dev/null || true)
pm4=$(grep -cE 'cp_graphics.cpp:203|Bad PM4' "$log" 2>/dev/null || true)
maxd=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
dumps=$(ls "$res/dump-$tag" 2>/dev/null | wc -l | tr -d ' ')
cam=$(grep -c '^Dump camera:' "$log" 2>/dev/null || true)
ups=$(grep -m2 -E '^Upscaler|MetalFX' "$log" 2>/dev/null | tr '\n' '|')
echo "tag=$tag max_draws=${maxd:-0} gcn422=${gcn:-0} pm4_203=${pm4:-0} dump_files=${dumps:-0} camera_lines=${cam:-0} upscaler='${ups}'"
echo "=== done $tag $(date +%T)"
