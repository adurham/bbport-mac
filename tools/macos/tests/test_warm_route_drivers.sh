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
sleep "${BB_WARM_SECONDS:-5}"
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
    bash "$work/tools/macos/warm_route.sh" 25 >"$work/route.out" 2>&1 || true
kill "$watcher" 2>/dev/null || true
wait "$watcher" 2>/dev/null || true

echo "== the run it started (the stand-in's environment)"
check "the run is hidden: BB_WARM_ONLY=1 reached the game" grep -q '^BB_WARM_ONLY=1$' "$work/env.txt"
check "the game is pointed at the copy, not the real user dir" grep -q "^BB_USER_DIR=$work/sandbox-user$" "$work/env.txt"
check "the pad file is the checkout's out/warm_pad" grep -q "^BB_PAD_FILE=$work/out/warm_pad$" "$work/env.txt"
check "the early quit is off (BB_WARM_QUIET=0), so a route is not cut at a lull" grep -q '^BB_WARM_QUIET=0$' "$work/env.txt"
check "the run is capped at the seconds asked for (BB_WARM_SECONDS=25)" grep -q '^BB_WARM_SECONDS=25$' "$work/env.txt"
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
check "the run ended by itself, it did not need the watchdog ($took s, cap 25 s)" test "$took" -lt 40
check "the script reported the run's own numbers" grep -q "the run's own numbers" "$work/route.out"

echo "== the same with no user dir to copy (a cold store)"
rm -rf "$work/sandbox-cold"
BB_GAME_DIR=$work/game BB_WARM_USER=$work/sandbox-cold TITLE_WAIT=1 LOAD_WAIT=1 \
    BB_WARM_SOURCE_USER=$work/does-not-exist \
    bash "$work/tools/macos/warm_route.sh" 16 >"$work/cold.out" 2>&1 || true
check "a sandbox is made anyway, empty" test -d "$work/sandbox-cold"
check "and no cache store was invented" test ! -e "$work/sandbox-cold/cache"

printf '\n%d checks passed, %d failed\n' "$pass" "$fail"
((fail == 0))
