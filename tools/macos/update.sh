#!/usr/bin/env bash
# tools/macos/update.sh: brings this checkout up to date with GitHub and rebuilds what changed.
#   bash tools/macos/update.sh            # the current branch
#   bash tools/macos/update.sh <branch>   # switch to a test branch (macos-0.4 is the main one)
# Local changes to tracked files are discarded (the branch matches GitHub exactly); bbport.ini,
# saves, deps-x86_64 and out/ are untracked and stay. The Mac app's Update button runs this.
# Everything it prints is also saved to out/last-update.log (to send when an update fails).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
mkdir -p out
exec > >(tee out/last-update.log) 2>&1
branch=${1:-$(git branch --show-current)}
[[ -n $branch ]] || { echo "STOP: no branch checked out; name one: bash tools/macos/update.sh macos-0.4" >&2; exit 1; }
old=$(git rev-parse HEAD)

echo "=== Fetching $branch from GitHub"
# Submodules are pinned and fetched by build.sh when missing; recursing here fails on refs that
# the submodules' own remotes no longer serve.
git fetch --no-recurse-submodules origin
git rev-parse --verify --quiet "origin/$branch" >/dev/null || { echo "STOP: no branch '$branch' on GitHub" >&2; exit 1; }
git checkout -q -f "$branch" 2>/dev/null || git checkout -q -f -b "$branch" "origin/$branch"
git reset -q --hard "origin/$branch"
new=$(git rev-parse HEAD)
if [[ $old == "$new" ]]; then
    echo "Already up to date: $(git log -1 --format='%h %s')"
else
    echo "Updated to: $(git log -1 --format='%h %s')"
    git log --format='  %h %s' "$old..$new" 2>/dev/null | head -20 || true
fi

# The build applies gpu/patches/fsr-vulkan to the FSR library's files in place, and git keeps
# those edits across a change of commit: another branch's versions of the patches would not
# apply over them. Back to the library's own files; build.sh applies this commit's patches.
fsr=gpu/third_party/fsr-vulkan
if [[ $old != "$new" && -e $fsr/.git ]]; then
    git -C "$fsr" checkout -q -- . && git -C "$fsr" clean -qfd
fi

# Each part remembers what it was last built from (a record in deps-*/ or out/, which stay). A
# part is skipped when its record matches and nothing rebuilt it since (its output is not newer
# than the record): every build script deletes its record when it starts, and a failed build
# writes none, so the next update builds again. BB_REBUILD=1: build everything anyway.
recorded() { [[ -f $1 ]] && cat "$1" || true; }
fresh() {  # fresh <record> <recipe> <output...>: the record matches, the outputs exist, none newer
    local record=$1 recipe=$2 output
    shift 2
    [[ ${BB_REBUILD:-} != 1 && -f $record && $(cat "$record") == "$recipe" ]] || return 1
    for output in "$@"; do
        [[ -e $output && ! $output -nt $record ]] || return 1
    done
}
# The app passes the game's environment (Extra variables too): variables that steer a build
# would make it build something other than what is recorded.
for var in BB_SKIP_GPU BB_DEPS BB_DEPS_ARM64 BB_ARCH BB_APP BB_RELEASE CC CXX CFLAGS CXXFLAGS LDFLAGS \
           CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER; do
    if printenv "$var" >/dev/null; then
        echo "note: $var is ignored by updates"
        unset "$var"
    fi
done
native=$([[ -f tools/macos/build_gpu_native.sh && -f deps-arm64/env.sh ]] && echo yes || true)

# Dependencies: setup_deps.sh skips what is built, but it still calls Homebrew (slow).
deps_recipe="$(git rev-parse HEAD:tools/macos/setup_deps.sh) $(git rev-parse HEAD:tools/macos/kosmickrisp-patches)"
if ! fresh deps-x86_64/.bb-recipe "$deps_recipe" deps-x86_64/env.sh deps-x86_64/setup.log; then
    echo "=== Dependencies"
    bash tools/macos/setup_deps.sh
    printf '%s\n' "$deps_recipe" > deps-x86_64/.bb-recipe
fi
# The native GPU process's own libraries, once they exist (BB_ARCH=arm64 setup_deps.sh).
if [[ -n $native ]] && ! fresh deps-arm64/.bb-recipe "$deps_recipe" deps-arm64/env.sh deps-arm64/setup.log; then
    echo "=== Dependencies (arm64)"
    BB_ARCH=arm64 bash tools/macos/setup_deps.sh
    printf '%s\n' "$deps_recipe" > deps-arm64/.bb-recipe
fi

# The game: this commit's code (all of it, and the submodules as checked out), the
# dependencies, the compiler and Homebrew's header-only libraries, and the native GPU process.
if git submodule status --recursive | grep -q '^[-+]'; then
    echo "note: a submodule is missing or not at the commit this branch pins (git submodule status)"
fi
build_recipe="$(git rev-parse 'HEAD^{tree}')
$(git submodule status --recursive)
$deps_recipe ${native:+arm64}
$(xcrun clang --version 2>/dev/null | head -1)
$("$(command -v brew || echo /opt/homebrew/bin/brew)" list --versions boost magic_enum robin-map glslang \
    spirv-tools 2>/dev/null | tr '\n' ' ' || true)"
build_outputs=(out/bb-probe out/gpu/libbbgpu.dylib)
[[ -n $native ]] && build_outputs+=(out/gpu-arm64/bb-gpu out/gpu-arm64/libbbgpu.dylib)
if fresh out/.bb-built "$build_recipe" "${build_outputs[@]}"; then
    echo "=== Build: already built from this code"
else
    echo "=== Build"
    bash tools/macos/build.sh
    # A native GPU process that failed to build (build.sh goes on without it) is tried again.
    if [[ -z $native || ! -f out/.bb-native-failed ]]; then
        printf '%s\n' "$build_recipe" > out/.bb-built
    fi
fi

app_recipe="$(git rev-parse HEAD:launcher-macos) $(git rev-parse HEAD:tools/macos/build_launcher.sh)"
if ! fresh out/.bb-app "$app_recipe" out/bbport.app/Contents/MacOS/bbport; then
    echo "=== Mac app"
    bash tools/macos/build_launcher.sh
    printf '%s\n' "$app_recipe" > out/.bb-app
    echo "The Mac app was rebuilt: quit and reopen it to use the new version."
fi
echo "=== Done"
