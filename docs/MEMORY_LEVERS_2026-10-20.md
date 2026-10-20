# Two better-shaped memory levers: measure, and what they cost

Measured 2026-10-20 on the M4 Max (36 GiB), **hidden runs at the owner's real settings**: no
`BB_VRAM_LIMIT_MB` anywhere in this batch, so the renderer's own share is not held down by a 6 GiB
cap and the port's budget formula is the thing under test. `BB_NATIVE_GPU=1 BB_KK_STATS=1
BB_FRAME_STATS=1`, sandbox `BB_DATA_DIR`, same warm route as always (title -> Continue -> walk/turn
loop, `bash tools/macos/warm_route.sh 420`), 420 s, 3 s `footprint -p` sampler, watchdog at 19456
MiB / 2 GiB free+inactive.

Harness: `tools/macos/memprobe_arm.sh TAG [ENV=VALUE ...]` (the trim arm's script without the
`BB_VRAM_LIMIT_MB=6144` the probe runs needed) and `tools/macos/memprobe_arm_report.py TAG`, which
turns the run's own log into `out/memprobe-arms/<tag>-metrics.json`.

Every window figure comes from the port's own `Frame stats:` line, one per 5 s: `worst frame` is
that window's worst frame, so "windows over 100 ms" is the count of windows whose worst frame was
over 100 ms. Draws/frame in the same line proves gameplay was reached (title ~24, gameplay 550-970).

## The four runs

| arm | gate | footprint 120-300 s mean | end | max | renderer's own (median / max) | driver's own | allocators (held) |
|---|---|---|---|---|---|---|---|
| control | none | **14305** | 15259 | 15259 | **6107 / 6378** | 7619 | 165 (7291 MiB) |
| A1 trim, 40 ms line | `BB_KK_TRIM_POOL=5 BB_KK_TRIM_LOADONLY=1` | **12336** | 12398 | 13351 | 6175 / 6303 | 5704 | 129 (4387 MiB) |
| A2 trim, 100 ms line | same | **15129** | 15259 | 15259 | 6407 / 6568 | 6906 | 153 (7220 MiB) |
| B reserve budget | `BB_MEM_BUDGET_RESERVE=1` | **14305** | 14305 | 14305 | **4431 / 6114** | 6764 | 153 (6664 MiB) |

| arm | windows > 100 ms | fps median | worst-frame median | at matched draws (500-620/frame) | trims | KosmicKrisp heaps created/s |
|---|---|---|---|---|---|---|
| control | 9 of 83 | 44.3 | 34.6 ms | 44.4 fps, 37.0 ms | 0 | 0.98 |
| A1 | **37 of 82** | 44.2 | **92.8 ms** | **34.5 fps, 115.3 ms** | 46 | **212.7** |
| A2 | 7 of 83 | 39.6 | 72.0 ms | 42.6 fps, 51.0 ms | 7 | **386.5** |
| B | **3 of 83** | 42.4 | **34.0 ms** | 41.2 fps, 38.5 ms (620-700 band) | 0 | 0.93 |

A1's run is 418 s of 420 (the harness killed it two seconds early; every number is from its own log,
nothing at the end is inferred). A2 and B are complete, exit status 0. No run died on
`gcn_to_vk.cpp:422` / `cp_graphics.cpp:203` this time.

## ARM A: trimming outside play does not work, at either threshold

The gate keeps `<secs>` as the **minimum** spacing and holds the trim armed until a `Commit()` where
the last reported flip was not play — `vulkan/play_state.h`, fed by `OnFlipStats` (frame time and
draws/frame it already had, no new signal).

Two thresholds, because the first one showed the fault line:

- **40 ms** (the frame stats' own long-frame reporting line) — 46 trims, and they landed on frames of
  **42-45 ms with 555-615 draws/frame**: a mild stutter during play, not a stall. Each trim then
  added its cost to that frame and pushed it over 100 ms. 18 of 36 steady-state windows hitch,
  worst-frame median 103-115 ms at matched draws, fps 44.4 -> 34.5 (−22 %). Reclaim is real though:
  **−1969 MiB steady, −2861 MiB end, allocator hold −2904 MiB**.
- **100 ms** (a frame that is already a hitch, so a trim cannot make a window hitchier) — only 7
  trims in 420 s, and the sawtooth regrows between them: **footprint 15129 steady / 15259 end, i.e.
  no reclaim at all** (control 14305 / 15259), while hitches stay at control level (7 vs 9 of 83).
  It still costs: worst-frame median 72.0 ms against control's 34.6, fps 39.6.

The mechanism is in the last column of the second table: **KosmicKrisp re-creates 213-387 heaps and
upload blocks per second** under either trim threshold, against 1/s in the control. Destroying a
command buffer destroys the allocator's encoding heaps and re-encoding allocates them again. The
work is proportional to the bytes reclaimed (a trim of 7-8 buffers measured a **median −1114 MiB**
footprint drop, ~1 GiB of allocator memory destroyed and rebuilt), and the pool regrows at ~1.6
buffers/s of play. So holding a 1.5-2 GiB ceiling *requires* ~200-390 heap create/destroy per second
no matter what the trigger is; the trigger can only decide which frame pays. There is no moment
during 600-900 draws/frame gameplay when that is free, and genuine stalls are too rare (7 in 420 s)
to hide it in.

**ARM A is not shippable, as shaped.** What it does prove is that the reclaim is real and cheap in
*bytes* — the cost is all in where it lands, which is a driver problem: the allocator ring the docs
already describe (`docs/KK_ALLOCATOR_TRIM_2026-10-20.md`, "What a driver patch would have to do")
removes the destroy/re-create cycle entirely and is still the only shape that gets the ceiling for
free.

## ARM B: a reserve-aware budget caps the renderer's share and does not churn

`BB_MEM_BUDGET_RESERVE=1` replaces the GPU budget with `hw.memsize − wired − compressor − 6 GiB`,
floored at 4 GiB, min()'d with the port's own `UnifiedMemoryBudget()` — it can only take budget away.
Inputs and result print on the existing 1 GiB-move rule:

```
GPU: memory budget 7638 MiB (reserve: hw.memsize 36864 MiB, less 15857 MiB wired and 7224 MiB
compressor, less 6144 MiB reserve = 7638 MiB, floor 4096 MiB; the port's own budget 26616 MiB,
so the reserve is the binding one)
```

84 such lines in 420 s: the ceiling was **18050 MiB at the start and 8685 MiB at the end, and hit the
4096 MiB floor** at peak pressure (wired peaked at 18349 MiB and compressor at 8539 MiB — 26.9 GiB of
a 36 GiB machine, exactly the state that precedes jetsam). The reserve, not the port's formula, was
binding all run; without the gate the budget sat at 26.4-28.5 GiB, i.e. it never bound at all.

What it bought:

- **the renderer's own share is capped**: median 4431 MiB against the control's 6107, max 6114
  against 6378 (the cap is what the median shows — the control sat above it for most of the run);
- **peak footprint −954 MiB** (14305 against 15259) and end footprint −954 MiB; the 120-300 s mean is
  unchanged (14305 both), and the reason is visible in the split: graphics memory fell 2531 MiB
  (renderer −1676, driver −855) while `phys_footprint` did not, so some of what left the texture
  cache went to memory outside the graphics ledger. Footprint is the number jetsam acts on, and it
  only improved at the peak;
- **hitches improved**: 3 of 83 windows over 100 ms against the control's 9, worst-frame median 34.0
  ms against 34.6;
- **no fps regression beyond the scene**: the walk route's position shifts run to run by the title
  press, so B's band is heavier (median 645 draws/frame against the control's 590). Same-phase
  comparison: the first 20 windows are identical at 48.2 fps, the last 30 are 41.0 against 41.8, and
  in its own band (620-700 draws/frame) B runs 41.2 fps with a 38.5 ms worst-frame median against
  the control's 41.8 fps / 37.0 ms late in the run;
- **no eviction churn**: heaps created 0.93/s against the control's 0.98/s, upload blocks 0.30/s
  against 0.33/s, live images steady at 722-800 (control 1106-1358) with a 1.1 % median swing between
  10 s reports (control 0.0 %, max 8.0 % both). The known failure mode — a budget so tight the cache
  evicts images in use — did **not** appear at a ~7-8.7 GiB ceiling. It appeared in the port's
  history when the budget was derived from KosmicKrisp's own unaccounted graphics memory; this
  formula asks the machine instead, which is why it can be tight without thrashing the cache.

## Recommendation

- **Ship nothing by default yet; take B to a longer validation.** B is the only one of the two that
  holds a ceiling without a cost in hitches, churn, or fps, and it is the only one that answers the
  actual failure (jetsam while the machine is at 27 GiB of wired + compressor). Its weakness is that
  the win is at the peak, not the mean: 954 MiB of peak footprint is a start, not a fix, and the
  6 GiB reserve is the knob that decides how much of the rest it gets.
- **Drop ARM A.** At a threshold where trims are free, the pool regrows and nothing is reclaimed; at
  a threshold where reclaim happens, the frame cost is the same 5 s clock's. `BB_KK_TRIM_LOADONLY`
  stays in the tree as the measurement that says so (default off, no behaviour change), and
  `docs/KK_ALLOCATOR_TRIM_2026-10-20.md` still names the real fix: an allocator ring in KosmicKrisp
  instead of one allocator per command buffer per encoder state.
