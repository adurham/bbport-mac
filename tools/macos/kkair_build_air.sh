#!/usr/bin/env bash
# tools/macos/kkair_build_air.sh AIR_DIR [JOBS]: compile every <key>.metal KosmicKrisp dumped into
# AIR_DIR (BB_KK_ARCHIVE_CAPTURE=1 + BB_KK_AIR_DIR, patch 0008) into <key>.metallib with `xcrun metal`,
# using the exact flags the driver recorded for that key in AIR_DIR/manifest.txt (the MSL language
# version - 4.1 on macOS 27+, kk_physical_device.c - and the math modes are part of the key, so
# a library compiled with different options can never be picked up for the wrong source).
#
# Incremental: a key whose .metallib is newer than its .metal is skipped. After an MSL change
# (a KosmicKrisp or port translation change) the new MSL hashes to new keys: run a capture pass
# again (it dumps only the misses) and then this script; stale .metallib files are harmless and
# can be pruned with PRUNE=1 (removes any .metallib with no .metal next to it).
set -euo pipefail
dir=${1:?usage: kkair_build_air.sh AIR_DIR [JOBS]}
jobs=${2:-$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo 8)}
[[ -f $dir/manifest.txt ]] || { echo "STOP: $dir/manifest.txt missing (no capture pass?)" >&2; exit 1; }
xcrun -f metal >/dev/null || { echo "STOP: the Metal toolchain is missing (xcodebuild -downloadComponent MetalToolchain)" >&2; exit 1; }
export DIR=$dir
sort -u "$dir/manifest.txt" | awk 'NF >= 2' | xargs -P "$jobs" -L 1 bash -c '
    key=$0; shift
    src=$DIR/$key.metal; lib=$DIR/$key.metallib
    [[ -f $src ]] || exit 0
    [[ -f $lib && $lib -nt $src ]] && exit 0
    tmp=$(mktemp -d)
    if xcrun metal "$@" -c "$src" -o "$tmp/a.air" 2>"$tmp/err" && xcrun metallib "$tmp/a.air" -o "$tmp/a.metallib" 2>>"$tmp/err"; then
        mv "$tmp/a.metallib" "$lib"
    else
        echo "FAILED $key: $(head -3 "$tmp/err")" >&2
    fi
    rm -rf "$tmp"
'
if [[ ${PRUNE:-0} == 1 ]]; then
    for lib in "$dir"/*.metallib; do
        [[ -f ${lib%.metallib}.metal ]] || rm -f "$lib"
    done
fi
n_src=$(find "$dir" -name '*.metal' | wc -l | tr -d ' ')
n_lib=$(find "$dir" -name '*.metallib' | wc -l | tr -d ' ')
echo "AIR: $n_lib metallibs for $n_src dumped MSL files in $dir ($(du -sh "$dir" | cut -f1) on disk with the sources)"
