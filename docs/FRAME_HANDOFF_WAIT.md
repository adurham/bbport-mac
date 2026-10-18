# FRAME_HANDOFF_WAIT — the counter that discriminates the two queue waits, and what it says: neither of them is the 1.36 ms bubble

Established 2026-10-18, answering the one question `BUBBLE_STALL_ANALYSIS.md` left open:

> the per-frame 1.36 ms bubble at the frame handoff is a GPU queue wait — but *which* of
> `kk_queue_submit()`'s two waits, `mtl_wait_for_event()` (`kk_queue.c:274`) or
> `mtl_command_queue_wait_for_drawable()` (`kk_queue.c:294`)?

`BB_COMMIT_TRACE=1` (new, default off) answers it, and the answer is uncomfortable but clean:
**neither wait is in the window.** Both are enqueued at the queue tail a *full frame* before the
GPU reaches them and are already resolved when it does, so neither can stall anything; the GPU
still goes dry 0.88 ms at the two commit boundaries where they sit, which is a boundary cost, not
a wait. **No fix gate was built**, because the measurement removes the premise ("something told the
GPU to wait") rather than naming a lever. The counter and the one default-off experiment it
justified (`BB_DRAWABLES=<n>`) ship as instrumentation.

## TL;DR

- **Counter: `BB_COMMIT_TRACE=1`**, off by default, 0.9 % of the driver's lines when on, no
  behaviour change with it off. Per Metal commit it records which wait was enqueued ahead of it and
  the GPU's own `GPUStartTime`/`GPUEndTime` from the MTL4 commit feedback, so `dry_ms` = "the GPU
  had this commit queued and did not run it", on the GPU's clock. Plus the presenter side: the
  layer's `maximumDrawableCount`/`displaySyncEnabled` at configure, every drawable acquire that
  blocked, and the draws-in-flight count. `tools/macos/commit_trace.py` reads it.
- **The two waits, measured over 148 s of gameplay, one run with the corrected summary (two runs in total) (53.4-54.5 fps, 680 draws/frame):**

  | wait enqueued ahead of the commit | commits | dry total | **dry/frame** | peak | dry ≥ 0.5 ms |
  |---|---|---|---|---|---|
  | `mtl_wait_for_event` (:274) | 8 028 (1.00 /frame) | 5 614 ms | **0.70 ms** | 1.02 ms | 7 962 (99 %) |
  | `mtl_command_queue_wait_for_drawable` (:294) | 7 989 (1.00 /frame) | 1 411 ms | **0.18 ms** | 0.45 ms | 0 |
  | none | 103 819 (12.9 /frame) | 825 ms | 0.10 ms | — | 35 |

  So on the face of it the **event wait dominates, ~4 : 1**. That is the answer to the question as
  posed — and it is the wrong question, because of the next row.
- **Both waits are pre-resolved.** The wait is enqueued 18.4 ms (event) / 18.7 ms (drawable)
  before the GPU reaches it — a whole frame, because the port submits a frame of run-ahead — and
  the resolver is issued in the *same* `kk_queue_submit()` call, microseconds later (the CPU
  `mtl_signal_event` at `:322`) or one frame earlier on the queue itself (`signalDrawable` at
  `:311`, which the GPU executes before the next frame's wait). **0 of 2 803** event-tagged detail rows
  had the GPU arrive before the signal (`lag < dry`); lag is 18.4 ms against 0.73 ms of dry.
  A wait whose resolver was issued 18 ms ago cannot be the thing that holds the GPU for 0.7 ms.
- **The drawable pool is not exhausted and cannot be:** `maximumDrawableCount=3`, `displaySync=1`
  (Fifo), and **draws in flight peak at 1** across 3 055 blocked acquires, with **zero** acquires
  returning nothing. `nextDrawable` blocks 7.8 ms (p50) on the presenting thread — that is the
  layer pacing to the display, not a shortage (a shortage would drive in-flight to 3 and start
  returning nil).
- **`BB_DRAWABLES=<n>` ships default off** as the experiment this counter justified (raise the
  pool above the swapchain's image count). It is measured to be a **no-op by construction** — you
  cannot run out of 3 drawables while never holding more than 1 — so it was not A/B'd with 2+2
  runs, and it is not a fix. Nothing was shipped that changes a wait.
- **So the frame handoff is still 1.36 ms and still unexplained by a wait.** What the counter does
  account for is **0.88 ms/frame** (0.70 event boundary + 0.18 drawable boundary) of GPU dry at
  exactly two commit boundaries per frame, with the committing having happened ~18 ms earlier.
  That is the shape of the MTL4 *commit boundary*, not of a queue wait — and ~0.5 ms of the bubble
  is inside a commit, where commit-level feedback cannot see it. The next measurement has to be an
  encoder-level one (what `mtl_bubble.py` did for E2) with the queue waits now *excluded*.

## Method

Standing config, hidden runs, `tools/macos/warm_route.sh 190`:

```
BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173 BB_WARM_SOURCE_USER=…/user \
ROUTE='ly=128,rx=128:5' CAPTURES=0 BB_COMMIT_TRACE=1 \
BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_RENDER_RES=854x480 BB_OUTPUT_RES=2560x1440 \
bash tools/macos/warm_route.sh 190
```

Driver rebuilt from `deps-arm64/src/mesa-kosmickrisp` (patch
`tools/macos/kosmickrisp-patches/0006-kosmickrisp-commit-trace.patch`, which applies cleanly on
top of 0001-0005 to a clean Mesa checkout — verified), port via `tools/macos/build.sh`
(`Built: x86_64 out/bb-probe`, `Built: out/gpu-arm64/bb-gpu (arm64)`, no `STOP:`).

| run | fps (3 reports) | draws/frame | counter lines |
|---|---|---|---|
| 1 | 53.8 / 54.5 / 54.2 | 679-680 | 6 000 detail (cap; first build) |
| 2 | 53.6 / 53.4 / 53.9 | 678-680 | 12 122 detail, 91 windows |

Both runs are steady rows in the standing sense (draws ≥ 500, t ≥ 60 s). Load average 5.5-6.4 on
the owner's desktop throughout; 0 shader/pipeline compiles during the counted windows, 0 recorder
syncs, GPU thread idle 0.4-0.5 % — the rows are clean. (The `ab_compare.py` overlap check was not
part of this pass: the two runs are a reproducibility check, not an A/B.)

Frame = 18.45 ms. 148 064 ms of gameplay counted after skipping the first 60 s (title + load);
`python3 tools/macos/commit_trace.py out/run2.log --skip 60`.

## The layer at startup, and the drawable accounting

The counter prints this once per swapchain, and it is the first time the presenter's parameters
have come from the layer itself rather than being assumed:

```
BBCT layer maxDrawables=3 (image_count=3) displaySync=1 framebufferOnly=1 opaque=0 drawable 3440x1440
```

- `maxDrawables=3` because the WSI sets `metal_layer.maximumDrawableCount = image_count` and the
  swapchain asks for `minImageCount + 1`. Fifo, `displaySync=1`. BUBBLE's premise was right about
  every value.
- **Draws in flight, at the acquire, over 3 055 blocked acquires: max = 1.** The pool is never
  more than a third used, and no acquire ever returned nil (0/3 055). The GPU-side
  `waitForDrawable` therefore cannot be waiting for pool space.
- `nextDrawable` blocks **7 802 us (p50), 9 052 (p90), 9 232 (max)** on the presenting thread, every
  frame. That is a *CPU* block and it is display pacing: the layer hands out a drawable at the
  display's cadence, and the presenting thread waits for it ~8 ms (half a 60 Hz refresh) before it
  can render the frame. It is real, it is where 8 ms of the presenting thread goes, and it is not
  the GPU timeline's 1.36 ms — `BUBBLE_STALL_ANALYSIS.md` already established the GPU is not
  waiting on the CPU here (commits go out a frame early). It is worth its own look, as a *CPU*
  item, on the presenting thread.

## The waits, measured

Run 2, 2 s windows over 148 s. Run 1 (first build, with an unbounded detail filter) agrees on the
shape per row: the once-per-frame event-tagged boundary paid dry p50 **0.73 ms** and the
drawable-tagged one **0.32 ms** in gameplay, the same two numbers within noise.

```
dry  = 7850 ms of 148064 ms wall = 5.3 %      busy = 140214 ms = 94.7 %
commits 119836; dry>=0.5 ms: 7997, dry>=1.0 ms: 32

site      commits   dry ms  %wall   busy ms  peak us  o500  o1000
none       103819      825   0.56  138939    23994     35     31
event        8028     5614   3.79     775     1020   7962      1
drawable     7989     1411   0.95     499      448      0      0
```

- **The counter's own per-frame census** — this is the answer, and it is not "20 commits, one of
  them slow": in gameplay there is **exactly one event-tagged commit per frame** (8 028 in 8 025
  frames) and it pays **0.70 ms every single frame** (99 % above 0.5 ms; p50 0.728, p90 0.776 — a
  fixed cost, not a tail). The drawable-tagged commit is also 1.0/frame and pays 0.18 ms.
- The rest of the frame is 12.9 no-wait commits per frame (14.9 commits/frame in total, the big
  12-command-buffer submissions, ~2 ms of GPU work each) and they pay **nothing**: 0.10 ms/frame
  across all 103 819 of them. So the dry is not "the queue was empty in general"; it is at two
  specific, once-per-frame boundaries.
- Scale check, and its honest limit: at the commit level dry sums to 5.3 % of wall where E2's trace
  says the GPU idles 31.8 % of the frame. The commit-level metric **cannot** see idle inside a
  commit (feedback gives one span per commit), and MTL4 commits overlap (the same arithmetic gives
  94.7 % busy against E2's 68 %). So treat 5.3 % as a lower bound and read the *split*, not the
  total. This is the one thing the counter is not good for.

## Why neither wait can be the stall

`kk_queue_submit()` enqueues both waits at the **queue tail**, then commits. The resolvers are:

- `mtl_wait_for_event` (:274) ← `mtl_signal_event` (:322), in the **same** `kk_queue_submit()`
  call, after the commit loop — microseconds later, and before the next submit.
- `mtl_command_queue_wait_for_drawable` (:294) ← `mtl_command_queue_signal_drawable` (:311), also
  queued on the GPU, executed after that command buffer.

Because the port submits ~a frame of run-ahead, the GPU reaches either wait **18.4 / 18.7 ms**
after it was enqueued (`lag_us`, p50, and p10 = 18.0 / 18.4 ms — it is not a tail of outliers). By
then the resolver has long since been issued: for the event, the CPU issued it in the same call;
for the drawable, the GPU itself executed the signal a frame earlier. The counter's own check:

```
event rows with lag < dry (the GPU arrived before the resolver): 0 of 2803
event:    dry p50 0.728 ms, lag p50 18 400 us
drawable: dry p50 0.327 ms, lag p50 18 745 us
```

A wait that was resolved 18 ms ago cannot hold anything. **What the GPU is doing for 0.70 ms at
that boundary is not waiting on either resolver.**

Note this also disposes of the "semaphore chain serialisation" variant of the event hypothesis
that BUBBLE listed as the alternative worth 1.2 ms: for it to be the cost, the *signalling* submit
would have to arrive late. It does not — it is in the same call. The event values advance by 2 per
frame (`ev_value` 16 487 → 22 093 over 140 s, one per submit), and every one of them is signalled
a frame before it is awaited.

## What the dry at those boundaries actually is, and what is left

At both boundaries the commit was in the queue ~18 ms before the GPU went dry, so the gap is not
"no notice" (the packing probe closed that) and not "a wait" (above). What remains is the **MTL4
commit boundary itself**: the GPU finishes the last command buffer of one committed submission and
does not start the first command buffer of the next for 0.70 ms — the same class of cost
`METAL4_PACKING_PROBE.md` measured at 19-26 us per *command-buffer* boundary, but paid at the
*submission* boundary once per frame, and 25-35x larger.

0.70 + 0.18 = **0.88 ms/frame** of the 1.36 ms bubble is accounted for that way. The remaining
~0.5 ms is inside a commit, which commit-level feedback cannot see — so the honest state is:
the bubble is two once-per-frame commit boundaries plus something inside the frame's first big
submission, and **the queue waits are now excluded as causes**.

## Gates

- **`BB_COMMIT_TRACE=1` / `BB_COMMIT_TRACE_FULL=1`** — the counter. Default off; with it off the
  driver takes the same path it always did (`kk_commit_trace_enabled()` once per submit/commit).
- **`BB_DRAWABLES=<n>`** — the layer's `maximumDrawableCount` instead of the swapchain's image
  count, never below it. Default off. **Not A/B'd and not a fix**: with draws-in-flight peaking at
  1 of 3 and no acquire ever failing, the pool cannot be the limiter, and a 2+2 campaign on a knob
  that is provably inert would be noise dressed as evidence. It stays for the next present
  question (a smaller pool *forcing* pacing, or a larger one under Immediate).
- **Not shipped:** anything that drops, moves or reorders a wait. Dropping `waitForDrawable` is a
  correctness change (the GPU must not write a drawable the compositor owns) and would buy 0.18 ms;
  dropping the event wait would buy 0.70 ms of *dry at a boundary*, and by the section above that
  dry is not caused by the wait — so it would be a guess, and the standing rule says a gate ships
  on a named cause, not on a coincidence of position.

## Next step

The bubble is now a boundary/burst question, not a wait question. In order of cost:

1. **Encoder census on the handoff boundary with the waits excluded.** `tools/macos/mtl_bubble.py`
   on a fresh export, but with this counter's `dry_ms` as the cross-check: the 0.70 ms event
   boundary should light up as the gap between the last encoder of frame N and the first of the
   frame's 12-CB big submission, and the remaining ~0.5 ms should show as an intra-commit hole.
   That tells us whether the cost is the submission boundary or the frame's own first encoder.
2. **The 7.8 ms `nextDrawable` block on the presenting thread** is a separate, larger CPU item and
   the counter now measures it every frame. If the presenting thread is also the one that has to
   hand the next frame's work over, that is worth a pipeline-stage counter of its own — but do not
   confuse it with the GPU's 1.36 ms, which is on the GPU timeline.
3. Do not spend a run on `BB_DRAWABLES` or on `BB_PRESENT_MODE=immediate` for *this* bubble: the
   drawable wait is 0.18 ms of it and display pacing is already 8 ms off the GPU's critical path.

## Artifacts

- `tools/macos/kosmickrisp-patches/0006-kosmickrisp-commit-trace.patch` — the counter (driver
  `kk_queue.c`/`kk_queue.h` + WSI `wsi_common_metal_layer.m`), applies on top of 0001-0005.
- `tools/macos/commit_trace.py` — the census; every number above comes out of it.
- `out/run1.log`, `out/run2.log` (raw counter output), `out/last-run.log`.
- Driver references: `kk_queue.c:274`, `:294`, `:311`, `:322`; `wsi_common_metal_layer.m`
  `wsi_metal_layer_configure` / `wsi_metal_layer_acquire_drawable`.
