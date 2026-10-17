#!/bin/bash
# Sync the native line's shared files into the main checkout, the one way that works.
# Usage: tools/macos/sync_to_main.sh <native-tip-before-range> [--commit <message>]
# Copies every file the given range touches (skipping gpu/shim/remote/ and gpu/CMakeLists.txt),
# stages NEW files too (git add -u alone silently drops them), runs the main-line guard,
# rebuilds main, and - with --commit - commits and pushes. Prints every step; fails loudly.
set -euo pipefail
N="$(cd "$(dirname "$0")/../.." && pwd)"
R="${BBPORT_MAIN_CHECKOUT:-$HOME/repos/bbport-mac}"
RANGE="${1:?usage: sync_to_main.sh <since-sha> [--commit <message>]}"
shift
MSG=""
if [ "${1:-}" = "--commit" ]; then MSG="${2:?--commit needs a message}"; fi
[ -d "$R/.git" ] || { echo "no main checkout at $R"; exit 2; }
cd "$N"
FILES=$(git diff --name-only "$RANGE"..HEAD | sort -u)
[ -n "$FILES" ] || { echo "range $RANGE..HEAD touches nothing"; exit 0; }
cd "$R"
for f in $FILES; do
  case "$f" in
    gpu/shim/remote/*|gpu/CMakeLists.txt) echo "skip: $f"; continue;;
  esac
  [ -f "$N/$f" ] || { echo "skip (absent in native): $f"; continue; }
  mkdir -p "$(dirname "$f")"
  if ! cmp -s "$N/$f" "$f" 2>/dev/null; then cp "$N/$f" "$f" && echo "copy: $f"; else echo "same: $f"; fi
done
# shellcheck disable=SC2086
git add -A -- $FILES
git status --short | grep -vE '^\?\?|^ m ' || echo "(no staged change: main is already in sync)"
bash tools/macos/check_main_line.sh
bash tools/macos/build.sh > "$HOME/.hermes/cache/scratch/sync-main-build.log" 2>&1 || { echo "MAIN BUILD FAILED"; grep -E 'error:' "$HOME/.hermes/cache/scratch/sync-main-build.log" | head -6; exit 1; }
echo "main build ok"
if [ -n "$MSG" ]; then
  git commit -q -m "$MSG"
  git push https://github.com/adurham/bbport-mac.git feature/display-options:feature/display-options 2>&1 | tail -1
  git log --oneline -1
fi
