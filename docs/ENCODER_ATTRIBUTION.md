# Encoder attribution: where the ~340 encoders/frame come from, and a ranked cutlist

Measured 2026-10-17 on the MacBook M4 Max (hidden runs), 2560x1440 output / FSR 3.1 preset 4
(`BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4`), a standing-still gameplay frame in the Hunter's Dream area.
Read with `docs/ENCODER_MERGE_SURVEY.md`, `docs/SUBMISSION_SURVEY.md`, `docs/METAL4_PACKING_PROBE.md`.
No source was modified for this survey; the only writes are this file and the run artifacts named below.

Question: `docs/METAL4_PACKING_PROBE.md` closed packaging (300 command buffers in one commit = 300
commits, 10.66 vs 10.64 ms) and showed the only lever left is **fewer encoders**, at ~27-35 us per
encoder boundary. So: **which producers create the encoders of a real frame, and which can go?**

## Method

| step | what | artifact |
|---|---|---|
| 1 | frame analyzer dump of one real gameplay frame, `BB_CAPTURE_TRIGGER` / `BB_CAPTURE_DIR`, warm route (`tools/macos/warm_route.sh 190`, `ROUTE='ly=128,rx=128:5'`), hidden | `out/analyzer/frame_1792277595.txt` (160 blocks), `out/analyzer/run1.log` |
| 2 | same config, second run with `BB_PASS_BREAK_TRACE=1` (the port's break-caller histogram, `gpu/render/scheduler.cpp:289`, capture at `:281`) and a second capture | `out/analyzer2/frame_1792277787.txt` (162 blocks), `out/last-run.log` |
| 3 | the driver's own encoder counters, `BB_KK_STATS=1` (`kk_device.c:91-140`), delta over six 10 s windows | `out/analyzer/run1.log` |
| 4 | stored Metal System Trace, same output config, `mtl_gaps.py` per-encoder distribution | `/Users/adam.durham/repos/bbport-metalfx-build/out/mtl-2560_fsr3_4-gpu.xml` |
| 5 | break caller addresses resolved with `nm -n` against `out/gpu-arm64/libbbgpu.dylib` (exact, not the nearest-exported-symbol guess `dladdr` makes) | this doc |

Repos: `/Users/adam.durham/repos/bbport-encmerge-build` (branch `fix/encoder-merge`, the build under
test), `/Users/adam.durham/repos/bbport-metalfx-build` (trace + tooling). Anchors below are line
numbers **in the encmerge worktree**, which differs slightly from the survey's.

Config actually running (from the run log): output 2560x1440, **scene raster 854x480** (preset 4 = 3x
render scaling), half-res targets 427x240, UI 1920x1080, FSR 3.1 context 854x480 -> 2560x1440. The
dump prints **guest** target sizes (1920x1080 scene / 960x540 half) because `SceneTargets` redirects
those draws to scaled proxies (`gpu/upscale/vk_scene_resolution.cpp:73-110` `SizeDivisor`/`ProxySize`); the GPU
area is 1/9 of what the dump's numbers suggest — which is the point: the boundary cost does not care.

## Headline

1. **378.7 encoders/frame measured** in this config (232.6 render + 146.1 compute, at 52.7 FPS /
   19.02 ms median). The stored trace of the same config says **340.9** (Vertex 212.0, Fragment
   210.7, Compute 128.7) — a different build line, 11% apart; use the trace for the *distribution*
   and the counters for *this* build.
2. **The guest's own pass structure is only 138-162 render passes/frame** (two captured frames:
   138 and 140; 883 and 889 draws). 104 of the 138 are single-draw passes (75%), 44 render into
   targets under 1/64 of 1920x1080. That whole visible structure is the floor: ~160 blocks.
3. **46.5 pass breaks/frame**, of which **81% is `Runtime::CopyBuffer` (36.8-38.0/frame)** — one
   port function accounts for most of the encoders that sit *above* the guest's pass structure.
4. Every break costs a render encoder end + a render encoder restart, and the copy that caused it
   takes a compute encoder: **~2-3 encoders per break ⇒ ~140 encoders/frame (37% of the frame) are
   break overhead, not guest work.** That is the cutlist's target.

## Per-frame encoder accounting

Measured from `BB_KK_STATS` deltas (six 10 s windows, `run1.log`); the identity
`MTL4 command buffers created == encoders ended` held over the whole run (2,680,803 both), which
confirms 1 command buffer per encoder (`SUBMISSION_SURVEY.md` Q5).

