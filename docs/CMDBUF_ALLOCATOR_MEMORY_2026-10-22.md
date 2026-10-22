# The allocator block: what it is, what was measured, what is dead

2026-10-22. The largest single resident cost in the port, and the mechanism behind the three GUI
deaths on this Mac (jetsam -> WindowServer). Everything here was measured on the M4 Max at the
owner's settings (3440x1440) with hidden runs, unless a line says otherwise.

## The number

KosmicKrisp (Mesa Vulkan-on-Metal) gives **each live `VkCommandBuffer` three
`MTL4CommandAllocator`s** - one per encoder state (pre-gfx, gfx, post-gfx) - created in
`kk_cmd_buffer.c` (`kk_init_encoder_state`), reset at every `vkBeginCommandBuffer` through
`kk_reset_encoder_state`, and released **only** in `kk_destroy_encoder_state`. `reset()` keeps the
allocation: an allocator grows to the high-water of what was encoded into it and stays there.

Measured with the driver's own `BB_KK_STATS=1`:

| moment | allocators | holding | process footprint |
|---|---|---|---|
| before an area load | 45 | **141 MiB** (~3 MiB each) | ~5 GiB |
| after the load, steady play | 213-285 | **7.5-10.8 GiB** (~40 MiB each) | 16-19 GiB |

Metal heaps over the same window: a flat ~4.8 GiB. So the growth is the allocators, not the heaps,
not images (1 GiB), not a leak over time - it is the area load's high-water, kept forever.

## Why the port holds 64 buffers

`CommandPool` (`gpu/vulkan/command_pool.cpp`) starts empty, grows by 4 on demand, and never frees.
The new `BB_CMDBUF_STATS=1` probe shows the shape: steady play *before* a load runs on **8 buffers
in 4 pools** at 1 792 commits per 10 s and 100% reuse; the load takes it to **68** (largest pool 24);
afterwards it stays at 54-86 for the rest of the run. Each frame uses ~26 command buffers (one per
recording segment, `MaxSegments = 64`), so every buffer in the pool is handed out again within about
half a second.

That last sentence is what makes the lifetime levers dead, and it is measured: the last-use
histogram reads `idle<=1s:58-70 <=5s:0-20 <=10s:0-7 older:0-4` in every window.

## What is falsified, by measurement (do not re-propose)

| lever | result |
|---|---|
| cap buffers per pool (`BB_MAX_CMDBUFFERS=4`, wait 5 ms then grow) | load needs 20-24 concurrent in one pool; the wait timed out, it grew anyway, allocators stayed 8.2 GiB, footprint **worse** (20 GiB) |
| timer that frees idle buffers then trims (earlier session) | 3.5-7% fps loss, 213-387 Metal heaps/s churn |
| port frees idle buffers (`BB_CMDBUF_RELEASE_IDLE`, `vkFreeCommandBuffers`) | release fires, pools shrink, **allocators never move** - Mesa's free path *recycles* the buffer into the pool's free list, and the `kk_cmd_buffer` with its allocators survives |
| LIFO handout (`BB_CMDBUF_LIFO=1`) so a cold tail could age | the pool is sized to the segment in-flight set, so even MRU selection touches every buffer within ~0.5 s; the histogram stays hot and the idle gate never matches |

## What was built anyway (default-off, unshipped)

1. **Driver patch `0009`** - `kk_ResetCommandBuffer` honours
   `VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT`: release encoders -> `MTL4CommandBuffer`s ->
   `MTL4CommandAllocator`s in dependency order, NULL them, `bb_mtl_released` makes a double release a
   no-op, and one accessor recreates them at the next begin. Only an application reset with the flag
   reaches it - no sweep, timer, submit, destroy or trim path calls it (grep-proven). Counters join
   the `BB_KK_STATS` line. A standalone Vulkan test passed all nine cases (release, double release,
   initial-state release, pool reset with release, free/recycle, 50 release-reuse cycles).
   **The driver was rebuilt and installed: LC_UUID `F324F17F-952D-3558-B330-EC0B472ED846`.**
2. **Port gates** - `BB_CMDBUF_STATS` (10 s line + last-use histogram), `BB_CMDBUF_RELEASE_IDLE`,
   `BB_CMDBUF_TRIM`, `BB_CMDBUF_LIFO`, and the dead `BB_MAX_CMDBUFFERS`. All default off; with every
   gate unset the old code paths run verbatim (checked line by line, 99/99).

In the game the release path does fire and the driver counts it (`released by reset 16 449 holding
16 361 MiB before release, recreated 16 449`) - and the footprint still does not fall, because every
release is followed by a recreate of the same buffer in the same second. Releasing memory that is
about to be re-grown is churn, not a fix.

The post-`Refresh` LIFO retry was then added on top (so the MRU choice also decides when the pre-pass
finds nothing) and re-measured: **the same shape**. One 179 s gameplay run at 42.7 fps had a clean
hitch profile (10 frames over 50 ms, 1.19 s, worst 348 ms) but the allocators still ended at 207-211
**holding 9.2 GiB**, with 30 237 release/recreate cycles. The histogram stayed `idle<=1s:62`. The run
lived longer only because the machine was quieter (free+inactive never fell below 4.9 GiB) - not
because the block shrank. This is the measurement that closes the lifetime approach on its own terms.

## What the data says to do instead

The lifetime levers are spent; the remaining shape of the problem is the amount of command stream
the port keeps alive, not its lifetime:

- **Shrink-to-fit after reset.** The allocator's `allocatedSize` after a reset is what the *busiest*
  frame wrote. If a buffer's recent use is far below its high-water, release and recreate a smaller
  allocator (the same release/recreate plumbing `0009` added). Prices into the ~2-3 GiB the load
  leaves behind rather than the whole block.
- **Submission shape.** 26 segment buffers per frame at ~40 MiB of encoded commands each is the bill.
  Fewer segments per frame, or shorter-lived segments, reduce it at the source. This is the direction
  the reference model named when the lifetime path failed: "the fix then has to be submission-shape
  (smaller command streams during load), not lifetime".
- **Revisit the load spike itself.** Why the load needs 20-24 concurrent segments in one pool while
  steady play needs 4 is a property of the load path, and it is what sets the high-water that is then
  held forever.

## Acceptance bar for whatever lands next (reference model)

After an area load plus 60 s of steady play: allocators <= 36, allocator bytes <= 1.5 GiB, footprint
<= pre-load + 2 GiB, fps within 1% of baseline, Metal heaps/s ~ 0 in steady state, zero GPU faults
across three consecutive loads of 10 min each.

## Artefact consequence of the driver rebuild

The shipped Mesa shader cache is keyed on the driver's LC_UUID, so with `F324F17F` it is skipped
(`Mesa cache skipped: built for driver C5B640F8, this driver is F324F17F`) until refilled with this
build (step 3 of `KK_PRELOAD_ARTEFACTS.md`). The AIR libraries and the MTL4 archive are keyed on the
generated MSL, which `0009` does not change, so they keep working.
