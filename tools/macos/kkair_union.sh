#!/usr/bin/env bash
# tools/macos/kkair_union.sh: the union capture over the warm route pack, with one route held out,
# and the held-out proof (docs/KK_AIR_ARCHIVE.md, "Union capture and generalisation").
#
# Why two capture passes: an MTL4 archive entry is keyed on the function hash of the MTLLibrary the
# pipeline was built from, and a library compiled at run time from MSL does NOT hash like the xcrun
# metallib of the same MSL (checked in isolation: runtime-captured entries miss when the metallib is
# loaded, metallib-captured entries hit). So a pipeline is only captured usefully once every one of
# its libraries comes from the shipped AIR:
#   pass A  every capture route with BB_KK_AIR_DIR + capture: dumps the MSL the AIR lacks (the archive
#           segments of this pass are runtime-keyed and are kept apart, never shipped)
#   offline kkair_build_air.sh over the union of dumped MSL (PRUNE=1)
#   pass B  every capture route again, AIR loaded, BB_KK_ARCHIVES=<shipped segment dir> and capture
#           appending into that same dir: segments now hold AIR-keyed pipelines; the run's own
#           "created in < 1 ms" count is the pipelines the earlier segments already covered
#   held-out route (never captured): ctrl (env unset, Metal cold, Mesa empty), warm (owner's store
#           copied, env unset, Metal kept), proof (three artefacts, Metal cold, empty store)
# The Mesa cache is chained through every run of the same 0008 driver build and shipped from the last.
#
#   HELDOUT=roam CAPTURE_ROUTES="yharnam attack combat spin sweep interact warp" \
#   STEPS="passA offline passB heldout" bash tools/macos/kkair_union.sh
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
here=$PWD
art=$here/out/kkair/artefacts
heldout=${HELDOUT:-roam}
routes=${CAPTURE_ROUTES:-yharnam attack combat spin sweep interact warp}
steps=${STEPS:-passA offline passB heldout}
log=$here/out/kkair/union.log
mkdir -p "$art/passA" "$art/base.archive.d"
say() { echo "$(date +%T) $*" | tee -a "$log"; }
has() { [[ " $steps " == *" $1 "* ]]; }
route_field() {  # NAME FIELD(2=secs 3=title 4=load 5=pad)
    sed -n "/^builtin_routes='/,/^'/p" tools/macos/warmfill.sh | awk -F'|' -v n="$1" -v f="$2" '$1 == n {print $f}'
}
nmetal() { find "$art/air" -name '*.metal' | wc -l | tr -d ' '; }
nseg() { ls "$art/base.archive.d"/*.mtl4archive 2>/dev/null | wc -l | tr -d ' '; }
run_route() {  # TAG ROUTE [STORE] [METAL] -- ENV...
    local tag=$1 name=$2; shift 2
    local secs title load pad
    secs=$(route_field "$name" 2); title=$(route_field "$name" 3); load=$(route_field "$name" 4); pad=$(route_field "$name" 5)
    [[ -n $secs ]] || { say "no route $name"; return 2; }
    ROUTE=$pad TITLE_WAIT=$title LOAD_WAIT=$load bash tools/macos/kkair_run.sh "$tag" "$secs" 0008 "$@" >>"$log" 2>&1
}
chain_mesa() {  # TAG: the run's Mesa cache becomes the shipped one (same driver build)
    [[ -d $here/out/kkair/$1/mesa ]] && { rm -rf "$art/mesa"; cp -cR "$here/out/kkair/$1/mesa" "$art/mesa"; }
}
summary() {  # TAG
    local d=$here/out/kkair/$1
    echo "$1: pipe=$(grep -c '^PIPE' "$d/pipe.txt" 2>/dev/null) draws_max=$(grep -oE '[0-9]+ draws/frame' "$d/out/last-run.log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1) wd=$(grep -c STOP "$d/mem.txt" 2>/dev/null) | $(grep '^KK air:' "$d/out/last-run.log" 2>/dev/null | tail -1)"
}

if has passA; then
    for r in $routes; do
        before=$(nmetal)
        mkdir -p "$art/passA/$r"
        MESA_SEED=$art/mesa run_route "A-$r" "$r" \
            BB_KK_AIR_DIR="$art/air" BB_KK_ARCHIVE_CAPTURE="$art/passA/$r"
        rc=$?
        chain_mesa "A-$r"
        say "passA $r exit $rc: MSL $before -> $(nmetal) (+$(($(nmetal) - before))) | $(summary "A-$r")"
        [[ -f $here/out/kkair/A-$r/STOPPED ]] && { say "STOP: machine busy"; exit 3; }
    done
fi
if has offline; then
    PRUNE=1 bash tools/macos/kkair_build_air.sh "$art/air" >>"$log" 2>&1
    say "offline AIR exit $?: $(tail -1 "$log")"
fi
if has passB; then
    for r in $routes; do
        before=$(nseg); kb=$(du -sk "$art/base.archive.d" | cut -f1)
        MESA_SEED=$art/mesa run_route "B-$r" "$r" \
            BB_KK_AIR_DIR="$art/air" BB_KK_ARCHIVES="$art/base.archive.d" BB_KK_ARCHIVE_CAPTURE="$art/base.archive.d"
        rc=$?
        chain_mesa "B-$r"
        new=$(ls "$art/base.archive.d"/*.mtl4archive | tail -n $(($(nseg) - before)) 2>/dev/null | xargs -n1 basename 2>/dev/null | tr '\n' ' ')
        say "passB $r exit $rc: segments $before -> $(nseg) [$new] $kb -> $(du -sk "$art/base.archive.d" | cut -f1) KB | $(summary "B-$r")"
        echo "$r ${new}" >>"$art/segments_by_route.txt"
        [[ -f $here/out/kkair/B-$r/STOPPED ]] && { say "STOP: machine busy"; exit 3; }
    done
fi
if has heldout; then
    run_route "H-$heldout-ctrl" "$heldout"; say "heldout ctrl exit $? | $(summary "H-$heldout-ctrl")"
    STORE=copy METAL=keep MESA_SEED=$here/out/kkair/H-$heldout-ctrl/mesa run_route "H-$heldout-warm" "$heldout"; say "heldout warm exit $? | $(summary "H-$heldout-warm")"
    MESA_SEED=$art/mesa run_route "H-$heldout-proof" "$heldout" \
        BB_KK_AIR_DIR="$art/air" BB_KK_ARCHIVES="$art/base.archive.d"
    say "heldout proof exit $? | $(summary "H-$heldout-proof")"
fi
pgrep -fl 'bb-gpu|bb-probe|xctrace' && say "WARNING: a game/trace process is still running" || say "no bb-gpu/bb-probe/xctrace left"
say "union done"
