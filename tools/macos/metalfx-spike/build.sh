#!/bin/bash
# Builds the MetalFX tail spike (standalone; does not touch the port).
#   spike_metal : pure Metal, KK-style textures                       (no deps)
#   spike_vk    : Vulkan on KosmicKrisp + vkExportMetalObjectsEXT -> MetalFX  (needs KK_DIR
#                 = a KosmicKrisp build carrying patch 0007; see README.md)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/build"
mkdir -p "$out"
repo="$(cd "$here/../../.." && pwd)"
CXX="xcrun clang++ -std=c++20 -O2 -fobjc-arc -arch arm64 -mmacosx-version-min=26.0"
$CXX "$here/spike_metal.mm" -o "$out/spike_metal" \
    -framework Foundation -framework Metal -framework MetalFX
echo "built $out/spike_metal"
if [[ -f "$here/spike_vk.mm" ]]; then
    VKINC="${VKINC:-$repo/deps-arm64/src/mesa-kosmickrisp/externals/mesa/include}"
    LOADER="${LOADER:-$repo/deps-arm64/lib}"
    [[ -d $VKINC/vulkan ]] || VKINC="$HOME/repos/bbport-motionfast-build/deps-arm64/src/mesa-kosmickrisp/externals/mesa/include"
    [[ -f $LOADER/libvulkan.1.dylib ]] || LOADER="$HOME/repos/bbport-motionfast-build/deps-arm64/lib"
    $CXX "$here/spike_vk.mm" -o "$out/spike_vk" -I"$VKINC" -DVK_USE_PLATFORM_METAL_EXT \
        -L"$LOADER" -lvulkan -Wl,-rpath,"$LOADER" \
        -framework Foundation -framework Metal -framework MetalFX -framework QuartzCore
    echo "built $out/spike_vk"
fi
