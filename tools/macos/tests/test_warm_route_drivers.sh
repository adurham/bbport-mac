#!/usr/bin/env bash
# tools/macos/tests/test_warm_route_drivers.sh: checks what tools/macos/warm_route.sh promises before
# it touches the game — that the run it starts is hidden (BB_WARM_ONLY=1), that the game is pointed
# at a copy of the user dir and never at the real one, and that the scripted pad presses the title's
# Continue and then drives the route — by running it against a stand-in for run.sh instead of the
# game. No GPU, no window, no game data.
#
#   bash tools/macos/tests/test_warm_route_drivers.sh
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
# Physical path: on macOS /var is a symlink to /private/var and the script compares paths it
# resolves itself against the ones here.
work=$(cd -- "$(mktemp -d "${TMPDIR:-/tmp}/warmroute-test.XXXXXX")" && pwd -P)
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

# A checkout-shaped tree with warm_route.sh in place and run.sh replaced by a stand-in that records
# the environment it was started with and then runs as long as BB_WARM_SECONDS says.
mkdir -p "$work/tools/macos" "$work/out" "$work/game" "$work/user/cache/CUSA00000-arm64" "$work/user/savedata/1"
cp "$root/tools/macos/warm_route.sh" "$work/tools/macos/"
printf 'the player\x27s save\n' >"$work/user/savedata/1/userdata0000"
printf 'marker blob' >"$work/user/cache/CUSA00000-arm64/0x0000000000000001.key"
cat >"$work/tools/macos/run.sh" <<'STUB'
#!/usr/bin/env bash
env | grep -E '^(BB_WARM_ONLY|BB_USER_DIR|BB_PAD_FILE|BB_GAME_DIR|BB_WARM_SECONDS|BB_WARM_QUIET|BB_NATIVE_GPU|BB_FRAME_STATS)=' | sort >"$STUB_OUT"
mkdir -p "$PWD/out"
# The log the real run writes, which is also the clock warm_route.sh reads to know whether the
# title's Continue took: the title screen's 24 draws a frame until the pad file says cross, then an
# area's. STUB_LOSE_FIRST=1 is a first press the game missed — the loop has to press again, which is
# what this stands in for.
: >"$PWD/out/last-run.log"
deadline=$(( $(date +%s) + ${BB_WARM_SECONDS:-5} ))
draws=24
presses=0
held=0
while :; do
    # A press is a rising edge of the pad file's 'cross', not a poll that finds it still held: the
    # real game reads the file when it changes, so one press is one state.
    if grep -q cross "$BB_PAD_FILE" 2>/dev/null; then
        if [[ $held == 0 ]]; then
            presses=$((presses + 1))
            if [[ ${STUB_LOSE_FIRST:-0} != 1 || $presses -gt 1 ]]; then draws=900; fi
        fi
        held=1
    else
        held=0
    fi
    printf 'Warm run: 3 pipelines created this run, 4 preloaded from the cache\n'
    printf 'Frame stats: 38.0 FPS, %s draws/frame, 12 shader/pipeline compiles, 3.4 ms\n' "$draws"
    (( $(date +%s) >= deadline )) && break
    sleep 0.5
done >"$PWD/out/last-run.log"
STUB
export STUB_OUT=$work/env.txt

user_before=$(find "$work/user" -print0 | sort -z | md5)

# The pad file is a state, not a log, so the script's timing is read by sampling it: whatever it
# holds at each moment is what the game would be holding.
(
    last=
    for _ in $(seq 1 400); do
        now=$(cat "$work/out/warm_pad" 2>/dev/null || true)
        if [[ $now != "$last" ]]; then
            printf '%s %s\n' "$(date +%s)" "$now" >>"$work/pad.log"
            last=$now
        fi
        sleep 0.25
    done
) &
watcher=$!

