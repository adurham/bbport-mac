#!/usr/bin/env bash
# tools/macos/upscaler_config_bench.sh: FSR 3.1 x startup-patched render size + preset sweep
# (Stage A of docs/PERF_LOOP.md; numbers in docs/UPSCALER_CONFIG_BENCH.md).
#   bash tools/macos/upscaler_config_bench.sh 2560 live:4 patch:4
#   bash tools/macos/upscaler_config_bench.sh 3440 patch:2 patch:3 patch:4
#
# Spec: [NAME=]MODE[:PRESET]
#   live:P     FSR 3.1 preset P on the live path: guest at 1080p, host scene targets scaled at
#              run time (BB_RENDER_RES unset -- what run.sh does with live_resolution=1).
#   patch:P    FSR 3.1 preset P on the startup-patched path: the GUEST's render size is patched
#              at start to the preset's size of the output, exactly what run.sh itself exports
#              for that path (BB_RENDER_RES + BB_OUTPUT_RES). The preset number no longer sizes
#              anything on this path (the patch does): it is passed for the label and the
#              saved setting only.
#   patchoff   upscaler off, scene startup-patched to 3x: the control that separates the guest
#              culling/raster win from the FSR chain cost (the docs' 16.50 ms anchor).
#   off        upscaler off, live (unpatched scene targets).
#
# Every run: hidden warm route (BB_WARM_ONLY, set by warm_route.sh), standing camera
# (ROUTE=ly=128,rx=128:5), BB_PASS_BREAK_TRACE=1, 2 port captures, SECS default 190. The ini is
# sed'ed from /tmp/bbport.ini.pristine for the output size (never edited itself); the worktree
# bbport.ini is restored to 16:9 / 2560x1440 when the series ends.
# ONE GPU workload at a time: waits politely while another checkout's bb-probe runs (5 s cadence,
# two clean checks 10 s apart). A run whose overlap file is non-empty, that lost the route, kept
# the title's draws or read degenerate (p50 > 40 ms, e.g. the first run of a new preset compiling
# ~28 pipelines) is VOID and re-run once under TAG_r2, keeping both.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
res=${1:?usage: upscaler_config_bench.sh 2560|3440 SPEC...}
shift
specs=${@:?one or more specs}
case $res in
2560) aspect="16:9"; out="2560x1440" ;;
3440) aspect="21:9"; out="3440x1440" ;;
*) echo "STOP: res must be 2560 or 3440" >&2; exit 1 ;;
esac
secs=${SECS:-190}
log=$here/out/upscaler-bench-$res.log
: >"$log"

# The startup patch's scene size per preset (output / scale, even-rounded; docs/METALFX_MEASURE.md
# uses the same sizes). Key: "<res> p<preset>".
render_res() {
    local key="$1 p$2"
    case $key in
    "2560 p1") echo 1706x960 ;;
    "2560 p2") echo 1506x846 ;;
    "2560 p3") echo 1280x720 ;;
    "2560 p4") echo 854x480 ;;
    "2560 p5") echo 2048x1152 ;;
    "3440 p1") echo 2294x960 ;;
    "3440 p2") echo 2024x848 ;;
    "3440 p3") echo 1720x720 ;;
    "3440 p4") echo 1146x480 ;;
    "3440 p5") echo 2752x1152 ;;
    *) echo "" ;;
    esac
}

set_ini() {
    sed -e "s/^aspect_ratio=.*/aspect_ratio=$1/" -e "s/^output_res=.*/output_res=$2/" \
        /tmp/bbport.ini.pristine >bbport.ini
}
set_ini "$aspect" "$out"
echo "bench ini: aspect_ratio=$aspect output_res=$out" | tee -a "$log"
echo "series: res=$res out=$out secs=$secs specs=$specs" | tee -a "$log"
{
    echo "gpu workloads before the series:"
    pgrep -fl 'bb-probe|bb-gpu|gpu_ab_run' || echo "  none"
} | tee -a "$log"

p50_of() { # frames CSV -> steady-row p50 (draws >= 500, t_s >= 60)
    awk -F, 'NR>1 && $16+0>=500 && $1+0>=60 {print $2+0}' "$1" 2>/dev/null |
        sort -n | awk '{a[NR]=$1} END{if(NR)printf "%.2f", a[int((NR-1)*0.5)+1]}'
}

