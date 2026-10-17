#!/usr/bin/env bash
# tools/macos/tests/test_warmfill_run.sh: warmfill.sh end to end *with* runs, against a stand-in for
# run.sh that writes blobs into BB_USER_DIR the way the game writes what it compiles. It is the only
# way to check the run loop — the per-route stats, the log copies, the report — without a GPU, a
# window or the game.
#
#   bash tools/macos/tests/test_warmfill_run.sh
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
# A checkout-shaped scratch tree, so warmfill's own out/ and logs are not the real ones.
tree=$(cd -- "$(mktemp -d "${TMPDIR:-/tmp}/warmfill-run.XXXXXX")" && pwd -P)
trap 'rm -rf "$tree"' EXIT
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

mkdir -p "$tree/tools/macos" "$tree/out" "$tree/game" \
    "$tree/user/cache/CUSA00000-arm64" "$tree/user/savedata/1/CUSA00000/SPRJ0005"
for script in warmfill.sh warm_route.sh warm_merge.sh; do
    cp "$root/tools/macos/$script" "$tree/tools/macos/"
done
printf "the player's save\n" >"$tree/user/savedata/1/CUSA00000/SPRJ0005/userdata0000"
printf 'the blob a previous route left\n' >"$tree/user/cache/CUSA00000-arm64/0x0000000000000001.key"
printf 'BBPC-the-cache-header-of-this-build' >"$tree/user/cache/CUSA00000-arm64/profile.bin"

# The stand-in for the game: it records the environment it was started with, writes the three blobs
# a compile pass would leave in the store under BB_USER_DIR (long enough, and magic-correct enough, to
# pass the merge's checks), writes the log run.sh would leave, and then lives as long as the run is
# capped at (BB_WARM_SECONDS), like the game does.
cat >"$tree/tools/macos/run.sh" <<'STUB'
#!/usr/bin/env bash
env | grep -E '^(BB_WARM_ONLY|BB_USER_DIR|BB_PAD_FILE|BB_WARM_SECONDS)=' | sort >"$STUB_ENV"
n=$(cat "$STUB_COUNT" 2>/dev/null || echo 0)
n=$((n + 1))
echo "$n" >"$STUB_COUNT"
store=$BB_USER_DIR/cache/CUSA00000-arm64
mkdir -p "$store"
{ printf 'sim-%s-key-' "$n"; head -c 64 /dev/zero; } | head -c 64 >"$store/sim-$n.key"
{ printf 'sim-%s-meta-' "$n"; head -c 32 /dev/zero; } | head -c 32 >"$store/sim-$n.meta"
printf '\x03\x02\x23\x07\x00\x06\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >"$store/sim-$n.spv"
mkdir -p "$PWD/out"
{
    printf 'Warm run: 12 pipelines created this run, 4 preloaded from the cache; compiles stopped - asking the game to quit\n'
    [[ ${STUB_ASSERT:-0} == 1 ]] && printf 'Bad PM4 packet at 0x766f84ed5c: dword 0x70d1b005\nSTOP: GPU library assertion failed (see GPU log above)\n'
    printf 'Frame stats: 38.0 FPS, %s draws/frame, 12 shader/pipeline compiles, 3.4 ms\n' "${STUB_DRAWS:-900}"
} >"$PWD/out/last-run.log"
sleep "${BB_WARM_SECONDS:-5}"
STUB
export STUB_ENV=$tree/stub-env.txt STUB_COUNT=$tree/stub-count.txt
unset STUB_DRAWS
# warmfill refuses to run without a build.
printf '#!/bin/sh\nexit 0\n' >"$tree/out/bb-probe"
chmod +x "$tree/out/bb-probe" "$tree/tools/macos/run.sh"
# A route short enough for a test, through the untracked conf file the docs describe — which also
# checks that path.
cat >"$tree/out/warmfill_routes.conf" <<'CONF'
# <name>|<secs>|<title_wait>|<load_wait>|<pad>|<description>
fast|25|2|2|ly=0,rx=128:1 ly=255,rx=160:1|a route that only exists for this test
CONF

