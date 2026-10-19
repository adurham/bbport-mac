#!/usr/bin/env bash
# tools/macos/pm4_synthetic.sh: the synthetic half of the 2026-10-19 campaign
# (docs/PM4_SYNTHETIC_2026-10-19.md). Drives tests/test_pm4_submit.cpp's arms - the split's real
# channel, wait/check/copy functions - with a guest that rewrites its command buffer as soon as
# submit returns, and appends one JSON line per arm to out/pm4_synthetic.jsonl.
#
#   ninja -C out/gpu pm4-submit-test          # or: bash tools/macos/build.sh (needs deps-x86_64)
#   bash tools/macos/pm4_synthetic.sh
#
# The fixed arm at 50 ms is the long one (~8 min for 10000 submissions); PM4_SYNTH_SKIP_LONG=1
# leaves it out.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
bin=out/gpu/pm4-submit-test
[[ -x $bin ]] || { echo "STOP: $bin is missing; build it (ninja -C out/gpu pm4-submit-test)" >&2; exit 1; }
out=${PM4_SYNTH_JSON:-out/pm4_synthetic.jsonl}
mkdir -p "$(dirname -- "$out")"

run() { # arm delay iters [extra...]
    local arm=$1 delay=$2 iters=$3
    shift 3
    echo "== $arm +${delay} us, $iters submissions $*"
    "$bin" --arm "$arm" --delay "$delay" --iters "$iters" "$@" 2>/dev/null | tail -1 | tee -a "$out"
}

# v8 (BB_SUBMIT_ASYNC=1): the positive control. The guest writes the buffer again as soon as submit
# returns, and the copy is taken late - the wider the delay, the more of the copy is the new words.
run old 0 20000
run old 100 5000
run old 500 2000
run old 2000 1000

# v9 (the fix): the call returns only after the copy, so the rewrite cannot land inside it.
run fixed 0 20000
run fixed 500 5000
run fixed 2000 2000

# The same fixed path with a guest that rewrites *during* submit (it breaks the in-process
# contract): the detector must see it, so a zero in the arms above is not a zero without a check.
run fixed 0 1000 --writer
run old 0 1000 --writer

if [[ ${PM4_SYNTH_SKIP_LONG:-0} != 1 ]]; then
    run fixed 50000 10000
    run old 50000 300
fi
echo "== $(grep -c '^{' "$out" 2>/dev/null || echo 0) arms in $out"
