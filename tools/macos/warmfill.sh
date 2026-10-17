#!/usr/bin/env bash
# tools/macos/warmfill.sh [options]: fills the pipeline cache from the game's own draws, in one
# command — snapshot the user dir, drive one or more warm routes in that copy, merge what they
# compiled into the real store, report what was added. docs/PIPELINE_WARMTH.md.
#
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/warmfill.sh
#   BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/warmfill.sh --routes all
#   bash tools/macos/warmfill.sh --list-routes
#
# Why a *route* and not just a warm run: a pipeline is made at the first draw with a given state
# key, so an area's pipelines only exist after something drew them (docs/PIPELINE_WARMTH.md,
# "How a pipeline comes into being"). The routes below press the title's Continue and then walk,
# turn, or fight, with the early quit off (BB_WARM_QUIET=0).
#
# Safety, in order of how much it would hurt:
# - The runs never see the player's saves: they run against a copy of the user dir ($BB_WARM_USER),
#   which is an APFS clone (`cp -cR`), and the game is told to use it through BB_USER_DIR.
# - Every run is hidden and windowed (BB_WARM_ONLY=1, gpu/shim/window.cpp); nothing appears on the
#   owner's screen, and the display mode is never switched.
# - The real store only ever gains blobs: tools/macos/warm_merge.sh adds, never deletes or renames,
#   and writes only under <real user>/cache/<store>/.
# - A run's store is content-addressed, so a repeat run adds nothing and costs nothing but time.
#
# Options:
#   --routes LIST     routes to run: names separated by commas, or "all" (default: all, the whole
#                     verified pack; --routes yharnam is the one short walk, for a quick fill)
#   --list-routes     print the routes and exit (no game, no writes)
#   --check-routes    check the route lines themselves (tokens, stick ranges, lengths) and exit;
#                     exits non-zero when a line is wrong — a route is data, and a typo in it costs
#                     a run to notice
#   --secs N          override each selected route's run length (seconds)
#   --user-dir DIR    the real user dir to snapshot and to merge into
#                     (default ${BB_DATA_DIR:-.}/user, the dir this checkout's run.sh uses)
#   --sandbox DIR     the scratch copy (default out/warmfill_user)
#   --keep-sandbox    reuse the existing sandbox instead of cloning the real dir again
#   --no-run          no game runs: merge an existing sandbox only
#   --no-merge        run the routes, merge nothing
#   --adopt-profile   hand --adopt-profile to the merge: install the sandbox's profile.bin when it
#                     differs from the real store's (what makes a fill after a version bump count)
#   --prefer-sandbox  hand --prefer-sandbox to the merge: a blob whose bytes differ is taken from
#                     the sandbox instead of kept from the real store
#   --dry-run         no runs and no writes: snapshot nothing, report what would be merged
#   --force           skip the guard that refuses to run while the game is running
#   --no-retry        do not re-run a route whose run crashed or never reached gameplay (see below)
#   -q, --quiet       the runs' own output is already logged; keep the report short
#
# A run that never got into the game, or that asserted part-way, measured nothing: its blob count
# is not a statement about the area, and a bare "0 blobs" would read as "already covered". warmfill
# reads each run's own log back — the highest draws/frame it saw (the title screen draws ~24, an
# area several hundred) and whether the GPU process died — says which happened, and re-runs such a
# route once (--no-retry turns that off). The numbers are in the report JSON too
# (reached_gameplay, max_draws_per_frame, gpu_crash, retried).
#
# Routes can also be added without touching this file: WARMFILL_ROUTES_FILE, or an untracked
# out/warmfill_routes.conf, with one "<name>|<secs>|<title_wait>|<load_wait>|<pad>|<description>" per
# line, the same shape as the built-in table below; a name that appears there replaces the
# built-in route of the same name.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD

prog=$(basename -- "$0")
die() {
    echo "STOP ($prog): $*" >&2
    exit 2
}

usage() {
    sed -n '2,/^set -euo pipefail$/p' "$0" | sed -e 's/^# \{0,1\}//' -e '$d'
    exit 1
}

