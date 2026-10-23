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

## M2 status at hand-off (the session died mid-milestone)

M2's **design is landed and complete in the headers**; its **implementation is not written**. The tree
compiles and links unchanged because nothing calls the new declarations yet - the state is a clean
hand-off point, not a broken one.

- `kk_render_defer.h` (~line 102): the hoist contract, the `kk_renc_hoist_why` refusal enum (ENC, ANY,
  ANY_EARLIER, RAW, BARRIER, HEAP, BUSY) and the five entry points.
- `kk_device.h` / `kk_device.c`: `kk_bb_defer_hoist()` from `BB_KK_DEFER_HOIST`, the per-reason
  counters, and the end-of-run print.
- `kk_cmd_buffer.h`: `renc.hoist_draw` / `hoist_next` / `hoist_open`, `kk_cmd_hoist_close()`.

Remaining: the bodies in `kk_render_defer.c` (`kk_renc_hoist_check`, `kk_renc_hoist_draw`,
`kk_renc_hoist_late_refusal`, `kk_renc_hoist_why_name`, and the hoist branch of
`kk_renc_slow_side_op`), the pass-open replay closing the hoist encoder, the call from `kk_draw`'s
pre-gfx generation (`kk_cmd_draw.c`), and `deps-arm64-probe/defer-m2-test/` (clone of the M1 harness,
plus a flag-set mode asserting the difference is exactly the pre-gfx dispatches moving ahead of the
render encoder).

Working rule from the two lost half-hours: a milestone's design goes into the tree (headers or a doc)
**before** its body is written, so a crash costs the body, never the reasoning.

## M2 - built and offline-proven (probe driver, default off)

`BB_KK_DEFER_HOIST=1` (needs `BB_KK_DEFER_RENDER=1` + `BB_KK_ONE_CMDBUF=1`): a draw whose pre-gfx
compute passes the gate is not split; its pre side ops are marked hoisted, go into the pass's hoist
compute encoder on the **pass's own command buffer** ahead of the render encoder, in record order, and
`kk_cmd_hoist_close` ends that encoder from the replay's pass-open node right before
`kk_renc_direct_begin` creates the render encoder. `kk_renc_slow_sync` is a no-op while the hoist
encoder is open, so nothing can create the render encoder early; non-hoisted compute takes
`kk_renc_slow_sync_force` and the M1 path. Post-gfx work and the tessellation path are unchanged.

The gate (`kk_renc_hoist_check`) refuses, per draw on its known reads and again per side op on its own
decoded addresses: ENC (render encoder exists), ANY (op flagged), ANY_EARLIER (earlier non-hoisted
ANY side op in this pass), RAW (read equals a write of an earlier non-hoisted side node), BARRIER
(in-pass barrier after a draw), HEAP (first heap use), BUSY (a compute encoder occupies the slot).
Hoisted nodes are excluded from ANY_EARLIER/RAW because they move ahead together in record order.

Evidence (run here, not accepted from the report): `ninja` exit 0, probe dylib UUID
`2E07D05D-746E-3EBE-812A-EC2A90F1D1C7`, `deps-arm64/lib/kosmickrisp` mtimes unchanged. M1 harness
still green (renc 93 / mtl 96 lines, both IDENTICAL, both controls detected). New
`deps-arm64-probe/defer-m2-test/` PASS: hoist-mode counters `draws=4 ops=4 encoders=3 refused=6
(enc=2 any=1 any_earlier=0 raw=0 barrier=1 heap=1 busy=1) late=1` (the one late refusal is the
synthetic D8 case only); hoisted dispatch order 1 2 10 12; the hoist Metal log equals the M1 log with
exactly those dispatches moved ahead of their render encoders; structure checks pass (each hoist
encoder closes immediately before its render encoder); predicate table passes all 19 rows; both
negative controls detected.

