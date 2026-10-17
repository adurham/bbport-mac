# Submission-structure survey (read-only source + trace survey)

Repos: `/Users/adam.durham/repos/bbport-mac-native` (port, branch `build/macos-native-0.6`),
`/Users/adam.durham/repos/bbport-kk-build` (the KosmicKrisp driver source),
`/Users/adam.durham/repos/bbport-metalfx-build` (today's Metal System Trace XMLs + `tools/macos/mtl_gaps.py`).
Started 2026-10-17. No builds, no runs, no GPU: every number below comes from the two trace exports
and the two run logs named where used. Read with `docs/ENCODER_MERGE_SURVEY.md` and `docs/KK_GPU_WALL.md`.

Traces used: `out/mtl-2560_fsr3_4-gpu.xml` (2560x1440, FSR 3.1, 183 frames, 19.28 ms/frame) and
`out/mtl-2560_off1-gpu.xml` (upscaler off, 168 frames, 25.54 ms/frame). Run logs:
`out/run-2560_fsr3_4.log` (port stderr + KosmicKrisp `BB_KK_STATS`).

## Headline (read this first)

The premise "~340 command buffers -> a few dozen" is right about the **number**, but the number is
**not** the port's `vkQueueSubmit` count. The port already submits only **~3 `vkQueueSubmit` per frame**
(`6.00 VkCommandBuffers each`, so ~18 `VkCommandBuffers`/frame). The driver's own counter agrees:
**~17-18 Metal queue commits/frame**. The ~340 units in the trace are **MTL4CommandBuffers created
inside KosmicKrisp, one per Metal encoder**, and all ~340 of them are already carried inside those
~18 commits. The 6.7 ms/frame of ~20 us "cross-submit" gaps sit at **MTL-command-buffer boundaries
inside one commit**, not at CPU->GPU handover points. So the count to cut is the driver's
**MTL4CommandBuffer-per-encoder**, not the port's submit count. There is essentially no port-side
"fewer/larger submissions" batching left to do.

## Q1 The submission structure: every path to a queue submit

### Port — `Scheduler::QueueSubmit` is the only command-buffer submit
`Scheduler::QueueSubmit` (`gpu/render/scheduler.cpp:878`) is the single `vkQueueSubmit`
(`scheduler.cpp:905`, inside the `CatEmit` timer). It is reached from two entry points, both of which
call it with the recorded segments of one submission:
- `Scheduler::SubmitExecution` (`scheduler.cpp:915`) — synchronous path; builds `cmdbufs` from
  `batch->segments` (`scheduler.cpp:948-958`) and calls `QueueSubmit` (`scheduler.cpp:963`).
- `Scheduler::SubmitAsync` (`scheduler.cpp:978`) — default (`BB_ASYNC_SUBMIT`); the submit goes out
  from the recording threads via `RecordOrdered` (`scheduler.cpp:1015-1033`, `QueueSubmit` at `:1026`).
- `Scheduler::Flush` (`scheduler.cpp:787`) -> `SubmitExecution`; `Scheduler::Finish`
  (`scheduler.cpp:796`) = `Flush()` + `Wait(tick)` (`:797-798`).

Callers of `Flush`/`Finish`, with the per-frame shape:

| caller | anchor | what it is | ~/frame |
|---|---|---|---|
| `Rasterizer::Flush` | `render/rasterizer.cpp:756-767` (`scheduler.Flush(info)` `:760`) | end of one guest submission's draw recording | ~3 |
| `Rasterizer::OnSubmit` | `render/rasterizer.cpp:771-779` | per-submission housekeeping (fault buffer `:772-776`, `ProcessDownloadImages` `:777`, GC `:778`, `TickFrame` `:779`) | ~3 (called with Flush) |
| `CommandProcessor::AfterBatch` | `cp/command_processor.cpp:462-472` | calls `OnSubmit()`+`Flush()` after each decoded guest submission | ~3 |
| `CommandProcessor` label path | `cp/command_processor.cpp:557`, `:564` | `r.Flush()` after the frame's signals | ~1 |
| `Rasterizer::RequestSignalFlush` | `render/raster_signals.cpp:92-101` | flush when guest labels are pending | few |
| `Presenter::Encode` frame end | `render/presenter.cpp:350-352` | `NoteFrameEndSubmission()` + `draw_scheduler.Flush(info)`; sets `frame->ready_tick` | 1 |
| `Presenter::PrepareBlankFrame` | `render/presenter.cpp:374-377` | blank flip still ends a submission | 0-1 |
| present submission | `render/presenter.cpp:692-707` | `present_scheduler.Flush(info)` with the acquire + frame-ready waits, then `Present()` | 1 |
| MetalFX upscaler | `upscale/vk_metalfx.cpp:373` (`Finish`), `:385`, `:421/:436` (`Flush`) | MetalFX modes only (Sync/Events/SubmitThread) | 0 (FSR3 path) |
| `TextureCache::DownloadImageMemory` sync | `textures/texture_cache_memory.cpp:124` (`scheduler.Finish`) | host readback drain | ~1 + GC |
| `BufferCache` host copy | `buffers/buffer_cache.cpp:205-240` (`submit2` `:228`) | one-off readback command buffer with a semaphore wait | per copy |
| `BufferCache::SubmitPendingArenaBinds` | `buffers/buffer_cache.cpp:3711`, hooked at `render/rasterizer.cpp:55` | arena binds appended to the *current* submission (no submit of its own) | - |
| `BbGuestMemory::RunOnce` | `shim/bbport_guest_memory.cpp:352-360` | one-off startup submits | 0 steady |
| guest GNM submits | `platform/gnm/gnm_driver.cpp:513/534/546/567` (`Submit`), ding-dong `:510-527` | guest DCB/ASC -> the same graphics queue (no separate `QueueSubmit`; the draw scheduler owns it) | - |

The measured count: `out/run-2560_fsr3_4.log` prints
`Recording: 3.0 submissions/frame, 6.00 command buffers each` (heavy scene; `1.00` during load) —
from `recorded_submissions`/`recorded_segments` in `Scheduler::QueueSubmit`
(`scheduler.cpp:911-912`), printed at `platform/videoout/frame_stats.cpp:656`. So the port issues
**~3 vkQueueSubmit/frame, each carrying ~6 VkCommandBuffers (the async recorder's segments),
~18 VkCommandBuffers/frame.**

### Driver — where KosmicKrisp commits
`kk_queue_submit` (`kk_queue.c:261`, installed as `driver_submit` at `:387`) loops the submit's
`VkCommandBuffer`s (`:289`) and, for each, calls `kk_queue_commit` (`:300-306`). `kk_queue_commit`
(`kk_queue.c:95-108`) bumps `KK_BBSTAT_COMMITS` and issues **one** `mtl_command_queue_commit`
(`:104`) over the command buffer's whole `submit_cmd_bufs` array. So **one Metal queue commit per
VkCommandBuffer**, and it may carry many MTL4CommandBuffers.

The driver's counter agrees with the port: `BB_KK_STATS` "Metal commits" (`kk_device.c:91-140`,
printed every 10 s) grows by 8424-9432 per 10 s during the heavy scene, i.e. **~17-18 commits/frame**
at the ~49-51 fps the same log reports.

### Explaining the 551 vs 340 gap
`out/mtl-2560_fsr3_4-gpu.xml` has **339.9 distinct command-buffer ids/frame** but
**551.4 distinct `gpu-submission-id`/frame**. Reading the trace directly: each `gpu-submission-id`
covers **exactly one** command buffer (100908 of 100915 rows); over the whole trace there are
**62197** distinct command buffers for **100908** submission ids, i.e. **62% of command buffers carry
two consecutive submission ids** whose GPU intervals are ~0.1 us apart (`/tmp/lat_probe` style probe,
`scratch/cbsub.py`). A submission id is therefore **a Metal GPU scheduling unit, one command buffer
each — not a CPU commit**. The CPU commit count is the single-digit-to-~18/frame in the logs above.
`mtl_gaps.py` labels any boundary whose two sides differ in `gpu-submission-id` "cross-submit"; since
almost every command-buffer boundary differs, its "cross-submit 340.4/frame" is essentially
**one per command-buffer boundary**, and the name over-states what it is.

## Q2 Where the boundary idle comes from

`python3 tools/macos/mtl_gaps.py out/mtl-2560_fsr3_4-gpu.xml` (fsr3_4):

```
bb-gpu depth-0 intervals 100915, span 3527.6 ms, frames 183 (19.28 ms/frame)
busy 12.53 ms/frame (65.0%), >=2 channels 0.05 ms/frame (0.4% of busy), idle 6.75 ms/frame
  command buffers/frame 339.9, submissions/frame 551.4, encoders/frame 340.9
idle gaps 97928 (535.1/frame), 6.75 ms/frame:
  same-cmdbuf    194.7/frame   0.04 ms/frame (mean    0.2 us)
  same-submit      0.0/frame   0.00 ms/frame (mean    0.0 us)
  cross-submit   340.4/frame   6.71 ms/frame (mean   19.7 us)
  size <5us        404.5/frame   0.37 ms/frame
  size 5-20us       67.9/frame   0.75 ms/frame
  size 20-100us     56.3/frame   2.21 ms/frame
  size 0.1-0.5ms     4.6/frame   1.10 ms/frame
  size >0.5ms        1.7/frame   2.33 ms/frame
  gaps >=0.1 ms by (kind, before, after) channel:
      1.79 ms/frame ('cross-submit', 'Fragment', 'Vertex')
      0.66 ms/frame ('cross-submit', 'Fragment', 'Compute')
      0.60 ms/frame ('cross-submit', 'Compute', 'Vertex')
```

`off1` is the same shape: idle 5.71 ms/frame, cross-submit 329.4/frame 5.61 ms (mean 17.0 us),
sizes `<5us 0.51`, `5-20 0.27`, `20-100us 0.47`, `0.1-0.5 0.76`, `>0.5 3.70` (ms/frame).

**cpu-late vs queued: the tooling.** The `mtl_gaps.py` copies in *both* repos are byte-identical
(`diff` empty) and this revision prints **no cpu-late/queued hint at all** — its classification is
only `same-cmdbuf / same-submit / cross-submit` (by `gpu-submission-id`) plus the size histogram and
the ">=0.1 ms by (kind,before,after) channel" table quoted above. (The narrative in
`KK_GPU_WALL.md` §3 mentions a cpu-late/queued split; the script as checked in does not produce one.)

There **is** a signal in the trace that the script ignores: the column with mnemonic `start-latency`,
schema name **"CPU to GPU Latency"** (present on 100% of depth-0 rows; mtl_gaps reads neither).
Reading it (`scratch/lat_probe.py`), per interval, and comparing to the gap before it:

| trace | CPU->GPU latency | 
|---|---|
| fsr3_4 | min 2.7, p50 512, p90 2992, p99 4075, max 25395 us (mean 992) |
| off1 | min 3.1, p50 1113, p90 4803, p99 6024, max 44858 us (mean 1905) |

Fraction of cross-submit gaps where the next interval was **already committed before the previous
work ended** (its latency >= the gap => queued, a scheduling bubble):

| gap size | fsr3_4 | reading |
|---|---|---|
| <5 us | 99.4% | queued — the 20 us-free encoder micro-gaps; a pure scheduling bubble |
| 5-20 us | 30% | mixed |
| 20-100 us | 1.8% | the next work was **not** already on the GPU: **CPU-late** (2.21 ms/frame) |
| 0.1-0.5 ms | 46% | mixed (frame-boundary edge) |
| >0.5 ms | 97% | queued — frame/present boundaries (2.33 ms/frame) |

Reading: the sub-5 us micro-gaps (already-committed work) and the >0.1 ms gaps (frame boundary /
present waits) are **queued**; the mid-size **20-100 us gaps (2.21 of the 6.75 ms) are the ones whose
next command buffer had not been handed over yet — CPU-late**. That mixes the two mechanisms: about
3.4 ms/frame of queued boundary/frame work and ~2.2-2.6 ms/frame of recording-side lateness.
Caveat: the latency is measured from the commit of the command buffer, and KK commits many
MTL command buffers in one `mtl_command_queue_commit` at one instant, so a late buffer of an early
commit also shows a large latency — treat the split as indicative, not exact.

## Q3 DownloadImage: does the port wait inside the frame?

`Runtime::DownloadImage` (`render/runtime.cpp:575-600`) itself **does not wait**: it does
`EndRendering` (`:578`), a `Transit` to `eTransferSrcOptimal` (`:581`), an optional barrier flush
(`:588`), and records one `copyImageToBuffer` (`:593`). The wait is one level up.

`TextureCache::DownloadImageMemory` (`textures/texture_cache_memory.cpp:73-130`) is where the block
is. If the image's guest memory is not in place, it takes a **HostCached** staging buffer (`:111`,
`GetStagingPool().Request(bytes, HostCached, 16, !sync)`) and then:
```
if (sync) {
    scheduler.Finish();        // :124  <-- full GPU drain
    write_back(staging);       // :125  CPU write-back into the guest's memory
} else {
    scheduler.DeferPriorityOperation([...] { write_back(staging); ... });   // :126-130
}
```
`scheduler.Finish()` = `Flush()` + `Wait(tick)` (`scheduler.cpp:796-799`), and `Wait` blocks the
calling thread on the work timeline semaphore (`scheduler.cpp:799-808`, `work_semaphore.Wait(tick)`).
So the port **does** wait on the GPU inside the frame, and the wait is a full drain to the current
tick, not just on the readback.