# name|secs|title_wait|load_wait|pad|what it walks.
#
# This table is the verified pack: every route here was run against the sandboxed save and kept
# because it added blobs (pipelines) the rest of the pack did not reach — docs/PIPELINE_WARMTH.md,
# "The route pack". `--routes all` is this table, in this order.
#
# The pad entries are warm_route.sh's "<pad file contents>:<seconds>"; the comma stands in for the
# space the pad file wants between tokens (src/runtime_pad.c reads button names and lx= ly= rx= ry=
# in 0..255, 128 at rest). `secs` is the whole run: the game quits at that mark (BB_WARM_SECONDS),
# and warm_route.sh only starts driving the pad at title_wait + load_wait, so a route shorter than
# that walks nowhere — warmfill raises the length of one that is too short rather than run it empty.
#
# title_wait 45 and load_wait 30 are measured, not guessed: the run's own log puts the title screen
# from ~5 s to the cross at 45 s, and the first frames of the area (900+ draws/frame) five seconds
# after it — the earlier load_wait of 75 s spent 70 s of every run standing still in an already
# loaded area. 30 s leaves 25 s of margin for a slower load; a press into a load is ignored, not
# fatal, and the loop tries it again.
builtin_routes='
yharnam|300|45|30|ly=0,rx=128:4 ly=0,rx=40:0.5 ly=0,rx=128:4 ly=0,rx=215:0.5 ly=255,rx=160:3|the short measured walk: forward, a look right, forward, a look left, back
roam|450|45|30|ly=0,rx=128:8 rx=60:0.35 rx=128:0.25 ly=0,rx=128:8 rx=60:0.35 rx=128:0.25 ly=0,rx=128:8 rx=60:0.35 rx=128:0.25 ly=0,rx=128:8 rx=60:0.35 rx=128:0.25 ly=0,rx=128:8 rx=60:0.35 rx=128:0.25 ly=0,rx=128:8 rx=60:0.35 rx=128:0.25 ly=0,rx=128:8 rx=60:0.35 rx=128:0.25 ly=0,rx=128:8 rx=60:0.35 rx=128:0.25|the lawnmower: eight long runs at eight headings, so the walk covers ground in every direction from wherever the save is
street|360|45|30|ly=0,rx=128:20 rx=60:1.2 rx=128:0.4 ly=0,rx=128:20 rx=60:1.2 rx=128:0.4|a long run one way, a half-turn, a long run back: the distance the short walk never covers
stairs|240|45|30|ry=190:1.5 ly=0,rx=128:5 ry=128:1 ly=0,rx=128:5 ry=205:2 ly=0,rx=128:5 rx=60:0.35 rx=128:0.25 ly=0,rx=128:5|look down and run: stairs, slopes and whatever is below them are their own draws
alleys|240|45|30|ly=0,rx=128:3 rx=60:0.5 rx=128:0.25 ly=0,rx=128:3 rx=200:0.5 rx=128:0.25 ly=255,rx=128:2 ly=0,rx=128:3 lx=0,rx=128:2 lx=255,rx=128:2|short bursts, 90-degree turns, back steps and strafes: side alleys, and the unwedging a long straight hold never does
attack|300|45|30|r1:2 r1:1 r1:1 circle:1.5 ly=0,rx=128:2 r2:2 l1:2 r1:2|light attacks, a roll, a walk, a heavy, the transform: combat states a plain walk never reaches
combat|360|45|30|ly=0,rx=128:4 r1:1 r1:1 r1:1 circle:1.2 ly=0,rx=128:4 r1:1 r2:1.2 l1:1 l1:1 r1:1 ly=0,rx=128:3 r1:1 r1:1 circle:1.2 l2:1 ly=0,rx=128:5 r1:1 r1:1 l1:1 r2:1.2|walk into the mob and fight it: combos, a heavy, the transform both ways, the gun and rolls, over a long run
spin|240|45|30|rx=128:6 rx=88:0.5 rx=128:5 rx=168:0.5 rx=128:5 rx=40:6 rx=128:5 rx=215:6 rx=128:4 ry=40:5 ry=215:5|turn in place, then look down and up: headings and the sky are their own draws
sweep|240|45|30|rx=128:1 rx=60:1.2 rx=128:1 rx=60:1.2 rx=128:1 rx=60:1.2 rx=128:1 rx=60:1.2 rx=128:1 rx=60:1.2 rx=128:1 rx=60:1.2 rx=128:1 rx=60:1.2 rx=128:1 rx=60:1.2 ry=30:3 ry=128:1 ry=225:3 ry=128:1|a full turn in eight steps, then the sky and the ground: every heading the area can be looked at from
interact|180|45|30|ly=0,rx=128:3 cross:1 ly=0,rx=128:3 cross:1 ly=0,rx=128:4 cross:1 circle:1 rx=60:0.6 rx=128:0.25 ly=0,rx=128:3 cross:1|run and press cross at whatever is there: doors, gates, ladders, lamps, and the menus and dialogues they open are their own draws
warp|300|45|30|ly=0,rx=128:6 cross:1.5 up:0.6 cross:1.5 down:0.6 cross:1.5 options:1.5 options:1.5 ly=0,rx=128:6 cross:1.5|run on and try the menus: the menu a lamp or a gravestone opens is the one way a scripted pad reaches a second area
'

