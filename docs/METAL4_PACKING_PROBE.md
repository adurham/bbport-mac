# Metal 4 packing probe — does packing encoders into fewer command buffers buy anything?

**Short answer: no.** 300 dependent render encoders per frame cost ~35 µs *per encoder* and
10.6 ms per frame regardless of whether they are packaged as 300 command buffers in one commit
array, one command buffer with 300 encoders, or 300 individually committed command buffers. The
per-encoder cost is not the command-buffer object or the commit call — it is the encoder boundary
itself (pass transition + drain), so repackaging is a no-op and only *fewer encoders* (or a cheaper
per-encoder transition/barrier) can move the needle.

Standalone probe, nothing to do with the port's driver code:
`~/repos/bbport-metal-microbench` (ObjC++ CLI, offscreen, MTL4 only). Host: MacBook M4 Max,
macOS 27.2 (26B5112a), Xcode 27.0, no game running.

## What was measured

One frame = **N = 300 dependent render encoders**. Pass `p` draws a full-screen triangle into
`pingpong[p&1]` (512×512 RGBA8) sampling `pingpong[(p&1)^1]`, so each pass reads what the previous
pass wrote. Every encoder ends with the same producer barrier the driver emits in
`kk_stop_encoder()` (`kosmickrisp/vulkan/kk_cmd_buffer.c:491` → `mtl_barrier_after_stages(all, all)`):

```objc
[enc barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:0];
[enc endEncoding];
```

All three variants encode the **identical** encoder sequence and barriers; only the packaging of
encoders into `MTL4CommandBuffer`s / `MTL4CommandQueue commit:count:` calls differs.

| variant | packaging |
|---|---|
| **A** | 300 separate `MTL4CommandBuffer`s, all committed in **one** `commit:count:` array |
| **B** | **one** `MTL4CommandBuffer` containing all 300 encoders (explicit barrier between them) |
| **C** | 300 separate `MTL4CommandBuffer`s, each **committed individually** (pessimistic) |

## Numbers — frame time

200 measured frames after 40 warmup, 2 frames in flight, median of 3 process runs.

| variant | mean ms | p50 ms | p95 ms | µs/encoder | CPU encode+commit ms |
|---|---|---|---|---|---|
| **A** 300 CBs, 1 commit array | **10.658** | 10.670 | 10.839 | 35.5 | 1.67 |
| **B** 1 CB, 300 encoders | **11.308** | 11.453 | 11.861 | 37.7 | 1.36 |
| **C** 300 CBs, 300 commits | **10.635** | 10.658 | 10.825 | 35.5 | 1.72 |

A and C are indistinguishable (0.2%, inside run-to-run noise). B — the "packed" case — is **6 %
slower**, consistently, across three runs. Committing 300 times per frame costs nothing measurable
(CPU encode+commit: 1.72 ms vs 1.67 ms, and it overlaps the 10.6 ms of GPU work anyway).

Two controls show the cost is *per encoder*, not per frame or per pixel:

* **Encoder-count scaling** (variant A, 512²): 50 encoders → 1.78 ms (35.6 µs/enc), 150 → 5.33 ms
  (35.5), 300 → 10.63 ms (35.4). Dead linear, extrapolating to ~0.35 ms for zero encoders.
* **Texture-size scaling** (300 encoders): 128², 256² and 512² all land at 10.5–11.0 ms/frame. The
  frame is *not* fill-bound — 16× the pixels changes nothing.
* **No-barrier control** (`--no-barrier`, diagnostic only): A 10.60 ms, C 10.67 ms, **B 8.28 ms**
  (−25 %). So the explicit full barrier inside one command buffer costs ~9 µs/encoder, and a floor of
  ~27.6 µs/encoder of pure pass-transition cost remains even with zero explicit barriers. In A and C
  removing the explicit barrier changes nothing — Metal's automatic hazard tracking across command
  buffers imposes the same dependency ordering (which is also why packing cannot help: the
  dependency, and the drain it implies, is per encoder either way).

## Numbers — GPU timeline (`xctrace` + `tools/mtl_gaps.py`)

`Metal System Trace`, attached to the running probe, `metal-gpu-intervals` exported and fed to the
port's `mtl_gaps.py` (see caveat below about the frame count). `busy + idle` is per frame.

**Variant A** (337 frames, span 3542.9 ms → 10.51 ms/frame):

