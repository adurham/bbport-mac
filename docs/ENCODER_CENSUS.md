# Encoder census: what the 378.7 encoders of one frame are, what any backend needs, and where the idle is

Stage B of `docs/PERF_LOOP.md`. Measured 2026-10-17, analysis only — no run, no build, no source
edit; the GPU was held by another session throughout and every number below comes from artifacts
already on disk. Build: `fix/encoder-merge` @ ab6d408, `out/gpu-arm64/libbbgpu.dylib`;
config 2560x1440 output / `BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4` (scene raster 854x480), warm route
`ROUTE='ly=128,rx=128:5'`, camera still, hidden. Read with `ENCODER_ATTRIBUTION.md` (the prior
attribution — this doc builds on it and does not redo it), `COPY_MERGE_RESULT.md`,
`INPASS_BARRIER_RESULT.md`, `SUBMISSION_SURVEY.md`, `METAL4_PACKING_PROBE.md`.

Marking: **[M]** measured in an artifact, **[D]** derived by arithmetic from [M] numbers,
**[A]** assumption stated inline.

## Headline

1. **378.7 encoders/frame** (232.6 render + 146.1 compute, 6 windows; the 13-window mean is 376.3 =
   231.2 + 145.2, same within window variance) and the identity
   `MTL4 command buffers created == render + compute` holds exactly over the whole run
   (2,680,803 = 1,655,916 + 1,024,887 **[M]**). One command buffer per encoder, nothing hidden
   behind the packaging.
2. **The guest's own pass structure is 138 of them** (138 render blocks, 70 distinct attachment
   sets). **All 45.4 pass breaks together are 45.4 more.** That is 183.4 = 48% of the frame. The
   other **130.9 (49.2 render + 81.7 compute) are not attributable from the artifacts on disk** —
   the residual the prior work left at ~48 + ~80, now bounded and named where possible.
3. **N_min ≈ 307 encoders/frame** (range 300-327) for any single-queue Metal backend on this frame's
   data dependencies: 136 render (138 attachment runs, 2 mergeable once the intervening compute
   floats) + 8 compute + 31 copy-edge encoders + 1 FSR + the unchanged 130.9 residual.
4. **N_vk ≈ 335** (range 325-345) inside bb-gpu + KosmicKrisp with targeted changes; the gap to
   N_min is **28.5 encoders (9.3%)**, so the Vulkan layering is *not* what stands between this frame
   and its floor — but the port's current shape is: **27.8 of the 45.4 breaks are removable under a
   relaxed rule**, and the two gates that exist reached 1.5.
5. **Hazard-genuine breaks: 18.4/frame under the strict rule as implemented, 17.6 under a relaxed
   order-aware rule** (of 45.4). The two counts are close but the *shape* is not: the strict rule
   creates 13.6 new breaks by deferring, which is why `BB_COPY_MERGE`+`BB_INPASS_BARRIERS` measured
   neutral (-0.85 encoders, -0.06 ms) while the relaxed rule has no such cost.
6. **Idle 6.75 ms/frame splits 3.68 CPU-late / 3.07 queued** [M, stored trace]. This *corrects* the
   prior partial estimate (~2.2-2.6 CPU-late / ~3.4 queued): the CPU-late side is the larger half.
7. The CPU side is **bb-gpu recording**, not the guest and not the commit path: the GPU thread
   spends **7.45 ms/frame encoding** (8.42 us/draw x 885 draws **[M]**), blocks 6.9 ms on GPU ticks
   (36.5%), and the guest render thread is *asleep* 16.4 ms/frame on `sceGnmSubmitDone` **[M]**.
   The CPU-late gaps cluster on `Compute->Vertex` and `Fragment->Compute` boundaries, i.e. on the
   recording thread opening the next render encoder.
8. **The ranked reachable merges total 43.2 encoders/frame (11.4%)**, worth **0.4-1.1 ms** at the
   trace's own 9.0-19.8 us per encoder, or **0.8-1.1 ms** at the in-situ 0.03-0.04 ms/break the copy
   experiment measured. Stage C is a ~1 ms line unless the 130.9-encoder residual is opened.

## Method and artifacts

