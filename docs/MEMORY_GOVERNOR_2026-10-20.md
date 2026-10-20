# Memory governor (renderer_share_cap + corrected reserve) and the command-allocator numbers

Measured 2026-10-20 on the M4 Max (36 GiB, owner's real background load), hidden 420 s runs, owner's
settings (3440x1440, fsr3 preset 4, frame generation on), no `BB_VRAM_LIMIT_MB`, sandbox
`BB_DATA_DIR`, `tools/macos/memprobe_arm.sh`. Control = the recorded `control` arm
(docs/MEMORY_LEVERS_2026-10-20.md); arms are not degenerate, so no fresh control was run.

## Code (all default off)

- `gpu/vulkan/instance.cpp` `GovernedBudget`: on with `renderer_share_cap` (bbport.ini, MiB;
  `BB_RENDERER_SHARE_CAP`) or `BB_MEM_BUDGET_RESERVE=1`. budget = min(port budget, cap, reserve),
  reserve = hw.memsize - (wired + compressor - own graphics ledger) - 6 GiB (the old formula's
  self-reference removed), floor = max(4 GiB, images referenced in the last 3 frames / 0.85).
  1 Hz sampling; falls <= 256 MiB/s; rises after 25 s of slack; one line per 1 GiB move naming the
  bound; 10 s status line; one-shot "memory pressure is from other apps (budget at floor X MiB)".
- `gpu/textures/texture_cache_memory.cpp`: per-frame gc_tick marks; with a gate on, an image touched
  in the last 2 frames is never evicted (counted as refused) and `evicted while referenced` is
  counted in every run; the VramShort signal also fires on integrated GPUs so idle guest-memory
  copies are returned.
- `renderer_share_cap` parsed in `bbport_settings.cpp` (default 0); `tests/test_settings.cpp` pins
  default/ini/env and that Save() keeps but never writes it.
- KosmicKrisp patch 0007 `BB_KK_ALLOC_DUMP=<s>[,<s>]` (with BB_KK_STATS): per-pool allocator dump.
  `command_pool.cpp` (BB_KK_STATS): in-flight-at-Commit histogram per port pool.

## Arms

| | control | Arm 1 cap 5120 (gov-cap4) | Arm 2 cap + reserve (gov-capres) |
|---|---|---|---|
| footprint mean 120-300 s / end / max | 14305 / 15259 / 15259 | 16212 / 16212 / 16212 | 14975 / 15259 / 15259 |
| renderer's share, time-weighted median / max | 6298 / 6378 | 6127 / 6322 | 6267 / 6395 |
| driver's share (last) | 7619 | 7652 | 7915 |
| allocators (held MiB) | 165 (7291) | 177 (8198) | 165 (7421) |
| live images | 1070-1358 | 478-1015 | 734-1048 |
| evicted while referenced (last 2 frames) | n/a | 0 (of 4413 evictions) | 0 (of 4396) |
| windows > 100 ms | 9 / 83 | 1 / 83 | 1 / 83 |
| fps median / last 30 | 44.3 / 41.8 | 45.0 / 40.7 | 45.5 / 41.2 |
| draws/frame median / max | 590 / 853 | 647 / 1097 | 790 / 907 |
| heaps created/s | 1.00 | 1.05 | 1.03 |
| binding bound (42 status lines) | port (26-28 GiB) | cap 42/42 | cap 42/42 (reserve 20.4-23.8 GiB) |

Arm 1 took four attempts: gov-cap (the first floor counted buffers, floor ratcheted to 7.1 GiB, cap
bound 3/42 — fixed in 6d0d3f7), gov-cap2 died at 337 s on `gcn_to_vk.cpp:422 Unknown
data_format=15`, gov-cap3 died at ~300 s on `gcn_to_vk.cpp:422 data_format=6`, gov-cap4 clean.
Arm 2 clean on its first attempt. Machine (wired/compressor/free+inactive MiB) start / mid / end:
Arm 1 4741/5883/16746, 14089/8187/6609, 14389/7968/7098; Arm 2 5172/6439/18030, 14899/7473/6830,
16185/7559/6661.

## Verdict: acceptance FAILS on the two memory criteria

- Renderer's share −171 / −31 MiB (needed −1536); peak footprint +953 / 0 (needed −500). Hitches,
  fps, heap churn and the eviction guard all pass.
- **Why the cap cannot move it:** "the renderer's share" is KosmicKrisp's heapUsage =
  `[MTLDevice currentAllocatedSize]` (kk_physical_device.c:1016-1033): every Metal allocation of the
  device, including the driver's own heaps (6.0 GiB live). What the texture collector can free is
  ~0.55 GiB of cached images (in-use set ~0.52 GiB) plus 0.64 GiB of guest-memory VRAM blocks; VMA
  total is ~3.8 GiB. A 5 GiB cap sits below the unevictable part, so the collector runs pinned at
  critical, evicts ~10 images/s that are re-created, and the number does not move.
- **The earlier arm B result was two artifacts:** its "renderer's share 4431 median" was a median
  over 1 GiB-move log lines (time-weighted: 5962 vs control 6298, −336), and its "−954 MiB peak" is
  one quantization step — `footprint -p` prints whole GB (13351/14305/15259/16212 MiB are 14/15/16/17
  GB), so the sampler cannot resolve anything under ~954 MiB. Any future footprint claim needs
  `ri_phys_footprint` or `footprint --unit` bytes.
- The corrected reserve never binds on this box (20-24 GiB) — correct behaviour (other apps were not
  pressing); the self-referential collapse is gone.

## Command allocators (Task 2)

- Three per VkCommandBuffer: `pre_gfx`, `gfx`, `post_gfx` (`kk_cmd_buffer.c:146-155`,
  `kk_cmd_buffer.h:232-234`). Metal cannot put compute inside a render encoder, so compute before
  the open render pass goes to pre_gfx and compute during it to post_gfx; they swap after each gfx
  commit (`cs_end`, :441/:446). Each encoder state owns a MTL4CommandBuffer + allocator.
- Sizes (dump at 200/400 s, both arms): 55 buffers / 165 allocators / 6.8-7.4 GiB in 6 pools; three
  pools hold 99%: 16 + 12 + 12 buffers at **183.7-185.9 MiB each (gfx ~158, pre/post ~12.6)**,
  median = max: the hold is spread evenly — every buffer sits at the same frame-size high-water.
  The three small pools: 8 x 2.5-4.9, 6 x 2.5, 1 x 0 MiB.
- Two buffers of one pool recording at once: never (most recording at once = 1 in every pool;
  `scheduler.cpp` gives each recorder thread its own pool, Commit/begin/end per submission).
- Port side: every pool reuses by tick and grows by 4 only when all are in flight. The three big
  pools (port pools 1-3 = driver pools of 16/12/12 buffers) peak at 13 / 9 / 9 in flight at a Commit
  (p99 11-13 / 7-9 / 7-9); the slack is the grow-by-4 granularity plus old peaks. A per-pool ring of
  depth max-in-flight+1 = 14 + 10 + 10 = 34 buffers at ~185 MiB: **projected plateau 6.1 GiB
  (p99+1: 5.8 GiB) vs 7.3 GiB today, i.e. −1.2 to −1.5 GiB**, with the risk that a full ring waits
  on the GPU where today it grows.
- **Go/no-go: no-go for a driver allocator change.** An allocator cannot be reset while its buffer is
  in flight, so a driver ring is bounded by the same in-flight count and buys the same ~1.3 GiB the
  port can get by capping its own ring, without touching Mesa. The structural number is ~185 MiB of
  encoding memory per submission-sized buffer; shrinking that (fewer/smaller encoders per buffer) or
  the frames in flight is what moves the 7 GiB.
