#!/usr/bin/env bash
# tools/macos/kkair_run.sh TAG SECS DRIVER [ENV=VALUE ...]: one hidden sizing run for the KK AIR +
# MTL4 archive work (patch 0008, docs/KK_AIR_ARCHIVE.md), with every cache that matters isolated:
#
#   DRIVER   base  = the 0001-0007 driver (deps-kkroot-base), i.e. today's driver with the timers
#            0008  = the 0001-0008 driver (deps-kkroot)
#   METAL=cold (default) deletes Metal's own compile cache for the GPU process's bundle id
#            (out/gpu-arm64/bb-gpu.app is io.github.bbport.kkair, so nothing else's cache is touched);
#            METAL=keep leaves it.
#   MESA=<dir> the KosmicKrisp (Mesa) disk cache the run uses (MESA_SHADER_CACHE_DIR); MESA_SEED=<dir>
#            copies that directory in first (the "shipped Mesa cache"); default an empty one.
#   STORE=empty (default) | copy, as sizing_run.sh.
#   CAPTURES="200 230 260 290": seconds after the start at which the port's own frame capture is
#            requested (BB_BUGREPORT_TRIGGER), into out/kkair/TAG/captures.
#
# Before starting it waits for any other bb-probe/bb-gpu (10 polls, 60 s apart) and stops if one is
# still running. The run itself is tools/macos/sizing_run.sh (hidden, BB_WARM_ONLY, sandboxed user dir,
# footprint watchdog), whose output directory is moved to out/kkair/TAG afterwards.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
tag=${1:?usage: kkair_run.sh TAG SECS DRIVER [ENV=VALUE ...]}
secs=${2:?}
driver=${3:?}
shift 3
case $driver in
    base) deps=$here/deps-arm64-base ;;
    0008) deps=$here/deps-arm64 ;;
    *) echo "DRIVER must be base or 0008" >&2; exit 2 ;;
esac
out=$here/out/kkair/$tag
mkdir -p "$here/out/kkair"
rm -rf "$out"
mkdir -p "$out"

for i in $(seq 1 10); do
    if pgrep -x bb-probe >/dev/null || pgrep -x bb-gpu >/dev/null; then
        echo "$(date +%T) busy (poll $i): $(pgrep -fl 'bb-probe|bb-gpu' | cut -c1-120 | head -2)"
        sleep 60
    else
        break
    fi
done
if pgrep -x bb-probe >/dev/null || pgrep -x bb-gpu >/dev/null; then
    echo "STOP: another bb-probe/bb-gpu still running after 10 polls" | tee "$out/STOPPED"
    exit 3
fi

metal_cache="$(getconf DARWIN_USER_CACHE_DIR)io.github.bbport.kkair"
if [[ ${METAL:-cold} == cold ]]; then
    rm -rf "$metal_cache"
fi
mesa=${MESA:-$out/mesa}
rm -rf "$mesa"
mkdir -p "$mesa"
if [[ -n ${MESA_SEED:-} ]]; then
    cp -cR "$MESA_SEED"/. "$mesa"/
fi
echo "kkair $tag: driver=$driver deps=$deps metal=${METAL:-cold} mesa=$mesa seed=${MESA_SEED:-none} store=${STORE:-empty} env: $*" | tee "$out/config.txt"
du -sk "$mesa" "$metal_cache" 2>/dev/null | tee -a "$out/config.txt"

trigger=$out/trigger
mkdir -p "$out/captures"
(
    start=$SECONDS
    for t in ${CAPTURES:-200 230 260 290}; do
        while ((SECONDS - start < t)); do sleep 1; done
        : >"$trigger"
        echo "$(date +%T) capture requested at ${t}s" >>"$out/captures.txt"
    done
) &
cap=$!

env "$@" \
    BB_GPU_DEPS="$deps" MESA_SHADER_CACHE_DIR="$mesa" \
    BB_BUGREPORT_TRIGGER="$trigger" BB_CAPTURE_DIR="$out/captures" \
    BB_GAME_DIR=${BB_GAME_DIR:-$HOME/Games/Bloodborne/CUSA03173} \
    BB_WARM_SOURCE_USER=${BB_WARM_SOURCE_USER:-$HOME/repos/bbport-mac-native/user} \
    bash tools/macos/sizing_run.sh "kkair-$tag" "$secs" 2>&1 | tee "$out/sizing.txt"
status=${PIPESTATUS[0]}
kill $cap 2>/dev/null
wait $cap 2>/dev/null
src=$here/out/sizing/kkair-$tag
for f in "$src"/*; do
    [[ $(basename "$f") == user ]] && continue
    mv "$f" "$out"/
done
ls "$src/user/cache" >"$out/store_dirs.txt" 2>/dev/null
rm -rf "$src"
du -sk "$mesa" "$metal_cache" 2>/dev/null | tee -a "$out/config.txt"
pgrep -fl 'bb-gpu|bb-probe' && { echo "leftover process; stopping"; pkill -x bb-gpu; pkill -x bb-probe; }
echo "kkair $tag done: exit $status"
exit $status