| # | artifact | what it gives | how used here |
|---|---|---|---|
| 1 | `bbport-encmerge-build/out/analyzer/frame_1792277595.txt` (160 blocks) | frame 1: per-block targets, dispatch sizes, draws, shader hashes, sampled/written images | block structure, attachment runs, N_min's render floor, dependency test |
| 2 | `bbport-encmerge-build/out/analyzer2/frame_1792277787.txt` (162 blocks) | frame 2, same route/config | cross-check (138/140 render blocks, 22 compute blocks, 883/889 draws) |
| 3 | `bbport-encmerge-build/out/analyzer/run1.log` | `BB_KK_STATS` cumulative counters + `Frame stats`/`Frame pacing`, every 10 s | the encoder totals and their split; CPU-thread accounting (B5) |
| 4 | `bbport-encmerge-build/out/last-run.log` | `BB_PASS_BREAK_TRACE=1` caller histograms per 5 s window | breaks/frame per caller; addresses resolved with `nm -n -C` against (5) |
| 5 | `bbport-encmerge-build/out/gpu-arm64/libbbgpu.dylib` | the build under test | exact symbol resolution (not `dladdr`'s nearest-export guess) |
| 6 | `bbport-metalfx-build/out/mtl-2560_fsr3_4-gpu.xml` + `tools/macos/mtl_gaps.py` | Metal System Trace of the same output config (183 frames, 19.28 ms/frame, 340.9 encoders) | per-encoder size distribution and the idle split (B5) |
| 7 | `bbport-kk-build/deps-arm64/.../kosmickrisp/vulkan/*.c` | driver source | the legality arguments in B3 |
| 8 | `bbport-encmerge-build/out/census/*` (new, gitignored) | scripts + JSON below | every number here is reproducible from them |

New scripts (all read-only over the artifacts above):

- `out/census/parse_dump.py` — dump grammar -> JSON (`frame_*.json`). Built from
  `gpu/render/vk_frame_capture.cpp` (`Write()` :96-124, `BeginPass()` :189-212, `Dispatch()` :236-247,
  `Sampled()` :264-268).
- `out/census/census_analysis.py` -> `census_dump.json` — attachment runs, boundary causes,
  dependency/floatability test, size classes.
- `out/census/parse_kkstats.py` -> `kk_stats.json` — differencing the cumulative KK counters and
  dividing by each window's own frame count (`fps * 10 s`).
- `out/census/parse_breaks.py` -> `breaks.json` — break histograms per caller, frames from the
  window's own FPS, addresses resolved with `nm`.
- `out/census/mtl_idle_split.py` -> `idle_fsr3_4.json` — `mtl_gaps.py` plus the two
  columns it ignores (`start-latency`, `channel-subtitle`); this is the B5 extension, described there.

Two things the dump cannot see, established from the writer before using it: it coalesces two
consecutive passes with an identical `(colour vector, depth)` into one block
(`vk_frame_capture.cpp:199-212`), so **a pass break inside a run of identical attachments is
invisible**; and it only records work that passes through `FrameCapture::Draw/Dispatch/Sampled`
hooks, so FFX's raw dispatches, port-internal passes (presenter, proxy fills/resolves, blitter) and
meta clears never appear.

## B1 — cause breakdown of one frame's 378.7 encoders

Denominator: `BB_KK_STATS` deltas, 6 steady 10 s windows, 52.7 FPS / 19.02 ms (**[M]**, as in
`ENCODER_ATTRIBUTION.md`). Re-measured here over 13 steady windows for this doc: 376.3 total
(231.2 render + 145.2 compute), i.e. the same number within window variance; the table below uses
378.7 so it reconciles with the prior doc line by line.

| cause | render | compute | total | evidence |
|---|---|---|---|---|
| guest RT change: one encoder per maximal run of identical attachments | 138.0 | — | 138.0 | **[M]** dump, 138 blocks (frame 2: 140) |
| `Runtime::CopyBufferRecord`: the copy's own pass split | 36.0 | — | 36.0 | **[M]** break trace, 8 steady windows |
| compute<->render transition: one encoder per dispatch run | — | 22.0 | 22.0 | **[M]** dump, 22 blocks / 26 dispatches |
| barrier split: `Runtime::FlushBarriers` ends the pass | 3.0 | — | 3.0 | **[M]** break trace |
| HLE copy flush: `ExecuteShaderHLE` | 3.4 | — | 3.4 | **[M]** break trace |
| scene-proxy transition / copy-back: `SceneTargets::Transition` 1.0 + `Runtime::CopyImage` 1.0 + `Runtime::DownloadImage` 1.0 | 3.0 | — | 3.0 | **[M]** break trace |
| the copy's own compute encoder: one `pre_gfx` per copy call (36.0 + 3.4 + 1.0 + 1.0 = 41.4 copies) | — | 41.4 | 41.4 | **[D]** `kk_cmd_copy.c:28` (`cs_get_compute(cmd, true)`) + `cs_end` per pass end, `kk_cmd_buffer.c:511-540` |
| FSR internal: FFX's 8 dispatches in **one** encoder (invisible to the dump — no FFX hash appears in either frame) | — | 1.0 | 1.0 | **[M]** `ENCODER_MERGE_SURVEY.md` Q3 |
| cache-management copy (`TextureCache::RefreshImage`) | 0.01 | — | 0.01 | **[M]** break trace: 0.03/frame run-mean, 0.00 in steady state |
| meta/resolve (`kk_cmd_meta.c:327`) | 0 | 0 | 0 | **[M]** no MSAA attachment in either dump and `encoders ended: render lite 0` (only full render encoders) |
| query reset/results (`kk_query_pool.c:272/286/569`) | 0 | 0 | 0 | **[M]** KK query counters zero in this frame class |
| upload/fill/clear (texture uploads, `vkCmdFillBuffer`, `vkCmdClearColorImage`) | — | in residual | — | not attributable from any artifact on disk |
| **unexplained residual** | **49.2** | **81.7** | **130.9** | **[D]** arithmetic: 232.6-(138+36.0+3.0+3.4+3.0+0.01) = 49.19; 146.1-(22.0+41.4+1.0) = 81.7 |
| **total** | **232.6** | **146.1** | **378.7** | **[M]** KK stats |

### Reconciling 340.9 (trace) with 378.7 (counters)

- The trace is a *different* build line and run (`bbport-metalfx-build`, 183 frames, 19.28 ms/frame).
  Its `Vertex 212.0` and `Fragment 210.7` intervals are two views of the same render encoders (they
  agree to 0.6%), and `Compute 128.7` is the compute encoders: **340.9 = 212.0 render + 128.7
  compute** **[M]**.
- This build: **378.7 = 232.6 render + 146.1 compute** **[M]**. The difference is +9.7% render and
  +13.5% compute — the same configuration at a slightly different frame (883-889 draws here) on a
  build nine hours apart, not a different encoder structure. Its own residual over the same floor is
  212.0 - 138 - 45.4 = 28.6, i.e. the trace is *closer* to the dump's floor than this build is.
- **Which is used for what:** 378.7 for every encoder-count conclusion (it is the same build, run and
  frame class as the dump and the break trace — the census denominator). 340.9 for the per-encoder
  size distribution and the idle split, because the stored trace is the only per-encoder timeline on
  disk and no trace of this build exists. When a per-encoder cost is applied to this build's count,
  the conversion is stated (6.75 ms / 340.9 = 19.8 us per encoder).

### How far the residual can be closed with these artifacts

Closed further than the prior doc:

- **The encoder classes are complete.** `created == render + compute` exactly over 2,680,803 command
  buffers, and `render lite` is 0, so no encoder kind hides outside the two counters. The residual is
  *not* a third class; it is work inside the two counted classes **[M]**.
- **`pre_gfx`-per-segment is bounded.** 18.0 Metal commits/frame (= 3.0 submissions x 6.00 command
  buffers **[M]**, and matching `SUBMISSION_SURVEY.md`), so at most 2 compute encoders per
  VkCommandBuffer per flush = **≤36 encoders/frame** can come from the per-segment pre/post_gfx
  mechanism. The copy term (41.4 **[D]**) already exceeds that, which is consistent: the copies *are*
  most of the pre_gfx population, not an addition to it.
- **The trace cannot split render encoders by kind.** Its schema has `channel-name`
  (Vertex/Fragment/Compute), `event-label` (a per-command-buffer pointer string) and
  `channel-subtitle` — which is **empty on 100% of the 100,915 depth-0 rows** **[M]**. There is no
  encoder-kind or pass-name column, so no further attribution is possible from the trace.

What remains unexplained, and what it must be:

- **49.2 render encoders.** By elimination these are render encoders the capture hooks never see:
  port-internal passes (presenter/blit, FSR composite, `SceneTargets` proxy fills and resolves, the
  blitter), KK meta/clear passes, and any `cs_start_render` the driver performs itself. The dump
  shows 13 no-attachment passes and 21 depth-only passes among its 138, so the port does clear and
  present through the hooks too — the residual is the part that does not.
- **81.7 compute encoders.** The same argument: uploads, fills, clears, `WriteMarker`, query
  plumbing, and pre_gfx encoders opened by anything that is not one of the 41.4 counted copies. The
  count is plausible in magnitude (KK's `cs_get_compute` has 14 call sites; the port's texture cache
  reports 16.3 texture-set misses/frame and 7 host copies/frame **[M]**, both of which can open one).

**The counter that would close it (not implemented, one run).** In `kk_cmd_buffer.c`, keep the two
existing counters but split each by *site*: add `kk_bbstat_add(KK_BBSTAT_ENC_SITE_BASE + s, 1)` at
`cs_start_render` (:335) and `kk_start_compute_encoder` (:419), with `s` set by the caller from a
small enum — for render: `begin_rendering` (`cs_get_render` :403/412), `barrier_split`
(`kk_CmdPipelineBarrier2` :683-685), `resume` (`Scheduler::BeginRendering` path); for compute:
`copy_buffer` (`kk_cmd_copy.c:28`), `copy_image`, `copy_buffer_to_image`, `fill`, `update`, `clear`,
`meta_resolve` (`kk_cmd_meta.c:327`), `dispatch_pre` / `dispatch_post`
(`kk_cmd_dispatch.c`, `kk_dispatch_precomp` `pre_gfx` flag), `query_reset`, `query_copy`,
`write_marker` (:1281), `ts_resolve`. Cost: one counter-only rebuild + one 190 s run at this config
(~6 min of GPU, plus the build) — no behaviour change, but it needs the GPU, so it must be scheduled
after the current series. That one run turns the 130.9 into an exact table.

## B2 — N_min: the floor any backend could reach on this frame's dependencies

Rules applied, explicitly:

- **R1 attachment runs.** One render encoder per maximal run of identical `(colour set, depth)`;
  distinct attachment sets cannot merge — that is the floor. **[M]** Frame 1: 138 blocks, 70 distinct
  sets, 94 blocks with 1 colour target, 8 with 2, 1 with 6, 1 with 7, 34 with none (21 depth-only,
  13 no-attachment), 76 distinct written targets, 132 distinct sampled targets.
- **R2 intervening compute may float.** Two places have `pass(attachments X) -> compute -> pass(X)`:
  blocks **35 -> 36 -> 37** (no attachments on either side) and **60 -> 61 -> 62** (the 64x64 +
  depth pair around the 3-dispatch pyramid block) **[M]**. If the compute block has no dependency
  through those passes it moves and the two render blocks merge: **138 - 2 = 136**.
- **R3 compute coalescing.** Adjacent dispatch runs with no hazard become one encoder. Only **2** of
  the 22 compute blocks are adjacent to another compute block (75->76: `1x1x1` then `15x15x1`;
  83->84: `1x1x16` then the port composite `240x135x1`) **[M]**; the other 18 are separated by render
  blocks. Of the 22, **7 are provably chained**: 6 sample exactly what the preceding pass wrote
  (blocks 14, 18, 22, 26, 30, 76 — the `8x8x1` / `15x15x1` dispatches **[M]**) and 1 more is bound by
  its consumer. The other 15 have no *image-level* dependency either way, so they could coalesce; the
  dump sees no dispatch that writes a **buffer**, so this is an upper bound on floatability, not a
  proof. Floor: **8 (7 chained + 1 bundle)**, ceiling 22.
- **R4 copies fold to pass edges, once per edge.** 41.4 copy calls/frame **[D]**. A copy must execute
  in *some* encoder on a single queue, so the floor is one compute encoder per pass interval that
  hosts at least one copy, not one per copy and not zero. Bounds from the gate arms **[M]**: with
  `BB_COPY_MERGE` 24.8 copies deferred and 13.6 draws ran into them, so waiting copies occupy at
  least 13.6 and at most 24.8 distinct intervals; the 12.0 copies that never defer add their own,
  giving **25.6-36.8 intervals**, best estimate **31**.
- **R5 the rest is held constant.** FSR 1 encoder (its 8 dispatches already share one; FFX's
  barriers collapse to intra-encoder barriers), and the 130.9 unattributable encoders stay in the
  sum because no artifact shows they are removable.

```
N_min = 136 render (R1,R2)
      +   8 compute (R3)
      +  31 copy edges (R4)
      +   1 FSR (R5)
      + 130.9 residual (R5, held constant)
      = 306.9  ->  ~307 encoders/frame      (range 300-327: R3 7-22, R4 25.6-36.8)
```

Per-rule savings against 378.7, counting only what the rule can actually give away (the residual is
held by R5):

- R1+R2: render 232.6 -> 138.0 runs - 2 merges + 49.2 residual = 185.2, i.e. **47.4 saved**
  (45.4 breaks + 2 merges).
- R3: 22 -> 8 = **14.0**; R4: 41.4 -> 31 = **10.4**; R5: 0.
- Total **71.8 = 19.0%** of the frame (378.7 -> 306.9).

Variant worth naming: with a second queue (a Vulkan transfer queue, which KosmicKrisp does not
expose), R4's copy encoders leave the render queue entirely and N_min drops to **~275**. That is not
reachable in this stack and is not used below.

## B3 — N_vk: what is reachable inside bb-gpu + KosmicKrisp

Each candidate with its legality anchor and its recoverable count (against 378.7):

| # | change | encoders recovered | legality argument (file:line that gates it) |
|---|---|---|---|
| C1 | **Copies stop ending the pass**, with the driver routing a mid-pass copy to **post_gfx** | **-32.8** (22.4 render + 10.4 compute) | Today `kk_CmdCopyBuffer2` calls `cs_get_compute(cmd, true)` (`kk_cmd_copy.c:28`) which always takes `pre_gfx`; `cs_end` appends `pre_gfx` **before** `gfx` (`kk_cmd_buffer.c:515-533`), so a copy recorded mid-pass would execute *before* the pass's draws — which is exactly why `Runtime::CopyBuffer` ends the pass (`runtime.cpp:455-495`, `EndRendering()` at :457). Passing `cmd->gfx.encoder == NULL` as the flag routes a mid-pass copy to `post_gfx`, which `cs_end` swaps into the pre_gfx slot only after `gfx` is appended (`:525-528`) — it then executes after the pass. On the port side this needs the relaxed hazard rule (below) and `BB_INPASS_BARRIERS` for the memory barrier the copy asks for (`kk_cmd_buffer.c:673-699`, `:700-707`). |
| C2 | **Memory-only barriers stay in the pass** (built: `BB_INPASS_BARRIERS`) | -3.0 render | `FlushBarriers` ends the pass unconditionally today (`runtime.cpp:317-347`); the driver already records an intra-encoder barrier when one is open (`kk_cmd_buffer.c:673-699`), and `kk_barrier_requires_encoder_split` (:678) does not fire for `TRANSFER_WRITE` sources. Invariant required: every encoder ends ALL->ALL (`kk_stop_encoder`, `:463-500`). |
| C3 | **Scene-proxy resolve / readback batching** | -3.0 render | `SceneTargets::Transition` ends the pass first (`gpu/upscale/vk_scene_resolution.cpp:177`), and `DownloadImage` does the same for a readback (`runtime.cpp:575-600`, `EndRendering` at :578). `SceneTargets::Entry::state` (`dirty`, `CopiedToProxy`) already encodes the ordering, so resolving once per level at the first reader, in one batched encoder, is legal; a readback still needs its own encoder after the pass (it is a copy), which is why only the render side is recovered. |
| C4 | **HLE dispatch flushes go to post_gfx** | -2.4 render | `ExecuteShaderHLE`'s flush (`gpu/render/shader_hle.cpp:110`, fallback :251) ends the pass for a copy-shader dispatch. The driver already has the post_gfx path for dispatches (`kk_dispatch_precomp(..., pre_gfx=false, ...)`, `:1027-1033` via `cs_get_compute` :443-460), so a copy-shader dispatch recorded inside the pass can execute after it. |
| C5 | **Compute coalescing in the driver** | -2.0 compute | Only 2 compute blocks in the whole frame are adjacent to another compute block (75->76, 83->84); `cs_get_compute` already reuses one `pre_gfx` per command buffer, so there is nothing else to merge. `pre_gfx` is a single encoder per CB, closed at every `cs_end` (`kk_cmd_buffer.c:511-540`) — closing it less often would need the submit order reworked, and buys nothing measurable here. |
| C6 | **MSAA split / suspend-resume** (`kk_barrier_requires_encoder_split` `:678`, `kk_meta_resolve_rendering` `kk_cmd_meta.c:327`) | 0 | `samples > 1` never fires at this config (`render full` only, no MSAA attachment in either dump). Hygiene only, as `ENCODER_ATTRIBUTION.md` §5 already concluded. |
| C7 | **Dynamic rendering** | 0 | The port already records `vkCmdBeginRendering`; the floor is the attachment set (R1), and the driver's render encoder is already "full", not "lite". |

```
N_vk = 378.7 - (32.8 + 3.0 + 3.0 + 2.4 + 2.0) = 378.7 - 43.2 = 335.5  ->  ~335
       (range 325-345; the copy interval count in C1, 25.6-36.8, is the soft term)
```

**Measured reality check.** The two gates that exist reached `232.6 -> 231.8` render encoders (-0.85)
and 45.8 -> 44.2 breaks. So C1-C5 are *not* the gates that exist: they need the driver change in C1
plus a relaxed hazard rule, and must be measured, not assumed — the strict gate's +0.47 ms
(`COPY_MERGE_RESULT.md`) is what happens when the rule is wrong.

## Hazard-genuine breaks, under the two rules

The decisive input for Stage C. Base frame: **45.4 breaks/frame** **[M]** (36.0 `CopyBufferRecord`,
3.4 `ExecuteShaderHLE`, 3.0 `FlushBarriers`, 1.0 each `SceneTargets::Transition` / `CopyImage` /
`DownloadImage`, 0.01 resume). The port's own semantics: `IsBufferAccessed` / `AccessBuffer`
(`runtime.cpp:106-152`, `:178-200`) and `DeferredCopyOverlaps` (`runtime.cpp:591-603`), applied by
`Rasterizer::BeginDraw` (`raster_bind.cpp:674/679`).

| | rule A — strict (what `BB_COPY_MERGE` implements) | rule B — relaxed (order-aware) |
|---|---|---|
| refusal condition | any read **or** write of the destination, any read **or** write of the source, any paged/global access, **or a barrier the tracker asks for at the copy's position** (`runtime.cpp:660-695`) | only a **later** command in the same pass touching the copy's bytes (a later reader would miss the bytes, a later writer would be overwritten), plus paged/global access |
| copies that must break | 12.0 **[M]** | 13.6 **[M]** (the draws that run into a waiting copy; the same conflict, observed on the draw side) |
| HLE copies that must break | 3.4 **[M]** | 1.0 **[D]** (same rule; most HLE batches have no later access) |
| image-state breaks (proxy transition, copy-back, readback) | 3.0 **[M]** | 3.0 **[M]** (a layout change and a readback are not copy-deferrable) |
| **hazard-genuine total** | **18.4 / 45.4 (41%)** | **17.6 / 45.4 (39%)** |
| breaks the rule *creates* while deferring | **+13.6 draws +2.0 barriers +6.5 HLE** **[M]** | 0 (no reordering happens, so nothing new conflicts) |
| effective floor the implemented rule reaches | **44.2** **[M]** (measured, `INPASS_BARRIER_RESULT.md`) | **17.6** (already includes the 3.0 image-state breaks; not yet implemented) |

Reading it: the two rules agree on *how many* breaks are genuinely required (18.4 vs 17.6). They
disagree on everything else. Rule A's real cost is not its refusals but that deferring moves the
break onto the next accessing draw, so 24.8 deferrals create 22.1 new breaks and the frame ends where
it started (-1.5 breaks measured). Rule B never reorders anything past an access, so its 27.8
removable breaks stay removed. **Stage C's ceiling is therefore set by rule B, not by rule A.**

Consistency check from the dump: of the 138 pass blocks, **0** sample a target they also write in the
same block (**[M]**) — there is no intra-pass image feedback in this frame, which is what makes rule
B's premise (moving a copy to the pass end changes nothing a later draw sees, unless it touches the
bytes) safe *at image granularity*. 76 of 138 blocks sample something and 45 depend on an earlier
write, so the frame is a dependency chain, not a bag of independent passes.

