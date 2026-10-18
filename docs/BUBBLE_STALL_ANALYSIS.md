# BUBBLE_STALL_ANALYSIS — the per-frame 1.36 ms GPU bubble is a queue *wait* at the frame handoff, and the commit-cadence item is closed

Established 2026-10-17, answering the two questions `GPU_TIMELINE_RECONCILIATION.md` (E2) left open:
what the recurring 1.33-1.38 ms bubble is, and whether "a quarter of commits go out 16-17 us before
use despite 18.3 ms of notice" is a real mechanism. Analysis only — the two E2 traces, no new GPU
work, no gate. Worktree `~/repos/bbport-encmerge-build` (`out/e2/{default,pipe_max}-gpu.xml`),
doc on `build/macos-native-0.6`.

## TL;DR

- **The bubble is at the frame boundary, once per frame, and it is 1.21-1.24 ms/frame — 70 % of the
  entire 1.73 ms 60 fps deficit.** Mean 1356 +- 47 us (default) / 1428 +- 37 us (pipe_max),
  0.89 / 0.87 per frame, min 1271 / max 1595. It is the single largest recurring gap in both traces
  and 33-34 % of all idle >= 150 us.
- **It is not a late commit and not a starved recorder.** The command buffer the GPU starts *after*
  the bubble was committed **325-407 us before the GPU went dry** — and this time the split is
  trustworthy, because the export's `CPU to GPU Latency` column is provably *commit*-based: across
  9 314 (default) / 16 900 (pipe_max) pairs of intervals that share a `cmdbuffer-id`,
  `lat_a - lat_b == start_a - start_b` **exactly** (median |diff| 0.0 us, median slope -1.00, zero
  spread). Work that was in the queue when the GPU stopped is work a serial MTL4 queue should have
  run. Something told the GPU to wait.
- **The driver has exactly two queue-level GPU waits, and both sit in `kk_queue_submit()`:**
  `mtl_wait_for_event()` (Vulkan timeline-semaphore wait, resolved by a CPU
  `mtl_signal_event()`) at `kk_queue.c:274`, and `mtl_command_queue_wait_for_drawable()` (swapchain
  drawable available) at `kk_queue.c:294`. Both are enqueued at the *queue tail*, before the commit
  of the following command buffers — which is precisely the geometry the trace shows. The fixed
  size, workload-independence and once-per-frame-at-the-frame-handoff position fit the **drawable
  wait** (present is `Fifo`, `displaySync=1`, `maxDrawables 3`, a 240 Hz mode) better than a
  data-dependent semaphore, but the trace cannot separate the two: this export has an empty
  `iosurface-accesses` column, no blit channel and no named encoders.
- **Commit cadence: CLOSED.** `METAL4_PACKING_PROBE.md` variant A is exactly the control E2 was
  missing: 300 command buffers **all committed up front in one `commit:count:` array** still show
  300.8 cross-submit gaps at 26.4 us = 7.95 ms/frame of idle, and total frame time is identical to
  variant C (300 individual commits): 10.658 vs 10.635 ms. Per-boundary drain is invariant to commit
  cadence, so "late commit" cannot be the mechanism for the port's 19.4 us drain. Do not build a
  streaming-commit gate.
- **No gate was built.** A named cause with a specific fix is what the standing rule requires, and
  the honest state is "one of two queue waits, discriminating measurement is 15 lines" — see
  *Next step*. Removing a queue wait is a correctness change (`waitForDrawable` exists so the GPU
  never writes a drawable the compositor still owns; the timeline wait exists because the guest's
  semaphore said so), so shipping one behind a gate on a guess is the wrong trade.

## Method

Re-ran the two E2 exports through a new census, `tools/macos/mtl_bubble.py` (same XML as
`mtl_gaps.py`; it also re-derives `mtl_gaps.py`'s totals, which match E2's):

```
python3 tools/macos/mtl_bubble.py ~/repos/bbport-encmerge-build/out/e2/default-gpu.xml  18.41
python3 tools/macos/mtl_bubble.py ~/repos/bbport-encmerge-build/out/e2/pipe_max-gpu.xml 18.40
```

Gap definitions are E2's: consecutive depth-0 `bb-gpu` intervals, gap = next.start - prev.end. The
bubble class is the mode of the >= 1 ms gaps +-15 % (1155-1563 us in default, 1224-1656 in
pipe_max), not a hard-coded
window, so it cannot be an artifact of picking 1.33-1.38.

## The bubble, measured

