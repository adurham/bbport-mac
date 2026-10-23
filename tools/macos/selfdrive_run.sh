#!/bin/bash
# Self-driving Bloodborne run: no hands on the pad. Presses Cross until the run reaches gameplay
# (draws/frame >= 200 in out/last-run.log), stands still for STAND seconds, then stops the game and
# keeps the stats. Env exactly as tools/macos/m2ab.sh sets it, plus BB_KK_HOIST_ORACLE if given.
set -uo pipefail
cd /Users/adam.durham/repos/bbport-mac-native || exit 1
tag=${1:-sd1}
stand=${STAND:-45}
pad=/tmp/bb-pad-$tag
log=/tmp/kk-m2-$tag.err
: > "$pad"
export BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173
export BB_GPU_DEPS=$PWD/deps-arm64-probe
export BB_NATIVE_GPU=1
if [ "${BB_KK_DEFER:-1}" != 0 ]; then
    export BB_KK_ONE_CMDBUF=1 BB_KK_DEFER_RENDER=1 BB_KK_DEFER_HOIST=1
fi
export BB_KK_STATS=1
export BB_KK_ALLOC_LOG=/tmp/kk-m2-$tag.alloc
export BB_FRAME_STATS=1 BB_FRAME_LOG=/tmp/kk-m2-$tag.frames.csv
export BB_PAD_FILE=$pad
unset BB_KK_RENC_LOG BB_KK_FLUSH_LOG
rm -f "$log" /tmp/kk-m2-$tag.frames.csv
bash tools/macos/run.sh >"$log" 2>&1 &
game=$!
echo "selfdrive: game pid $game, log $log"
# wait for the game to boot
for _ in $(seq 1 60); do sleep 1; grep -q 'Frame stats' "$log" 2>/dev/null && break; done
sleep 3
# press Cross until gameplay is reached (draws/frame >= 200)
reached=0
for i in $(seq 1 80); do
    printf 'cross\n' > "$pad"; sleep 0.8; : > "$pad"; sleep 0.6
    d=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | tail -1 | awk '{print $1}')
    if [[ ${d:-0} -ge 200 ]]; then reached=1; echo "selfdrive: gameplay at press $i ($d draws/frame)"; break; fi
done
: > "$pad"
if [[ $reached -eq 0 ]]; then echo "selfdrive: NEVER reached gameplay (last draws ${d:-?})"; fi
sleep "$stand"
# stop
pkill -TERM -f 'bbprobe|bb-probe' 2>/dev/null
sleep 8
pkill -KILL -f 'bbprobe|bb-probe' 2>/dev/null
pkill -TERM -f 'bb-gpu' 2>/dev/null
wait "$game" 2>/dev/null
echo "selfdrive: done (reached=$reached)"
grep -a 'pre-gfx hoist (BB_KK_DEFER_HOIST)' "$log" | tail -1
grep -a 'hoist oracle' "$log" | tail -1 || true
exit 0
