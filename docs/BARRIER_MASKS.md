# Barrier stage masks (`BB_BARRIER_MASKS`)

Branch fix/barrier-masks, 2026-10-17. Follows docs/KK_GPU_WALL.md. That doc names the lever: the
driver's `BB_KK_BARRIER=vk` mode can only let a pass's vertex work overlap the previous pass's
fragment work if the port's barriers stop naming vertex-side destination stages. Today nearly all of
them say `ALL_COMMANDS`.

## Modes

| BB_BARRIER_MASKS | what the port's barriers wait in |
|---|---|
| legacy (default) | as before: the Runtime memory barrier waits in ALL_COMMANDS, and sampled textures transition for ALL_COMMANDS |
| safe | the stages that consume the data (table below). The memory barrier is emitted exactly when legacy emits it. Unknown consumers keep ALL_COMMANDS |
| strict | safe, but the stage of the binding itself instead of the pipeline's stages, and a memory barrier only for a buffer conflict that names its consumer (experimental) |

The masks only matter to a driver that reads them. With KosmicKrisp's default `BB_KK_BARRIER=legacy`,
every encoder ends with ALL->ALL, so the mode changes nothing on the GPU. Measure with `BB_KK_BARRIER=vk`.
`BB_FRAME_STATS=1` prints a `Barrier masks` line every 10 s: memory barriers waiting in all, vertex-side
or fragment-side stages; image barrier batches by side; new-stage chains; and the destination bits that
keep each flush vertex-side.

## The map: who emits what, and who consumes it

### 1. Runtime::FlushBarriers: the global memory barrier (render/runtime.cpp)

src = stages of the accesses since the last barrier (AccessBuffer/AccessGlobal), srcAccess = their
writes; legacy dst = ALL_COMMANDS, MEMORY_READ|WRITE. What triggers a flush, and its real consumer:

| trigger (IsBufferAccessed caller) | consumer of the data | safe dst |
|---|---|---|
| vertex buffer ranges (raster_bind ResolveVertexBuffers) | vertex attribute fetch | VERTEX_INPUT, VERTEX_SHADER |
| index buffer (ResolveIndexBuffer) | index fetch | VERTEX_INPUT, VERTEX_SHADER |
| guest V# buffers, GDS (BindGuestBuffer, BindSpecialBuffer) | the shader stages of the pipeline that bind buffers | those stages (e.g. VS+FS); compute for dispatches |
| paged "any memory" binding (BB_LAYER_MEMORY) | that binding's stages | the stages, forced even without a tracked conflict. A paged **write** makes the next flush ALL (global_write) |
| indirect args / count (DrawIndirect, DispatchIndirect) | DRAW_INDIRECT; compute for the GPU check (IndirectGuard) | DRAW_INDIRECT, COMPUTE |
| CopyBuffer, FillBuffer, UpdateBuffer, CopyFromGuestChunk, Upload/DownloadImage, CopyImageThroughBuffer | transfer | ALL_TRANSFER |
| copy-shader HLE (shader_hle.cpp), detiler dispatches | compute | COMPUTE |
| explicit FlushBarriers() (labels, signals, submits, presenter, upscaler, scene proxies, button prompts, the backing-sample copy) | unknown or everything | ALL_COMMANDS (unchanged) |

Accesses that a narrowed barrier ordered before stages S only are **carried**, reads included (the
WAR case). A later IsBufferAccessed on an overlapping range, whose consumer has a stage outside S,
triggers one more barrier. That barrier's first scope includes the carried stages. A full barrier
clears everything. A barrier is only ever narrowed, never dropped: safe emits the memory barrier
whenever legacy would.

### 2. Image layout transitions (Runtime::Transit -> Image::GetBarriers, textures/image.cpp)

src = the image's tracked state (pl_stage, access, layout); dst = what the caller passes.