| line item | render enc/frame | compute enc/frame | source |
|---|---|---|---|
| guest/port attachment runs seen by the analyzer (frame 1) | 138 | 22 blocks (26 dispatches) | dump |
| ... (frame 2) | 140 | 22 blocks (26 dispatches) | dump |
| pass breaks, each splitting one run in two | +46.5 (measured) | — | break trace |
| FSR 3.1 chain: 8 dispatches in **one** encoder, +1-3 with ExtraSharpen/composite | — | 1-3 | survey Q3 |
| copies that ride each break, one compute encoder per CopyBuffer | — | ~38 | break trace × 1 |
| driver-created: MSAA split (`kk_cmd_buffer.c:678`), meta resolve pass (`kk_cmd_meta.c:327`), query resets (`kk_query_pool.c:272/286`), query results (`:569`) | 0 observed | 0 observed | no MSAA in this title's frame; not separable from the residual |
| **measured total** | **232.6** | **146.1** | `BB_KK_STATS` |
| unexplained residual | ~48 | ~80 | arithmetic, see below |

The residual is honest, not hand-waving: the dump coalesces any two consecutive passes with the same
colour+depth set into one block (`vk_frame_capture.cpp:207-210`), so a break, a
`kk_flush_render_pass` restart (`kk_cmd_draw.c:944-979`), or a recorder segment cut
(~18 `VkCommandBuffer`s/frame) is invisible to it; and the dump only sees work recorded through
`Scheduler`'s hooks, so FFX's raw `vkCmdDispatch`es and the port's meta clears/fills/uploads never
appear as compute blocks. ~48 render + ~80 compute encoders/frame are that class. Separating them
needs one counter per `cs_get_compute`/`cs_start_render` call site in the driver — the recommended
next instrumentation step (§ "What one frame cannot determine").

Cross-check against the trace's size distribution (`mtl_gaps.py`, fsr3_4):

```
Fragment  4.60 ms/frame, 210.7 enc/frame | 0-20us 147.6/1.17ms  20-100us 57.2/2.43ms  100-500us 5.9/1.00ms
Vertex    3.68 ms/frame, 212.0 enc/frame | 0-20us 196.1/1.04ms  20-100us  6.6/0.26ms  100-500us 9.3/2.37ms
Compute   4.30 ms/frame, 128.7 enc/frame | 0-20us  93.3/0.83ms  20-100us 29.7/1.13ms  100-500us 4.7/0.84ms
idle 6.75 ms/frame: cross-submit 340.4/frame 6.71 ms (mean 19.7 us)
```

~148 render encoders (71% of them) and ~93 compute encoders (73%) finish in under 20 us. Total work
in those blocks: 2.0 ms of 12.5 ms busy. The tail is not where the time goes — the *boundaries* are.

## The pass list of one frame (dump frame 1, 160 blocks)

Sizes are guest sizes (see Method); "R" = render pass, "C" = compute block.

| # | phase | blocks | R | C | dispatches | draws | largest target | what it is |
|---|---|---|---|---|---|---|---|---|
| A | #0-#1 | 2 | 2 | 0 | 0 | 2 | 1920x1080 | display copy (previous UI target -> display buffer) + scene depth clear |
| B | #2-#7 | 6 | 6 | 0 | 0 | 54 | 4096x4096 | shadow: 3 cascades x (draws pass + clear pass) |
| C | #8-#31 | 24 | 12 | 12 | 12 | 12 | 64x64 | 6x (cs 64x1x1 -> 64x64 D32 1-draw -> cs 8x8 -> 64x64 RG16F 1-draw) |
| D | #32-#37 | 6 | 4 | 2 | 2 | 223 | 1920x1080 | G-buffer 6 colours + D32S8, 219 draws; 2 big cs (32640 / 46848 groups); 2 no-target 1-draw passes |
| E | #38-#47 | 10 | 9 | 1 | 1 | 10 | 1920x1080 | half-res / linear-depth / mask chain + cs 8192 |
| F | #48-#54 | 7 | 7 | 0 | 0 | 267 | 1920x1080 | light volumes (2x B10G11R11), 260 draws |
| G | #55-#61 | 7 | 6 | 1 | 3 | 6 | 1920x1080 | depth pyramid 960x540/480x270 + cs 3 dispatches |
| H | #62-#77 | 16 | 14 | 2 | 2 | 132 | 1920x1080 | scene colour / forward / effects (RGBA16F ping-pong) + 3 cs |
| I | #78-#84 | 7 | 5 | 2 | 2 | 65 | 960x540 | half-res resolves + the port's composite cs 9a9cf8a9 (240x135) |
| J | #85-#125 | 41 | 41 | 0 | 0 | 43 | 1920x1080 | post/bloom pyramid: 41 fullscreen 1-2 draw passes, 1920x1080 -> 7x6 |
| K | #126-#135 | 10 | 9 | 1 | 1 | 14 | 1920x1080 | combine / tonemap ping-pong (1920x1080, 960x540) |
| L | #136-#157 | 22 | 22 | 0 | 0 | 54 | 1920x1080 | UI: stencil ping-pong, 1-2 draws per pass |
| M | #158-#159 | 2 | 1 | 1 | 3 | 1 | - | final pass + cs 306ce628 (3 dispatches) |
| | **total** | **160** | **138** | **22** | **26** | **883** | | 883 draws vs the log's 885-886: same frame |