start=$(date +%s)
BB_GAME_DIR=$work/game BB_WARM_USER=$work/sandbox-user TITLE_WAIT=2 LOAD_WAIT=2 \
    ROUTE='ly=0,rx=128:1 ly=255,rx=160:1' \
    bash "$work/tools/macos/warm_route.sh" 30 >"$work/route.out" 2>&1 || true
kill "$watcher" 2>/dev/null || true
wait "$watcher" 2>/dev/null || true

echo "== the run it started (the stand-in's environment)"
check "the run is hidden: BB_WARM_ONLY=1 reached the game" grep -q '^BB_WARM_ONLY=1$' "$work/env.txt"
check "the game is pointed at the copy, not the real user dir" grep -q "^BB_USER_DIR=$work/sandbox-user$" "$work/env.txt"
check "the pad file is the checkout's out/warm_pad" grep -q "^BB_PAD_FILE=$work/out/warm_pad$" "$work/env.txt"
check "the early quit is off (BB_WARM_QUIET=0), so a route is not cut at a lull" grep -q '^BB_WARM_QUIET=0$' "$work/env.txt"
check "the run is capped at the seconds asked for (BB_WARM_SECONDS=30)" grep -q '^BB_WARM_SECONDS=30$' "$work/env.txt"
check "the game data path is passed through" grep -q "^BB_GAME_DIR=$work/game$" "$work/env.txt"

echo "== the copy"
check "the copy holds the player's save" test -f "$work/sandbox-user/savedata/1/userdata0000"
check "the copy holds the pipeline store" test -f "$work/sandbox-user/cache/CUSA00000-arm64/0x0000000000000001.key"
check "the copy is a copy, not the real dir" test "$(stat -f%i "$work/sandbox-user/savedata/1/userdata0000")" != "$(stat -f%i "$work/user/savedata/1/userdata0000")"
check "the real user dir is untouched" test "$(find "$work/user" -print0 | sort -z | md5)" = "$user_before"
check "the real user dir gained no out/ or pad file" test ! -e "$work/user/out"
check "the pad file is where the script said" test -f "$work/out/warm_pad"

echo "== the pad it drove (sampled; seconds since the run started)"
took=$(($(date +%s) - start))
awk -v s="$start" '{t = $1 - s; sub(/^[0-9]+ /, ""); printf "  +%ss  [%s]\n", t, $0}' "$work/pad.log" | head -8
check "the title's Continue was pressed first" grep -q ' cross$' "$work/pad.log"
check "the route followed, with the sticks the route asked for" grep -q 'ly=0 rx=128' "$work/pad.log"
check "and a release (an empty pad file) follows the press" grep -qE '^[0-9]+ $' "$work/pad.log"
check "the run ended by itself, it did not need the watchdog ($took s, cap 30 s)" test "$took" -lt 45
check "the script reported the run's own numbers" grep -q "the run's own numbers" "$work/route.out"

echo "== the same with no user dir to copy (a cold store)"
rm -rf "$work/sandbox-cold"
BB_GAME_DIR=$work/game BB_WARM_USER=$work/sandbox-cold TITLE_WAIT=1 LOAD_WAIT=1 \
    BB_WARM_SOURCE_USER=$work/does-not-exist \
    bash "$work/tools/macos/warm_route.sh" 16 >"$work/cold.out" 2>&1 || true
check "a sandbox is made anyway, empty" test -d "$work/sandbox-cold"
check "and no cache store was invented" test ! -e "$work/sandbox-cold/cache"

echo "== the timing a route line depends on: the walk starts after the title's Continue and the load"
# The first entry of the route above is ly=0 (running). It must not appear before the title's cross
# has taken and the load is behind us: a walk that started early would press into the title's
# Continue a second time, and one that started late would spend the run standing still.
first_stick=$(awk '$0 ~ /ly=/ {print $1; exit}' "$work/pad.log")
first_cross=$(awk '$0 ~ /cross$/ {print $1; exit}' "$work/pad.log")
check "the title's Continue was pressed first" test -n "$first_cross"
check "the walk starts after the press and the load, not before" \
    bash -c "p=$((first_stick - first_cross)); [ \"\$p\" -ge 2 ]"