## B5 — the idle split, named

Method: `out/census/mtl_idle_split.py`, a superset of `tools/macos/mtl_gaps.py` on the same stored
trace. It reproduces `mtl_gaps.py` exactly (100,915 depth-0 intervals, busy 12.53, idle 6.75,
340.9 encoders) and adds the column `mtl_gaps` ignores: `start-latency` ("CPU to GPU Latency", ns
from the CPU commit of the command buffer to the GPU start of the interval). Every idle gap is
classified by the interval that follows it:

- **queued** — `latency >= gap`: the next command buffer was already committed before the previous
  work ended, so the GPU idled with work available. That is Metal's per-encoder drain, a scheduling
  bubble.
- **cpu-late** — `latency < gap`: the next command buffer had not been handed over when the GPU ran
  dry. The recording side is late.

| gap size | gaps/frame | ms/frame | queued ms | cpu-late ms | cpu-late share |
|---|---|---|---|---|---|
| <5 us | 404.5 | 0.37 | 0.37 | 0.01 | 1% |
| 5-20 us | 67.9 | 0.75 | 0.16 | 0.58 | 78% |
| 20-100 us | 56.3 | **2.21** | 0.06 | **2.15** | 97% |
| 0.1-0.5 ms | 4.6 | 1.10 | 0.67 | 0.43 | 39% |
| >0.5 ms | 1.7 | 2.33 | 1.82 | 0.51 | 22% |
| **total** | **535.1** | **6.75** | **3.07** | **3.68** | 55% |