| caller | legacy dst | consumer | safe dst |
|---|---|---|---|
| sampled textures: TransitSampled (BindTextures), BindTexturesFromSet | ALL_COMMANDS | the shader stages of the pipeline that bind images (from the translated shaders' resource lists, Shader::Info::images, not descriptor-set-layout flags) | those stages (FS only for most draws, VS+FS where the vertex shader samples, COMPUTE for dispatches) |
| storage images, general layout, feedback loops (TransitSampled first branches) | ALL_COMMANDS / ALL_GRAPHICS | read+write in any stage | unchanged |
| color / depth attachments (raster_targets.cpp) | COLOR_ATTACHMENT_OUTPUT / EARLY+LATE tests | already exact | unchanged |
| transfers (UploadImage, DownloadImage, PrepareTransfer, ClearImage) | COPY / CLEAR / RESOLVE | already exact | unchanged |
| presenter, upscaler, scene proxies, button prompts | their own stages | already narrow | unchanged |

**The trap the KK review flagged:** a texture read in a new stage with no layout change must still
get a barrier from the last writer. Legacy covered this case by accident: every sampled transition
said ALL_COMMANDS, so later readers in any stage were already in scope. In safe, the state's
`pl_stage` is the union of the stages already ordered after the last write. A same-layout read whose
stages are a subset needs nothing. A read in a new stage gets a chain barrier
{src = pl_stage, srcAccess = 0, dst = the new stage, old = new = layout}. That barrier forms an
execution dependency chain from the writer, and the earlier barrier already made the writes
available. pl_stage then becomes the union. The next writer's barrier starts from that union (WAR).
The `new-stage chains` counter in the stats line counts these.

**Same batch:** barriers in one vkCmdPipelineBarrier2 are not ordered against each other, but the
image state assumes they run in sequence. When a barrier is queued for an image range that already has
a queued barrier, the queued one also waits in the new one's stages, and the new one starts from the
queued one's first scope. A same-layout chain the queued one covers is folded into it
(Runtime::MergeQueuedChains, the `merged` counter).

### 3. Per-draw signals (raster_signals.cpp, vk_gpu_labels.cpp)

End-of-pipe labels go through AMD buffer markers or portable vkCmdUpdateBuffer. Before a portable
label, the port calls an explicit FlushBarriers (ALL), followed by ALL->TRANSFER|HOST and
TRANSFER->ALL|HOST barriers. The CPU and any later stage may consume them. **Unchanged.**

### 4. Other blanket barriers left as they are (and why)

- Scheduler::QueueSubmit wait stages (ALL / COLOR_OUTPUT): semaphore waits for the swapchain and
  frames; a cross-submission dependency on unknown work.
- SceneTargets::Transition src = ALL_COMMANDS: the proxy's last use is not tracked.
- Fault buffer, occlusion, timestamps, indirect guard, upscaler (FSR, MetalFX, frame generation):
  a few per frame, outside the draw stream, consumer = their own compute plus everything after.
- SetBackingSamples (MSAA backing switch): src = ALL for a fresh image.

## Measured (2026-10-17, M4 Max, stock deps + the fix/kk-gpu-wall KK dylib with patch 0005)

Each run was hidden: `tools/macos/gpu_ab_run.sh` with a standing camera
(`ROUTE='ly=128,rx=128:5'`), `BB_KK_BARRIER=vk`, a Metal System Trace 30 s into gameplay (3 s), and
two port captures (Ctrl+Shift+C path). Legacy and safe alternated L1 S1 L2 S2 L3 S3 L4 S4, then
strict1, legacy5 and strict2. No other bb-probe was running during any run (overlap files empty), and
there were 0 asserts. The output was **3440x1440** (this checkout's bbport.ini), not the 2560x1440 of
KK_GPU_WALL.md, so absolute frame times run about 2.5 ms higher than there. frame_ms is the 850-950 draw
bucket (all of this scene). The KK counters are the last 10 s window. busy, overlap and idle come from
`tools/macos/mtl_gaps.py`.

| run | n | frame p50 | p25 | p75 | mean | render enc waited | of them before vertex | % | vtx-side from mem/buf | from images | GPU busy ms/f | >=2 ch % of busy | idle ms/f | cmdbufs/f |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| legacy1 | 4734 | 26.51 | 26.29 | 26.71 | 26.67 | 63332 | 61229 | 96.7 | 62834 | 57022 | 22.71 | 1.3 | 3.43 | 323.7 |
| safe1   | 4614 | 26.66 | 26.43 | 27.00 | 26.99 | 68339 | 52318 | 76.6 | 46998 | 39717 | 23.48 | 1.2 | 3.42 | 338.4 |
| legacy2 | 4717 | 26.58 | 26.32 | 26.86 | 26.76 | 63364 | 61300 | 96.7 | 63582 | 57022 | 23.26 | 1.4 | 3.40 | 327.4 |
| safe2   | 4677 | 26.72 | 26.47 | 26.94 | 26.91 | 67790 | 51785 | 76.4 | 46424 | 39717 | 23.17 | 1.1 | 3.48 | 337.3 |
| legacy3 | 4642 | 26.45 | 26.25 | 26.73 | 26.68 | 62952 | 61000 | 96.9 | 63241 | 56565 | 23.11 | 1.2 | 3.41 | 328.2 |
| safe3   | 4609 | 26.75 | 26.49 | 27.05 | 26.94 | 67080 | 51019 | 76.1 | 45667 | 39823 | 23.41 | 1.1 | 3.47 | 339.7 |
| legacy4 | 4669 | 26.59 | 26.34 | 26.83 | 26.86 | 62773 | 60785 | 96.8 | 64167 | 56716 | 23.75 | 1.2 | 3.42 | 329.9 |
| safe4   | 4648 | 26.74 | 26.49 | 27.11 | 27.07 | 67633 | 51786 | 76.6 | 46953 | 39290 | 23.43 | 1.2 | 3.38 | 340.8 |
| strict1 | 4601 | 26.82 | 26.58 | 27.11 | 27.09 | 70667 | 50527 | 71.5 | 35917 | 39717 | 23.68 | 1.2 | 3.42 | 347.9 |
| legacy5 | 4647 | 26.56 | 26.32 | 26.83 | 26.85 | 63676 | 61649 | 96.8 | 63624 | 57171 | 23.25 | 1.2 | 3.35 | 325.6 |
| strict2 | 4603 | 26.64 | 26.40 | 27.10 | 26.90 | 69051 | 49826 | 72.2 | 34467 | 39290 | 23.25 | 1.3 | 3.49 | 334.5 |

- **frame_ms:** legacy p50 is 26.45-26.59 (mean 26.54) and safe is 26.66-26.75 (mean 26.72). Safe is
  slower in all 4 alternated pairs, by +0.15/+0.14/+0.30/+0.15 ms. On a fixed camera the run-to-run
  spread is about 0.15 ms, so this is a small but consistent **loss**, not a win. Strict: 26.82 and
  26.64.
- **What the masks achieved in the driver:** render encoders that wait before their vertex stage
  fell from 96.8% to 76.4% (safe) and 72% (strict). Vertex-side barriers from images fell by 30%
  (57k to 39.7k per 10 s), and those from memory/buffers by 27% (safe) and 45% (strict).
- **What that bought on the GPU:** nothing. Two-channel overlap stayed at 1.1-1.4% of busy time
  (0.3 ms/frame) in every run, and idle stayed at 3.4 ms/frame. A pass whose vertex stage may start
  early only overlaps if the previous pass is still in flight, and with roughly 3 in 4 passes still
  holding their vertex stage (and compute and blit encoders waiting on everything in `vk` mode), the
  chances do not arise.
- **What it cost:** about 10-15 more Metal command buffers (encoders) per frame (cmdbufs/f column),
  and render encoders that waited rose from 63k to 68k per 10 s. **Probable cause, not traced:**
  extra barriers that split passes. A consumer in a stage a narrowed barrier did not cover needs a
  barrier of its own (carried accesses), where legacy's ALL_COMMANDS barrier had already covered
  it. New-stage image chains are too rare to explain it (about 370 per 10 s). The +0.15 ms most
  likely comes from these extra encoders, but no per-site count was taken to prove it.
- **Why most flushes stay vertex-side** (safe, `Barrier masks` line, per 10 s, about 37k vertex-side
  memory barriers): transfer destinations 25k (copies, uploads, fills, CP DMA), vertex shader 15.6k (the
  vertex shader binds the constant buffers that earlier copies wrote: a real consumer), vertex input
  9k, indirect 0.5k. The transfer case is a **driver** classification: patch 0005's
  `kk_bb_dst_needs_vertex_wait` treats TRANSFER as vertex-side, yet transfers run in blit/compute
  encoders that wait on all prior work in `vk` mode anyway (kk_start_compute_encoder). Treating
  TRANSFER/COPY/CLEAR as not vertex-side in the driver could remove the largest bucket. This is
  reasoned from the patch, not measured. That change is in the KK patch series,
  which this branch does not touch: it is the next experiment.

### Captures

`tools/macos/barrier_masks_caps.py` compares pixels (`tools/macos/barrier_masks_ab.py` prints the
table above). The capture
moments differ by run, and the scene animates (fog drift, candles, the idle pose), so no pair of
captures is pixel-identical, **legacy against legacy included**. The comparison is therefore against
that noise floor (3440x1440, 8-bit, max channel diff):

| pairs | n | mean abs diff | px > 32 | worst 64x64 tile |
|---|---|---|---|---|
| legacy-legacy | 20 | 9.24 (max 17.01) | 3.03% (max 17.12%) | 59.8 |
| safe-safe | 12 | 11.44 (max 18.83) | 5.59% (max 23.03%) | 64.0 |
| legacy-safe | 40 | 9.83 (max 18.67) | 4.19% (max 19.36%) | 63.3 |

Legacy-safe sits inside the range of same-mode pairs (the largest diff of all is safe3 against
safe4, two runs of the same mode, and legacy3 against legacy5 reaches 17%), so the diffs are capture
timing, not the barriers. Strict captures are not in the table; they were checked visually only. A visual check
of legacy5, safe4, strict1 and strict2 side by side found no missing geometry, black areas, broken
shadows, fog or post effects. As in KK_GPU_WALL.md, still frames cannot rule out a one-frame temporal
hazard. Because safe brings no win, it stays off.

## Verdict

`safe` captures **none** of the 2-2.5 ms barrier ceiling. It is +0.15-0.3 ms/frame slower, with
overlap unchanged at about 1.2%. The default stays `legacy`. The switch, the tracker work (the
new-stage chain, the carried accesses) and the stats line remain as the base for the driver-side
experiment above. Once the driver stops counting transfer destinations as vertex-side, `safe` is the
mode to measure again.

## Update 2026-10-17: patch 0006 measured — the driver-side experiment is closed

The experiment both docs above pointed at: KosmicKrisp patch 0006, `BB_KK_TRANSFER_WAIT`
(branch `fix/kk-transfer`). In `vk` mode with the masks, transfer destinations no longer hold
the next render encoder's vertex stage (`=0`; `=1` reproduces patch 0005's classification).
Series `tools/macos/ab_transfer.sh` in `~/repos/bbport-transfer-build`: 4 alternated pairs
(ttw1/ttw0) + 2 legacy runs, hidden, same standing route, 3440x1440, **zero voids, zero
retries, no foreign `bb-probe` in any run** (a pre-reboot attempt was voided by contention and
was re-run clean).

frame_ms p50 (draws>=500, t_s>=60; all tags 883-890 draws):

| pair | ttw1 (wait on) | ttw0 (patch 0006) | delta (1→0) |
|---|---|---|---|
| 1 | 27.23 | 27.34 | +0.11 |
| 2 | 27.58 | 28.02 | +0.44 |
| 3 | 27.69 | 27.29 | -0.40 |
| 4 | 28.03 | 28.23 | +0.20 |

Mean **-0.09 ms**, signs mixed — no effect. Legacy baselines: 27.01 and one outlier at 29.66
(that run is 2.65 ms above its twin with no foreign probe; the whole series also sits ~+0.6 ms
above the pre-reboot band with a monotonic upward drift — machine state, not the patch; the
alternated pairs cancel drift).

**The mechanism fired in every experiment run** (KK counters): "transfer-only barriers kept
off the vertex stage" reads 0 in all four ttw1 runs and ~445-467k per 10 s in the ttw0 runs;
vertex-side barriers from memory/buffers drop ~720k → ~350k, and render encoders waiting before
their vertex stage fall ~1.04M → 0.91M (per 10 s). None of it moves the frame: the same lesson
as the masks themselves — with passes still overwhelmingly holding their vertex stage, the
overlaps this frees never occur, and the wall is fragment work plus the serialization.

Conclusion: the driver-side barrier line (0005 modes, masks, 0006 transfer classification) is
**measured and closed** — no frame-time win anywhere along it; `legacy` remains the default and
`vk` stays unproven (~0.5-0.9 ms at best). Harness note: one `xctrace record` hung at its 3 s
limit during this series (0 % CPU, frozen trace, 22 min stall — killed; that run's
`mtl-tleg1-gpu.xml` is 0 bytes). The bench scripts have no timeout around xctrace; wrap it in a
watchdog when scheduling long series.

Evidence: `~/repos/bbport-transfer-build/out/ab_transfer2.log`, `transfer_results.json`,
`frames-ttw*.csv`, `run-ttw*.log` (counter blocks), 20 captures.
