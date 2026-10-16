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
#   --routes LIST     routes to run: names separated by commas, or "all" (default: yharnam)
#   --list-routes     print the routes and exit (no game, no writes)
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
#   -q, --quiet       the runs' own output is already logged; keep the report short
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
    grep '^# ' "$0" | sed 's/^# \{0,1\}//' | head -46
    exit 1
}

# name|secs|title_wait|load_wait|pad|what it walks.
#
# The pad entries are warm_route.sh's "<pad file contents>:<seconds>"; the comma stands in for the
# space the pad file wants between tokens (src/runtime_pad.c reads button names and lx= ly= rx= ry=
# in 0..255, 128 at rest). `secs` is the whole run: the game quits at that mark (BB_WARM_SECONDS),
# and warm_route.sh only starts driving the pad at title_wait + load_wait, so a route shorter than
# that walks nowhere — warmfill raises the length of one that is too short rather than run it empty.
builtin_routes='
yharnam|300|45|75|ly=0,rx=128:4 ly=0,rx=40:0.5 ly=0,rx=128:4 ly=0,rx=215:0.5 ly=255,rx=160:3|the measured walk of an area: forward, a look right, forward, a look left, back
attack|300|45|75|r1:2 r1:1 r1:1 circle:1.5 ly=0,rx=128:2 r2:2 l1:2 r1:2|light attacks, a roll, a walk, a heavy, the transform: combat states a plain walk never reaches
spin|240|45|75|rx=128:6 rx=88:0.5 rx=128:5 rx=168:0.5 rx=128:5 rx=40:6 rx=128:5 rx=215:6 rx=128:4 ry=40:5 ry=215:5|turn in place, then look down and up: headings and the sky are their own draws
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

# --- options -------------------------------------------------------------------------------------
routes=yharnam
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
routes_json=""
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
        ran=$(date +%s)
        echo
        echo "=== route $name: ${secs}s (title ${title}s, load ${load}s)"
        if ! BB_WARM_USER=$sandbox ROUTE="$pad" TITLE_WAIT=$title LOAD_WAIT=$load \
            bash "$route_tool" "$secs" >"$state/$name.out" 2>&1; then
            echo "warning: route $name exited non-zero (see $state/$name.out)"
        fi
        took=$(($(date +%s) - ran))
        total_secs=$((total_secs + took))
        cp -c "$root/out/last-run.log" "$state/$name.log" 2>/dev/null || cp "$root/out/last-run.log" "$state/$name.log" 2>/dev/null || true
        tail -1 "$state/$name.out" 2>/dev/null || true
        grep -E '^Warm run:' "$state/$name.log" 2>/dev/null | tail -1 || true
        grep -E '^Frame stats' "$state/$name.log" 2>/dev/null | tail -1 || true
        echo "route $name took ${took}s"
        find "$sandbox/cache" -type f 2>/dev/null | sed "s|^$sandbox/||" | sort >"$state/$name.names"
        new=$(( $(wc -l <"$state/$name.names") - $(comm -12 "$before_names" "$state/$name.names" | wc -l) ))
        gone=$(( $(wc -l <"$before_names") - $(comm -12 "$before_names" "$state/$name.names" | wc -l) ))
        echo "store after $name: $(wc -l <"$state/$name.names" | tr -d ' ') blobs ($new the route made, $gone the game dropped)"
        routes_run=$((routes_run + 1))
        [[ -n $routes_json ]] && routes_json="$routes_json,"
        routes_json="$routes_json{\"route\":\"$name\",\"secs\":$secs,\"took_s\":$took,\"new_blobs\":$new,\"dropped_blobs\":$gone}"
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
if ((do_merge && added > 0)); then
    echo "  next:     the game preloads them at its next start (log line 'Preloaded N pipelines')"
fi
echo "  logs:     $state/<route>.log, $state/merge.json"

# The report as JSON, for a hook or a script to read (out/warmfill-report.json).
{
    printf '{"started":"%s","real_user":"%s","sandbox":"%s","dry_run":%s,"blobs_before":%d,"blobs_after":%d,"added_to_real":%d,"routes":[' \
        "$started" "$real_user" "$sandbox" "$((dry_run ? 1 : 0))" "$blobs_before" "$blobs_after" "$added"
    printf '%s' "$routes_json"
    printf '],"merge":%s}\n' "$(cat "$state/merge.json" 2>/dev/null || echo null)"
} >"$root/out/warmfill-report.json"
