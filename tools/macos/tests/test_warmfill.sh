#!/usr/bin/env bash
# tools/macos/tests/test_warmfill.sh: checks tools/macos/warmfill.sh end to end without the game —
# the snapshot, the report and the merge into the real store, plus the refusals — against a pair of
# synthetic user dirs. The route runs themselves (which need the game) are checked separately by
# tools/macos/tests/test_warm_route_drivers.sh; the merge by tools/macos/tests/test_warm_merge.sh.
#
#   bash tools/macos/tests/test_warmfill.sh
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
warmfill=$root/tools/macos/warmfill.sh
work=$(cd -- "$(mktemp -d "${TMPDIR:-/tmp}/warmfill-orb.XXXXXX")" && pwd -P)
trap 'rm -rf "$work"' EXIT
pass=0
fail=0
check() {
    local desc=$1
    shift
    if "$@" >/dev/null 2>&1; then
        pass=$((pass + 1))
        printf '  ok   %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf '  FAIL %s\n' "$desc" >&2
    fi
}
# fill LOG ARGS...: warmfill with everything it prints kept.
fill() {
    local log=$1
    shift
    bash "$warmfill" "$@" >"$log" 2>&1
}

blob() { { printf '%s' "$3"; head -c "$2" /dev/zero; } | head -c "$2" >"$1"; }

# A user dir with a filled native store, the saves of a player, and a sandbox that a (pretended)
# route has already written to: three blobs and one SPIR-V file the real store does not have.
mkdir -p "$work/real-user/cache/CUSA00000-arm64" "$work/real-user/savedata/1/CUSA00000/SPRJ0005"
printf "the player's save\n" >"$work/real-user/savedata/1/CUSA00000/SPRJ0005/userdata0000"
for i in 1 2 3; do
    blob "$work/real-user/cache/CUSA00000-arm64/0x000000000000000$i.key" 64 "real-blob-$i"
done
printf 'BBPC-the-cache-header-of-this-build' >"$work/real-user/cache/CUSA00000-arm64/profile.bin"
cp -cR "$work/real-user" "$work/sandbox"
for i in 4 5; do
    blob "$work/sandbox/cache/CUSA00000-arm64/0x000000000000000$i.key" 64 "route-blob-$i"
done
printf '\x03\x02\x23\x07\x00\x06\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' \
    >"$work/sandbox/cache/CUSA00000-arm64/0x0000000000000007_0.spv"
outside_before=$(find "$work/real-user" -not -path '*/cache/*' -print0 | sort -z | md5)

echo "== 1. an existing sandbox is merged into the real store, and the report says what happened"
check "the run succeeded" fill "$work/one.log" \
    --no-run --user-dir "$work/real-user" --sandbox "$work/sandbox"
check "the two key blobs arrived" \
    test -f "$work/real-user/cache/CUSA00000-arm64/0x0000000000000004.key" -a \
    -f "$work/real-user/cache/CUSA00000-arm64/0x0000000000000005.key"
check "the SPIR-V arrived too" test -f "$work/real-user/cache/CUSA00000-arm64/0x0000000000000007_0.spv"
check "the report counts the three merged, two key and one spv, three already there" \
    grep -q 'added 3 (2 key, 0 meta, 1 spv), 3 identical' "$work/one.log"
check "the report points at the logs" grep -q 'merge.json' "$work/one.log"
check "the report JSON is written" test -f "$root/out/warmfill-report.json"
check "the report JSON is valid" python3 -m json.tool "$root/out/warmfill-report.json"
check "it says how many went into the real store" \
    python3 -c "import json,sys; d=json.load(open('$root/out/warmfill-report.json')); sys.exit(0 if d['added_to_real'] == 3 and d['merge']['invalid'] == 0 else 1)"
check "the saves are byte for byte what they were" \
    test "$(find "$work/real-user" -not -path '*/cache/*' -print0 | sort -z | md5)" = "$outside_before"
check "no warmfill state was written into the real user dir" test ! -e "$work/real-user/out"

echo "== 2. --dry-run reports and writes nothing"
before=$(find "$work/real-user" "$work/sandbox" -print0 | sort -z | md5)
check "the dry run succeeded" fill "$work/two.log" \
    --dry-run --user-dir "$work/real-user" --sandbox "$work/sandbox" --routes all
check "neither user dir changed, byte for byte" \
    test "$(find "$work/real-user" "$work/sandbox" -print0 | sort -z | md5)" = "$before"
check "it lists the routes it would run" grep -q 'route spin' "$work/two.log"
check "and previews the merge of the sandbox it has" grep -q 'store CUSA00000-arm64: added 0' "$work/two.log"

echo "== 3. --list-routes needs neither game nor user dir"
check "it lists the built-in routes" bash -c "bash '$warmfill' --list-routes | grep -q yharnam"

echo "== 4. refusals (each one exits non-zero and does nothing)"
st=0
fill "$work/three.log" --routes nope --no-run --user-dir "$work/real-user" --sandbox "$work/sandbox" || st=$?
check "an unknown route name is refused" test "$st" != 0
check "and it names the route" grep -q "no route named 'nope'" "$work/three.log"
st=0
fill "$work/four.log" --no-run --user-dir "$work/real-user" --sandbox "$work/no-sandbox" || st=$?
check "--no-run without a sandbox is refused" test "$st" != 0
check "and it says which path it looked at" grep -q 'no-sandbox' "$work/four.log"
st=0
fill "$work/five.log" --no-run --user-dir "$work/does-not-exist" --sandbox "$work/sandbox" || st=$?
check "a user dir that does not exist is refused" test "$st" != 0
check "and it says to run the game once" grep -q 'run the game once' "$work/five.log"
st=0
fill "$work/six.log" --no-run --user-dir "$work/real-user" --sandbox "$work/real-user" || st=$?
check "a sandbox that is the real user dir is refused" test "$st" != 0
check "and it says why" grep -q 'same directory' "$work/six.log"
st=0
mkdir -p "$work/real-user/inside"
fill "$work/seven.log" --no-run --user-dir "$work/real-user" --sandbox "$work/real-user/inside" || st=$?
check "a sandbox inside the real user dir is refused" test "$st" != 0
check "and it says why" grep -q 'inside the real user dir' "$work/seven.log"
st=0
fill "$work/eight.log" --no-run --user-dir "$work/real-user" --sandbox "$work/sandbox8" --no-run --force || st=$?
check "a sandbox that does not exist with --no-run is refused" test "$st" != 0

echo "== 5. with no game data, a run refuses before it touches anything"
st=0
before=$(find "$work/real-user" "$work/sandbox" -print0 | sort -z | md5)
BB_GAME_DIR=$work/no-game fill "$work/nine.log" --user-dir "$work/real-user" --sandbox "$work/sandbox9" || st=$?
check "a missing game dir is refused" test "$st" != 0
check "and it names the path" grep -q 'BB_GAME_DIR is not a directory' "$work/nine.log"
check "the run never started: no sandbox was made" test ! -e "$work/sandbox9"
check "and neither user dir moved" \
    test "$(find "$work/real-user" "$work/sandbox" -print0 | sort -z | md5)" = "$before"
st=0
fill "$work/ten.log" --user-dir "$work/real-user" --sandbox "$work/sandbox10" || st=$?
check "with BB_GAME_DIR unset the game is looked for and, if absent, said so" \
    grep -q 'no game data\|out/bb-probe is missing' "$work/ten.log"

printf '\n%d checks passed, %d failed\n' "$pass" "$fail"
((fail == 0))