Who forces `sync = true`:
- `ProcessDownloadImages` (`texture_cache_memory.cpp:60-71`) calls `DownloadImageMemory(image_id, true)`
  (`:68`) for every queued image. It is the fence path ("Every fence comes here (~450 a frame)",
  `:61`) and is reached from `Rasterizer::OnSubmit` (`render/rasterizer.cpp:777`), `cp/cp_tasks.cpp:135,
  189, 203` and `cp/cp_compute.cpp:88, 433, 465`. The `downloads_queued` guard means the lock/drain
  only happens when something is queued.
- `GarbageCollectImages::evict` (`texture_cache_memory.cpp:240`) calls it `sync=true` on every eviction
  of a GPU-written image.

Why the readback exists: to refresh the **guest's own memory** (`info.guest_address`) so the guest /
CPU sees GPU-modified images — the comment at `texture_cache_memory.cpp:118-121` cites issue #122
(motion blur read a render target as a buffer). The write-back does `TryWriteBacking` +
`buffer_cache.InvalidateMemory`. The in-place branch (`:101-109`) already avoids the staging copy when
the guest memory is bound in place; the staging/`Finish` path is the fallback.

Quantify: the ENCODER_MERGE_SURVEY counts `DownloadImage` pass-breaks at ~0.84/frame, so the drain is
about **one per frame** (plus GC evictions). Each is a GPU drain; in the trace those are the
queued-side large gaps (`>0.5 ms` 1.7/frame 2.33 ms and `0.1-0.5 ms` 4.6/frame 1.10 ms), and because
it blocks the recording thread, it also feeds the 20-100 us CPU-late gaps of Q2. The trace cannot
attribute a specific gap to DownloadImage (no label), so this is the code-path reading, not a measured
per-call cost.

