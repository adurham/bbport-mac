#!/usr/bin/env bash
# tools/macos/kk_preload_install.sh [SRC] [DEST]: copy the KosmicKrisp preload artefacts (patch 0008,
# docs/KK_AIR_ARCHIVE.md "Shipping: the kk_preload switch") from a capture into the place the game
# looks for them, and stamp them with the driver they were captured against.
#
#   SRC   the capture's artefact dir (default out/kkair/artefacts): air/, base.archive.dedup.d/, mesa/
#   DEST  default ~/Games/Bloodborne/kk-artefacts (BB_KK_PRELOAD_DIR in tools/macos/run.sh)
#   KK_DRIVER  the driver dylib the Mesa cache belongs to (default: deps-kkroot's, the capture driver)
#
# What is copied: the AIR libraries (*.metallib + manifest.txt; the .metal sources stay in SRC), the
# DE-DUPLICATED archive segments (base.archive.dedup.d, never passA/: runtime-keyed captures never
# hit), and the Mesa disk cache. The driver's LC_UUID goes to DEST/DRIVER_UUID: Mesa keys its cache on
# it, so run.sh only uses mesa/ when the driver it is about to load has that UUID.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
src=${1:-$PWD/out/kkair/artefacts}
dest=${2:-$HOME/Games/Bloodborne/kk-artefacts}
driver=${KK_DRIVER:-$PWD/deps-kkroot/lib/kosmickrisp/libvulkan_kosmickrisp.dylib}
for d in air base.archive.dedup.d mesa/mesa_shader_cache; do
    [[ -d $src/$d ]] || { echo "STOP: $src/$d missing" >&2; exit 1; }
done
[[ -f $driver ]] || { echo "STOP: driver $driver missing" >&2; exit 1; }
uuid=$(otool -l "$driver" | awk '/LC_UUID/ {f=1} f && $1 == "uuid" {print $2; exit}')
[[ -n $uuid ]] || { echo "STOP: no LC_UUID in $driver" >&2; exit 1; }

tmp=$dest.tmp.$$
rm -rf "$tmp"
mkdir -p "$tmp/air" "$tmp/archive" "$tmp/mesa"
cp -c "$src"/air/*.metallib "$tmp/air/"
cp "$src/air/manifest.txt" "$tmp/air/"
cp -c "$src"/base.archive.dedup.d/*.mtl4archive "$tmp/archive/"
cp -cR "$src/mesa/mesa_shader_cache" "$tmp/mesa/"
echo "$uuid" >"$tmp/DRIVER_UUID"
shasum -a 256 "$driver" | cut -d' ' -f1 >"$tmp/DRIVER_SHA256"
[[ -f $dest/README.md ]] && cp "$dest/README.md" "$tmp/"
[[ -f docs/KK_PRELOAD_ARTEFACTS.md && ! -f $tmp/README.md ]] && cp docs/KK_PRELOAD_ARTEFACTS.md "$tmp/README.md"
rm -rf "$dest.old"
[[ -d $dest ]] && mv "$dest" "$dest.old"
mv "$tmp" "$dest"
rm -rf "$dest.old"
n_air=$(find "$dest/air" -name '*.metallib' | wc -l | tr -d ' ')
n_seg=$(find "$dest/archive" -name '*.mtl4archive' | wc -l | tr -d ' ')
echo "KK preload artefacts in $dest: $n_air AIR libraries ($(du -sh "$dest/air" | cut -f1)), $n_seg archive segments ($(du -sh "$dest/archive" | cut -f1)), Mesa cache ($(du -sh "$dest/mesa" | cut -f1)) for driver $uuid"