routes_file=${WARMFILL_ROUTES_FILE:-}
if [[ -z $routes_file && -f out/warmfill_routes.conf ]]; then
    routes_file=out/warmfill_routes.conf
fi
routes_all=$builtin_routes
if [[ -n $routes_file ]]; then
    [[ -f $routes_file ]] || die "WARMFILL_ROUTES_FILE is not a file: $routes_file"
    routes_all="$routes_all$(cat "$routes_file")"
fi

# The names, in the order they first appear; the last line for a name wins, so a file's version of
# a built-in route replaces it.
route_names() {
    printf '%s\n' "$routes_all" | awk -F'|' 'NF > 1 && $1 !~ /^#/ && !seen[$1]++ {print $1}'
}
route_line() {
    printf '%s\n' "$routes_all" | awk -F'|' -v name="$1" 'NF > 1 && $1 == name {line = $0} END {print line}'
}

list_routes() {
    local name line
    for name in $(route_names); do
        line=$(route_line "$name")
        IFS='|' read -r _ secs title load pad desc <<<"$line"
        printf '%-10s %4ss  (title %ss, load %ss)  %s\n' "$name" "$secs" "$title" "$load" "$desc"
        printf '           pad: %s\n' "$pad"
    done
}

# The route lines themselves, checked without the game. A route is data, and the ways data goes
# wrong cost a run each to notice: a token spelled wrong is a button that is simply never pressed,
# a stick outside 0..255 is clamped or ignored, an entry with no seconds never happens, and a route
# shorter than title_wait + load_wait + 15 never walks at all (warm_route.sh stops driving 15 s
# before the end). --check-routes prints each line's verdict and exits non-zero if any is wrong.
check_routes() {
    local name line secs title load pad desc bad tok v state dur entry
    local ok=1
    for name in $(route_names); do
        line=$(route_line "$name")
        IFS='|' read -r _ secs title load pad desc <<<"$line"
        bad=""
        [[ $secs =~ ^[0-9]+$ ]] || bad="$bad secs '$secs' is not a number;"
        [[ $title =~ ^[0-9]+$ ]] || bad="$bad title_wait '$title' is not a number;"
        [[ $load =~ ^[0-9]+$ ]] || bad="$bad load_wait '$load' is not a number;"
        if [[ $secs =~ ^[0-9]+$ && $title =~ ^[0-9]+$ && $load =~ ^[0-9]+$ ]] &&
            ((secs < title + load + 15)); then
            bad="$bad secs $secs is under title_wait + load_wait + 15 = $((title + load + 15)), so the route never walks;"
        fi
        [[ -n $desc ]] || bad="$bad no description;"
        for entry in $pad; do
            if [[ $entry != *:* ]]; then bad="$bad pad entry '$entry' has no :seconds;"; continue; fi
            state=${entry%:*}
            dur=${entry##*:}
            if [[ ! $dur =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
                bad="$bad pad entry '$entry' has no numeric seconds;"
            elif ! awk -v d="$dur" 'BEGIN{exit !(d > 0)}' </dev/null; then
                bad="$bad pad entry '$entry' holds for zero seconds;"
            fi
            [[ -n $state ]] || bad="$bad pad entry '$entry' presses nothing;"
            for tok in ${state//,/ }; do
                case $tok in
                lx=* | ly=* | rx=* | ry=*)
                    v=${tok#*=}
                    if [[ ! $v =~ ^[0-9]+$ ]] || ((v > 255)); then
                        bad="$bad stick '$tok' is not 0..255;"
                    fi
                    ;;
                mdx=* | mdy=*) ;;
                cross | circle | square | triangle | l1 | r1 | l2 | r2 | l3 | r3 | options | touchpad | \
                    touchpad_left | touchpad_right | up | down | left | right | replay) ;;
                *) bad="$bad '$tok' is not a button or stick the pad reads;" ;;
                esac
            done
        done
        if [[ -n $bad ]]; then
            ok=0
            printf 'BAD   %-10s%s\n' "$name" "$bad"
        else
            printf 'ok    %-10s %4ss  (title %ss, load %ss, %s pad steps)\n' "$name" "$secs" "$title" "$load" "$(printf '%s\n' $pad | wc -l | tr -d ' ')"
        fi
    done
    ((ok)) || echo "STOP: a route line above is wrong: a fill with it would cost a run and measure nothing"
    return $((ok ? 0 : 1))
}

# --- options -------------------------------------------------------------------------------------
routes=all
secs_override=
user_dir=
sandbox=
keep_sandbox=0
do_run=1
do_merge=1
dry_run=0
adopt_profile=0
prefer_sandbox=0
force=0
retry=1
quiet=0
while [[ $# -gt 0 ]]; do
    case $1 in
    --routes)
        routes=${2:?--routes needs a list}
        shift
        ;;
    --list-routes)
        list_routes
        exit 0
        ;;
    --check-routes)
        check_routes
        exit $?
        ;;
    --secs)
        secs_override=${2:?--secs needs a number}
        shift
        ;;
    --user-dir)
        user_dir=${2:?--user-dir needs a path}
        shift
        ;;
    --sandbox)
        sandbox=${2:?--sandbox needs a path}
        shift
        ;;
    --keep-sandbox) keep_sandbox=1 ;;
    --no-run) do_run=0 ;;
    --no-merge) do_merge=0 ;;
    --adopt-profile) adopt_profile=1 ;;
    --prefer-sandbox) prefer_sandbox=1 ;;
    --dry-run)
        dry_run=1
        do_run=0
        ;;
    --force) force=1 ;;
    --no-retry) retry=0 ;;
    -q | --quiet) quiet=1 ;;
    -h | --help) usage ;;
    *) usage ;;
    esac
    shift