outside_before=$(find "$tree/user" -not -path '*/cache/*' -print0 | sort -z | md5)
# --force: the running-game guard refuses when any bb-probe or bb-gpu process exists, and another
# checkout on this machine may well be running one right now (the guard is for the owner's own play
# session). tools/macos/tests/test_warmfill.sh covers everything that does not need a run.
start=$(date +%s)
st=0
env BB_GAME_DIR=$tree/game bash "$tree/tools/macos/warmfill.sh" \
    --force --routes fast --user-dir "$tree/user" --sandbox "$tree/out/warmfill_user" \
    >"$tree/fill.log" 2>&1 || st=$?
took=$(($(date +%s) - start))
check "the fill ran" test "$st" = 0

echo "== the run"
check "the stand-in was started with the copy of the user dir" \
    grep -q "^BB_USER_DIR=$tree/out/warmfill_user$" "$tree/stub-env.txt"
check "hidden, as a route run must be" grep -q '^BB_WARM_ONLY=1$' "$tree/stub-env.txt"
check "the sandbox was cloned from the real user dir" \
    test -f "$tree/out/warmfill_user/cache/CUSA00000-arm64/0x0000000000000001.key"
check "and holds the saves too" test -f "$tree/out/warmfill_user/savedata/1/CUSA00000/SPRJ0005/userdata0000"
check "the route's own output was kept" test -f "$tree/out/warmfill/fast.out"
check "the run's log was kept for the report" test -f "$tree/out/warmfill/fast.log"
check "the report quotes the run's own numbers" grep -q 'Warm run: 12 pipelines created this run' "$tree/fill.log"
check "and its frame stats" grep -q 'Frame stats: 38.0 FPS' "$tree/fill.log"
check "the fill took about the route's length, not minutes ($took s for a 25 s route)" test "$took" -lt 70

echo "== the merge"
check "the three blobs the run wrote arrived in the real store" \
    test -f "$tree/user/cache/CUSA00000-arm64/sim-1.key" -a \
    -f "$tree/user/cache/CUSA00000-arm64/sim-1.meta" -a \
    -f "$tree/user/cache/CUSA00000-arm64/sim-1.spv"
check "the blob that was already there is untouched" \
    grep -q 'a previous route left' "$tree/user/cache/CUSA00000-arm64/0x0000000000000001.key"
check "the saves are byte for byte what they were" \
    test "$(find "$tree/user" -not -path '*/cache/*' -print0 | sort -z | md5)" = "$outside_before"
check "the real store's profile.bin was seen as identical" grep -q 'profile.bin identical' "$tree/fill.log"
check "the run's three blobs are all new, the one already there identical" \
    grep -q '"added":3,"added_key":1,"added_meta":1,"added_spv":1,"identical":1' "$tree/out/warmfill/merge.json"

echo "== the report"
check "it counts what the route made" grep -q 'the route made' "$tree/fill.log"
check "the JSON says the route made three blobs, in one run of that route" \
    python3 -c "import json,sys; d=json.load(open('$tree/out/warmfill-report.json')); r=d['routes'][0]; sys.exit(0 if r['route']=='fast' and r['new_blobs']==3 and r['took_s']>=25 and len(d['routes'])==1 else 1)"
check "the JSON says three went into the real store" \
    python3 -c "import json,sys; d=json.load(open('$tree/out/warmfill-report.json')); sys.exit(0 if d['added_to_real']==3 and d['merge']['invalid']==0 else 1)"
check "and the profile gate it went through is in the merge report" \
    python3 -c "import json,sys; d=json.load(open('$tree/out/warmfill-report.json')); sys.exit(0 if d['merge']['stores'][0]['profile']=='identical' else 1)"

echo "== a second invocation, with a fresh snapshot"
st=0
env BB_GAME_DIR=$tree/game bash "$tree/tools/macos/warmfill.sh" \
    --force --routes fast --user-dir "$tree/user" --sandbox "$tree/out/warmfill_user" \
    >"$tree/fill2.log" 2>&1 || st=$?
check "the second fill ran" test "$st" = 0
check "its three blobs are new, the four already there are identical" \
    grep -q '"added":3,"added_key":1,"added_meta":1,"added_spv":1,"identical":4' "$tree/out/warmfill/merge.json"
check "the first run's blobs were not copied twice" \
    test "$(find "$tree/user/cache" -name 'sim-1.*' | wc -l | tr -d ' ')" = 3
