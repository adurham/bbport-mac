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

## The probe log: what the block actually is (2026-10-22, later)

BB_KK_ALLOC_LOG (instrumented side-slot driver, deps-arm64-probe, app driver untouched) writes one
line per allocator reset: identity, kind, age in resets, `allocatedSize` before and after. One
602 136-line run at the owner's settings:

| kind | allocators | median | sum |
|---|---|---|---|
| gfx | 86 | **122.7 MB** | **10 072 MB** |
| pre | 86 | 13.2 MB | 1 007 MB |
| post | 86 | 13.2 MB | 1 007 MB |

- Capacity is a **ratchet, not an append**: over 2 076 resets of one allocator it increased 16 times,
  the increases clustering at the area load (0 -> 74 -> 122 MB in ~670 resets, then flat for 1 200).
- The uniformity across all 86 buffers is the signature of one huge load frame spread evenly over the
  pool, then frozen.
- **Compaction (BB_KK_COMPACT_AT, one generation bump): measured and dead.** At 90 s it rebuilt 68
  buffers, releasing 204 allocators holding **8 721 MiB**; one stats window later the recreated
  allocators held **9 412 MiB**. They regrow immediately: the write is real, not stale capacity.
  This is the no-go branch the reference model named.

So neither lifetime nor capacity can fix it. The only remaining lever is **what the driver writes
per draw**: 122 MB for one buffer's share of a ~1 500-draw frame is tens of KB per draw, three orders
of magnitude above what Metal command encoding should need. The next investigation is the gfx
encoder path in `kk_cmd_buffer.c` / the argument and descriptor emission, priced in bytes per draw.

## Segment size: a real dial, not the fix (2026-10-22, measured)

`BB_VK_SEGMENT_KB` (default 64; `scheduler.cpp:124-125`) sets how many bytes of closures go into one
recording segment, and therefore how many render passes each Metal command buffer holds. Measured on
the probe driver, hidden runs, same save:

| BB_VK_SEGMENT_KB | command buffers | allocators held | frame rate (indicative, load-contaminated) |
|---|---|---|---|
| 8 | 147 | **20 847 MiB** | 10 fps |
| 64 (default) | 86 | **~10 000 MiB** | 42 fps |
| 256 | 105 | **5 812 MiB** | 21 fps |

So the block moves with segmentation in both directions - smaller segments multiply the buffers and
explode the block, larger ones halve it - but the frame-rate cost goes the other way, and none of
these settings is a fix. The trade is buffer count against slabs per buffer.

Two data-quality notes, both learned the hard way here: the driver's `command allocators N holding M
MiB` counter is the authoritative live number (a sum over every allocator ever seen in the
`BB_KK_ALLOC_LOG` file double-counts dead ones and overstated the 8 KB run by ~60x), and cross-run
frame rates on this machine are not comparable while other work is running - only the memory figures
are solid.

## The last two attempts: shared buffer, and folding compute into it (measured dead)

**1. One command buffer per VkCommandBuffer (`BB_KK_ONE_CMDBUF`).** Implemented in the probe driver:
render passes share one `MTL4CommandBuffer`, ended early whenever compute must interleave. Measured
A/B, same dylib, same route: gate on gives **max 50 command buffers per recording** against 56 off,
gfx allocator median 76.0 MB against 104.8 MB, totals 5 712 MB against 6 087 MB (**6%**), frame rate
identical (43.0 vs 42.8 fps), `order violations 0`. Why it cannot win: a Metal 4 command buffer is
linear and can only encode commands allocated from its own allocator, while pre-gfx compute is
discovered inside an open render pass and must execute before it.

**2. Folding those compute encoders into the shared buffer (`BB_KK_FLUSH_LOG` histogram first).** A
`BB_KK_FLUSH_LOG` run counted 32 493 early ends: 22 282 `compute_before_pass` with the render encoder
closed (the cheap case), 5 998 normal ends, 3 561 `draw_split` (only 368 with an encoder open), 652
`compute_before_append`. On that basis the compute-fold was built: when a pre/post compute encoder is
requested with no render encoder open, it is created on the shared buffer instead of its own.
Measured in gameplay: **`early ends for in-pass compute 540 197`** - the compute that dominates real
frames is started *inside* render passes (post-gfx query writes, event sets, heap resets), which can
never be folded. gfx allocator ended at **53 quanta / 125.2 MB** per buffer, block still **9.2 GB**,
`order violations 0`. The 22 k cheap events were a title/load artefact, not the gameplay shape.

So the entire lifetime/capacity/segmentation/sharing family is now measured dead on this driver. The
only route left that the reference model sanctions is **structural: deferred command recording** -
record a render pass into a driver-side list and emit at `vkCmdEndRendering` (variant A), or replay
the whole queue at submit via Mesa's `vk_cmd_enqueue` (variant B). Both put the commands in one
buffer because nothing is encoded until the pass is closed; both are multi-day changes to the
submission path, and both must be gated and proved against the same bar: <= 1 Metal command buffer
per recording, allocator median <= 2 quanta, total allocator bytes <= ~500 MB, footprint <= 9 GiB
flat for 10 minutes, fps >= 41, zero order violations, and a pixel-identical capture set.

## The rewrite: milestones M0-M1 (probe driver, default off)

Per the reference model's plan, variant A (deferred render recording) is being built in the probe
driver behind `BB_KK_DEFER_RENDER`. Two milestones are in:

- **M0 - the abstraction.** Every Metal render-encoder call now goes through `kk_render_enc.h`
  (`kk_renc_*`), with a direct backend as the default and the deferred backend behind the knob.
  Direct-path equivalence was proven by comparing compiled objects: same calls in the same order,
  no allocations added.
- **M1 - the deferred list + an offline proof.** New `kk_render_defer.{h,c}`: a pass records 104-byte
  op nodes (state, draws, barriers, timestamps, labels, side ops with their read/write addresses) and
  replays them into one render encoder at the pass's end or at a mid-pass compute side op. The proof
  is offline, no GPU: `deps-arm64-probe/defer-m1-test/run.sh` drives the same op stream through both
  backends and compares the call logs. **Both logs are byte-identical - `renc` 93 lines, `mtl` 96
  lines - and a perturbed control is caught in both.** The UUID is a probe build; the installed
  driver is untouched.

Two things this milestone taught, both worth keeping:

- **The first "identical" was false.** The test compared Metal calls grouped per encoder, which cannot
  see a reordering - the one property M1 exists to preserve. Judging on raw `cmp` of the whole
  `direct.mtl` vs `deferred.mtl` exposed it: compute encoders were emitted before the render encoder
  instead of mid-pass. The harness now compares raw global order and fails on any reordering.
- **The direct backend does not end the render encoder at a mid-pass side op.** It keeps one encoder
  open and interleaves the compute inside it. The deferred backend now mirrors that exactly
  (`kk_renc_sync` replays the recorded ops and leaves the encoder open), which is why the raw order
  can match at all.

M2 (hoist pre-gfx compute) and M3 (sink post-gfx work) are gated separately; M1 deliberately changes
the data structure only, so the allocator block is still ~10 GB until they land.