Shape of the frame: **104 single-draw render passes holding 5,203 indices in total** (fullscreen
triangles and quads), **44 passes into targets under 1/64 of the frame**, and one 219-draw G-buffer
pass plus one 260-draw light pass that carry the actual scene. Phases C, J, K, L are 99 render passes
(72%) for 155 draws — this is the ~92 encoders under 20 us that the survey identified, now named.

## Where the break overhead comes from (measured, not surveyed)

`BB_PASS_BREAK_TRACE=1`, six consecutive 5 s windows, 44.7-46.8 breaks/frame; addresses resolved
against `libbbgpu.dylib` with `nm -n` (the survey's `dladdr` names were approximate; these are exact):

| caller | breaks/frame | share | site |
|---|---|---|---|
| `Vulkan::Runtime::CopyBuffer` | 36.8-38.0 | 81% | `gpu/render/runtime.cpp:455`, `EndRendering()` at **:457** |
| `Vulkan::Runtime::FlushBarriers` | 3.0 | 6.5% | `gpu/render/runtime.cpp:317`, `EndRendering()` at **:327** |
| `Vulkan::ExecuteShaderHLE` | 2.3-3.4 | 5.8% | `gpu/render/shader_hle.cpp:110` |
| `Vulkan::SceneTargets::Transition` | 1.0 | 2.2% | `gpu/upscale/vk_scene_resolution.cpp:177` |
| `Vulkan::Runtime::CopyImage` | 1.0 | 2.2% | `gpu/render/runtime.cpp:621` (break inside the copy-back path) |
| `Vulkan::Runtime::DownloadImage` | 1.0 | 2.2% | `gpu/render/runtime.cpp:575`, `EndRendering()` at **:578** |
| `Vulkan::Scheduler::BeginRendering` | 0.1-0.3 | <1% | resume path, `scheduler.cpp:170-236` |
| **total** | **46.5** | 100% | |

Who calls `Runtime::CopyBuffer` 37 times a frame: the guest's own DMA_DATA / CP copy tasks
(`gpu/cp/cp_tasks.cpp:235` `DmaDataTask`, `:268` `CopyDataTask` -> `Rasterizer::CopyBuffer`
`gpu/render/raster_memory.cpp:52`, `runtime.CopyBuffer` at `:75`; issued in guest stream order from
`cp/cp_graphics.cpp:441`, `cp/cp_compute.cpp:257`) and the batched copy-shader HLE
(`shader_hle.cpp:110`, fallback at `:251`). None of these are port-invented copies — they are the
game's own buffer traffic, and the port ends the render pass for each one because
`vkCmdCopyBuffer` is illegal inside `vkCmdBeginRendering`.

## The cutlist

Ranked by encoders/frame recoverable. "saved" counts the encoders that disappear, at the measured
27-35 us per encoder (Metal 4 probe; the trace's mean boundary is 19.7 us — take the range).

### 1. Buffer copies end the render pass — `Runtime::CopyBuffer` (36.8-38.0 breaks/frame)

- **Producer:** `gpu/render/runtime.cpp:457` (`EndRendering()` at the top of `CopyBuffer`,
  `:455-495`), reached from `gpu/render/raster_memory.cpp:75` <- `cp/cp_tasks.cpp:235/268`.
- **Estimated saving:** ~2 render encoders per break = **74-76 encoders/frame**; up to ~110 if the
  copy's own compute encoder is folded into an already-open compute encoder. 20-29% of the frame,
  i.e. **2.0-3.9 ms/frame** at 27-35 us. This single function is the largest lever in the port.
- **Hazard rule:** *never reorder a copy past a draw that reads its destination or writes its
  source.* In KosmicKrisp a copy recorded while a render encoder is open goes to **post_gfx**, i.e.
  it executes after the whole pass (`kk_cmd_buffer.c:443-460`, `cs_get_compute`), so merging is legal
  **only if no draw between the copy and the pass end touches the destination range** — and, for the
  source, only if the pass's later draws do not re-read a range the copy is about to overwrite. The
  port already tracks exactly this: `Runtime::IsBufferAccessed` / `AccessBuffer`
  (`runtime.cpp:106-152`).
- **Sketch:** in `CopyBuffer`, if a pass is open *and* no pending draw accesses the destination, push
  the copy onto a per-pass batch (`std::vector<vk::BufferCopy>` + src/dst pair) and emit it in one
  `vkCmdCopyBuffer` from `EndRendering`'s tail; on any access hit, flush the batch and break as
  today. Gate with `BB_COPY_MERGE=1`, default off, and prove it with the frame-difference check the
  port already has.

### 2. HLE copies — `ExecuteShaderHLE` (2.3-3.4 breaks/frame)

- **Producer:** `gpu/render/shader_hle.cpp:110` (batched copy shader, >=8 regions), fallback to
  `Runtime::CopyBuffer` at `:251`.
- **Estimated saving:** 5-7 encoders/frame on its own; it shares fix #1 (once `CopyBuffer` defers, the
  HLE path inherits it) so there is nothing extra to build.
- **Hazard rule:** same as #1 (these are the same copies, batched).

### 3. Memory-only barriers end the render pass — `Runtime::FlushBarriers` (3.0 breaks/frame)

- **Producer:** `gpu/render/runtime.cpp:327` — unconditional `EndRendering()` even when
  `image_barriers` is empty and only a global memory barrier is pending (`:318-325`).
- **Estimated saving:** 6-9 encoders/frame (0.2-0.3 ms).
- **Hazard rule:** a `vkCmdPipelineBarrier` with no image barriers is **legal inside a render pass**;
  the driver already has an in-pass path that turns it into an intra-encoder `DISPATCH|BLIT` barrier
  (`kk_cmd_buffer.c:673-699`, `:700-707`). The catch is ordering: the port's barrier uses
  `dstStageMask = eAllCommands` (`runtime.cpp:329-334`) and an in-pass barrier cannot order other
  command buffers — which is fine only because in this legacy model every encoder ends ALL->ALL. Keep
  that invariant, or fall back to breaking when the pending writes came from another command buffer.
- **Sketch:** skip `EndRendering()` when `image_barriers.empty()`; let the driver emit the in-pass
  memory barrier.

### 4. Scene-proxy transitions and resolves — `SceneTargets` (3.0 breaks/frame)

- **Producers:** `gpu/upscale/vk_scene_resolution.cpp:177` (`Transition`, `EndRendering` first),
  `:192` (`Copy`, proxy fill/resolve), `runtime.cpp:578` (`DownloadImage`).
- **Estimated saving:** ~6-9 encoders/frame; also removes the per-pass proxy resolve that makes the
  scaled config's copy traffic (`scene proxies sampled directly: 325k bindings` in the run log).
- **Hazard rule:** resolving a proxy is a copy from/to a *bound attachment*: it must happen after the
  last write and before the first read of that level, and an attachment written then read must still
  be stored. `SceneTargets::Entry::state` already encodes exactly this (`dirty`, `CopiedToProxy`);
  the merge is "resolve once per level per frame at the first reader", not per pass.
- **Sketch:** make `NativeAccess` / `ResolveAll` batch all dirty levels into one compute encoder at
  pass end instead of one `EndRendering`+copy per level.

### 5. Driver hygiene, no measured saving here — MSAA and meta resolves

- `kk_barrier_requires_encoder_split` splits **unconditionally** on `samples > 1`
  (`kk_cmd_buffer.c:678`), and `kk_meta_resolve_rendering` (`kk_cmd_meta.c:327`) opens a whole extra
  render pass per resolved MSAA attachment because the port never sets SUSPENDING/RESUMING
  (`scheduler.cpp:169-236`, `flags = 0`).
- **Estimated saving: 0 in this title's frame** — no MSAA resolve is visible in either dump and the
  run log shows none, so this clause never fires at this config. Keep it as hygiene: gate the split on
  "the barrier actually touches the MSAA attachment" and implement suspend/resume so a resolve does
  not need a second pass.

### 6. Not candidates (accounted, deliberately excluded)

- **The guest's own pass structure** (138-162 runs): phases C, J, K, L are 99 passes with distinct
  attachments; Metal cannot merge different attachment sets into one encoder, and reordering guest
  passes is outside the hazard rules. This is the floor, and at 340-380 encoders/frame the floor is
  about 40-47% of the count.
- **The FSR 3.1 chain:** 8 dispatches recorded into **one** compute encoder (survey Q3); the FFX
  barriers already collapse to intra-encoder barriers. Merging further would mean replacing FFX's
  per-dispatch `flushBarriers` with a single memory barrier, which its resource-state tracking
  forbids.
- **Packaging / submission shape:** closed by `METAL4_PACKING_PROBE.md` (no-op) and
  `SUBMISSION_SURVEY.md` (3 submits/frame, ~18 `VkCommandBuffer`s).
- **The 6x 64x64 culling cycle** (phase C: 12 render + 12 compute = 24 encoders, 6.3%): guest
  dispatches interleaved with 64x64 passes; every block has a different target, so there is no legal
  merge port-side. Worth revisiting only if the guest's own code is ever patched.

Sum of the actionable items: **~90-135 encoders/frame (24-36%)**, i.e. roughly **2.4-4.7 ms/frame**
at the measured per-encoder cost, before counting the attachment store/load each removed break also
saves. Items 1 and 2 alone are ~85% of that.

## What one frame cannot determine

- **The residual ~48 render and ~80 compute encoders/frame** (§ accounting). The dump cannot see a
  break (it coalesces same-target passes), FFX's raw dispatches, meta clears/fills, uploads, query
  resets, or per-segment `pre_gfx` encoders. One counter per `cs_start_render` /
  `cs_get_compute` call site (`kk_cmd_buffer.c:335`, `:419`, callers at `:443-460`) would close this
  in a single run; that is the next instrumentation step, and it is a counter, not a behaviour change.
- **Which of the 46.5 breaks are hazard-genuine.** The break trace gives the caller, not whether the
  copy's destination was later read by a draw in the same pass. #1's gate needs that access data at
  runtime (the port has it, `IsBufferAccessed`); without it the saving estimate is an upper bound.
- **Per-encoder cost at this config.** The 27-35 us figure is the 300-encoder micro-benchmark at its
  size; the trace's own boundary mean is 19.7 us and its distribution is bimodal (queued vs
  CPU-late). The savings above are quoted as a range for that reason.