So the split is **3.68 ms/frame CPU-late and 3.07 ms/frame queued** [M] — the CPU-late half is the
larger one, which **corrects** the prior partial estimate (~2.2-2.6 CPU-late, ~3.4 queued). One
qualification: **0.51 ms of the cpu-late figure comes from the `>0.5 ms` bucket at 0.04 gaps/frame**
(a handful of long hitches over the 183 frames, mean 11.7 ms each), so the steady-state cpu-late is
**~3.2 ms/frame** and the hitch tail is the rest. The `20-100 us` bucket alone is 2.15 ms of
cpu-late, and its cpu-late gaps cluster on
`Compute->Vertex` (21.1 + 16.1 + 6.0 = 43.2/frame) and `Fragment->Compute` (21.4/frame): the next
interval is a **render encoder start**, i.e. the recording thread was still encoding the render
encoder/draw state when the previous compute finished. The `>0.5 ms` and `0.1-0.5 ms` buckets (3.43 ms)
are frame/present boundaries and are mostly queued (78%/61%), so they are not starvation.

Naming the CPU side, from `run1.log` (the same 19.0 ms / 52.7 FPS frame class) **[M]**:

| CPU side | number | reading |
|---|---|---|
| **bb-gpu recording** (GPU thread) | **8.42 us/draw x 885 draws = 7.45 ms/frame** (39% of the frame), GPU-thread idle 0.3% | the recording thread is busy essentially the whole frame; this is the pool the 3.68 ms of cpu-late comes out of |
| of which draw preparation | `bb:DrawPrepx7` 137-147% of a core, `bb:DrawRec` 59-60%, `bb:VkRecorderx3` 62-63% | seven prep workers + the recorder threads of the port, i.e. the port's own encoding, not Metal |
| driver submission | 18.0 Metal commits/frame (3.0 `vkQueueSubmit` x 6.00 command buffers), `bb:GpuComm` 39-40% | the commit path is ~0.3-0.5 ms of the frame; not the late side |
| queued/GPU waits | GPU ticks 36.5% of the frame (6.9 ms), host copies 12.2% (2.3 ms) | the thread waits on ticks (frame pacing) and on host copies (12.2%), the two things that also let the GPU run dry when they land between encoders |
| **Rosetta guest** | game process 90% of one core, `GXWorkerx5` 41%, `com.apple.rosetta.exceptionserver` 5%; **guest blocked 16.4 ms/frame on `sceGnmSubmitDone`** | the guest is *asleep* waiting for the previous frame; it is not the late side at this config |