check "no .warmfill-tmp was left in the real store" \
    test "$(find "$tree/user" -name '*.warmfill-tmp' | wc -l | tr -d ' ')" = 0

echo "== a run that never got into the game is a dead run, not a zero"
# The stand-in writes the draws/frame a run's own log would carry. 900 is an area (the stub's usual
# number); 24 is the title screen. A run stuck at the title measured nothing, and reporting its
# blob count as if it were the area's coverage is the lie this check exists to stop: warmfill says
# so, and re-runs the route once.
before_n=$(cat "$tree/stub-count.txt")
st=0
env BB_GAME_DIR=$tree/game STUB_DRAWS=24 bash "$tree/tools/macos/warmfill.sh" \
    --force --routes fast --user-dir "$tree/user" --sandbox "$tree/out/warmfill_user" \
    >"$tree/fill3.log" 2>&1 || st=$?
ran=$(( $(cat "$tree/stub-count.txt") - before_n ))
check "the fill still ran" test "$st" = 0
check "the dead route was re-run once (the stand-in ran twice, it ran $ran)" test "$ran" = 2
check "the log says the run never reached gameplay" grep -q 'NEVER REACHED GAMEPLAY' "$tree/fill3.log"
check "and says why it is not a zero about the area" grep -q 'say nothing about the area' "$tree/fill3.log"
check "a live run that adds nothing is told apart from it" \
    grep -q 'reached gameplay' "$tree/fill.log"
check "the report JSON carries the retry and the draws the run actually reached" \
    python3 -c "import json,sys; d=json.load(open('$tree/out/warmfill-report.json')); r=d['routes'][0]; sys.exit(0 if r['retried']==1 and r['reached_gameplay']==0 and r['max_draws_per_frame']==24 and r['gpu_crash']==0 and d['retries']==1 else 1)"
check "and the summary line counts the retry" grep -q 'retries:  1 route' "$tree/fill3.log"
check "the two runs of the route are both in its wall clock" \
    python3 -c "import json,sys; d=json.load(open('$tree/out/warmfill-report.json')); sys.exit(0 if d['routes'][0]['took_s'] >= 50 else 1)"

echo "== --no-retry leaves a dead run reported, but does not run it twice"
before_n=$(cat "$tree/stub-count.txt")
env BB_GAME_DIR=$tree/game STUB_DRAWS=24 bash "$tree/tools/macos/warmfill.sh" \
    --force --no-retry --routes fast --user-dir "$tree/user" --sandbox "$tree/out/warmfill_user" \
    >"$tree/fill4.log" 2>&1 || true
ran=$(( $(cat "$tree/stub-count.txt") - before_n ))
check "no re-run: the stand-in ran once more, not twice ($ran)" test "$ran" = 1
check "but a dead run is still reported as one" grep -q 'NEVER REACHED GAMEPLAY' "$tree/fill4.log"
check "and no retry is counted" python3 -c "import json,sys; d=json.load(open('$tree/out/warmfill-report.json')); sys.exit(0 if d['retries']==0 and d['routes'][0]['retried']==0 else 1)"

echo "== a run whose GPU process died is reported as a crash, not as coverage"
export STUB_ASSERT=1
before_n=$(cat "$tree/stub-count.txt")
env BB_GAME_DIR=$tree/game bash "$tree/tools/macos/warmfill.sh" \
    --force --routes fast --user-dir "$tree/user" --sandbox "$tree/out/warmfill_user" \
    >"$tree/fill5.log" 2>&1 || true
unset STUB_ASSERT
ran=$(( $(cat "$tree/stub-count.txt") - before_n ))
check "the crashing route was re-run once ($ran runs)" test "$ran" = 2
check "the log names the assertion" grep -q 'GPU process died mid-run' "$tree/fill5.log"
check "and says the blob count is a floor, not the coverage" grep -q 'floor, not the route' "$tree/fill5.log"
check "the JSON marks the crash" \
    python3 -c "import json,sys; d=json.load(open('$tree/out/warmfill-report.json')); r=d['routes'][0]; sys.exit(0 if r['gpu_crash']==1 and r['retried']==1 else 1)"

printf '\n%d checks passed, %d failed\n' "$pass" "$fail"
((fail == 0))