| | default | pipe_max |
|---|---|---|
| count | 47 (0.89 /frame) | 83 (0.87 /frame) |
| duration | 1356 +- 47 us (1271-1443) | 1428 +- 37 us (1284-1595) |
| ms/frame | **1.21** | **1.24** |
| share of idle >= 150 us | 33 % (63.7 of 191.6 ms) | 34 % (118.5 of 347.1 ms) |
| **preceding encoder** | Fragment, 11.2 us median (44/47) | Fragment, 12.1 us (83/83) |
| preceding encoder's commit→start | 17.8-18.1 ms (committed a frame early) | 17-26 ms |
| **following encoder** | Compute, 5.5 us (44/47) | Compute, 5.6 us (78/83) |
| following encoder's commit→start | 1763 us mean | 1753 us mean |
| following CB committed before the dry point | **+407 us** | **+325 us** |

Both traces agree to within their own noise (pipe's 5 % longer bubble is the same shape), which is
the first thing that kills the CPU-side explanations: pipe_max has a full frame of submit notice
(`first_submit -18.30`) and the same bubble.

## Position: the frame handoff, then a burst

Phase-locking every frame on the bubble's *end* (frame = bubble_end[k] .. bubble_end[k+1], 18.16 /
18.24 ms mean) gives the canonical frame. Busy and idle per 1 ms of phase, mean over 44 / 77 frames:

```
phase ms   0      1-8        9-12      13        14-15     16        17-18   | bubble
default  1.09  0.9-1.1/ms  2.25 ms   FSR 1.51   0.7 ms  2.10 ms   0.18   | 1.36 ms
gap      0.07  0.02-0.09  of gap   (42/43   of gap  of gap           |
                                     frames)
```

- **What is on the far side:** a dense opening burst — **47.1-47.4 encoder intervals in the first
  1 ms** of the frame (vs 8-9 /ms in the middle of the scene), 1.09-1.08 ms of work. The bubble ends
  where the frame's first big submission starts executing.