So: **3.68 ms CPU-late = the port's recording thread (draw encode + render-encoder setup), with host
copies and the frame-pacing tick wait as the two secondary contributors; the guest and the commit
path are not late.** Consistent with the port's own counter: 36.5% of the GPU thread's frame is spent
waiting for GPU ticks, and the GPU is idle 6.75 ms of the same frame — the two are the same wait seen
from both sides.

## What this implies for Stage C

Ranked by encoders recoverable, all against rule B (the only rule that keeps what it defers):

| rank | merge | encoders | ~ms at 9.0-19.8 us/encoder | risk |
|---|---|---|---|---|
| 1 | **copies -> post_gfx** (driver `kk_cmd_copy.c:28` routing + relaxed hazard rule + `BB_INPASS_BARRIERS`) | **32.8** (22.4 R + 10.4 C) | 0.3-0.65 | the one real hazard: a copy that lands after a draw that reads its destination. The refusal set is exactly `DeferredCopyOverlaps` (13.6/frame measured); 0 late conflicts in every gate run so far. Must be measured against the same-mode capture noise floor. |
| 2 | in-pass memory barriers (built, default off) | 3.0 | 0.03-0.06 | already implemented and measured safe; fires only once #1 defers copies |
| 3 | scene-proxy resolve + readback batching | 3.0 | 0.03-0.06 | `Entry::state` already tracks the ordering; the readback's encoder stays |
| 4 | HLE dispatch flushes -> post_gfx | 2.4 | 0.02-0.05 | same path as #1 |
| 5 | compute coalescing in the driver | 2.0 | 0.02-0.04 | only 2 boundaries exist |
| | **total** | **43.2 (11.4%)** | **0.4-0.9** (0.8-1.1 at the in-situ 0.03-0.04 ms/break over the 27.8 breaks removed) | |

