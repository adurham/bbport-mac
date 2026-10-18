# BB_INPASS_BARRIERS result: cutlist #3 works, #1+#3 lands on the baseline

Measured 2026-10-17 on the MacBook M4 Max (hidden runs, laptop on the charger), branch
`fix/encoder-merge`, config `BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4` at 2560x1440, warm route with the
camera still (`ROUTE='ly=128,rx=128:5'`), four arms x 2 runs, alternated (base1, inpass1, copy1,
both1, base2, inpass2, copy2, both2; 190 s each, `CAPTURES=3`, `BB_PASS_BREAK_TRACE=1`). This is
cutlist item #3 of `bbport-mac-native/docs/ENCODER_ATTRIBUTION.md`, the other half of
`COPY_MERGE_RESULT.md` (item #1, which alone moved the break and cost +0.47 ms).

## What was built

`BB_INPASS_BARRIERS=1` (default off): `Runtime::FlushBarriers` / `FlushNarrowed` no longer call
`Scheduler::EndRendering()` for a barrier that carries **no image transition** when a render pass is
open. The barrier is recorded as before (`memoryBarrierCount = 1`, `dstStageMask = ALL_COMMANDS`,
`dstAccessMask = MEMORY_READ|MEMORY_WRITE`), so it lands between the draws of the open pass and the
driver records an intra-encoder barrier (KosmicKrisp `kk_cmd_buffer.c`, `kk_CmdPipelineBarrier2`
with `cmd->gfx.encoder` set: `mtl_barrier_after_encoder_stages(encoder, VERTEX, FRAGMENT)`).

The rule that gates it (`Runtime::BarrierStaysInPass`):

- the pending barrier set has **no image barriers** — a layout change is not in-pass material, so
  anything with a transition ends the pass as before;
- a pass is open (`Scheduler::IsRendering()`);
- the destination is `ALL_COMMANDS` (the default `BB_BARRIER_MASKS=legacy` barrier); a narrowed
  destination names stages the driver's own split rule looks at, so the gate stays out of it.

Everything else — the counters, `accesses.Clear()`, `global_write`, `pending_writes` — is unchanged,
so the access tracker still thinks the barrier happened; only the encoder boundary is skipped.

The invariant this relies on, stated in the code comment: **every encoder ends ALL->ALL**
(`kk_stop_encoder`, `BB_KK_BARRIER=legacy`). An in-pass barrier orders only what this encoder records
next, so any work it has to order that ran in an earlier encoder is already covered by that encoder's
full release. `BB_BREAK_STORE_PROBE`-style store/load games do not apply here: nothing is stored.

Spec caveat, recorded deliberately: `vkCmdPipelineBarrier` inside `vkCmdBeginRendering` is only
plainly legal with the dynamic-rendering local-read extensions; we own the driver, the gate is
default off, and this is measured rather than assumed. Gate off takes the old `EndRendering` path
(one extra predictable branch, no behavioural change).

## Numbers (per frame; the last 60 s of each run)

| run | p50 ms | p25 | p75 | present_wait | draws | cmd bufs | render enc | compute enc | breaks |
|---|---|---|---|---|---|---|---|---|---|
| base1 | 18.95 | 18.76 | 19.13 | 6.31 | 883 | 379.0 | 232.7 | 146.2 | 46.2 |
| base2 | 19.02 | 18.84 | 19.20 | 6.42 | 883 | 378.3 | 232.6 | 145.7 | 45.3 |
| inpass1 | 19.04 | 18.85 | 19.22 | 6.35 | 889 | 379.2 | 233.2 | 146.0 | 45.3 |
| inpass2 | 19.02 | 18.83 | 19.22 | 6.42 | 888 | 379.1 | 233.2 | 145.9 | 45.6 |
| copy1 | 19.57 | 19.36 | 19.78 | 6.82 | 883 | 391.7 | 242.0 | 149.8 | 55.0 |
| copy2 | 19.58 | 19.35 | 19.80 | 6.89 | 888 | 393.3 | 242.9 | 150.4 | 55.7 |
| both1 | 18.95 | 18.77 | 19.12 | 6.26 | 889 | 377.2 | 231.7 | 145.5 | 44.0 |
| both2 | 18.91 | 18.74 | 19.10 | 6.40 | 885 | 377.6 | 231.9 | 145.8 | 44.5 |

Means and deltas:

| arm | p50 ms | render enc | cmd bufs | compute enc | breaks | barriers kept in-pass |
|---|---|---|---|---|---|---|
| base | 18.98 | 232.6 | 378.6 | 145.9 | 45.8 | — |
| inpass | 19.03 (+0.05) | 233.2 (+0.6) | 379.1 (+0.5) | 145.9 (0.0) | 45.5 (-0.3) | 0.0 / frame |
| copy | 19.57 (+0.59) | 242.4 (+9.8) | 392.5 (+13.9) | 150.1 (+4.2) | 55.4 (+9.6) | — |
| **both** | **18.93 (-0.06)** | **231.8 (-0.85)** | **377.4 (-1.2)** | **145.7 (-0.2)** | **44.2 (-1.5)** | **13.8 / frame** |

**Copy+inpass minus base: -0.85 render encoders/frame (-0.4%), -1.2 command buffers, -0.06 ms p50.**
Against copy alone it is -10.65 encoders and -0.65 ms, i.e. the pair exactly undoes the regression
item #1 caused. It does not beat the baseline.

## Where the break went (`BB_PASS_BREAK_TRACE=1`, breaks/frame)

| caller | base | copy | both |
|---|---|---|---|
| `Runtime::CopyBufferRecord` (the copy's own break) | 36.8 | 0 | 0 |
| `Runtime::FlushBarriers` | 3.0 | 17.7 | **5.0** |
| `Rasterizer::BeginDraw` (waiting-copy hazard) | 0 | 12.3 | 13.6 |
| `Runtime::CopyBuffer` (copy that cannot wait: old path) | 0 | 12.0 | 12.0 |
| `ExecuteShaderHLE` | 2.9 | 9.2 | 9.4 |
| `Runtime::FillBuffer` | 0 | 0.7 | 0.7 |
| `SceneTargets::Transition` / `CopyImage` / `DownloadImage` | 3.0 | 3.0 | 3.0 |

Reading it: item #3 removes the breaks item #1 moved into `FlushBarriers` (17.7 -> 5.0, -12.7, and
13.8 barriers/frame are actually kept in-pass). What is left over from item #1 is not a barrier at
all: the copies that the hazard rule *refuses* to defer (`Runtime::CopyBuffer` 12.0, it must end the
pass itself), the draws that run into a waiting copy (`BeginDraw` 13.6, up from 12.3 because the pass
now stays open longer), and `ExecuteShaderHLE`'s own flushes (9.4). That is why both arms together
sit ~1.5 breaks above the baseline instead of ~10 below it.

Alone, `BB_INPASS_BARRIERS` fires **0 times per frame**: in the base build every barrier that arrives
while a pass is open carries an image transition (the sampled-texture transition queued for the draw
that is about to bind), and the copies' own breaks are `CopyBufferRecord`, not barrier flushes. With
item #1 on, the deferred copies leave memory-only barriers behind (`srcAccessMask = TRANSFER_WRITE`),
and those are the 13.8/frame the gate keeps.

The driver does not split the encoder for them, and that is visible in the numbers: 13.8 kept barriers
buy 12.7 fewer breaks and 10.65 fewer render encoders (one encoder boundary per break). It is also
explainable from `kk_barrier_requires_encoder_split`: its `any_write_access` set is
color/depth/shader/storage/memory writes and **not** `TRANSFER_WRITE`, so a copy-ordering barrier is
recorded as an intra-encoder Metal barrier. A barrier left over from a *shader* write in the same
position would still split the pass inside the driver (that set does match), which is the case the
gate never reaches here.

## Correctness

Same captures and metric as `COPY_MERGE_RESULT.md` (`out/ib_caps.py`, the
`barrier_masks_caps.py` metric: mean |diff| of the max channel, share of pixels off by >32, worst
64x64 tile), 3 shots per run, 2 runs per arm:

| pair | n | mean diff | >32 | worst tile |
|---|---|---|---|---|
| base-base (noise floor) | 3 | 5.50 | 0.42% | 45.2 |
| inpass-inpass (noise floor) | 3 | 7.69 | 1.33% | 39.7 |
| copy-copy (noise floor) | 3 | 6.22 | 1.39% | 46.8 |
| both-both (noise floor) | 3 | 5.91 | 1.40% | 42.9 |
| **base-both (the gate)** | 12 | **5.99** | **0.86%** | **36.5** |
| base-inpass | 12 | 7.32 | 1.75% | 58.8 |
| base-copy (item #1, unchanged) | 12 | 7.13 | 1.32% | 46.1 |

The gate's own pair (`base-both`, mean 5.99, >32 0.86%) sits between the two same-mode floors it can
be compared with (base-base 5.50/0.42%, both-both 5.91/1.40%) and well under the widest floor of the
set (inpass-inpass 7.69). The one cross-arm number that is above its own floor is the copy arm's
worst shot (>32 3.80% against a copy-copy floor of 2.11%), which is item #1 unchanged from
`COPY_MERGE_RESULT.md` (on-on there was 11.69% >32) and is not what this gate changes. Visually
(`cap-ib_base1-1.png` vs
`cap-ib_both1-1.png`, same standing camera) the two frames are the same scene, pose, HUD, candle
glows and haze.

- 0 late conflicts in all four copy-merge runs (`Copy merge (10 s): ... 0 late conflicts`).
- No capture shows the haze/pose drift that the earlier `BB_COPY_MERGE` draft produced; the hazard
  rule is untouched by this gate.

## Conclusion

Item #3 is implementable and safe exactly as specified - a memory-only barrier in an open pass really
does stop being an encoder boundary, 13.8 times per frame - but **#1+#3 does not reduce this frame's
encoder count or its time**: -0.85 render encoders/frame (-0.4%) and -0.06 ms p50 against base, both
inside run-to-run noise. What the pair does is cancel item #1's +0.47 ms / +9.8 encoders. The break
does not move again from the barrier; it stays at the three places item #1 created and #3 cannot
reach:

1. `Runtime::CopyBuffer` 12.0/frame - copies that cannot wait (destination already accessed in the
   pass, source written after the copy). These break the pass themselves.
2. `Rasterizer::BeginDraw` 13.6/frame - draws that must not pass a waiting copy.
3. `ExecuteShaderHLE` 9.4/frame - flushes around dispatches inside the pass.

The next honest lever is therefore not a fourth barrier mode but the hazard rule itself (what must
stay after a waiting copy), or moving those three break sites off the render encoder. Until then
`BB_COPY_MERGE` and `BB_INPASS_BARRIERS` stay default off: together they are performance-neutral, and
neutral-with-two-gates is worse than the baseline's single code path.

Reproduce: `bash out/ib_ab.sh` (8 hidden runs, `gpu_ab_run.sh` with `CAPTURES=3
BB_PASS_BREAK_TRACE=1`), `python3 out/ib_analyze.py out/run-ib_*.log` (table above, writes
`out/ib_arms.json`), `python3 out/ib_caps.py cap-ib_base cap-ib_inpass cap-ib_copy cap-ib_both`.