An async / double-buffered version needs: (a) never call `scheduler.Finish()` — always take the
deferred branch (`:126-130`); (b) a ring of >=2 HostCached staging slots keyed by the submission tick,
freed only when that tick has passed (`FreeDeferred`, `:130`; the machinery is the priority-op queue,
`PopPendingOperations`, `scheduler.cpp:814+`); (c) keep the image (and its guest pages) alive and
correctly invalidated until the deferred write-back lands, so the guest never reads a half-updated
page — that is exactly the ordering `:118-121` says the sync path was added to guarantee. The port
already has the pieces; the risk is entirely in (c).

## Q4 Required vs merely-batched (deferrable)

| submit | consumes its completion? | verdict |
|---|---|---|
| `Rasterizer::Flush` per guest submission (`rasterizer.cpp:760`) | yes — writes the guest's fence labels / GPU signals (`raster_signals.cpp`), which the guest polls (`command_processor.cpp:477+` `sceGnmSubmitDone`) | **required.** Not deferrable past the guest's fence; the draw work *inside* it could move but the labels must land |
| Presenter frame-end flush (`presenter.cpp:352`) | yes — `frame->ready_semaphore`/`ready_tick`; `WaitRendered` (`presenter.cpp:398-406`) and the present submit wait on the tick | **required** |
| Present submit (`presenter.cpp:701`) | yes — waits on acquire + frame-ready, signals present-ready; `Present()` blocks on it (`:703-707`) | **required**, cannot be deferred |
| Blank-frame flush (`presenter.cpp:377`) | yes — a flip ends its submission; earlier work may be waited on | **required** (only on blank flips) |
| `RequestSignalFlush` (`raster_signals.cpp:98`) | yes — pending labels (`HasUnsubmittedSignals`) | **required** |
| MetalFX flush/Finish (`vk_metalfx.cpp:373/385/421/436`) | mode-dependent; Sync mode waits, Events/SubmitThread signal events on the GPU | **required in MetalFX modes**, absent on the FSR3 path |
| `DownloadImageMemory(sync)` -> `scheduler.Finish` (`texture_cache_memory.cpp:124`) | only for the CPU write-back (there is no GPU consumer of the readback) | **DEFERRABLE** — the one clean candidate (Q3) |
| GC eviction download (`texture_cache_memory.cpp:240`) | same, only the CPU write-back | **DEFERRABLE** (same change) |
| arena binds (`buffer_cache.cpp:3711`, hooked `rasterizer.cpp:55`) | no submit of its own; binds must be visible to that submission's draws | **required**, but already batched into an existing submit |
| `BufferCache` host copy (`buffer_cache.cpp:228`) | yes — the host reads the copied data behind a fence | **required** (when used) |
| `BbGuestMemory::RunOnce` (`bbport_guest_memory.cpp:355`) | yes — waits its own fence | startup only, not per-frame |
| `Rasterizer::Finish` (`rasterizer.cpp:769`) | yes | teardown/tests, not per-frame |

