# What is left over 50 ms, now that the compiles are preloaded

Measured 2026-10-22 on `4f8e360` (native line, `kk_preload=1`, driver KosmicKrisp `C5B640F8` with
patches 0001-0008), M4 Max, macOS 27.2, 3440x1440 out, upscaler `fsr3` preset 3, `fps_mode=uncap`.
Three hidden `sizing_run.sh` runs on the owner's own save, store = a clone of his store
(`STORE=copy`), default walk route. Machine not idle: a 8.7 GiB virtual machine and post-reboot
Spotlight/MediaAnalysis were resident, and the footprint watchdog (`free+inactive < 4 GiB`) ended
every run 50-90 s in - so these are short samples, not a full session.

## The numbers

| run | flips | walk | fps | frames >= 50 ms | s in them | worst | compiles on the drawing thread |
|---|---|---|---|---|---|---|---|
| A (`kk_preload=1`) | 2 896 | 44 s at >= 300 draws | 42.5-45.2 | 16 | 3.68 | 2 081 ms | 1 frame, 30 pipelines |
| A2 (same, second launch) | 1 892 | 38 s | 49.4 | 13 | 1.80 | 414 ms | 1 frame, 30 pipelines |
| B (VRAM capped 10 GiB) | 2 311 | - | - | - | - | - | ended at area load |

Steady state after the area load, from the run's own 5 s windows: **42.5 fps, worst frame 25-29 ms,
0 shader/pipeline compiles, 0.0 ms in the driver**, 1 484 draws/frame, GPU command thread 8.7 us/draw.

So in a covered area on the owner's store the compile class is gone from play. What is left is:

1. **One burst at the area load: 30 pipelines, all archive misses.** A: 30 pipelines in one frame,
   **2 016.9 ms** of `vkCreateGraphicsPipelines` on the drawing thread (the frame is 2 081 ms).
   A2, same save, second launch (Metal's own cache warm): the same 30 pipelines again, **329 ms**
   (frame 414 ms). The burst is **not** a one-time cost - it recurs every launch, 6x cheaper when
   Metal has seen the MSL before.
2. **A scatter of 60-300 ms frames, no compiles:** bb-gpu draining the guest's page faults
   (3 frames, 141 ms), IPC round trips (2-3 frames, 45-60 ms), and 4-5 frames over 50 ms whose time
   is on the guest's side with none of the instrumented phases naming it. These are 4-8% of the
   hitch time each and are the first real samples of the post-compile class.

The burst's layer split (30 pipelines, joined `BB_PIPE_TIMING` x `BB_KK_TIMING` by thread and order):

| layer | A (Metal cold) | A2 (Metal warm) |
|---|---|---|
| port GCN -> SPIR-V | 16 ms (0.8%) | 13 ms (4%) |
| Mesa NIR -> MSL | 252 ms (12.6%) | - |
| Metal MTLLibrary | **1 096 ms (54.8%)** | 194 ms (59%) |
| Metal pipeline state | 620 ms (31.0%) | 123 ms (37%) |

All 30 are `hit=0`: the shipped AIR + archive do not hold their libraries or pipelines.

## Why the archive misses them

The port's own store already holds all 30 (`warm_merge.sh --dry-run`: 1 776 blobs identical, **0
added**), so this is not translation coverage - it is Metal-side coverage. Their libraries are part
of the run's 278 `compiled from MSL`, and an MTL4 archive entry is keyed on the function hash of the
**metallib**, so a library compiled at run time never matches a shipped AIR library (the reason for
pass A/pass B in `KK_AIR_ARCHIVE.md`).

Checked against the capture routes: of the 31 pipeline keys this run created, **26 appear in no
capture route at all** (each of the 7 routes covers 3-4 of 31). The capture set was built on the
save position as it was then; the owner has since moved, and area-load states are positional. The
archive is a snapshot of *where he had been*, not of the area.

## What the data argues for, in order

1. **A capture pass on the current save position** (the mechanical part): the 30 load-burst
   pipelines are reachable in 22 s of a hidden run on this route, so a pass-A/pass-B over today's
   position would capture their MSL + archive entries and turn the burst into ~0.3 ms lookups. It is
   a per-position patch, not a fix for the next new area.
2. **The per-pipeline cost is the real lever, not the count.** 67 ms cold / 11 ms warm per missed
   pipeline, of which Metal's compiler is 85%+ (`PIPELINE_SIZING.md` §5a: fan the frame's misses out
   to 4 threads). The archive only ever helps where a capture has been.
3. **The memory envelope now bounds every measurement** (and the owner's play): 12-16 GiB footprint
   against a 36 GiB machine with a VM resident leaves `free+inactive` under 4 GiB, which is the
   jetsam recipe that has already taken the GUI down three times
   (`references/gpu-memory-footprint.md`). The per-pool MTL4CommandAllocator ring is still the only
   lever there bigger than noise - and until it exists, hidden runs cannot outlast the watchdog.

## Method note (reusable)

`tools/macos/hitch_attribution.py` reads a run's `frames.csv` and attributes every frame >= 50 ms to
a side (bb-gpu busy vs waiting for the guest) and then to a named phase inside that side, using the
identity `frame_ms = gpu_idle_ms + ga_wall_ms`. It prints the ranked classes, and the worst frames
with every candidate column, so the class can be re-derived from raw numbers:

```
BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 STORE=copy bash tools/macos/sizing_run.sh TAG 300
python3 tools/macos/hitch_attribution.py out/sizing/TAG/frames.csv
python3 tools/macos/hitch_attribution.py out/sizing/TAG/frames.csv --min-draws 0   # incl. loads
```
