#!/usr/bin/env bash
# tools/macos/kkair_proof.sh [SECS]: the whole cold proof for KosmicKrisp patch 0008 (shipped AIR +
# MTL4 pipeline archive + shipped Mesa cache), in one command, hidden runs only
# (docs/KK_AIR_ARCHIVE.md). Every run goes through tools/macos/kkair_run.sh, which waits for any
# other bb-probe/bb-gpu (10 polls x 60 s) and stops instead of overlapping.
#
#   1. ctrl-base   today's driver (0001-0007), Metal cache cold, Mesa cache empty, store empty
#   2. ctrl-0008   the 0008 driver with all three env vars UNSET, same caches: must log no
#                  "KK air"/"KK archive" line and be no slower than ctrl-base
#   3. capture1    0008 + BB_KK_ARCHIVE_CAPTURE=1 + BB_KK_AIR_DIR: dumps every MSL into the AIR dir
#                  (<blake3>.metal + manifest.txt with its xcrun flags) and fills the Mesa cache
#                  that will be shipped (artefacts/mesa)
#   4. offline     tools/macos/kkair_build_air.sh: <key>.metallib for every dumped MSL
#   5. capture2    0008 with the AIR present + BB_KK_ARCHIVE_CAPTURE=artefacts/base.mtl4archive: the
#                  base archive is captured against the very AIR libraries that will be shipped
#   6. proof       0008 with BB_KK_AIR_DIR + BB_KK_ARCHIVES=base + a copy of the shipped Mesa
#                  cache, Metal cache deleted, store empty: the bar
#   7. report      tools/macos/kkair_report.py over all runs + capture comparison proof vs ctrl-base
#
# Each step's result is in out/kkair/<tag>/; the report in out/kkair/report.{txt,json}.
# STEPS="ctrl-base ctrl-0008 capture1 offline capture2 proof report" picks a subset.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
secs=${1:-300}
art=$here/out/kkair/artefacts
mkdir -p "$here/out/kkair"
log=$here/out/kkair/proof.log
steps=${STEPS:-ctrl-base ctrl-0008 capture1 offline capture2 proof report}
say() { echo "$(date +%T) $*" | tee -a "$log"; }
has() { [[ " $steps " == *" $1 "* ]]; }

if has ctrl-base; then
    bash tools/macos/kkair_run.sh ctrl-base "$secs" base >>"$log" 2>&1; say "ctrl-base exit $?"
fi
if has ctrl-0008; then
    bash tools/macos/kkair_run.sh ctrl-0008 "$secs" 0008 >>"$log" 2>&1; say "ctrl-0008 exit $?"
fi
if has capture1; then
    rm -rf "$art/air" "$art/mesa"
    mkdir -p "$art/air"
    MESA=$art/mesa bash tools/macos/kkair_run.sh capture1 "$secs" 0008 \
        BB_KK_ARCHIVE_CAPTURE=1 BB_KK_AIR_DIR="$art/air" >>"$log" 2>&1
    say "capture1 exit $?: $(ls "$art/air"/*.metal 2>/dev/null | wc -l | tr -d ' ') MSL files dumped"
fi
if has offline; then
    bash tools/macos/kkair_build_air.sh "$art/air" >>"$log" 2>&1; say "offline AIR exit $?: $(tail -1 "$log")"
fi
if has capture2; then
    # The capture is a directory of segments: every serializeAsArchiveAndFlushToURL empties the
    # serializer, so each write holds only the pipelines since the previous one (patch 0008).
    rm -rf "$art/base.mtl4archive" "$art/base.archive.d"
    MESA_SEED=$art/mesa bash tools/macos/kkair_run.sh capture2 "$secs" 0008 \
        BB_KK_AIR_DIR="$art/air" BB_KK_ARCHIVE_CAPTURE="$art/base.archive.d" >>"$log" 2>&1
    rc=$?
    # Mesa's disk cache keys on the driver's build-id, so the shipped Mesa cache must come from the
    # very driver binary that runs: capture2's (seeded with capture1's, refilled by this build).
    if [[ -d $here/out/kkair/capture2/mesa ]]; then
        rm -rf "$art/mesa" && cp -cR "$here/out/kkair/capture2/mesa" "$art/mesa"
    fi
    say "capture2 exit $rc: base archive $(ls "$art/base.archive.d"/*.mtl4archive 2>/dev/null | wc -l | tr -d ' ') segments, $(du -sk "$art/base.archive.d" 2>/dev/null | cut -f1) KB"
fi
if has proof; then
    MESA_SEED=$art/mesa bash tools/macos/kkair_run.sh proof "$secs" 0008 \
        BB_KK_AIR_DIR="$art/air" BB_KK_ARCHIVES="$art/base.archive.d" >>"$log" 2>&1
    say "proof exit $?"
fi
if has report; then
    runs=()
    for t in ctrl-base ctrl-0008 capture1 capture2 proof; do
        [[ -f $here/out/kkair/$t/pipe.txt ]] && runs+=("$here/out/kkair/$t")
    done
    python3 tools/macos/kkair_report.py "${runs[@]}" --json "$here/out/kkair/report.json" \
        --compare "$here/out/kkair/ctrl-base" "$here/out/kkair/proof" \
                  "$here/out/kkair/ctrl-base" "$here/out/kkair/ctrl-0008" | tee "$here/out/kkair/report.txt"
    {
        echo "artefact sizes on disk:"
        du -sh "$art/base.archive.d" "$art/mesa" 2>/dev/null
        echo "AIR metallibs: $(find "$art/air" -name '*.metallib' | wc -l | tr -d ' ') files, $(find "$art/air" -name '*.metallib' -exec stat -f %z {} + | awk '{s+=$1} END {printf "%.1f MB", s/1e6}')"
        echo "dumped MSL: $(find "$art/air" -name '*.metal' | wc -l | tr -d ' ') files, $(find "$art/air" -name '*.metal' -exec stat -f %z {} + | awk '{s+=$1} END {printf "%.1f MB", s/1e6}')"
    } | tee -a "$here/out/kkair/report.txt"
fi
pgrep -fl 'bb-gpu|bb-probe|xctrace' && say "WARNING: a game/trace process is still running" || say "no bb-gpu/bb-probe/xctrace left"