- **What is on the near side:** the thinnest part of the frame — the last 1 ms holds only 7.3
  intervals / 111 us of work, i.e. the GPU is already scraping the bottom of the queue when the
  bubble opens. The interval that ends at the bubble is a single ~11-12 us Fragment encoder (the
  last tiny pass of the frame's tail), whose command buffer was committed a *frame* earlier.
- The bubble is therefore not "the FSR chain's entry" (that is 13 ms later, latch 3 us — committed
  just-in-time, and it is the one pass that reliably pays no bubble), not a readback, and not a
  scene-target transition.

## What it is not

1. **Not workload elasticity.** Frame busy time is rock-stable (13.00 +- 0.12 ms/frame in default)
   while the bubble varies 1271-1443 us; corr(bubble, preceding frame's busy) = **0.13**. A cadence
   wait that absorbed slack would anti-correlate hard. It is a fixed cost.
2. **Not the recorder running dry of notice.** See the commit-basis proof above (+325..407 us).
3. **Not a foreign GPU client.** The whole export contains 17 `WindowServer` intervals and 4 `kitty`
   ones against 25 649 `bb-gpu` rows; nothing else overlaps the gap. (Load average 4-11 on the
   owner's desktop is real, but it is not on the GPU timeline.)
4. **Not a Metal 4 property.** The micro-bench never shows a >= 0.5 ms per-frame hole in variant A
   (300 CBs pre-committed): its largest gap class is 1.4/frame in 0.1-0.5 ms. The bubble is
   specific to the port's frame handoff.
5. **Not the encoder merge / packing story.** `METAL4_PACKING_PROBE.md` already priced it: 300
   encoders in 1 CB is 6 % *slower*.

## Where it must come from, in the driver

`kk_queue_submit()` (`deps-arm64/.../kosmickrisp/vulkan/kk_queue.c`) is the only place that puts a
bare wait into the queue ahead of committed work:

```c
for (wait in submit->waits)                                  /* kk_queue.c:271 */
   mtl_wait_for_event(queue->mtl_handle, sync->mtl_handle, wait->wait_value);   /* :274 */
...
   if (cmd_buffer->drawable)                                 /* :293 */
      mtl_command_queue_wait_for_drawable(queue->mtl_handle, cmd_buffer->drawable);  /* :294 */
```

Both are queue-tail waits: everything committed *after* them is stalled, everything before runs.
The trace's geometry — work committed 0.3-0.4 ms before the dry point, then 1.36 ms of nothing —
is what a tail wait looks like when its resolver arrives 1.36 ms later. `cmd_buffer->drawable` is
set by `kk_encode_drawable_present()` (`kk_wsi.c:92`) on the present command buffer, and is
signalled back at `kk_queue.c:311`. The other resolver is the CPU: `mtl_signal_event()` in
`kk_queue_submit`'s signal loop (`kk_queue.c:322`).

Two things make the drawable wait the better suspect, neither conclusive: its duration is set by
the display/compositor rather than by guest data (hence the fixed 1.36 ms and the +-47 us spread),
and it is unavoidably once per frame, whereas a semaphore wait would have to be the *frame-handoff*
wait specifically, one of ~7 submissions, every frame, forever.

## The commit-cadence verdict: CLOSED

The reconciliation's item was: *"why does bb-gpu hand the GPU 302 command buffers one at a time when
it has 18.3 ms of notice and only 6.7 ms of recording to do? A quarter of the work is still
committed ~16 us before use."* The micro-bench already answered it; E2 just did not connect them:

| variant | packaging | frame ms | cross-submit gaps | idle at those gaps |
|---|---|---|---|---|
| **A** | 300 CBs, **one** `commit:count:` array, all up front | 10.658 | 300.8/frame | 26.4 us each = **7.95 ms/frame** |
| **C** | 300 CBs, 300 individual commits | 10.635 | 146.6/frame | 47.8 us each = 7.01 ms/frame |

A is the control that matters: with every command buffer committed before the GPU starts, the
per-boundary gap does **not** shrink (26.4 us, in fact larger than C's mean, with more of them), and
total frame time is unchanged to 0.2 %. The 19-26 us is paid at the *MTL4 command-buffer boundary*
(encoder boundary + drain), not at the commit call. So:

- Pre-committing a frame's buffers cannot reduce the 5.86 ms drain. **No streaming-commit gate.**
- The 18.4 ms frame is not "late commits despite notice"; the recorder's just-in-time quarter is a
  symptom of the 302-boundary structure, not the cost.
- What survives from E2's item 1 is *only* the frame-handoff bubble above (1.21-1.24 ms), which is
  a wait, not a cadence.

## What this does to the 60 fps arithmetic

16.67 ms budget, 18.40 measured, deficit 1.73. The bubble is 1.21-1.24 ms of it and is the single
largest addressable item — bigger than the bespoke upscaler (0.615) and the encoder merges (0.6)
combined, and unlike them it is idle, not work. Everything else in the frame stays as E2 left it:
busy 12.5-13.2 ms, drain 5.86 ms at ~302 boundaries, fragment 5.41 / vertex 3.64 / compute 3.52.

If the bubble is the drawable wait, closing it does not delete GPU work; it removes a *stall* the
frame pays for presenting into a display-synced drawable — i.e. the frame would hand the next
frame's burst to the GPU 1.36 ms earlier and the frame period would follow. If it is the timeline
wait, the same 1.36 ms moves, but the fix is on the guest-semaphore side, not the WSI.

## Next step (cheap, and the only measurement that discriminates)

One counter in bb-gpu, no xctrace: behind `BB_COMMIT_TRACE=1`, log per commit
`mach_absolute_time(), queue, n_cb, (flags: wait_event | wait_drawable)` from
`kk_queue_commit()`/`kk_queue_submit()` into a ring, and stamp the wait's *resolution* (the first
commit after the `waitForEvent`/`waitForDrawable` call returns). Two lines of output per frame —
`first_commit_delay` and `dry_ms` (time the queue had work but the GPU did not start) — settle it:

- if `dry_ms` tracks the drawable wait's resolution delay -> the gate to build is *present
  scheduling* (acquire later / present without blocking the queue / `maximumDrawableCount`), and it
  is worth 1.2 ms;
- if it tracks a `wait_for_event` -> the guest semaphore chain has a frame-handoff serialisation
  worth the same, and the fix belongs where the semaphore is signalled, not in the WSI.

Both are >= 1.2 ms, well past the 0.5 ms bar; neither should be guessed at. `KK_BBSTAT_RERECORDS`
(`kk_device.h:160`) is worth printing at the same time — `rerecord_and_commit_cmd_buffer()` is the
other per-submit cost the trace cannot see, and it holds `queue->mutex` across a full re-record.

## Artifacts

- `tools/macos/mtl_bubble.py` (this doc's census; reproduces every number above from the two E2 XMLs)
- `~/repos/bbport-encmerge-build/out/e2/{default,pipe_max}-gpu.xml` (unchanged E2 exports)
- driver references: `kk_queue.c:271-322`, `kk_wsi.c:88-95`, `kk_cmd_buffer.h:241`