wait_quiet() {
    local tries=0
    while :; do
        if pgrep -x bb-probe >/dev/null; then
            tries=$((tries + 1))
            ((tries > 400)) && return 1
            sleep 5
            continue
        fi
        sleep 10
        pgrep -x bb-probe >/dev/null && continue # two clean checks 10 s apart
        return 0
    done
}

one_run() { # TAG ENVSTRING
    local tag=$1 envs=$2
    wait_quiet || { echo "SKIP $tag: another checkout's bb-probe still running" | tee -a "$log"; return; }
    echo "=== RUN $tag ($envs) ===" | tee -a "$log"
    # shellcheck disable=SC2086
    env BB_GAME_DIR="$HOME/Games/Bloodborne/CUSA03173" \
        BB_WARM_SOURCE_USER="$HOME/repos/bbport-mac-native/user" \
        ROUTE='ly=128,rx=128:5' CAPTURES=2 BB_PASS_BREAK_TRACE=1 \
        bash tools/macos/gpu_ab_run.sh "$tag" "$secs" $envs >>"$log" 2>&1
    local draws took overlaps p50 verdict=OK
    draws=$(grep -oE '[0-9]+ draws/frame' out/last-run.log | sort -n | tail -1 | cut -d' ' -f1)
    took=$(grep -c "Continue took" out/route-"$tag".txt 2>/dev/null || true)
    overlaps=$(wc -l <out/overlap-"$tag".txt | tr -d ' ')
    p50=$(p50_of "out/frames-$tag.csv")
    if [[ -z ${p50:-} ]]; then verdict=VOID; fi
    if ((overlaps != 0)); then verdict=VOID; fi
    if ((${took:-0} < 1)); then verdict=VOID; fi
    if ((${draws:-0} < 500)); then verdict=VOID; fi
    if [[ -n ${p50:-} ]] && awk -v p="$p50" 'BEGIN{exit !(p+0 > 40)}'; then verdict=VOID; fi
    {
        echo "VERDICT $tag: draws=${draws:-0} took=${took:-0} overlap_lines=$overlaps p50=${p50:-?} $verdict"
        grep -E '^(Patches: scene|Scene resolution:|Output resolution:|UI: native|Camera motion: scene render size|Upscaler: FSR 3 context|Upscaler: FSR 3.1 available)' \
            out/run-"$tag".log 2>/dev/null | sed 's/^/  /' || true
    } | tee -a "$log"
    [[ $verdict == OK ]] || echo "VOID $tag (re-run once as ${tag}_r2)" | tee -a "$log"
}

for spec in $specs; do
    name=""
    if [[ $spec == *=* ]]; then name=${spec%%=*}; spec=${spec#*=}; fi
    case $spec in
    live:*)
        preset=${spec#live:}
        tag="${res}_${name:-live_fsr3_$preset}"
        envs="BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=$preset" ;;
    patch:*)
        preset=${spec#patch:}
        rr=$(render_res "$res" "$preset")
        [[ -n $rr ]] || { echo "STOP: no startup-patch size for $res p$preset" >&2; exit 1; }
        tag="${res}_${name:-patch_fsr3_$preset}"
        envs="BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=$preset BB_RENDER_RES=$rr BB_OUTPUT_RES=$out" ;;
    patchoff)
        rr=$(render_res "$res" 4)
        tag="${res}_${name:-patch_off}"
        envs="BB_UPSCALER=off BB_RENDER_RES=$rr BB_OUTPUT_RES=$out" ;;
    off)
        tag="${res}_${name:-off}"
        envs="BB_UPSCALER=off" ;;
    *)
        echo "STOP: unknown spec $spec" >&2
        exit 1 ;;
    esac
    one_run "$tag" "$envs"
    if grep -q "^VOID $tag " "$log"; then
        one_run "${tag}_r2" "$envs"
    fi
done

# Leave the worktree's ini as the 2560x1440 / 16:9 one the rest of the work expects.
set_ini "16:9" "2560x1440"
{
    echo "gpu workloads after the series:"
    pgrep -fl 'bb-probe|bb-gpu|gpu_ab_run' || echo "  none"
} | tee -a "$log"
echo "series exit: ini restored to aspect_ratio=16:9 output_res=2560x1440" | tee -a "$log"