Three consequences, stated plainly:

1. **The encoder count is not the frame.** 11.4% fewer encoders, ~1 ms. The other 6.75 ms of idle is
   not encoder boundaries — 55% of it is the recording thread being late, which fewer encoders only
   help *indirectly* (each removed encoder also removes one MTL4 command-buffer create + begin + end
   + commit entry: 5.6 us of CPU per encoder in the micro-benchmark, 1.67 ms/300 CBs).
2. **The floor is the guest's pass structure, not the port's breaks.** 138 of 378.7 encoders are the
   guest's attachment sets; 45.4 are breaks; 130.9 are unattributable. Even a perfect merge leaves
   ~307, and Stage C can reach ~335.
3. **The Vulkan layering is not the blocker.** N_vk (335) is 28.5 encoders (9.3%) above N_min (307),
   and what separates them is the copy line (a single-queue copy needs an encoder) and the compute
   floatability the port cannot prove without buffer-dependency tracking — not the render-pass model,
   not the driver's encoder management. Per `PERF_LOOP.md`'s own rule, that points at implementing
   in bb-gpu rather than at a new backend.

## What one frame cannot determine

- **The 130.9-encoder residual** (49.2 render + 81.7 compute). It is 35% of the frame and it is the
  single largest unknown in this census. The counter in B1 would close it in one run; until then
  N_min holds it constant and N_vk cannot claim a single encoder of it.
