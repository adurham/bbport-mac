#!/usr/bin/env bash
# out/mc/drive.sh TAG SECS PHASES... : hidden run of THIS checkout (bbport-mfxq-build), Continue at
# the title, then the pad PHASES ("<pad tokens, comma-separated>:<seconds>", looped) once gameplay
# draws flow; BB_CAMERA_LOG=1 so the log shows whether the camera moves. DUMP_AT (s after gameplay,
# 0 = no dump) arms BB_DUMP_TRIGGER for FRAMES frames. Never kills foreign processes: waits.
set -uo pipefail
tag=${1:?tag}; secs=${2:?secs}; shift 2
q=$HOME/repos/bbport-mfxq-build
[[ -x $q/out/bb-probe ]] || { echo "STOP: not built"; exit 1; }
res=$q/out/mc; mkdir -p "$res"
exec >>"$res/drive-$tag.txt" 2>&1
echo "=== drive $tag $(date +%T) checkout $q @ $(git -C "$q" rev-parse --short HEAD)+dirty secs=$secs phases=$*"
while pgrep -x bb-probe >/dev/null || pgrep -x bb-gpu >/dev/null; do echo "$(date +%T) foreign game up; waiting"; sleep 10; done
pad=$res/pad-$tag; : > "$pad"
rm -rf "$res/dump-$tag"; mkdir -p "$res/dump-$tag"
dtrig=$res/trig-$tag; rm -f "$dtrig"
csv=$res/frames-$tag.csv; rm -f "$csv"
cd "$q" || exit 1
export BB_GAME_DIR=${BB_GAME_DIR:-$HOME/Games/Bloodborne/CUSA03173}
env BB_WARM_ONLY=1 BB_WARM_QUIET=0 BB_WARM_SECONDS=$((secs + 40)) BB_NATIVE_GPU=1 \
    BB_USER_DIR="$q/out/curve3440/user" BB_PAD_FILE="$pad" BB_FRAME_STATS=1 BB_CAMERA_LOG=1 \
    BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_OUTPUT_RES=3440x1440 BB_FRAME_GEN=0 \
    BB_DUMP_TRIGGER="$dtrig" BB_DUMP_DIR="$res/dump-$tag" BB_DUMP_FRAMES=${FRAMES:-10} \
    BB_FRAME_LOG="$csv" ${EXTRA_ENV:-} \
    bash tools/macos/run.sh > "$res/route-$tag.txt" 2>&1 < /dev/null &
game=$!
echo "game pid $game"
sleep 5
mine() { for p in $(pgrep -x bb-gpu) $(pgrep -x bb-probe); do ps -o command= -p "$p" 2>/dev/null | grep -q "$q" && echo "$p"; done; }
(   # watchdog: footprint > 19 GiB or free+inactive < 4 GiB
    while kill -0 $game 2>/dev/null; do
        page=$(vm_stat | awk '/page size of/{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/){print $i; exit}}')
        fr=$(vm_stat | awk '/Pages free:/{v=$3; gsub(/\./,"",v); print v}')
        ina=$(vm_stat | awk '/Pages inactive:/{v=$3; gsub(/\./,"",v); print v}')
        mib=$(( (fr + ina) * page / 1048576 )); foot=0
        for p in $(pgrep -x bb-gpu); do
            ps -o command= -p "$p" | grep -q "$q" || continue
            foot=$(/usr/bin/footprint -p "$p" 2>/dev/null | awk '/Footprint:/{for(i=1;i<=NF;i++) if($i=="Footprint:"){v=$(i+1); u=$(i+2)}; if(u~/GB/){v*=1024} else if(u~/KB/){v/=1024} printf "%.0f", v; exit}')
        done
        echo "$(date +%s) foot_mib=${foot:-0} freeinactive_mib=$mib" >> "$res/foot-$tag.txt"
        if [[ ${foot:-0} -gt 19456 || $mib -lt 4096 ]]; then
            echo "$(date) WATCHDOG foot=$foot free+inactive=$mib; killing" | tee -a "$res/foot-$tag.txt"
            for p in $(mine); do kill "$p"; done
        fi
        sleep 10
    done ) &
wd=$!
maxdraws() { tail -1 "$csv" 2>/dev/null | awk -F, '{printf "%d", $16}'; }
start=$SECONDS; hits=0; d=0
while (( SECONDS - start < 150 )); do
    d=$(maxdraws)
    if [[ -n ${d:-} && $d -ge 500 ]]; then hits=$((hits + 1)); (( hits >= 3 )) && break; else hits=0; fi
    printf 'cross\n' > "$pad"; sleep 1.0; : > "$pad"; sleep 2.0
    kill -0 $game 2>/dev/null || break
done
echo "gameplay at ${d:-0} draws/frame after $((SECONDS - start)) s; settle ${SETTLE:-8} s"
sleep "${SETTLE:-8}"
gp=$SECONDS; armed=0
end=$((SECONDS + secs))
while (( SECONDS < end )) && kill -0 $game 2>/dev/null; do
    for entry in "$@"; do
        st=${entry%:*}; printf '%s\n' "${st//,/ }" > "$pad"
        echo "$(date +%T) pad: ${st//,/ }"
        t=${entry##*:}; t0=$SECONDS
        while (( SECONDS - t0 < ${t%.*} )); do
            if (( armed == 0 && ${DUMP_AT:-0} > 0 && SECONDS - gp >= ${DUMP_AT:-0} )); then
                : > "$dtrig"; armed=1; echo "$(date +%T) dump armed during: $st"
            fi
            sleep 1
        done
        (( SECONDS < end )) || break
    done
done
: > "$pad"
for p in $(mine); do kill "$p" 2>/dev/null; done
sleep 4
for p in $(mine); do kill -9 "$p" 2>/dev/null; done
kill $wd 2>/dev/null
cp -f "$q/out/last-run.log" "$res/run-$tag.log"
log=$res/run-$tag.log
echo "tag=$tag max_draws=$(maxdraws) dumps=$(ls "$res/dump-$tag" | wc -l | tr -d ' ') camera_lines=$(grep -c '^Camera: ' "$log") dump_cam=$(grep -c '^Dump camera' "$log") gcn422=$(grep -c 'gcn_to_vk.cpp:422' "$log") pm4_203=$(grep -cE 'cp_graphics.cpp:203|Bad PM4' "$log")"
echo "=== done $tag $(date +%T)"
