#!/usr/bin/env bash
# tools/macos/m2ab.sh: one M2 (BB_KK_DEFER_HOIST) run against the probe driver.
#   bash tools/macos/m2ab.sh 1        # hoist on  -> /tmp/kk-m2-h1.*
#   bash tools/macos/m2ab.sh 0        # hoist off -> /tmp/kk-m2-h0.*  (baseline)
# Driver: deps-arm64-probe (the probe prefix). The installed app driver is never touched.
set -uo pipefail
h=${1:?usage: m2ab.sh <0|1>}
label=${2:-h$h}
cd -- "$(dirname -- "$0")/../.."

export BB_GAME_DIR=${BB_GAME_DIR:-$HOME/Games/Bloodborne/CUSA03173}
export BB_GPU_DEPS=$PWD/deps-arm64-probe
export BB_KK_ONE_CMDBUF=1
export BB_KK_DEFER_RENDER=1
export BB_KK_DEFER_HOIST=$h
export BB_KK_STATS=1
export BB_KK_ALLOC_LOG=/tmp/kk-m2-$label.alloc
export BB_KK_FLUSH_LOG=/tmp/kk-m2-$label.flush
export BB_KK_RENC_LOG=/tmp/kk-m2-$label.renc

rm -f /tmp/kk-m2-$label.alloc /tmp/kk-m2-$label.flush /tmp/kk-m2-$label.renc
echo "m2ab: hoist=$h driver=$(dwarfdump --uuid "$BB_GPU_DEPS/lib/kosmickrisp/libvulkan_kosmickrisp.dylib" | awk '{print $2}')"
echo "m2ab: logs /tmp/kk-m2-$label.{alloc,flush,renc} ; stderr below"
exec bash tools/macos/run.sh 2>&1 | tee /tmp/kk-m2-$label.err