done

# --- what this run will touch --------------------------------------------------------------------
real_user_want=${user_dir:-${BB_DATA_DIR:-.}/user}
real_user=$(cd -- "$real_user_want" 2>/dev/null && pwd) ||
    die "no user dir at $real_user_want: run the game once so the layout exists, or pass --user-dir DIR (the dir run.sh uses for the layout you play)"
sandbox=${sandbox:-$root/out/warmfill_user}
mkdir -p "$root/out"
sandbox=$(cd -- "$(dirname -- "$sandbox")" && pwd)/$(basename -- "$sandbox")
[[ $sandbox != "$real_user" ]] || die "the sandbox and the real user dir are the same directory"
case $sandbox in "$real_user"/*) die "the sandbox is inside the real user dir ($sandbox)";; esac

state=$root/out/warmfill
merge_tool=$root/tools/macos/warm_merge.sh
route_tool=$root/tools/macos/warm_route.sh
[[ -x $merge_tool || -f $merge_tool ]] || die "missing $merge_tool"
[[ -x $route_tool || -f $route_tool ]] || die "missing $route_tool"

if ((do_run)); then
    if [[ -n ${BB_GAME_DIR:-} ]]; then
        [[ -d $BB_GAME_DIR ]] || die "BB_GAME_DIR is not a directory: $BB_GAME_DIR"
    else
        for candidate in "$HOME/Games/Bloodborne/CUSA03173" "$HOME/Games/shadPS4/CUSA03173-109"; do
            if [[ -d $candidate ]]; then
                export BB_GAME_DIR=$candidate
                break
            fi
        done
        [[ -n ${BB_GAME_DIR:-} ]] ||
            die "no game data: set BB_GAME_DIR to the merged 1.09 folder (tried $HOME/Games/Bloodborne/CUSA03173 and $HOME/Games/shadPS4/CUSA03173-109). Without the game nothing draws, and a route compiles nothing."
    fi
    [[ -x $root/out/bb-probe ]] || die "out/bb-probe is missing: build first (bash tools/macos/build.sh)"
    # A running game owns the store the merge writes to; a route would also fight it for the GPU.
    if ((!force)) && { pgrep -x bb-probe >/dev/null 2>&1 || pgrep -x bb-gpu >/dev/null 2>&1; }; then
        die "the game looks like it is running (a bb-probe or bb-gpu process): quit it, or pass --force if that is another copy"
    fi
fi

echo "warm fill: routes '$routes' -> sandbox $sandbox; real store of $real_user"
echo "           game ${BB_GAME_DIR:-<not needed: --no-run>}; log dir $state"

# --- the routes, resolved ------------------------------------------------------------------------
selected=
for name in $(printf '%s' "${routes//,/ }"); do
    if [[ $name == all ]]; then
        selected="$selected $(route_names)"
        continue
    fi
    line=$(route_line "$name")
    [[ -n $line ]] || die "no route named '$name' (--list-routes shows the routes)"
    selected="$selected $name"
done
[[ -n ${selected// /} ]] || die "no routes selected"

# --- dry run: report, write nothing --------------------------------------------------------------
if ((dry_run)); then
    echo
    echo "dry run (nothing is written anywhere):"
    for name in $selected; do
        IFS='|' read -r _ secs title load pad desc <<<"$(route_line "$name")"
        printf '  route %-9s %ss\n' "$name" "${secs_override:-$secs}"
    done
    if [[ -d $sandbox/cache ]]; then
        merge_opts=""
        ((adopt_profile)) && merge_opts="$merge_opts --adopt-profile"
        ((prefer_sandbox)) && merge_opts="$merge_opts --prefer-sandbox"
        echo
        bash "$merge_tool" --dry-run $merge_opts "$sandbox" "$real_user"
    else
        echo "  no sandbox at $sandbox yet: nothing to merge"
    fi
    exit 0
fi

# --- the snapshot --------------------------------------------------------------------------------
mkdir -p "$state"
if ((do_run)); then
    if [[ $keep_sandbox == 1 && -d $sandbox ]]; then
        echo "snapshot: keeping the existing sandbox $sandbox ($(find "$sandbox/cache" -type f 2>/dev/null | wc -l | tr -d ' ') blobs)"
    else
        rm -rf "$sandbox"
        # cp -c clones on APFS: the saves and this cache cost nothing to copy, and nothing the run
        # writes reaches the original.
        if ! cp -cR "$real_user" "$sandbox" 2>/dev/null; then
            cp -aR "$real_user" "$sandbox"
        fi
        echo "snapshot: $real_user -> $sandbox (APFS clone, $(find "$sandbox" -type f | wc -l | tr -d ' ') files)"
    fi
elif [[ ! -d $sandbox ]]; then
    die "--no-run needs an existing sandbox: $sandbox"
fi

before_names=$state/before.names
find "$sandbox/cache" -type f 2>/dev/null | sed "s|^$sandbox/||" | sort >"$before_names" || true
blobs_before=$(wc -l <"$before_names" | tr -d ' ')
profile_before=$(md5 -q "$sandbox/cache"/*/profile.bin 2>/dev/null | md5 || echo none)