- **Where the copies actually sit.** The dump cannot place a copy (it has no copy record and it
  coalesces identical-attachment restarts), so R4's 31 is a range 25.6-36.8 estimated from the gate
  arms, and rule B's 13.6 is only observable *through* a gate that defers.
- **Whether 0.03-0.04 ms/break is real.** That number comes from a -0.06 ms delta over 1.5 breaks with
  run-to-run stddev ~0.2 ms; it is an upper bound, not a measurement of the marginal encoder. The
  trace's own per-encoder idle is 19.8 us (9.0 us queued + 10.8 us cpu-late), and the micro-benchmark
  says 35.5 us for a synthetic dependent chain. The three do not agree, and Stage C's size depends on
  which is right.
- **Frame-to-frame variance beyond two frames.** The two captures agree closely (138/140 render
  blocks, 22 compute blocks, 26 dispatches, 883/889 draws, identical 22 compute blocks by shader
  hash), but both are the same standing camera in the Hunter's Dream. A combat frame with particles,
  more shadow casters and screen-space effects was not captured, and phase C's 6x culling cycle may
  scale with it.
- **What the 22 compute blocks actually are.** Shader hashes and group counts only (`64x1x1`,
  `32640x1x1`, `240x135x1`, ...). The identities — culling, depth pyramid, the port's composite — are
  inferred from position and size, not proven.
- **Whether the trace's build differs structurally.** 340.9 vs 378.7 encoders is 10% and is explained
  as build/frame variance, but nothing on disk proves the two runs had the same pass structure.

## B4 — decision (consult verdict)

Consulted Fable 2026-10-17 21:10 CDT with the numbers above (378.7 encoders, N_min 307, N_vk 335,
idle 3.68 CPU-late / 3.07 queued, the 45.4-break anatomy, the neutral gates). Question put: confirm
or refuse the kill, is the 43.2-encoder merge defensible anyway, and what is highest-EV for the
remaining ~2.9 ms.

