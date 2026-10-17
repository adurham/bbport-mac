#!/usr/bin/env bash
# examples/game_lib/build.sh: builds the no-op game library (BB_GAME_LIB, docs/GAME_LIBRARY.md).
# Output: out/game_lib/libbbgame-noop.dylib (Linux: .so). On macOS the library is x86_64: it is
# loaded by the game process (out/bb-probe, x86-64 under Rosetta), never by the arm64 bb-gpu.
#   bash examples/game_lib/build.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
mkdir -p out/game_lib
if [[ $(uname) == Darwin ]]; then
    out=out/game_lib/libbbgame-noop.dylib
    clang -arch x86_64 -mmacosx-version-min="${MACOSX_DEPLOYMENT_TARGET:-14.0}" -std=c11 -O2 -g \
        -Wall -Wextra -Werror -fvisibility=hidden -Isrc -dynamiclib \
        -install_name @rpath/libbbgame-noop.dylib examples/game_lib/noop.c -o "$out"
    lipo -archs "$out" | grep -qx x86_64 || { echo "STOP: $out is not x86_64" >&2; exit 1; }
else
    out=out/game_lib/libbbgame-noop.so
    cc -std=c11 -O2 -g -Wall -Wextra -Werror -fvisibility=hidden -fPIC -shared -Isrc \
        examples/game_lib/noop.c -o "$out"
fi
nm -g "$out" | grep -q '[[:space:]]_\{0,1\}bbgame_init$' || { echo "STOP: $out exports no bbgame_init" >&2; exit 1; }
echo "Built: $out"