# --- the runs ------------------------------------------------------------------------------------
started=$(date '+%Y-%m-%d %H:%M:%S')
total_secs=0
routes_run=0
retries=0
routes_json=""
# The title screen draws ~24 a frame and an area several hundred, so the highest draws/frame a run's
# log reached says whether it ever got into the game at all. (BB_WARMFILL_GAMEPLAY_DRAWS overrides
# the floor, for a build whose numbers differ.)
gameplay_draws=${BB_WARMFILL_GAMEPLAY_DRAWS:-200}

# What the run's own log says about it: the highest draws/frame it reached, whether that beat the
# gameplay floor, and whether the GPU process died on the way (a library assertion in the log).
verdict() {   # LOGFILE -> "max_draws reached crashed"
    local log=$1 max=0 reached=0 crashed=0
    if [[ -f $log ]]; then
        max=$(grep -oE '[0-9]+ draws/frame' "$log" 2>/dev/null | awk '{print $1}' | sort -n | tail -1)
        max=${max:-0}
        ((max >= gameplay_draws)) && reached=1
        if grep -qE 'GPU library assertion failed|Bad PM4 packet|assert_fail_debug_msg' "$log"; then crashed=1; fi
    fi
    printf '%s %s %s\n' "$max" "$reached" "$crashed"
}

