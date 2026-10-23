#!/usr/bin/env bash
# tools/macos/run.sh: runs the macOS build through the normal run.sh pipeline.
#   BB_GAME_DIR=~/Games/shadPS4/CUSA03173-109 bash tools/macos/run.sh
# First-light defaults (override by setting them): 30 FPS (no frame-rate patch). The upscaler is
# the one saved in bbport.ini (overlay menu), off until one is chosen.
#
# Two layouts:
# - a checkout: deps-x86_64/ (tools/macos/setup_deps.sh) and out/bb-probe (tools/macos/build.sh);
#   bbport.ini, out/ (generated files, logs) and user/ (saves) live in the checkout.
# - the release app (tools/macos/package.sh): this script inside bbport.app/Contents/Resources/
#   bbport, next to bin/ and lib/ (marker file .packaged). Nothing is written into the app:
#   bbport.ini, out/ and user/ go to BB_DATA_DIR, by default ~/Library/Application Support/bbport.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
if [[ -f .packaged ]]; then
    export VK_DRIVER_FILES="$PWD/lib/kosmickrisp/kosmickrisp_mesa_icd.json"
    export BB_PROBE="$PWD/bin/bb-probe"
    # The app's own Vulkan loader, by path: DYLD_LIBRARY_PATH would not survive bash starting.
    export BB_VULKAN_LIBRARY="$PWD/lib/libvulkan.1.dylib" SDL_VULKAN_LIBRARY="$PWD/lib/libvulkan.1.dylib"
    export BB_DATA_DIR=${BB_DATA_DIR:-$HOME/Library/Application Support/bbport}
    # Python must not write __pycache__ into the signed app.
    export PYTHONDONTWRITEBYTECODE=1
    data=$BB_DATA_DIR
    [[ -f $VK_DRIVER_FILES ]] || { echo "STOP: KosmicKrisp missing from the app ($VK_DRIVER_FILES)" >&2; exit 1; }
    [[ -x $BB_PROBE ]] || { echo "STOP: $BB_PROBE missing from the app" >&2; exit 1; }
else
    DEPS=${BB_DEPS:-$PWD/deps-x86_64}
    # shellcheck disable=SC1091
    source "$DEPS/env.sh"
    [[ -f ${VK_DRIVER_FILES:-} ]] || { echo "STOP: KosmicKrisp missing (${VK_DRIVER_FILES:-unset}); run tools/macos/setup_deps.sh" >&2; exit 1; }
    [[ -x out/bb-probe ]] || { echo "STOP: out/bb-probe missing; run tools/macos/build.sh" >&2; exit 1; }
    export BB_PROBE="$PWD/out/bb-probe"
    data=${BB_DATA_DIR:-.}