**Deferrable list: the DownloadImage / GC readback drains only.** Everything else has a live guest or
present consumer. The port's mid-frame submits are therefore not "batched there for convenience" —
they are fence/present points.

## Q5 VkCommandBuffer vs MTL4CommandBuffer, and port batching

**Not 1:1.** One `VkCommandBuffer` -> one `kk_cmd_buffer` (`kk_cmd_buffer.h:232-239`), which holds
three encoder states (`gfx` plus `cmp[2]` used as `pre_gfx`/`post_gfx`; `kk_cmd_buffer.c:144-154`).
Each encoder gets its **own** MTL4CommandBuffer, created lazily:
- render pass start: `cs_start_render` -> `cmd->gfx.cmd_buf = mtl_new_command_buffer(dev->mtl_handle)`
  (`kk_cmd_buffer.c:342`, function at `:335`; called from `cs_get_render` `:412` when
  `need_to_start_render_pass`).
- compute stretch: `kk_start_compute_encoder` -> `es->cmd_buf = mtl_new_command_buffer(...)`
  (`kk_cmd_buffer.c:423`, function at `:419`; called via `cs_get_compute` `:449/:454`).
Each finished command buffer is appended to `cmd->submit_cmd_bufs` (`kk_cmd_buffer.c:506`, inside
`kk_stop_encoder` `:463`). So **one VkCommandBuffer owns many MTL4CommandBuffers (one per encoder)**,
all handed over together by one `mtl_command_queue_commit` (`kk_queue.c:95-108`). The trace bears this
out: encoders/frame 340.9 ~= command buffers/frame 339.9, i.e. ~1 MTL command buffer per encoder.