```
bb-gpu depth-0 intervals 202666, span 3542.9 ms, frames 337 (10.51 ms/frame)
channels: [('Vertex', 101358), ('Fragment', 101308)]
busy 2.53 ms/frame (24.0%), >=2 channels 0.00 ms/frame (0.0% of busy), idle 7.99 ms/frame
  Fragment    1.53 ms/frame,  300.6 enc/frame | 0-20us 300.6/ 1.53ms ...
  Vertex      1.00 ms/frame,  300.8 enc/frame | 0-20us 300.8/ 1.00ms ...
  command buffers/frame 0.9, submissions/frame 601.4, encoders/frame 300.8
idle gaps 202665 (601.4/frame), 7.99 ms/frame:
  same-cmdbuf    300.6/frame   0.04 ms/frame (mean    0.1 us)
  same-submit      0.0/frame   0.00 ms/frame (mean    0.0 us)
  cross-submit   300.8/frame   7.95 ms/frame (mean   26.4 us)
  size <5us        325.7/frame   0.15 ms/frame
  size 5-20us       79.3/frame   0.67 ms/frame
  size 20-100us    195.0/frame   6.92 ms/frame
  size 0.1-0.5ms     1.4/frame   0.24 ms/frame
```

**Variant B** (131 frames, span 1419.4 ms → 10.83 ms/frame):

```
bb-gpu depth-0 intervals 76748, span 1419.4 ms, frames 131 (10.83 ms/frame)
channels: [('Vertex', 38411), ('Fragment', 38337)]
busy 7.20 ms/frame (66.4%), >=2 channels 0.00 ms/frame (0.0% of busy), idle 3.64 ms/frame
  Fragment    4.58 ms/frame,  292.6 enc/frame | 0-20us 290.3/ 4.52ms ...
  Vertex      2.62 ms/frame,  293.2 enc/frame | 0-20us 293.2/ 2.62ms ...
  command buffers/frame 0.0, submissions/frame 585.9, encoders/frame 293.2
idle gaps 76747 (585.9/frame), 3.64 ms/frame:
  same-cmdbuf    585.9/frame   3.64 ms/frame (mean    6.2 us)
  same-submit      0.0/frame   0.00 ms/frame (mean    0.0 us)
  cross-submit     0.0/frame   0.00 ms/frame (mean    0.0 us)
  size <5us        581.6/frame   0.11 ms/frame
  size 0.1-0.5ms    0.8/frame   0.21 ms/frame
  size >0.5ms        3.2/frame   3.32 ms/frame
```

**Variant C** (215 frames, span 2263.9 ms → 10.53 ms/frame):

```
bb-gpu depth-0 intervals 125293, span 2263.9 ms, frames 215 (10.53 ms/frame)
channels: [('Vertex', 64021), ('Fragment', 61272)]
busy 3.34 ms/frame (31.7%), >=2 channels 0.14 ms/frame (4.1% of busy), idle 7.19 ms/frame
  Fragment    2.11 ms/frame,  285.0 enc/frame | 0-20us 282.1/ 2.04ms ...
  Vertex      1.37 ms/frame,  297.8 enc/frame | 0-20us 297.8/ 1.37ms ...
  command buffers/frame 1.4, submissions/frame 582.8, encoders/frame 300.4
idle gaps 118486 (551.1/frame), 7.19 ms/frame:
  same-cmdbuf    404.5/frame   0.19 ms/frame (mean    0.5 us)
  cross-submit   146.6/frame   7.01 ms/frame (mean   47.8 us)
  size <5us        423.9/frame   0.17 ms/frame
  size 5-20us       44.6/frame   0.45 ms/frame
  size 20-100us     64.7/frame   2.99 ms/frame
  size 0.1-0.5ms    17.4/frame   3.26 ms/frame
```