**Known gap, not fixed:** a *late* refusal (a pre side op refused after its draw passed the gate) leaves
the pass unsplit, so that op would be encoded on its own command buffer while the shared buffer is
open - `KK_BBSTAT_ONECB_ORDER_VIOLATIONS` in release, an assert in debug. Every pre-gfx path in
`kk_draw` is refused at the gate instead (tessellation and prefix-sum are ANY, first heap use is HEAP,
predicate/unroll addresses are in the gate's read set, an occupied slot is BUSY), and the per-op
re-check can only differ from the gate through work this draw itself adds - so it should not fire in
the real driver, but that is a trace, not a proof. **The runtime A/B must show `late refusals = 0`.**
If it ever fires, the fix is to split the pass at the refusal, not to widen the gate.

## M2 in the real driver (probe, `BB_KK_DEFER_HOIST=1`): measured, and INERT

One run at the owner's settings, `BB_NATIVE_GPU=1` + probe driver (UUID `2E07D05D`), deferred backend and
hoist both latched, game quit at ~8 min. `BB_KK_STATS` at exit:

```
pre-gfx hoist (BB_KK_DEFER_HOIST) on: draws hoisted 0, side ops hoisted 0, hoist encoders 0;
refusals 238645 (pass encoder already created 51677, may-touch-anything 97997, earlier unknown
side op 0, read-after-write 0, barrier after draw 0, first heap use 88971, pre_gfx busy 0);
late refusals 0; max Metal command buffers per recording 96 (gfx only 48); order violations 0
```

**Zero draws hoisted.** The gate refuses the entire population, and the refusals name exactly the
work that forces the splits: **tessellation (97 997, flagged `KK_RENC_SIDE_ANY`)** and **first heap use
(88 971)**, with the rest being draws that arrive after the pass's render encoder exists (51 677).
Read-after-write and barrier refusals: zero. No late refusals, no order violations - the safety model
held; it is just too conservative to ever hoist anything this game emits.

Allocator block unchanged: 70 gfx allocators, **7.83 GB** summed, median **130.5 MB** each (pre-M2:
86 / 10.07 GB / 122.7 MB), 602 Metal command buffers live, 565 606 created, max 48 gfx buffers per
recording.

**What that means.** M2 as specified cannot move this number: the pre-gfx work that splits the passes is
precisely the work the gate refuses by construction (`ANY` for tessellation because its accesses are not
decoded, `HEAP` because the heap reset is in-pass post-gfx work). The next vector is therefore not M3 - it
is to give the **tessellation side op a decoded read/write set** instead of `ANY` (it reads the draw's own
index/vertex inputs and writes fresh heap outputs, so it is hoistable once that is established), and to
decide whether the heap reset can be emitted ahead of the unroll safely. Until then the deferred-recording
route leaves the block where it is.

**Caveat on that run's frame rate (corrected the same night).** It was not a perf measurement: the run
script had `BB_KK_RENC_LOG` on, which writes one line per render-encoder call - **1.0 GB / 24.5 M lines in
five minutes** - and that alone explains the 12 fps the owner saw. The hoist counters are unaffected by
logging (the gate's decisions do not depend on it) and stand: zero hoists, block unchanged. `m2ab.sh` no
longer sets that log; it collects `BB_FRAME_STATS`/`BB_FRAME_LOG` instead so fps is read from the frame
CSV. The run also missed the Mesa shader cache (new driver UUID `2E07D05D` vs the preload's `C5B640F8`),
so it filled `~/.cache/mesa_shader_cache` from scratch; the next run on the same build is warm.

### Second run (clean, no render log, warm shader cache): confirms it

215 s of play, `BB_FRAME_STATS`/`BB_FRAME_LOG` on, `BB_KK_RENC_LOG` off.

```
pre-gfx hoist on: draws hoisted 0, side ops hoisted 0, hoist encoders 0; refusals 841380
(pass encoder already created 249036, may-touch-anything 350986, earlier unknown side op 0,
read-after-write 0, barrier after draw 0, first heap use 241358, pre_gfx busy 0);
late refusals 0; max Metal command buffers per recording 102 (gfx only 51); order violations 0
```

Allocator block identical to the pre-M2 measurement: **86 gfx allocators, 10 151 MB summed, median
132.7 MB** (before: 86 / 10 072 MB / 122.7 MB). 1 833 923 Metal command buffers created in 215 s
(8 500/s), max 51 gfx per recording.

Frame distribution that run (11 329 frames): mean 19.0 ms (**52.6 fps**), median 9.9 ms, p95 40.7 ms
(~25 fps), worst 522 ms; the five worst 5-second windows averaged 37-40 ms. So the frame rate is bimodal
- ~100 fps looking at cheap views, ~25 fps in the heavy ones - and the earlier 12 fps was the render log,
not this.

**Verdict on M2: correct, proven, and inert.** The gate refuses 841 380 of 841 380 candidate
evaluations. The population is exactly the work that splits the passes: tessellation 350 986 (flagged
`ANY` because its accesses are not decoded), first heap use 241 358, and draws arriving after the pass's
render encoder exists 249 036. Zero read-after-write and zero barrier refusals means the safety analysis
never even gets to the interesting cases.

So the memory route is blocked on one thing: **the tessellation pre-gfx side op carries no address list**
(`kk_cmd_draw.c` calls `kk_renc_side_op(..., KK_RENC_SIDE_ANY, NULL, 0u, NULL, 0u)`), so the gate cannot
prove it safe and refuses every tessellated draw - which in this game is most of them. Until that path
gets a decoded read/write set, deferred recording cannot reduce a single split on this title.

## Where the route stands after M2, and the next two measurements

Decision taken with the reference model (2026-10-23, after the clean run):

- **Do not invent a partial read-address set for the tessellation dispatch.** Its shader reads go through
  descriptor tables; index buffer + vertex/tess params + the three table roots cover the driver's reads,
  not the shader's, and a wrong hoist here is silent corruption, not a crash.
- **Prove the pass's WRITE set is empty instead**, from the Vulkan contract plus pipeline reflection:
  (1) an in-pass self-dependency barrier that can reach vertex/tess - already counted, measured 0;
  (2) transform feedback active on an earlier draw in the pass; (3) an earlier draw whose pipeline writes
  memory as a shader side effect (SSBO store, storage-image store, atomics); (4) any attachment in
  GENERAL / feedback-loop layout; (5) driver-internal side ops writing only fresh heap / per-draw params.
  If those hold, nothing a hoisted dispatch can see was written in this pass, so its unknown reads cannot
  matter. Residual risk to state before shipping: this is a GNM translation and may rely on behaviour that
  today only works because the encoder splits serialise everything.
- **Validate with an oracle, not an assertion**: decode the dispatch's real binding list from the argument
  tables the driver itself wrote, intersect with the pass write set, and require the gate never to be more
  permissive than the oracle. Ships behind a flag, permanently.
- **Two numbers before more work**: (a) is the 249k "pass encoder already created" a CASCADE of the first
  ANY refusal in each pass (in which case fixing ANY collapses both buckets)? The gate currently reports the
  first reason only, so this has to be counted per reason behind a debug flag; (b) does
  `MTL4CommandAllocator` capacity actually scale with the number of command buffers encoded into it - a
  standalone Metal 4 bench, because the whole route rests on that premise and it has never been measured
  in isolation.

Both measurements are running as of 2026-10-23 00:05.

## The premise, measured in isolation (2026-10-23): the route has a real ~10x ceiling

`deps-arm64-probe/mtl-split-bench/` - one `MTL4CommandAllocator`, reset once, N Metal command buffers
encoded, `allocatedSize` read back; 3 reps per point, spread 0 B (deterministic).

- **Capacity is a function of command buffers per reset, nothing else.** ~**2.38-2.44 MB per render
  command buffer**, R^2 1.000000, flat whether the buffer carries 0, 50 or 200 draws (draws add ~811 B
  each, encoders ~28 KB; both second-order). Holding 10 200 draws constant: 1 CB = 10.5 MB,
  51 CBs = 124.5 MB - 11.8x.
- **Floor** 25 784 B after reset; one render CB 2.40 MB; a compute CB ~0.52 MB; an empty CB 32 KB.
- **An allocator never shrinks on reset** - it holds its high-water mark for life (256 CBs pins 608 MB,
  and a later 1-CB recording still reads 608 MB). Submission is irrelevant: capacity is set at encode
  time (a no-commit run is byte-identical).
- **Model check against the game:** worst recording 51 gfx CBs x 2.38-2.44 MB = 121-125 MB, i.e. 92-94% of
  the measured 132.7 MB per allocator. The block is set by the single worst recording, not the average.
- **Ceiling:** 1 CB per recording at the same ~10.2k draws = ~10.5 MB per allocator -> **~0.9-1.6 GB**
  instead of 10.15 GB (at 20k draws 18.5 MB / ~1.6 GB). Lowering the 11.7 average changes nothing while
  any recording still splits into 51.

So deferred recording is worth finishing: the target is the worst-case split count per recording.

## The hoist gate, rebuilt on a proof instead of a blanket flag

The tessellation pre-gfx side op no longer carries `KK_RENC_SIDE_ANY`. It is now gated by a per-pass
write-set predicate derived from pipeline reflection (`kk_shader_info.bb_mem` / `bb_mem_stage`, computed
on the app's NIR in `kk_bb_reflect_memory`, before the driver's own emulation stores are added):

- `SHADER_WRITE` - an earlier draw in the pass whose pipeline writes memory as a shader side effect
  (`nir->info.writes_memory` or `nir_intrinsic_writes_external_memory`: SSBO/global stores and atomics,
  storage-image stores and atomics), or a side op flagged as writing;
- `XFB` - transform feedback (`nir->xfb_info`; the driver does not advertise the feature);
- `FEEDBACK` - an attachment (or its resolve target) in a layout other than
  COLOR/DEPTH_STENCIL/DEPTH/STENCIL/ATTACHMENT_OPTIMAL, i.e. anything a shader could sample while it is
  being rendered (GENERAL, feedback-loop, local-read); plus a dynamic feedback loop.
- `BARRIER`, `ANY_EARLIER`, `RAW`, `ENC`, `HEAP`, `BUSY` unchanged. `ANY` survives for ops that genuinely
  cannot be bounded (e.g. no bound VS, `write_u32_array`), and the libkk tessellation kernels now carry a
  narrower `KK_RENC_SIDE_DRIVER` flag (they only touch the draw's own pool params and geometry heap).

Validated by an **oracle** (`BB_KK_HOIST_ORACLE=1|abort`, permanent, default off) that decodes the real
binding list of each accepted op from the descriptor bytes the driver itself wrote and intersects it with
the pass's write set (attachments + decoded draws + non-hoisted side ops); the gate may never be more
permissive than the oracle. Offline: gate vs oracle agree on the clear cases, and a deliberately broken
reflection is caught with both ranges printed (`oracle test: PASS`, `mismatches=1` on the broken case).
`BB_KK_HOIST_WHY_ALL=1` counts every applicable reason per evaluation (the gate reports the first only),
plus a cascade counter that separates ENC/BUSY caused by an ANY-only refusal's split from genuine ones.

Offline gates after the change: M1 harness both logs byte-identical with both controls caught; M2 harness
PASS with the tessellation chain now hoisting end to end; a whole-pass hoist case added. Probe UUID
`2A454A3C-8DF2-3D0F-B414-AC7EBFBCAFBC`; installed driver untouched.

**Still unmeasured: the ceiling in the real driver.** Needs one runtime run with `BB_KK_HOIST_WHY_ALL=1`
and `BB_KK_HOIST_ORACLE=abort` plus an image-diff against hoist-off.

## The heap reset joins the hoist (2026-10-23) - the last refusal that stood in front of every pass

The gate's `HEAP` refusal (first heap use in a recording) was cascading: 147 216 of 147 216 renderings
with pre-gfx work refused on it, and every later draw in those passes then refused as ENC or BUSY.

`kk_heap` now routes the reset as a **pre side op of the hoisted draw** when that draw passed the gate:
`libkk_write_u32(..., pre_gfx = true, heap bottom = 0)`, so `kk_dispatch_precomp` records the node before
`cs_get_compute` and the reset is encoded into the pass's hoist encoder ahead of the allocating dispatch.
`HEAP` alone no longer refuses (`if (heap_first && mask != 0u)`), the reason and counter are kept for draws
refused for something else, and a non-hoisted draw keeps the old in-pass post_gfx placement. `uses_heap` is
cleared by `cs_start_render`, not by the gate, so the "first use in this pass" decision lives in `kk_heap`
keyed on `renc.hoist_draw` - which is why the gate's `heap_first` alone could not be trusted.

Offline: M1 harness still byte-identical with both controls; M2 harness PASS, with the heap section showing
`reset -> VS+TCS -> tess -> prefix -> tess -> render encoder` inside one hoist encoder, the non-hoisted
cases keeping the post_gfx placement, and the wrong-placement control caught. Probe UUID
`A6E705F8-3333-3765-A206-B4C281B26E9E`, installed driver untouched (sha256 `87ea3824…`).

Also fixed before this: the gate had lost its early-out when it became mask-based (it walked the whole op
list even when the render encoder already existed) - restored for the default path, `BB_KK_HOIST_WHY_ALL`
still pays the walk.

Unmeasured at runtime: whether draws now hoist, and what that does to ENC/BUSY, the block and fps.
