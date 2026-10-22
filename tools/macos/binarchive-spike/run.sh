#!/usr/bin/env bash
# The binarchive spike's measurement matrix. Every step is a fresh process; "cold" deletes the
# spike's own Metal compile cache first ($DARWIN_USER_CACHE_DIR/io.github.bbport.binarchive-spike,
# private to this tool thanks to its embedded bundle id, so nothing the game uses is touched).
# Waits for bb-probe/bb-gpu to be idle before every step (another session may be benchmarking).
#
#   bash tools/macos/binarchive-spike/build.sh
#   python3 tools/macos/binarchive-spike/extract_msl.py out/binarchive-spike/msl 20 7
#   bash tools/macos/binarchive-spike/run.sh            # -> out/binarchive-spike/results/*.csv
set -uo pipefail
cd -- "$(dirname -- "$0")/../../../out/binarchive-spike"
mkdir -p results lib arc
r=results
cache="$(getconf DARWIN_USER_CACHE_DIR)io.github.bbport.binarchive-spike"

wait_idle() {
    for _ in $(seq 1 10); do
        if pgrep -x bb-probe >/dev/null || pgrep -x bb-gpu >/dev/null; then sleep 60; else return 0; fi
    done
    echo "STOP: bb-probe/bb-gpu still running after 10 polls" >&2
    exit 3
}
cold() { rm -rf "$cache"; }
step() {   # step NAME [ENV=..] -- spike args
    local name=$1; shift
    wait_idle
    env "$@" >"$r/$name.csv" 2>"$r/$name.err"
    printf '%-24s %s\n' "$name" "$(grep SUMMARY "$r/$name.csv" | sed 's/SUMMARY //')"
    grep -v '^$' "$r/$name.err" | head -3
}

A=a$(date +%s)   # a source text Metal has never seen
B=b$(date +%s)
C=c$(date +%s)

# Offline AIR libraries (front end done at build time), for the lib* modes.
if [[ ! -f lib/19.fs.metallib ]]; then
    for f in msl/*.metal; do
        xcrun metal -std=metal4.1 -ffast-math -c "$f" -o "lib/$(basename "$f" .metal).air" &&
        xcrun metallib "lib/$(basename "$f" .metal).air" -o "lib/$(basename "$f" .metal).metallib"
    done
fi

# 1. baseline: cold, then the same source again (Metal's own cache warm)
cold; step plain_cold         ./spike plain msl "$A"
      step plain_warm         ./spike plain msl "$A"
# 2. build an archive from source B (a cold capture is also a cold baseline)
cold; step capture_B          ./spike capture msl "$B" -o "$PWD/arc/B.mtl4archive"
ls -l arc/B.mtl4archive
# 3. new process, Metal cache cold, same MSL text B: lookupArchives, and PSO straight from the archive
cold; step lookup_B           ./spike lookup  msl "$B" arc/B.mtl4archive
cold; step archive_B          ./spike archive msl "$B" arc/B.mtl4archive
# 4. archive B against different MSL text C (one comment line differs): does it still hit?
cold; step lookup_C_with_B    ./spike lookup  msl "$C" arc/B.mtl4archive
# 5. precompiled AIR (.metallib) libraries: back end only, no archive / with an archive
cold; step lib_cold           ./spike libonly msl - -lib lib
cold; step libcapture         ./spike libcapture msl - -lib lib -o "$PWD/arc/L.mtl4archive"
cold; step liblookup          ./spike liblookup  msl - -lib lib arc/L.mtl4archive
cold; step libarchive         ./spike libarchive msl - -lib lib arc/L.mtl4archive
# 6. incremental: two archives (pipelines 0-9 and 10-19) captured separately, looked up together
cold; step lib_cap_lo SPIKE_RANGE=0:9   ./spike libcapture msl - -lib lib -o "$PWD/arc/L_lo.mtl4archive"
cold; step lib_cap_hi SPIKE_RANGE=10:19 ./spike libcapture msl - -lib lib -o "$PWD/arc/L_hi.mtl4archive"
cold; step liblookup_two      ./spike liblookup msl - -lib lib arc/L_lo.mtl4archive arc/L_hi.mtl4archive
cold; step liblookup_lo_only  ./spike liblookup msl - -lib lib arc/L_lo.mtl4archive
ls -l arc lib/*.metallib | awk '{s+=$5} END {print "bytes listed:", s}'
du -sh arc lib