How to read these three: the **total** is invariant (10.51 / 10.83 / 10.53 ms per frame, matching the
app's own 10.66 / 11.31 / 10.64) and only the *attribution* between "encoder interval" and "idle gap"
moves. In A each 300-encoder frame produces ~300 idle gaps of ~26 µs — one per command-buffer
boundary — plus 2.5 ms of busy. In B the same waiting is counted *inside* the encoder intervals
(busy 66 %, gaps down to a 6.2 µs mean), because within a single command buffer the trace's
per-encoder interval already spans the drain that A reports as a gap. Nothing got faster; the idle
was renamed. C is A plus a slightly noisier mix of the same two components.

Caveats for MTL4 traces, worth knowing before reading `mtl_gaps.py` output on the real port:

* `frame-number` follows commit boundaries. Variant C (300 commits/frame) made the tool invent
  26772 "frames" in a 2.3 s trace, so the frame count must be passed explicitly
  (`span_ms / measured period`), as done above.
* `gpu-submission-id` / `cmdbuffer-id` are **not** commit / command-buffer identity for MTL4.
  Variant A commits **once** per frame and still reports ~600 distinct submission ids per frame, so
  the `same-cmdbuf` / `same-submit` / `cross-submit` classification is unreliable here; the totals and
  the gap-size histogram are the trustworthy part. On bb-gpu, "cross-submit" gaps should therefore be
  cross-checked against the driver's own commit counts (`KK_BBSTAT_COMMITS`).

## Verdict

Packing 300 dependent encoders into fewer `MTL4CommandBuffer`s / fewer commits changes nothing:
A (300 CBs, one commit) = 10.658 ms, C (300 CBs, 300 commits) = 10.635 ms, B (one CB, 300 encoders) =
11.308 ms — packing is a **no-op, and marginally harmful**. The ~35 µs/frame-encoder cost (27 µs even
with the explicit barrier removed and a single command buffer; ~9 µs more for the full barrier)
follows the encoder boundary, so only reducing the number of encoders — or making the per-encoder
transition cheaper (`BB_KK_BARRIER` work, eliminating passes) — can recover the ~6.7 ms/frame of
encoder-boundary idle.

## What KosmicKrisp actually does with commits

**One `commit:count:` array per `VkCommandBuffer`** — not one per `MTL4CommandBuffer`, and not one for
the whole frame.

* `kk_queue_commit()` (`kk_queue.c:94-109`) builds `MTL4CommitOptions` and issues exactly one
  `mtl_command_queue_commit(queue->mtl_handle, util_dynarray_begin(&cmd->submit_cmd_bufs),
  util_dynarray_num_elements(&cmd->submit_cmd_bufs, mtl_command_buffer *), options)` at
  **`kk_queue.c:104-107`**. So one call = one commit = one array of *all* MTL4 command buffers that
  belong to that `VkCommandBuffer`.
* The array is "one `MTL4CommandBuffer` per encoder": the buffer is created at
  **`kk_cmd_buffer.c:342`** (render, `cs_start_render`) and **`kk_cmd_buffer.c:423`** (compute,
  `kk_start_compute_encoder`), ended at **`kk_cmd_buffer.c:504`** and appended to
  `cmd->submit_cmd_bufs` at **`kk_cmd_buffer.c:506`**.
* `kk_queue_submit()` (`kk_queue.c:260-329`) loops over `submit->command_buffers` at
  **`kk_queue.c:289`** and calls `rerecord_and_commit_cmd_buffer()` (`kk_queue.c:301`) or
  `kk_queue_commit()` (`kk_queue.c:305`) **once per VkCommandBuffer**.

So the driver already batches every MTL4 command buffer of a `VkCommandBuffer` into a single commit;
the thing it does *per encoder* is create a command buffer and (at submit) commit once per
VkCommandBuffer. Per this probe, restructuring those commits (fewer arrays, one array for several
VkCommandBuffers, or one command buffer per frame) cannot reduce the boundary idle — the cost is
inside each encoder boundary, not in the commit or the command-buffer object.

## Reproduce

```sh
cd ~/repos/bbport-metal-microbench
clang++ -std=c++17 -fobjc-arc -O2 main.mm -o bb-mtlpack -framework Metal -framework Foundation

for v in a b c; do ./bb-mtlpack $v --frames 200 --warmup 40; done

# controls
./bb-mtlpack a --frames 150 --warmup 30 --encoders 50
./bb-mtlpack a --frames 150 --warmup 30 --encoders 150
./bb-mtlpack a --frames 150 --warmup 30 --size 128
./bb-mtlpack b --frames 150 --warmup 30 --no-barrier

# trace one variant
(./bb-mtlpack a --frames 50 --warmup 20 --seconds 60 > out/a-trace.log 2>&1 &)
xcrun xctrace record --template 'Metal System Trace' --attach $(pgrep -f 'bb-mtlpack a') \
    --time-limit 3s --output out/a.trace
xcrun xctrace export --input out/a.trace --xpath \
    '/trace-toc/run[1]/data/table[@schema="metal-gpu-intervals"]' > out/a-gpu.xml
MTL_GAPS_PROC=bb-mtlpack python3 tools/mtl_gaps.py out/a-gpu.xml 337
```

`tools/mtl_gaps.py` is `bbport-metalfx-build/tools/macos/mtl_gaps.py` with the process filter made
configurable via `MTL_GAPS_PROC` (default `bb-gpu`); nothing else changed.

Deviations from the original plan, for the record:

1. **ObjC++, not Swift.** `barrierAfterStages:beforeQueueStages:visibilityOptions:` is marked
   `SwiftPrivate` in Metal's `apinotes` on this SDK, so the per-encoder barrier cannot be encoded from
   Swift at all. Metal 4 itself is fully available (`MTL4CommandQueue`/`MTL4CommandBuffer`/
   `MTL4CommandAllocator`/`MTL4Compiler` all used); this is not an MTL3 substitute.
2. Variant B's 3 s trace refused to export twice (`xctrace export` died with SIGBUS on the trace, even
   for `--toc`); a 1 s trace of the same run exported fine, so B's numbers above come from that
   (131 frames). A and C used 3 s / 2 s traces.
3. `mtl_gaps.py` needed the process-name filter change and an explicit frame count (see caveats).
4. 512×512 turned out to be far too small to be fill-bound, so the frame is encoder-bound by
   construction; `--size` was added to prove that (128²/256²/512² identical). The absolute
   µs/encoder here (~35) is higher than the port's ~20 µs because this pass is a full-screen opaque
   pass with an explicit full barrier; the transferable result is the invariance across packagings.
