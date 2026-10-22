#!/usr/bin/env bash
# tools/macos/sizing_run.sh TAG SECS [ENV=VALUE ...]: one hidden warm-route run with the pipeline
# instrumentation on, into out/sizing/TAG/ (docs/PIPELINE_SIZING.md).
#
# What it writes there:
#   pipe.txt   BB_PIPE_TIMING: one line per pipeline created (its shaders, render target, draw
#              count, the async policy's verdict, the port's translating and the driver's time) and
#              one FRAME line per frame (wall time, draws, pipelines created in it)
#   kk.txt     BB_KK_TIMING: one line per graphics pipeline from KosmicKrisp itself (NIR->MSL,
#              MTLLibrary, Metal pipeline state, and whether the disk cache had the shader)
#   binds.txt  BB_SHADER_BINDS: every distinct shader program the game *bound* (SH register writes)
#   frames.csv BB_FRAME_LOG, one line per flip (frame_ms, draws, compiles, the port's own split)
#   run.log    the run's whole log; mem.txt the watchdog's samples
#
# The run is hidden and sandboxed: BB_WARM_ONLY=1 (set by warm_route.sh, not overridable) and the
# user dir is a copy under out/sizing/TAG/user, so neither the owner's saves nor his pipeline store
# are written. STORE=empty (default) starts that copy with no pipeline store, so the run compiles
# the area's pipelines from nothing — which is the population a first visit pays for; STORE=copy
# keeps the store the run was cloned from, for the steady-state question.
#
# The watchdog is the one this machine runs by (ps RSS is 5-9x low on this renderer): `footprint`
# of bb-probe above 19 GiB, or free+inactive under 4 GiB, stops the run and says so.
#
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/sizing_run.sh cold 150
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 STORE=copy bash tools/macos/sizing_run.sh warm 150
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
tag=${1:?usage: sizing_run.sh TAG SECS [ENV=VALUE ...]}
secs=${2:?usage: sizing_run.sh TAG SECS [ENV=VALUE ...]}
shift 2
if pgrep -x bb-probe >/dev/null; then
    echo "STOP: a bb-probe is running (owner or another session); not starting" >&2
    exit 2
fi
[[ -x out/gpu-arm64/bb-gpu ]] || { echo "STOP: out/gpu-arm64/bb-gpu missing; run tools/macos/build_gpu_native.sh" >&2; exit 1; }

dir=$here/out/sizing/$tag
source_user=${BB_WARM_SOURCE_USER:-$here/user}
store_mode=${STORE:-empty}
rm -rf "$dir"
mkdir -p "$dir"
# The user dir the run sees: a clone (so the saves cost nothing), minus the pipeline store unless
# asked for. BB_DATA_DIR stays inside the run's own directory: nothing is written to the owner's
# user dir, and bbport.ini is a copy (never the owner's).
if [[ -d $source_user ]]; then
    cp -cR "$source_user" "$dir/user" 2>/dev/null || cp -aR "$source_user" "$dir/user"
else
    mkdir -p "$dir/user"
fi
if [[ $store_mode == empty ]]; then
    rm -rf "$dir/user/cache"
fi
owner_ini=$HOME/Library/Application\ Support/bbport/bbport.ini
if [[ -f $owner_ini ]]; then
    cp "$owner_ini" "$dir/bbport.ini"
elif [[ -f $here/bbport.ini ]]; then
    cp "$here/bbport.ini" "$dir/bbport.ini"
fi

: >"$dir/pipe.txt"
: >"$dir/kk.txt"
: >"$dir/binds.txt"
: >"$dir/mem.txt"

# warm_route.sh reads the run's log at $PWD/out/last-run.log to know whether the title's Continue
# took and whether the area was reached; with BB_DATA_DIR sandboxed the log is written under the
# run's own directory, so point the path it reads at it.
mkdir -p "$dir/out"
ln -sfn "$dir/out/last-run.log" "$here/out/last-run.log"

echo "sizing run $tag: ${secs}s, store=$store_mode, user=$dir/user"
env "$@" \
    BB_GAME_DIR=${BB_GAME_DIR:?set BB_GAME_DIR} \
    BB_DATA_DIR="$dir" BB_USER_DIR="$dir/user" BB_CONFIG="$dir/bbport.ini" \
    BB_WARM_USER="$dir/user" BB_WARM_SOURCE_USER="$source_user" \
    BB_NATIVE_GPU=1 BB_FRAME_STATS=1 BB_FRAME_LOG="$dir/frames.csv" \
    BB_ASYNC_STATIC_VI=${BB_ASYNC_STATIC_VI:-1} \
    BB_PIPE_TIMING="$dir/pipe.txt" BB_KK_TIMING="$dir/kk.txt" BB_SHADER_BINDS="$dir/binds.txt" \
    bash tools/macos/warm_route.sh "$secs" >"$dir/route.txt" 2>&1 &
route=$!

# Watchdog. The two numbers this machine's runs are held to: footprint above 19 GiB, or
# free+inactive under 4 GiB, means the renderer is about to take the machine with it.
(
    while kill -0 $route 2>/dev/null; do
        # bb-gpu (the native renderer) is the process that holds the graphics memory; bb-probe is
        # watched only while bb-gpu does not exist yet.
        pid=$(pgrep -x bb-gpu | head -1)
        [[ -n ${pid:-} ]] || pid=$(pgrep -x bb-probe | head -1)
        stamp=$(date +%T)
        if [[ -n ${pid:-} ]]; then
            foot=$(footprint "$pid" 2>/dev/null | sed -n 's/.*Footprint: \([0-9.]*\) *\([KMG]\)B.*/\1 \2/p' | head -1)
            free_inactive=$(vm_stat | awk '/Pages free|Pages inactive/ {gsub(/\./,"",$3); sum+=$3} END {printf "%.2f", sum*16384/1073741824}')
            echo "$stamp pid=$pid footprint=$foot free_inactive_gib=$free_inactive" >>"$dir/mem.txt"
            foot_gib=$(awk -v f="${foot:-0 M}" 'BEGIN{split(f,a," "); v=a[1]+0; if (a[2]=="M") v/=1024; if (a[2]=="K") v/=1073741824; print v}')
            if awk -v a="$foot_gib" -v b="$free_inactive" 'BEGIN{exit !(a>19 || b<4)}'; then
                echo "$stamp STOP: footprint ${foot_gib} GiB / free+inactive ${free_inactive} GiB" >>"$dir/mem.txt"
                pkill -x bb-probe; pkill -x bb-gpu
                break
            fi
        fi
        sleep 5
    done
) &
watch=$!
wait $route
route_status=$?
kill $watch 2>/dev/null
wait $watch 2>/dev/null
echo "route exit $route_status; $(wc -l <"$dir/pipe.txt" | tr -d ' ') pipe lines, $(wc -l <"$dir/kk.txt" | tr -d ' ') kk lines, $(wc -l <"$dir/binds.txt" | tr -d ' ') bind lines"
pgrep -x bb-gpu >/dev/null && { echo "stopping a leftover bb-gpu"; pkill -x bb-gpu; }
exit $route_status