check "and the press came at title_wait, not at once ($((first_cross - start)) s in)" \
    bash -c "p=$((first_cross - start)); [ \"\$p\" -ge 1 ] && [ \"\$p\" -le 8 ]"

echo "== a first press the game misses: the loop presses again (the 19:09 run's bug)"
# The build of 2026-10-16 19:09 lost a press at a fixed 45 s entirely (its log had no pad-file line
# for it at all) and the run sat at the title for its whole length, compiling nothing. The stand-in
# loses the first press; the loop has to notice, from the log, that the game is still at the title
# and press again.
rm -f "$work/pad.log"
(
    last=
    for _ in $(seq 1 400); do
        now=$(cat "$work/out/warm_pad" 2>/dev/null || true)
        if [[ $now != "$last" ]]; then
            printf '%s %s\n' "$(date +%s)" "$now" >>"$work/pad.log"
            last=$now
        fi
        sleep 0.25
    done
) &
watcher=$!
rm -rf "$work/sandbox-retry"
BB_GAME_DIR=$work/game BB_WARM_USER=$work/sandbox-retry STUB_LOSE_FIRST=1 TITLE_WAIT=1 LOAD_WAIT=1 \
    ROUTE='ly=0,rx=128:1 ly=255,rx=160:1' \
    bash "$work/tools/macos/warm_route.sh" 30 >"$work/retry.out" 2>&1 || true
kill "$watcher" 2>/dev/null || true
wait "$watcher" 2>/dev/null || true
crosses=$(grep -c ' cross$' "$work/pad.log")
check "the lost press was noticed and pressed again ($crosses crosses)" test "$crosses" -ge 2
check "and the walk followed the press that took" grep -q 'ly=0 rx=128$' "$work/pad.log"
check "the route's own output says how many presses it took" \
    grep -q "the title's Continue took after 2 press" "$work/retry.out"

echo "== a route line from the pack, driven as written"
# One of the pack's own pad lines (tools/macos/warmfill.sh), not a test's: the tokens a route is
# made of are only proved to be drivable by warm_route.sh itself. 'stairs' opens with a look down.
pack_pad=$(awk -F'|' '$1 == "stairs" {print $5}' "$root/tools/macos/warmfill.sh")
check "warmfill.sh has a stairs route with a pad line" test -n "$pack_pad"
rm -f "$work/pad.log"
(
    last=
    for _ in $(seq 1 400); do
        now=$(cat "$work/out/warm_pad" 2>/dev/null || true)
        if [[ $now != "$last" ]]; then
            printf '%s %s\n' "$(date +%s)" "$now" >>"$work/pad.log"
            last=$now
        fi
        sleep 0.25
    done
) &
watcher=$!
rm -rf "$work/sandbox-stairs"
BB_GAME_DIR=$work/game BB_WARM_USER=$work/sandbox-stairs TITLE_WAIT=1 LOAD_WAIT=1 \
    ROUTE="$pack_pad" bash "$work/tools/macos/warm_route.sh" 35 >"$work/stairs.out" 2>&1 || true
kill "$watcher" 2>/dev/null || true
wait "$watcher" 2>/dev/null || true
awk '{sub(/^[0-9]+ /, ""); printf "  [%s]\n", $0}' "$work/pad.log" | head -6
check "its first state is the look down (ry=190), as the line says" \
    grep -q '^[0-9]* ry=190$' "$work/pad.log"
check "then the run forward, as the line says" grep -q 'ly=0 rx=128$' "$work/pad.log"
check "and the second look down (ry=205) is reached" grep -q 'ry=205' "$work/pad.log"
check "the run still ended hidden and by itself" grep -q 'BB_WARM_ONLY=1' "$work/env.txt"

printf '\n%d checks passed, %d failed\n' "$pass" "$fail"
((fail == 0))
