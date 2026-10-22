# Encoder overlap on KosmicKrisp: what serialises the encoders, and what overlap is worth

Branch `probe/kk-overlap`, 2026-10-22. Hidden 420 s standing runs (`tools/macos/overlap_series.sh`),
FSR 3.1 preset 3 at a 3440x1440 output (raster 1720x720), ~885-894 draws/frame. Each arm has a
3 s Metal System Trace at about 150 s of gameplay, read with `mtl_encoders.py` and `mtl_gaps.py`, and
3 port captures (`overlap_caps.py`). KosmicKrisp was built in this worktree's own `deps-arm64`. The
port's dylib was not touched.

## Verdict: the fence / untracked-resource patch is CLOSED, and it was not built

None of the barrier modes gets two GPU channels busy for 1.0 ms/frame and still draws a right
picture. A fence patch that is correct has to keep every wait the port's barriers ask for. The
`nodep` diagnostic drops all of the vertex waits and still reaches only 0.80 ms of time with two
channels busy, and it does not change the frame (-0.02 ms). So the fence patch cannot meet either
half of the bar.

## 1. The serialiser (cite: pinned Mesa dc41592 with patches 0001-0006 applied)

1. **What actually serialises the encoders.** Every encoder ends with KosmicKrisp's own barrier:
   `barrierAfterStages:ALL beforeQueueStages:ALL`. Upstream this is at `kk_cmd_buffer.c:370`. With
   patches 0001-0006 applied it is at `kk_cmd_buffer.c:491` (`kk_stop_encoder`). This barrier is the
   driver's only cross-encoder synchronisation:
   - `vkCmdPipelineBarrier2` emits nothing between encoders (`kk_cmd_buffer.c:609` and following).
   - `vkCmdWaitEvents2` is a no-op (`kk_event.c:111`).
2. **Under it, the port's own barriers would serialise the encoders anyway.** In `vk` mode the
   driver follows the Vulkan barriers. Even then, 3.22M of 3.35M waiting render encoders wait
   **before their vertex stage**. The vertex-side barriers come from memory/buffer barriers (3.34M)
   and image barriers (2.98M). The cause is in the port: `Runtime::FlushBarriers` and `TransitSampled`
   name ALL_COMMANDS or vertex-side destinations. Narrowing those masks has already been measured to
   lose (docs/BARRIER_MASKS.md).
3. **Metal's automatic hazard tracking is NOT involved.** All resources are created untracked:
   - `KK_MTL_RESOURCE_OPTIONS` = `MTL_RESOURCE_TRACKING_MODE_UNTRACKED` (`kk_private.h:48-51`).
   - It is used for every heap (`kk_bo.c:43`), every heap buffer (`mtl_heap.m:69`), every heap
     texture, which inherits the heap's options (`mtl_heap.m:81`), and every no-copy buffer
     (`mtl_device.m:340`).
   - MTL4 queues do no automatic tracking anyway.

   So "replace the automatic tracking with untracked resources plus fences" has nothing to replace.
4. **The command-buffer structure sets the cost but not the order.** There is one MTL4CommandBuffer
   per encoder (`kk_cmd_buffer.c:342` render, `:423` compute) and one commit array per
   VkCommandBuffer (`kk_queue.c` `kk_queue_commit`). A standalone MTL4 microbenchmark
   (`tools/macos/overlap_bench`) ran 300 encoders, each in its own command buffer, in one commit,
   with the sync shape varied:

   | sync shape | ms per frame | µs per encoder |
   |---|---|---|
   | full ALL->ALL barrier | 10.03 | ~33 |
   | lite | 10.36 | ~35 |
   | consumer barrier | 10.36 | ~35 |
   | MTLFence | 10.42 | ~35 |
   | none | 10.40 | ~35 |

   So the per-encoder boundary costs about 33-35 µs whatever the synchronisation, and fences do not
   make it cheaper. Packing the same encoders into one command buffer: full barrier 10.18 ms, fence
   9.43, none 7.91. With heavy vertex and fragment work, a fence recovers 3.1 of the 5.1 ms that
   `none` recovers. That is the most fences could ever give, and only with real independent vertex
   work behind the barrier. The game's frame has very little of that work (next section).

## 2. The barrier modes, measured