route_secs_taken=0
route_run() {   # NAME SECS TITLE LOAD PAD: one hidden run of one route, its output and log kept
    local name=$1 secs=$2 title=$3 load=$4 pad=$5 ran
    ran=$(date +%s)
    echo
    echo "=== route $name: ${secs}s (title ${title}s, load ${load}s)"
    if ! BB_WARM_USER=$sandbox ROUTE="$pad" TITLE_WAIT=$title LOAD_WAIT=$load \
        bash "$route_tool" "$secs" >"$state/$name.out" 2>&1; then
        echo "warning: route $name exited non-zero (see $state/$name.out)"
    fi
    route_secs_taken=$(($(date +%s) - ran))
    total_secs=$((total_secs + route_secs_taken))
    cp -c "$root/out/last-run.log" "$state/$name.log" 2>/dev/null || cp "$root/out/last-run.log" "$state/$name.log" 2>/dev/null || true
    tail -1 "$state/$name.out" 2>/dev/null || true
    # The run's own numbers, when there are any: a run that died before the game wrote its log
    # (a missing build, a crash) leaves none, and saying so beats a grep error.
    if [[ -f $state/$name.log ]]; then
        grep -E '^Warm run:' "$state/$name.log" | tail -1 || true
        grep -E '^Frame stats' "$state/$name.log" | tail -1 || true
    else
        echo "no game log for $name: the run did not get as far as writing one (out/last-run.log)"
    fi
    echo "route $name took ${route_secs_taken}s"
}

if ((do_run)); then
    for name in $selected; do
        IFS='|' read -r _ secs title load pad desc <<<"$(route_line "$name")"
        secs=${secs_override:-$secs}
        # The pad is only driven from title_wait + load_wait on and warm_route.sh stops 15 s before
        # the run's end: below that the route would sit at the title and compile nothing.
        if ((secs < title + load + 30)); then
            echo "note: route $name: ${secs}s is under the ${title}s + ${load}s the run spends before the pad moves; raised to $((title + load + 30))s"
            secs=$((title + load + 30))
        fi
        took=0
        retried=0
        route_run "$name" "$secs" "$title" "$load" "$pad"
        took=$((took + route_secs_taken))
        read -r max_draws reached crashed <<<"$(verdict "$state/$name.log")"
        max_draws=${max_draws:-0}
        # A run that never reached the game, or that asserted part-way through, measured nothing:
        # its blob count is about the run, not about the area. Re-run it once — the assertion seen
        # on the first fill was transient (the run after it walked the whole area) — and say why,
        # so a zero is never read as "already covered" when it was a dead run.
        if ((retry)) && { ((crashed)) || ((!reached)); }; then
            retried=1
            retries=$((retries + 1))
            if ((crashed)); then
                echo "route $name: the run's GPU process died mid-run (an assertion in $state/$name.log); re-running it once, its blob count is about a dead run, not about the area"
            else
                echo "route $name: the run never reached gameplay (highest ${max_draws} draws/frame; the title screen draws ~24); re-running it once, it walked nothing"
            fi
            route_run "$name" "$secs" "$title" "$load" "$pad"
            took=$((took + route_secs_taken))
            read -r max_draws reached crashed <<<"$(verdict "$state/$name.log")"
            max_draws=${max_draws:-0}
        fi
        # What the log says the run did, so a zero is never reported as if it were a measurement.
        if ((crashed)); then
            echo "route $name: the GPU process died mid-run (an assertion in the log): it walked part of its circuit, so its blob count is a floor, not the route's coverage"
        elif ((!reached)); then
            echo "route $name: NEVER REACHED GAMEPLAY (highest ${max_draws} draws/frame, the title's is ~24): this run did not get into the game, so its blobs say nothing about the area"
        else
            echo "route $name: reached gameplay (highest ${max_draws} draws/frame)"
        fi
        find "$sandbox/cache" -type f 2>/dev/null | sed "s|^$sandbox/||" | sort >"$state/$name.names"
        new=$(( $(wc -l <"$state/$name.names") - $(comm -12 "$before_names" "$state/$name.names" | wc -l) ))
        gone=$(( $(wc -l <"$before_names") - $(comm -12 "$before_names" "$state/$name.names" | wc -l) ))
        echo "store after $name: $(wc -l <"$state/$name.names" | tr -d ' ') blobs ($new the route made, $gone the game dropped)"
        if ((do_run)) && ((reached)) && ((!crashed)) && ((new == 0)); then
            echo "route $name: a live run that added no blobs: what it walked is already covered by the routes before it (or by the owner's own play)"
        fi
        routes_run=$((routes_run + 1))
        [[ -n $routes_json ]] && routes_json="$routes_json,"
        routes_json="$routes_json{\"route\":\"$name\",\"secs\":$secs,\"took_s\":$took,\"new_blobs\":$new,\"dropped_blobs\":$gone,\"max_draws_per_frame\":$max_draws,\"reached_gameplay\":$reached,\"gpu_crash\":$crashed,\"retried\":$retried}"
        cp "$state/$name.names" "$before_names"
    done