**Does the port already batch several VkCommandBuffers per vkQueueSubmit? Yes.** `Scheduler::SubmitExecution`
assembles `cmdbufs` from `batch->segments` (`scheduler.cpp:948-958`) and `SubmitAsync` from the
recording threads' segments (`scheduler.cpp:1017-1026`), and both call `QueueSubmit` with that span.
The measured value is **6.00 VkCommandBuffers per submission, 3.0 submissions/frame** ("Recording:"
line, `frame_stats.cpp:656`; run log `out/run-2560_fsr3_4.log`), i.e. ~18 VkCommandBuffers -> ~18 Metal
queue commits per frame, matching the driver's "Metal commits" ~17-18/frame.

## Ranked attack list

1. **Cut MTL4CommandBuffers/frame in the driver, not submits in the port.** ~340 MTL4CommandBuffers/
   frame, one per encoder (`kk_cmd_buffer.c:342`, `:423`), each ending a Metal scheduling boundary with
   ~20 us of dead time (cross-submit 340.4/frame, 6.71 ms/frame). Put many encoders in **one**
   MTL4CommandBuffer per `kk_cmd_buffer` (Metal allows several encoders per command buffer), or reuse
   one command buffer across the frame. *Risk:* MTL command buffers are one-time-use and are
   re-recorded by KK (`kk_queue.c:112`, `rerecord_and_commit_cmd_buffer`); the per-encoder allocator /
   argument-table / `submit_cmd_bufs` ordering (`kk_cmd_buffer.c:506`, `cs_end` `:511`) must be kept,
   and Metal may still serialise encoders — measure before believing the full 6.7 ms.
