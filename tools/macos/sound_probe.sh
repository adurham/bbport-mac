#!/usr/bin/env bash
# Warm sound-diagnostic run: hidden window, timer sink, WAV dump of what the game
# would send to the device. Never launches the visible game.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
export PATH=/opt/homebrew/bin:$PATH
export BB_GAME_DIR=${BB_GAME_DIR:-/Users/adam.durham/Games/Bloodborne/CUSA03173}
export BB_NATIVE_GPU=1
export BB_WARM_ONLY=1
export BB_WARM_SECONDS=${BB_WARM_SECONDS:-90}
export BB_AUDIO_STATS=1
export BB_AUDIO_DUMP=${BB_AUDIO_DUMP:-$PWD/out/audio-dump}
# BB_AUDIO_DEVICE=1 keeps the hidden run but feeds the real output device (audible game audio).
if [[ -n ${BB_AUDIO_DEVICE:-} ]]; then export BB_AUDIO_DEVICE; fi
if [[ -n ${BB_AUDIO_TRACE:-} ]]; then export BB_AUDIO_TRACE; fi
mkdir -p "$(dirname "$BB_AUDIO_DUMP")"
rm -f "${BB_AUDIO_DUMP}".port*
echo "=== sound-probe: $BB_AUDIO_DUMP (warm ${BB_WARM_SECONDS}s) ==="
bash tools/macos/run.sh
echo "=== sound-probe finished: exit $? ==="
