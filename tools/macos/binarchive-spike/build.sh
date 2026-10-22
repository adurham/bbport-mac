#!/usr/bin/env bash
# Build the binarchive spike into out/binarchive-spike/ (standalone; links only Metal/Foundation,
# touches nothing of bb-gpu or the installed KosmicKrisp).
#
# The embedded Info.plist gives the tool its own bundle identifier, so Metal keeps its compile
# cache for it in $(getconf DARWIN_USER_CACHE_DIR)io.github.bbport.binarchive-spike/ and not in
# the shared per-user one: run.sh can delete that directory for a truly cold run without touching
# the cache the game (or any other tool) uses.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
o=out/binarchive-spike
mkdir -p "$o"
clang -fobjc-arc -O2 -arch arm64 -mmacosx-version-min=27.0 \
    -framework Foundation -framework Metal \
    -Wl,-sectcreate,__TEXT,__info_plist,tools/macos/binarchive-spike/Info.plist \
    tools/macos/binarchive-spike/spike.m -o "$o/spike"
echo "built $o/spike"
