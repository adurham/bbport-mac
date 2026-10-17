# BB_COPY_MERGE result: the copy's pass break moves, it does not go

Measured 2026-10-17 on the MacBook M4 Max (hidden runs, laptop on the charger), branch
`fix/encoder-merge`, config `BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4` at 2560x1440, warm route with the
camera still (`ROUTE='ly=128,rx=128:5'`), 3 runs gate off + 3 gate on, alternated (base1, probe1,
base2, probe2, base3, probe3; 190 s each). This is cutlist item #1 of
`bbport-mac-native/docs/ENCODER_ATTRIBUTION.md` (`Runtime::CopyBuffer`, 81% of all pass breaks).

## What was built

`BB_COPY_MERGE=1` (default off): a `vkCmdCopyBuffer` cannot be recorded inside a render pass, so the
port ends the pass at every copy (two render encoders each). With the gate, the copy waits on a
per-pass batch and is recorded when the pass ends anyway, in guest order (one `vkCmdCopyBuffer` per
buffer pair; consecutive copies of one pair share it). The gate is byte-identical off: the off path
is the old function body, unchanged.

Hazard rule (the copy now runs after *every* draw of the pass, so nothing recorded after it may touch
its bytes): any read or write of the destination, any write or read of the source, a paged binding
(`BB_LAYER_MEMORY`, any range), an overlap with a copy already waiting, and a barrier the tracker asks
for at the copy's own position all fall back to the old path. Access of commands already recorded come
from `IsBufferAccessed`; accesses of draws recorded *after* the copy are caught before those draws are
recorded (`Rasterizer::BeginDraw` -> `DeferredCopyOverlaps` ends the pass there, which is the break the
copy would have caused). `untracked_reads` adds the reads the tracker never records (vertex streams,
GDS). Anything that still touches a waiting range is counted as a late conflict (0 in every run).

## Numbers (per frame; 6 ten-second windows each)

| run | p50 ms | p25 | p75 | present_wait | draws | cmd bufs | render enc | compute enc | breaks |
|---|---|---|---|---|---|---|---|---|---|
| base1 | 19.01 | 18.81 | 19.20 | 6.44 | 890 | 379.6 | 233.7 | 146.0 | 44.9 |
| base2 | 18.97 | 18.80 | 19.17 | 6.38 | 885 | 378.8 | 232.9 | 145.9 | 45.5 |
| base3 | 19.04 | 18.83 | 19.24 | 6.37 | 885 | 379.6 | 233.0 | 146.6 | 46.6 |
| probe1 | 19.56 | 19.33 | 19.79 | 6.81 | 892 | 391.4 | 242.0 | 149.4 | 53.9 |
| probe2 | 19.44 | 19.25 | 19.65 | 6.68 | 885 | 390.9 | 241.4 | 149.5 | 54.4 |
| probe3 | 19.48 | 19.28 | 19.68 | 6.75 | 885 | 390.1 | 241.1 | 149.0 | 53.8 |

Gate on minus gate off: **p50 +0.47 ms**, command buffers **+11.3**, render encoders **+8.5**,
compute encoders **+3.3**, pass breaks **+8.8**. The gate makes the frame slightly slower, not faster.

Where the break went (`BB_PASS_BREAK_TRACE=1`, caller counts summed over each run):
`Runtime::CopyBufferRecord` 274k -> 0 (the copy no longer ends the pass), but
`Runtime::FlushBarriers` 23k -> 124k, `Runtime::CopyBuffer`'s barrier path 0 -> 88k,
`Rasterizer::BeginDraw`'s hazard breaks 0 -> 87k, `ExecuteShaderHLE` 21k -> 69k.

## Why

A copy's destination is almost always read by a draw in the same pass, and that draw needs a barrier
(the port asks `IsBufferAccessed` when it binds the range). Today the copy's break and that barrier
share one pass end, so the barrier is recorded with no pass open and costs nothing. With the copy
deferred, the same barrier is recorded while the pass is open, so it ends and restarts the pass
instead — the two render encoders move from the copy to the barrier, and the copies that *do* reach a
natural pass end add one more such break in a pass that would otherwise have continued.

So item #1 alone cannot win in this title: it needs item #3 (a memory barrier that is legal inside the
pass, `kk_cmd_buffer.c:673-699`) to stop the `FlushBarriers` path from splitting passes. With #3 the
copies can wait and the barrier can stay in-pass; without it, deferring only relocates the break.

## Correctness

- Captures, 3 shots per run, `barrier_masks_caps.py` metric (mean |diff| of the max channel, share of
  pixels off by >8 and >32, worst 64x64 tile): off-off pairs (n=6) mean 7.71, >32 2.78%; on-on pairs
  (n=6) mean 11.94, >32 11.69%; off-on pairs (n=19) mean 9.49, >32 6.52% — the gate-on/off diffs sit
  inside the same-mode spread, so the captures show no content change.
- 0 late conflicts in every gate-on run (the safety net never fired).
- An earlier build of the gate deferred copies without adding the vertex-stream/GDS reads to the
  hazard check; its captures differed from gate-off by mean 17-20 (>32 15-21%, clearly outside the
  noise floor) and the difference was visible as haze/fog and pose. That hole is fixed above; it is
  the reason the rule includes `untracked_reads`.

Reproduce: `bash out/cmp_ab.sh cm_base1:0 cm_probe1:1 ...` (gpu_ab_run.sh with `CAPTURES=3
BB_PASS_BREAK_TRACE=1`), `python3 out/cmp_parse.py out/run-TAG.log out/frames-TAG.csv`,
`python3 out/cmp_caps.py cap-cm_*`.
