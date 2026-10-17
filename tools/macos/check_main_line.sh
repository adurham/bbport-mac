#!/bin/bash
# The main line has no bb-gpu process: files synced from the native line must guard every
# reference to remote/ (the split-process IPC headers). Run this after a native->main sync.
set -u
cd "$(dirname "$0")/../.." || exit 2
bad=0
while IFS= read -r f; do
    [ -f "$f" ] || continue
    if grep -q '#include "remote/' "$f" && ! grep -q '__has_include("remote/' "$f"; then
        echo "unguarded remote include: $f"; bad=1
    fi
    if grep -q 'BbRemote::' "$f"; then
        if ! grep -q 'BBPORT_.*_REMOTE\|__has_include("remote/' "$f"; then
            echo "unguarded BbRemote use: $f"; bad=1
        fi
    fi
done < <(grep -rl 'remote/\|BbRemote::' gpu/ src/ 2>/dev/null | grep -v '^gpu/shim/remote/' | grep -v '^gpu/third_party/')
if [ $bad -eq 0 ]; then echo "check_main_line: ok (every remote/ reference in the main line is guarded)"; else echo "check_main_line: FAILED - guard with __has_include + an ifdef the way frame_stats.cpp does"; exit 1; fi