fi
: "${BB_GAME_DIR:?set BB_GAME_DIR to the merged 1.09 game folder}"
export BB_PREBUILT=1
# No online play on the Mac yet (0.51's online module isn't built here): run.sh would otherwise
# contact the community server with BB_ONLINE=1 and the game still play offline.
unset BB_ONLINE BB_NET_MODULE
# The frame rate is the menu's setting now (bbport.ini fps_mode; hooks in the game's code): no
# forced default here, so a BB_FPS environment value still overrides for one run.
if [[ -n ${BB_FPS:-} ]]; then export BB_FPS; fi
# Memory model: 0.6 gives the new one to every GPU (run.sh). On the Mac it is untested (it needs
# the game's memory imported into the GPU driver), so the 0.3 model stays the default here;
# BB_PC_MODEL=1 tries the new one.
export BB_PC_MODEL=${BB_PC_MODEL:-0}
mkdir -p "$data/out"
# tools/macos/capture_condition.sh asks for the bug-report capture (Ctrl+Shift+C's path) by
# creating this file. (BB_CAPTURE_TRIGGER is the frame analyzer's, render/vk_frame_capture.cpp.)
export BB_BUGREPORT_TRIGGER=${BB_BUGREPORT_TRIGGER:-$(cd -- "$data" && pwd)/out/bugreport_trigger}
rm -f "$BB_BUGREPORT_TRIGGER"
# Present mode for one run (docs/macos-title-artifact.md): BB_VSYNC=1|0 or
# BB_PRESENT_MODE=fifo|immediate, else bbport.ini present_mode / vsync.
if [[ -n ${BB_VSYNC:-} ]]; then export BB_VSYNC; fi
if [[ -n ${BB_PRESENT_MODE:-} ]]; then export BB_PRESENT_MODE; fi
# FPS counter on by default, as the Linux launcher does; the overlay menu (F1, ` or §) toggles
# it and saves the choice.
ini=${BB_CONFIG:-$data/bbport.ini}
export BB_CONFIG=$ini
grep -q '^show_fps=' "$ini" 2>/dev/null || echo "show_fps=1" >> "$ini"
# Upscaler: bbport.ini, which the overlay menu saves; BB_UPSCALER=off|fsr3|taa|metalfx overrides
# it for one run. Off until chosen: FSR 3.1, the port's own default, is unproven on KosmicKrisp.
grep -q '^upscaler=' "$ini" 2>/dev/null || echo "upscaler=off" >> "$ini"
# Character motion vectors, as the Linux launcher defaults: without them temporal upscalers
# (FSR 3.1, MetalFX, TAA) smear animated characters. BB_OBJECT_MOTION=0 turns them off.
grep -q '^object_motion=' "$ini" 2>/dev/null || echo "object_motion=1" >> "$ini"
# Fullscreen: fill (the default) renders the chosen resolution and scales it to the whole screen,
# the display keeps its own mode (a resolution change never blanks it); exclusive puts the display
# itself on the output's resolution (1:1 where the panel has it, a mode switch elsewhere).
# The F1 menu's Window section, or BB_FULLSCREEN_MODE=fill|exclusive for one run.
grep -q '^fullscreen_mode=' "$ini" 2>/dev/null || echo "fullscreen_mode=exclusive" >> "$ini"
# Keep a copy of everything the port prints for diagnosis.
# Keep the previous run's log: a quick relaunch would otherwise overwrite the one to report.
log=$data/out/last-run.log
[[ -f $log ]] && mv -f "$log" "$data/out/previous-run.log"
exec > >(tee "$log") 2>&1
version=$(cat .version 2>/dev/null || git log -1 --format='%h' 2>/dev/null || echo unknown)
# Paths in the log as ~/…, so a log shared in a report doesn't show the account name.
tilde() { [[ -n ${HOME:-} && $1 == "$HOME"/* ]] && printf '~/%s' "${1#"$HOME"/}" || printf '%s' "$1"; }
echo "macOS run: $(sw_vers -productVersion), bbport $version, game $(tilde "$BB_GAME_DIR"), fps ${BB_FPS:-menu}, upscaler ${BB_UPSCALER:-from $(tilde "$ini")}"
# KosmicKrisp preload (patch 0008, docs/KK_AIR_ARCHIVE.md "Shipping"): precompiled AIR libraries,
# Metal 4 pipeline archives and a Mesa shader cache, so a first visit to an area does not compile its
# pipelines. OFF unless bbport.ini has kk_preload=1 (BB_KK_PRELOAD=0/1 overrides for one run). The
# artefacts live in BB_KK_PRELOAD_DIR (default ~/Games/Bloodborne/kk-artefacts, filled by
# tools/macos/kk_preload_install.sh). With it off nothing below sets anything: the driver sees none of
# BB_KK_AIR_DIR / BB_KK_ARCHIVES / MESA_SHADER_CACHE_DIR and takes its upstream path.
kk_preload=${BB_KK_PRELOAD:-}
if [[ -z $kk_preload && -f $ini ]]; then
    while IFS= read -r line || [[ -n $line ]]; do
        [[ $line =~ ^kk_preload=([01])$ ]] && kk_preload=${BASH_REMATCH[1]}
    done < "$ini"
fi
if [[ $kk_preload == 1 ]]; then
    kkp=${BB_KK_PRELOAD_DIR:-$HOME/Games/Bloodborne/kk-artefacts}
    kk_air=0 kk_seg=0
    if [[ -d $kkp/air ]]; then
        for f in "$kkp"/air/*.metallib; do [[ -f $f ]] && kk_air=$((kk_air + 1)); done
    fi
    if [[ -d $kkp/archive ]]; then
        for f in "$kkp"/archive/*.mtl4archive; do [[ -f $f ]] && kk_seg=$((kk_seg + 1)); done
    fi
    # The Mesa cache is keyed on the driver's build-id (its LC_UUID): only point the driver at the
    # shipped one when the driver bb-gpu loads is the build the cache was filled by.
    kk_dylib=${BB_GPU_DEPS:-$PWD/deps-arm64}/lib/kosmickrisp/libvulkan_kosmickrisp.dylib
    [[ -f .packaged ]] && kk_dylib=$PWD/lib/kosmickrisp/libvulkan_kosmickrisp.dylib
    kk_uuid=$(otool -l "$kk_dylib" 2>/dev/null | awk '/LC_UUID/ {f=1} f && $1 == "uuid" {print $2; exit}')
    kk_want=$(cat "$kkp/DRIVER_UUID" 2>/dev/null || true)
    kk_mesa="not used"
    if [[ -d $kkp/mesa/mesa_shader_cache && -n $kk_uuid && $kk_uuid == "$kk_want" ]]; then
        # The driver writes into its cache: it gets its own copy in the data dir (an APFS clone,
        # made once per driver build), so the shipped one stays as shipped and the player's own
        # pipelines are kept on top of it from then on.
        kk_cache=$data/out/kk-mesa-$kk_uuid
        if [[ ! -d $kk_cache/mesa_shader_cache ]]; then
            rm -rf "$kk_cache.tmp" && mkdir -p "$kk_cache.tmp" &&
                cp -cR "$kkp/mesa/mesa_shader_cache" "$kk_cache.tmp/" && mv "$kk_cache.tmp" "$kk_cache"
        fi
        if [[ -d $kk_cache/mesa_shader_cache ]]; then
            export MESA_SHADER_CACHE_DIR=$kk_cache
            kk_mesa="Mesa cache $(tilde "$kk_cache") (seeded from $(tilde "$kkp/mesa"), driver $kk_uuid)"
        else
            kk_mesa="Mesa cache: copying $(tilde "$kkp/mesa") to $(tilde "$kk_cache") failed; not used"
        fi
    elif [[ -d $kkp/mesa/mesa_shader_cache ]]; then
        kk_mesa="Mesa cache skipped: built for driver ${kk_want:-unknown}, this driver is ${kk_uuid:-unknown}"
    fi
    if ((kk_air > 0)); then export BB_KK_AIR_DIR=$kkp/air; fi
    if ((kk_seg > 0)); then export BB_KK_ARCHIVES=$kkp/archive; fi
    echo "KK preload: on (kk_preload=1), $(tilde "$kkp"): $kk_air AIR libraries, $kk_seg archive segments, $kk_mesa"
    ((kk_air > 0 || kk_seg > 0)) || echo "KK preload: nothing found in $(tilde "$kkp"); run tools/macos/kk_preload_install.sh"
fi
# bbport (M2, 2026-10-23): deferred command recording + pre-gfx hoisting. One switch:
#   BB_KK_DEFER=0 -> the pre-M2 path exactly (split every pre-gfx draw, no deferral).
# Measured with it on: command-allocator block 12.2 GB -> 2.6 GB, worst recording 48 -> 18 gfx Metal
# command buffers, mean 52.6 -> 56.3 fps, p95 40.7 -> 26.1 ms, 941k draws hoisted, 0 order violations,
# and the hoist oracle (decode real bindings, abort on any overlap with the pass write set) clean over
# 113,645 ops. Set BB_KK_DEFER=0 in the environment for a launcher run to fall back.
if [ "${BB_KK_DEFER:-1}" != 0 ]; then
    export BB_KK_ONE_CMDBUF=${BB_KK_ONE_CMDBUF:-1}
    export BB_KK_DEFER_RENDER=${BB_KK_DEFER_RENDER:-1}
    export BB_KK_DEFER_HOIST=${BB_KK_DEFER_HOIST:-1}
fi
exec bash run.sh "$@"