**Verdict: the encoder line is killed as a frame-time lever, and the kill is *tighter* than the
gate's arithmetic.** Fable priced the queued half directly: 3.07 ms / 340.9 encoders = **9.0 us per
boundary**, so 43.2 encoders = **0.39 ms GPU + 0.24 ms CPU** (5.6 us/encoder of CB create+begin+end+
commit) = **~0.6 ms best case**, not the 1.0-1.2 ms this doc's 0.03 ms/break figure suggested. Even
reaching N_min (71.8 fewer) is ~0.5-1.0 ms. Against a ~2.9 ms gap, that cannot buy the frame.

Fable raised one caveat that would have split the accounting: the 9 us might be a per-*command-buffer*
cost rather than per-encoder, in which case packing encoders into fewer CBs would attack the queued
3.07 ms *and* ~1.9 ms of recorder CPU without touching the pass structure. **That hypothesis is
already closed by measurement** — `METAL4_PACKING_PROBE.md` ran exactly that micro-benchmark (300
dependent encoders as 300 CBs in one commit array / one CB with 300 encoders / 300 individual
commits, identical encoder sequence and barriers): 10.658 / 11.308 / 10.635 ms, i.e. packing is a
no-op and the packed variant is **6% slower**, with the per-encoder cost dead linear (35.5 us at 50,
150 and 300 encoders) and invariant to texture size. So 9 us is the pass transition + drain itself,
not the CB object. The kill holds on both readings **[M]**.

**On (2) — build the merge anyway: no.** The CPU channel is 0.24 ms; it is not a prerequisite for
Stage D, for pipelining, or for anything the earlier Fable review ranked (#6 is dead). The one piece
Fable would keep is the **relaxed order-aware hazard rule** (`Hazard-genuine breaks` table, rule B):
the gate already exists, it reverses rule A's 22.1-created-breaks regression, and it is worth
~0.3-0.4 ms — gated on "under a day *and* shipping with a validator that diffs rule B against rule
A". Not built in this loop; recorded as the only surviving item of the copy line.

**On (3) — the ordering is reversed from what this runbook assumed.** Fable's argument, from our own
numbers:

- The anchor is upscaler-**off** at 16.50 ms. A *free* upscaler therefore lands exactly at budget
  with zero margin; a realistic 1-1.5 ms temporal one misses. **Stage D cannot be the whole answer
  at this config, however well it goes.**
- The only pool big enough to give margin is idle: 6.75 ms against 12.53 ms busy. At ~1 ms idle the
  frame is ~13.5 ms *with FSR as it is today*.
- The structural suspect is that the recorder is **not allowed to run ahead**: recorder 7.45 ms < GPU
  12.53 ms, yet the GPU goes CPU-late 3.68 ms, and the guest is asleep 16.4 ms on `sceGnmSubmitDone`.
  That combination is what a frame-serialized pipeline looks like (recording N gated on completion of
  N-1), not a throughput mismatch. A fully pipelined frame would run at ~max(2.6, 7.45, 12.5) ~ 13 ms,
  not 19.

So the recorded next action is a **measurement, not a build**: per-frame timeline of guest-submit,
recorder start/end, first-CB GPU start, last-CB GPU end, and the overlap of recorder(N) against
GPU(N-1). If recorder(N) starts within ~0.5 ms of GPU(N-1) finishing on every frame, one frame of
run-ahead (SubmitDone returning after recording rather than after GPU completion) is the lever and it
is worth up to the whole 3.68 ms.

What would reverse *that* ranking, per the consult: (a) the timeline shows the recorder already runs a
full frame ahead, in which case per-draw cost is the target (8.42 us/draw is 3-5x a lean Vulkan->Metal
path, with 7 draw-prep workers at 137-147% of a core); (b) hard in-frame GPU->CPU dependencies the
guest consumes (the 3.0 readback encoders) that cannot be made one frame stale; (c) the CB micro-bench
showing 9 us is per-encoder — **already measured**, so the queued 3.07 ms is a floor and only the
CPU-late half is attackable.

**Gate clauses, all three, recorded:**

1. Kill if `(340 - N_min) x 0.03-0.035 ms < 3 ms`: **fires** — 0.4-1.2 ms depending on the per-break
   price, 0.6 ms at the measured 9.0 us/boundary. Encoder line killed as a frame lever.
2. `N_vk ~= N_min` -> Vulkan layering is not the blocker: **335 vs 307, gap 28.5 (9.3%)**, and the gap
   is the single-queue copy encoder (R4) plus compute floatability that needs buffer-dependency
   tracking (R3) — neither is the Vulkan render-pass model. Implement in bb-gpu, do not consider a
   backend. No hybrid HAL earned.
3. Stage C is therefore **not implemented**: the ranked merges buy ~0.6 ms and the loop does not
   spend a build on 0.6 ms when the target is 2.9. The only exception retained is rule B above.

Repositioning of Stage D, recorded so the next session does not misread it: D proceeds because it is
the loop's remaining named lever and the encoder line is capped, but its expected value is **margin +
quality, not the path to 60**, and it must be measured against the 16.50 ms upscaler-off anchor rather
than against FSR3_4 alone.