| arm | drawable | flat p50 / p99 (60-110 s) | late p50 / p99 | render / compute enc per frame | V / F / C ms per frame | vertex-under-previous-fragment ms per frame | ≥2 channels ms per frame | GPU idle ms per frame | picture |
|---|---|---|---|---|---|---|---|---|---|
| ctrl-1 (legacy) | 3440x1440 | 20.57 / 21.39 | 20.67 / 21.47 | 178 / 119 | 3.46 / 6.45 / 2.71 | 0.005 | 0.054 | 5.36 | ok |
| nodep-1 | 3440x1440 | 20.55 / 23.29 | 20.63 / 21.97 | 179 / 121 | 4.01 / 6.92 / 2.90 | 0.552 | **0.797** | 7.75 | looks ok (unsafe) |
| lite-1 | 3440x1440 | 21.15 / 22.44 | 21.13 / 22.74 | 188 / 124 | 4.19 / 6.88 / 2.85 | 0.168 | 0.183 | 5.55 | ok |
| ctrl-2 (legacy) | 1512x982* | 20.77 / 21.26 | 21.17 / 22.71 | 177 / 119 | 3.42 / 6.34 / 2.69 | 0.005 | 0.062 | 5.59 | ok |
| vk-1 | 1512x982* | **19.87** / 20.58 | **19.79** / 21.00 | 192 / 128 | 4.54 / 7.35 / 3.01 | 0.158 | 0.173 | 6.59 | ok (captures 5-6.6 mean abs diff from ctrl-2, the same order as ctrl-vs-ctrl) |
| none-1 | 1512x982* | 18.65 / 19.20 | 18.70 / 19.23 | 182 / 121 | 4.07 / 7.04 / 3.64 | 0.570 | **2.469** | 4.65 | **WRONG**: luma 5 vs 33, a black world |
| free (no barrier on any encoder) | - | never left the title (22-23 presses of Continue in 90 s) | | | | | | | unusable |
| cfree (nodep, plus compute holds only dispatch/blit) | - | never left the title | | | | | | | unusable |

\* Midway through the series the AW dropped off the dock, so the hidden window fell back to the
built-in panel. The scene raster stayed 1720x720 and the output stayed 3440x1440 (from the run
log). Compare only within a display group: ctrl-1/nodep-1/lite-1, and ctrl-2/vk-1/none-1.
0 asserts in any run (`gcn_to_vk.cpp:422`, `cp_graphics.cpp:203`).

How to read it:
- The only arm that clears 1 ms of two-channel time is `none`, and its picture is wrong.
- `nodep` overlaps vertex work under the previous fragment work (0.55 ms) and leaves the frame
  unchanged. Frame p50 does not follow GPU overlap, because the GPU is idle 5-8 ms of every frame.
- `vk` is -0.9 ms against its matching control (19.87 vs 20.77) with **no** gain in overlap
  (0.17 ms). It is one run, at the edge of the 2x-noise band. Its gain is not overlap. The most
  likely source is the drain cost of the ALL->ALL barrier on encoders that have no Vulkan barrier.
  That is a different lever from this one, and it is already priced in KK_GPU_WALL.md
  (-0.5 to -0.9 ms, the owner's eyes are the gate).

## Bar result

The bar needs ≥2-channel time of at least 1.0 ms/frame AND frame p50 down by at least 1.0 ms
(2+2 runs). The barrier modes miss it:
- With a right picture, the best two-channel time is 0.80 ms (nodep, frame unchanged).
- `vk` gives -0.9 ms of frame with 0.17 ms of overlap.

The fence patch could only do worse than these. It was not built.

## What would have to change instead

The serialisation is real, but it is not where the time goes. The time goes to boundaries:
~300 encoders/frame x ~33 µs, and that cost does not change with the sync mode. The levers are the
ones the earlier docs already name: fewer encoders (pass breaks, ENCODER_MERGE_SURVEY.md), and on the
driver side, encoders packed into one command buffer per VkCommandBuffer **together with** cheaper
barriers (microbench: 10.18 -> 9.43 ms with fences, 7.91 ms with none).

## Files

- `tools/macos/overlap_series.sh`: the series harness. It checks that the machine is idle and that
  memory is free before each run, and it runs the watchdog.
- `tools/macos/overlap_caps.py`: compares captures.
- `tools/macos/overlap_bench/`: the MTL4 sync-shape microbenchmark.
- `tools/macos/kosmickrisp-patches/0007-kosmickrisp-barrier-mode-free.patch`: the `free`/`cfree`
  diagnostic modes. They are not a default, and both hang at the title.
- `docs/kk-overlap/report.json`.
