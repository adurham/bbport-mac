#!/usr/bin/env bash
# tools/macos/metalfx_matrix.sh: MetalFX/scene-scale hidden A/B matrix (docs/METALFX_MEASURE.md).
#   [SECS=140] bash tools/macos/metalfx_matrix.sh 3440 off2 mfx1 fsr3_5
# Spec names: off<N> = upscaler off; mfx{1,2,3,4,5} = MetalFX preset (1 Quality, 2 Balanced,
# 3 Performance, 4 Ultra Performance, 5 Ultra Quality); fsr3_<n> = FSR 3.1 at preset <n>
# (the same scene scale MetalFX gives, but in-process: no exportable-buffer copies, has RCAS).
# Every run: hidden warm route, standing camera (ROUTE=ly=128,rx=128:5), TRACE=1 + CAPTURES=2,
# SECS default 190 (SECS env overrides), alternated with off runs. Waits politely while another
# checkout's bb-probe runs (5 s cadence, two clean checks 10 s apart); a run refused five times
# in a row is skipped, not fatal; void verdicts name the tags to re-run.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
res=${1:?usage: metalfx_matrix.sh 2560|3440 SPEC...}   # 2560 or 3440
shift
specs=${@:?one or more specs}
case $res in
2560) aspect="16:9"; out="2560x1440" ;;
    3440) aspect="21:9"; out="3440x1440" ;;
    *) echo "STOP: res must be 2560 or 3440" >&2; exit 1 ;;
esac
secs=${SECS:-190}
# The ini the warm run reads is this checkout's (Settings: ./bbport.ini). The output must sit in
# the aspect's list: with 21:9, 2560x1440 snaps back to 3440x1440 (NearestOutput picks by height).
sed -e "s/^aspect_ratio=.*/aspect_ratio=$aspect/" -e "s/^output_res=.*/output_res=$out/" \
    /tmp/bbport.ini.pristine > bbport.ini
echo "matrix ini: aspect_ratio=$aspect output_res=$out"
for spec in $specs; do
    case $spec in
    off*) env="BB_UPSCALER=off" ;;
    mfx*) env="BB_UPSCALER=metalfx BB_UPSCALE_PRESET=${spec#mfx}" ;;
    fsr3_*) env="BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=${spec#fsr3_}" ;;
    *) echo "STOP: unknown spec $spec" >&2; exit 1 ;;
    esac
    tag="${res}_${spec}"
    tries=0
    while : ; do
        if pgrep -x bb-probe >/dev/null; then
            tries=$((tries + 1))
            ((tries > 400)) && { echo "SKIP $tag: another checkout's bb-probe still running"; break; }
            sleep 5
            continue
        fi
        sleep 10
        pgrep -x bb-probe >/dev/null && continue   # require two clean checks 10 s apart
        break
    done
    [[ $tries -gt 400 ]] && continue
    echo "=== RUN $tag ($env) ==="
    env BB_GAME_DIR="$HOME/Games/Bloodborne/CUSA03173" \
        BB_WARM_SOURCE_USER="$HOME/repos/bbport-mac-native/user" \
        ROUTE='ly=128,rx=128:5' TRACE=1 CAPTURES=2 \
        bash tools/macos/gpu_ab_run.sh "$tag" "$secs" $env
    draws=$(grep -oE '[0-9]+ draws/frame' out/last-run.log | sort -n | tail -1 | cut -d' ' -f1)
    scene=$(grep -E '^Scene resolution' out/last-run.log | head -1)
    overlaps=$(wc -l < out/overlap-"$tag".txt | tr -d ' ')
    took=$(grep -c "Continue took" out/route-"$tag".txt || true)
    echo "VERDICT $tag: draws=$draws took=$took overlap_lines=$overlaps"
    echo "  $scene"
    [[ $overlaps -eq 0 && $took -ge 1 && ${draws:-0} -ge 500 ]] || echo "VOID $tag (re-run needed)"
done