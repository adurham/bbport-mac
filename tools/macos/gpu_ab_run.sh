#!/usr/bin/env bash
# tools/macos/gpu_ab_run.sh TAG SECS [ENV=VALUE ...]: one hidden warm-route run of THIS checkout
# for GPU A/Bs (docs/KK_GPU_WALL.md): the frame log, optionally a Metal System Trace of bb-gpu and
# port frame captures once gameplay draws flow.
#
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/gpu_ab_run.sh legacy1 180 BB_KK_BARRIER=legacy
#
# Outputs (out/): frames-TAG.csv (BB_FRAME_LOG), run-TAG.log, route-TAG.txt, overlap-TAG.txt (any
# bb-probe NOT from this checkout seen during the run: non-empty = the run is void),
# mtl-TAG.trace + mtl-TAG-gpu.xml (TRACE=1), cap-TAG-N.png (CAPTURES=N port captures, the
# Ctrl+Shift+C path, CAPTURE_GAP s apart).
# Env: TRACE (0), TLIMIT (3s), TRACE_AFTER (s of gameplay before the trace, 20), CAPTURES (0),
# ROUTE (warm_route.sh's walk; a standing route like 'ly=128,rx=128:5' keeps the camera still,
# so captures of two runs are comparable), BB_GAME_DIR (default ~/Games/Bloodborne/CUSA03173).
# Refuses to start while any bb-probe runs (the owner may be playing; another agent's run would
# void the numbers).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
tag=${1:?usage: gpu_ab_run.sh TAG SECS [ENV=VALUE ...]}
secs=${2:?usage: gpu_ab_run.sh TAG SECS [ENV=VALUE ...]}
shift 2
if pgrep -x bb-probe >/dev/null; then
    echo "STOP: a bb-probe is running (owner or another agent); not starting" >&2
    exit 2
fi
log=$here/out/frames-$tag.csv
rm -f "$log" out/cap-"$tag"-*.png
trigger=$here/out/bugreport_trigger
env "$@" BB_FRAME_LOG="$log" BB_ASYNC_STATIC_VI=1 BB_KK_STATS=${BB_KK_STATS:-1} \
    BB_BUGREPORT_TRIGGER="$trigger" \
    BB_GAME_DIR=${BB_GAME_DIR:-$HOME/Games/Bloodborne/CUSA03173} \
    bash tools/macos/warm_route.sh "$secs" > out/route-"$tag".txt 2>&1 &
route=$!
( while kill -0 $route 2>/dev/null; do
      for g in $(pgrep -x bb-probe); do
          c=$(ps -o command= -p "$g")
          case "$c" in *"$here"*) ;; *) echo "$(date +%T) $g ${c:0:100}" ;; esac
      done
      sleep 3
  done ) > out/overlap-"$tag".txt 2>&1 &
if [[ ${TRACE:-0} == 1 || ${CAPTURES:-0} -gt 0 ]]; then
    for _ in $(seq 1 120); do
        sleep 5
        n=$(grep '^Frame stats' out/last-run.log 2>/dev/null | tail -1 | grep -o '[0-9]* draws/frame' | cut -d' ' -f1)
        if [[ ${n:-0} -gt 300 ]]; then
            sleep "${TRACE_AFTER:-20}"
            P=""
            for g in $(pgrep -x bb-gpu); do
                case "$(ps -o command= -p "$g")" in *"$here"*) P=$g ;; esac
            done
            [[ -n $P ]] || break
            for ((i = 1; i <= ${CAPTURES:-0}; i++)); do
                before=$(wc -l < out/last-run.log)
                : > "$trigger"
                for _ in $(seq 1 40); do
                    png=$(tail -n +"$((before + 1))" out/last-run.log | sed -n 's/^Capture: wrote \(.*\.png\) (.*/\1/p' | tail -1)
                    [[ -n $png ]] && break
                    sleep 0.25
                done
                [[ -n ${png:-} && $png != /* ]] && png=$here/$png
                [[ -n ${png:-} && -f $png ]] && cp -f "$png" out/cap-"$tag"-"$i".png
                sleep "${CAPTURE_GAP:-2}"
            done
            if [[ ${TRACE:-0} == 1 ]]; then
                rm -rf out/mtl-"$tag".trace
                if [[ -n ${TRACE_SIGINT_S:-} ]]; then
                    # --time-limit has hung and left unexportable traces on this Mac; a SIGINT
                    # after a fixed time finalises reliably. Bounded: TERM after 60 s more.
                    xcrun xctrace record --template 'Metal System Trace' --attach "$P" \
                        --output out/mtl-"$tag".trace > out/mtl-"$tag".txt 2>&1 &
                    xp=$!
                    sleep "$TRACE_SIGINT_S"
                    kill -INT "$xp" 2>/dev/null
                    # A multi-GB document takes minutes to finalise: TERM before it finishes leaves
                    # "Document Missing Template Error" (a half-written .trace). Wait 300 s.
                    for _ in $(seq 1 300); do kill -0 "$xp" 2>/dev/null || break; sleep 1; done
                    if kill -0 "$xp" 2>/dev/null; then
                        echo "xctrace still up after 300 s: TERM"
                        kill -TERM "$xp" 2>/dev/null
                    fi
                    wait "$xp" 2>/dev/null
                else
                    xcrun xctrace record --template 'Metal System Trace' --attach "$P" \
                        --time-limit "${TLIMIT:-3s}" --output out/mtl-"$tag".trace > out/mtl-"$tag".txt 2>&1
                fi
            fi
            break
        fi
        kill -0 $route 2>/dev/null || break
    done
fi
wait $route
cp out/last-run.log out/run-"$tag".log
# The trace document is finalised by xctrace itself, after the SIGINT: exporting before it stops
# growing produced "Document Missing Template Error" (a 2.1 GB half-written .trace).
settle_trace() {
    local t=$1 last=-1 cur i
    for i in $(seq 1 180); do
        cur=$(du -sk "$t" 2>/dev/null | awk '{print $1}')
        if [[ -n ${cur:-} && $cur == "$last" ]]; then
            sleep 2
            return 0
        fi
        last=${cur:-0}
        sleep 1
    done
}
if [[ -d out/mtl-$tag.trace ]]; then
    settle_trace out/mtl-$tag.trace
    for i in 1 2 3; do
        if xcrun xctrace export --input out/mtl-"$tag".trace \
            --xpath '/trace-toc/run[1]/data/table[@schema="metal-gpu-intervals"]' \
            > out/mtl-"$tag"-gpu.xml 2> out/mtl-"$tag"-export.err; then
            [[ -s out/mtl-$tag-gpu.xml ]] && break
        fi
        echo "export attempt $i failed: $(head -2 out/mtl-$tag-export.err | tr '\n' ' ')"
        sleep 5
    done
fi
echo "done $tag (overlap lines: $(wc -l < out/overlap-"$tag".txt))"