2. **Make the DownloadImage/GC readback asynchronous** — remove `scheduler.Finish()` from
   `texture_cache_memory.cpp:124`, always use the deferred branch (`:126-130`), with >=2 staging slots
   keyed by tick. *Risk:* the guest must not read the back-written guest memory before the write-back
   lands (issue #122 class); this is the whole reason the sync path exists — needs care, not a toggle.
3. **Chase the CPU-late 20-100 us gaps (2.21 ms/frame) on the recording side.** The Q2 latency reading
   says that class was not committed when the GPU went idle — i.e. the emit/record path, not the
   submit count. Attack 2 feeds this directly. *Risk:* needs a CPU-side trace
   (metal-application-encoders) to confirm; the GPU-intervals table alone cannot.
4. **Frame-boundary gaps (>0.5 ms, 1.7/frame, 2.33 ms)** are queued present/acquire waits
   (FSR3 present path, `presenter.cpp:692-707`; `FramesAhead` `presenter.cpp:362`). *Risk:* guest flip
   cadence and presentation semantics.
5. **Do not spend effort on "fewer/larger `vkQueueSubmit`s".** There are only ~3/frame and ~6 command
   buffers each; a port-side batching change is a no-op for the boundary idle. (*No risk — it is a
   non-change.*)

## What the numbers do NOT prove

- Metal's `gpu-submission-id` semantics are **inferred** from the trace (one id per command buffer,
  62% of command buffers with two adjacent ids). The `mtl_gaps.py` label "cross-submit" is therefore
  not a proven CPU->GPU handover; it is a difference of those ids, which almost every command-buffer
  boundary has. The claim "these boundaries are inside the ~18 commits" rests on the logs' commit
  count (~17-18/frame), not on a per-boundary commit id the trace does not carry.
- The queued-vs-CPU-late split uses the ignored "CPU to GPU Latency" column; it is a proxy and is
  blurred by KK committing many MTL command buffers at one instant.
- No build, run, capture or source edit was done for this survey beyond this file; all per-frame
  numbers are from `out/mtl-2560_fsr3_4-gpu.xml`, `out/mtl-2560_off1-gpu.xml`,
  `out/run-2560_fsr3_4.log` and the driver's `BB_KK_STATS` print (`kk_device.c:91-140`).

## Measured 2026-10-17: the drain probe — null at this config

`BB_DOWNLOAD_SYNC_PROBE=1` (branch `fix/encoder-merge`, commit ab6d408): `ProcessDownloadImages`
passes `sync = !gate` so the per-frame readback takes the existing deferred branch
(`DeferPriorityOperation`); the GC-eviction download stays sync (it frees the image after). Gate
off = byte-identical. Gate liveness proven directly (`ps eww` on the live bb-gpu showed the env).
2+2 alternated hidden runs, 2560x1440 off:

| arm | p50 (pair 1) | p50 (pair 2) |
|---|---|---|
| base | 23.250 ms | 23.260 ms |
| probe | 23.290 ms | 23.240 ms |
| delta | +0.04 ms | -0.02 ms |

Mechanism, and why the null is clean: at this config the frame is **present-bound** — present_wait
~13.0 of 23.25 ms, unchanged between arms — so `scheduler.Finish()`'s Wait was already returning
immediately; the drain hides behind the vblank wait. No guest misbehavior in any arm (no crash,
assert or draw anomaly over 190 s). Conclusion: attack #2 is worth nothing at present-bound
configs; it can only cost wall clock when the GPU is the critical path, so re-measure at a
GPU-bound configuration before ever implementing the staging-slot work (#122).