- **Frame-to-frame variance beyond two frames.** Two captures agree closely (138/140 render passes,
  22 compute blocks, 26 dispatches, 883/889 draws), but both are standing still in one area; a
  combat frame with particles and extra shadow casters was not captured.
- **The identity of phase C and the 392x331 chain.** The dump gives targets, sizes and shader hashes,
  not semantics; C is "6 identical 4-block cycles next to the shadows" and J is "a 41-pass pyramid",
  nothing more.
- **Whether `Runtime::CopyImage`'s 1.0 break/frame is really that function.** Its return address is
  +0x2f80 into a heavily inlined body, so the symbol is right but the exact call path (proxy
  copy-back vs. guest image copy) is inferred, not proven.

## Artifacts

- `bbport-encmerge-build/out/analyzer/frame_1792277595.txt` — frame 1 (160 blocks), with constants.
- `bbport-encmerge-build/out/analyzer2/frame_1792277787.txt` — frame 2 (162 blocks).
- `bbport-encmerge-build/out/analyzer/run1.log` — run 1 (capture + `BB_KK_STATS`), 52.7 FPS,
  median 19.02 ms.
- `bbport-encmerge-build/out/last-run.log` — run 2 (`BB_PASS_BREAK_TRACE=1`), the break histograms
  quoted above.
- `bbport-metalfx-build/out/mtl-2560_fsr3_4-gpu.xml` + `tools/macos/mtl_gaps.py` — the per-encoder
  size distribution.
