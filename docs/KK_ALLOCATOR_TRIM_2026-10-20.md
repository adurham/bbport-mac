# KosmicKrisp's command-allocator hold: what can be released, and what a trim costs

Measured 2026-10-20 on the M4 Max at the owner's settings (aspect 21:9, 3440x1440, preset 4
UltraPerformance, fsr3), hidden runs, `BB_NATIVE_GPU=1 BB_VRAM_LIMIT_MB=6144 BB_KK_STATS=1`,
`bash tools/macos/warm_route.sh 420` from `tools/macos/memprobe_trim_arm.sh`, sandbox
`BB_DATA_DIR` only. Branch `probe/kk-allocator-trim` (gate + arm script), based on the native line.

## Where the hold is

KosmicKrisp gives every `VkCommandBuffer` **three** `MTL4CommandAllocator`s, one per encoder state —
`kk_init_encoder_state` (`deps-arm64/.../kosmickrisp/vulkan/kk_cmd_buffer.c:112`), called for
`pre_gfx`, `gfx` and `post_gfx` in `kk_create_cmd_buffer` (same file, :147, :150, :153). Each
allocator holds the encoding memory of the commands recorded through it.

## Does a release path exist? Yes, but not in-place

| what | where | returns memory? |
|---|---|---|
| `[MTL4CommandAllocator reset]` | `kk_reset_encoder_state`, `kk_cmd_buffer.c:200` | **No** |
| `mtl_release(allocator)` (destroy) | `kk_destroy_encoder_state`, `kk_cmd_buffer.c:78` | **Yes** |
| `vkTrimCommandPool` | `kk_TrimCommandPool`, `kk_cmd_pool.c:140` | **No**, by itself |
| `vkFreeCommandBuffers` | `vk_common_FreeCommandBuffers` | only recycles (pool has `RESET_COMMAND_BUFFER_BIT`) |

Reset is the only reclaim call in steady state and it keeps the allocation. Measured directly with
`MTL4CommandAllocator` (8 allocators, 2000 encoders each):

```
after encoding        TOTAL allocatedSize = 789.57 MiB   phys_footprint = 301.6 MiB
after reset() on all  TOTAL allocatedSize = 789.57 MiB   phys_footprint = 301.6 MiB
after releasing all   phys_footprint =  21.1 MiB
```

`kk_TrimCommandPool` is not a release path for the port either: it calls `vk_command_pool_trim`,
which only destroys command buffers already sitting on the pool's *free* list
(`destroy_free_command_buffers`, `vulkan/runtime/vk_command_pool.c:51,260`), and this pool sets
`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`, so `vkFreeCommandBuffers` recycles instead of
destroying. The working pair is therefore **free the idle command buffers, then trim the pool** —
that reaches `ops->destroy` → `kk_destroy_cmd_buffer` → `kk_destroy_encoder_state` →
`mtl_release(allocator)`.

The port never did this: `CommandPool` grows by four and reuses by tick, holding every buffer it
ever allocated (`gpu/vulkan/command_pool.cpp`). So each buffer's allocator keeps its high-water mark
for the life of the process — that is the 6-7 GiB, and it is why resetting shows a plateau rather
than a leak.

## What the gate does

`BB_KK_TRIM_POOL=<seconds>`, default off (`gpu/vulkan/command_pool.cpp`). On the GPU-caught-up tick
`Commit()` already reads, it frees every entry with `tick <= done` (the identical predicate the
reuse search uses), trims the pool, and prints one line per trim with buffers freed, entries
before/after and phys_footprint before/after. Nothing in flight is touched. With the gate unset
there is no free, no trim and no print, and `Commit()`'s tick reads are unchanged.

## Measured arms (420 s each, gameplay reached: 700-950 draws/frame)

Two independent pairs, because the port crashes on pre-existing asserts now and then (three of five
attempts died on `gcn_to_vk.cpp:422` / `cp_graphics.cpp:203`, with the gate off as well):

| arm | gate | footprint 120-300 s mean | end | allocators (hold) | fps median | worst-frame median | windows > 100 ms |
|---|---|---|---|---|---|---|---|
| A2 control | off | 16212 MiB | 17166 | 1809 (8896 MiB) | 44.9 | 31.5 ms | 1 of 83 |
| B trim | 5 s | 14544 MiB | 15259 | 609 (5889 MiB) | 43.3 | 87.8 ms | 33 of 83 |
| A4 control | off | 15117 MiB | — (crash at 310 s) | 165 (7428 MiB) | 50.0 | 28.1 ms | 2 of 61 |
| B2 trim | 5 s | 13449 MiB | 13351 | 129 (4043 MiB) | 46.6 | 72.9 ms | 24 of 83 |

**Steady state: −1668 MiB of phys_footprint in both pairs** (−10.3 % and −11.0 % of the window
mean). The driver's own allocator ledger falls further — −3007 MiB and −3385 MiB — because
`allocatedSize` is not all resident.

A single trim drops a median 752 MiB immediately (84 trims, max −1962 MiB) and the hold refills
between trims: this is a sawtooth, not a one-way release. The pool needs those buffers and
re-creates them.

## The cost

The trim is not free. Median worst frame goes 28-32 ms → 73-88 ms and most 5 s windows contain a
>100 ms hitch (24-33 of 83, against 1-2 of 83 without). Median FPS falls 1.5-3.4 (3.5-7 %).

So: ~1.7 GiB for a ~80 ms hitch every 5 s at this interval. A longer interval, or trimming only at
an area load, would trade less often for the same ceiling; the sawtooth peak is what matters for
jetsam, not the average.

## What a driver patch would have to do

The structural fix is not in the port, because nothing in the driver can shrink a live allocator:
either

- **destroy and lazily re-create `es->allocator`** on `vkResetCommandPool`/`vkTrimCommandPool` when
  the encoder state has no live command buffer (the native-path analogue of the renderer's old
  `d6704c6`, which trimmed idle command buffers and never reached this driver), or
- **stop owning one allocator per command buffer per encoder state** — pool a small ring of
  allocators per `VkCommandPool` and hand one to each buffer at `begin`.

Either makes the trim unnecessary; both need the driver patch series, not a port gate.