fi

blobs_after=$(find "$sandbox/cache" -type f 2>/dev/null | sed "s|^$sandbox/||" | sort | wc -l | tr -d ' ')
profile_after=$(md5 -q "$sandbox/cache"/*/profile.bin 2>/dev/null | md5 || echo none)
if [[ $profile_before != "$profile_after" ]]; then
    echo
    echo "note: the sandbox's profile.bin changed during the run: this build's version constants (or device profile) differ from the snapshot's, so the game cleared the sandbox store and rebuilt it. The real store is from the other build too: a plain merge would be thrown away at its next start ($merge_tool --adopt-profile is what makes the fill count)."
fi

# --- the merge -----------------------------------------------------------------------------------
added=0
if ((do_merge)); then
    echo
    merge_opts=""
    ((adopt_profile)) && merge_opts="$merge_opts --adopt-profile"
    ((prefer_sandbox)) && merge_opts="$merge_opts --prefer-sandbox"
    if bash "$merge_tool" $merge_opts --json "$state/merge.json" "$sandbox" "$real_user"; then
        added=$(sed -n 's/.*"added":\([0-9]*\).*/\1/p' "$state/merge.json" | head -1)
    else
        st=$?
        added=0
        if [[ $st == 2 ]]; then
            echo "nothing to merge: the sandbox holds no pipeline store ($sandbox/cache)"
        else
            echo "warning: the merge failed (exit $st)"
        fi
    fi
else
    echo
    echo "not merging (--no-merge): the sandbox's blobs stay in $sandbox"
fi

# --- the report ----------------------------------------------------------------------------------
echo
echo "warm fill done: $((blobs_after - blobs_before)) blobs added to the sandbox's stores, $added merged into the real one"
echo "  sandbox:  $blobs_before -> $blobs_after files ($sandbox)"
echo "  real:     $(find "$real_user/cache" -type f 2>/dev/null | wc -l | tr -d ' ') files ($real_user/cache)"
echo "  runs:     $routes_run route(s), $((total_secs / 60))m $((total_secs % 60))s of game time"
((retries)) && echo "  retries:  $retries route(s) re-run because their first run never reached gameplay or died"
if ((do_merge && added > 0)); then
    echo "  next:     the game preloads them at its next start (log line 'Preloaded N pipelines')"
fi
echo "  logs:     $state/<route>.log, $state/merge.json"

# The report as JSON, for a hook or a script to read (out/warmfill-report.json).
{
    printf '{"started":"%s","real_user":"%s","sandbox":"%s","dry_run":%s,"blobs_before":%d,"blobs_after":%d,"added_to_real":%d,"retries":%d,"routes":[' \
        "$started" "$real_user" "$sandbox" "$((dry_run ? 1 : 0))" "$blobs_before" "$blobs_after" "$added" "$retries"
    printf '%s' "$routes_json"
    printf '],"merge":%s}\n' "$(cat "$state/merge.json" 2>/dev/null || echo null)"
} >"$root/out/warmfill-report.json"
