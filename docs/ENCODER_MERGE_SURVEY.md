# Encoder-merge investigation notes (read-only source survey)
Repos: /Users/adam.durham/repos/bbport-kk-build (driver KK + port), /Users/adam.durham/repos/bbport-mac-native
(canonical port line; upscale/ files identical). Started 2026-10-17. No builds/runs/git.

## Q1 KosmicKrisp encoder stop/start sites
Driver source: deps-arm64/src/mesa-kosmickrisp/externals/mesa/src/kosmickrisp/vulkan/
Model: three MTL4 command buffers per kk_cmd_buffer: pre_gfx / gfx / post_gfx (kk_cmd_buffer.c:150-154).
Each holds ONE open encoder at a time.

1. kk_stop_encoder (kk_cmd_buffer.c:463) - the only encoder-end: barrier_after_stages(ALL,before) +
   endEncoding + end_command_buffer + append to submit_cmd_bufs. lite drops VERTEX/OBJECTS/MESH (480-491).
2. cs_end (kk_cmd_buffer.c:511) - ends pre_gfx (522), promotes post_gfx (524/529), ends gfx (533).
   Callers: kk_cmd_buffer.c:210/211 (reset), 267/268 (vkEndCommandBuffer), 683 (barrier split), 1285 (marker);
   kk_cmd_draw.c:634 (kk_CmdEndRendering2KHR), 975 (restart); kk_query_pool.c:286, 569.
3. split condition kk_barrier_requires_encoder_split (kk_cmd_buffer.c:558-606) + split at 678-685:
   samples>1 (MSAA ALWAYS splits) OR (write_available && dst texture-read) OR ((ds|storage) write &&
   input-attachment read). Genuine hazard, else.
4. cs_start_render (kk_cmd_buffer.c:335-400): new cmd buffer + render encoder per pass.
   Callers: kk_cmd_draw.c:445 (vkCmdBeginRendering), cs_get_render (403-416) when
   need_to_start_render_pass (draw.c:470 no-framebuffer, draw.c:977 restart).
5. kk_start_compute_encoder (419-440): new cmd buffer + compute encoder. cs_get_compute (443-460) REUSES
   pre_gfx when no gfx encoder is open, else post_gfx. Callers: kk_cmd_copy.c:29,94,124,183,300 (all
   pre_gfx=true -> a copy never needs the render pass to end, in the driver), kk_cmd_dispatch.c:23,83,132,
   kk_cmd_buffer.c:1033 (kk_dispatch_precomp = every libkk_* kernel), kk_cmd_draw.c:1974 (tess),
   kk_query_pool.c:571.
6. kk_flush_render_pass (kk_cmd_draw.c:944-979): same-pass restart on sample-location or
   COLOR_ATTACHMENT_MAP change (store ops applied, cs_end, need_to_start_render_pass=true).
7. kk_meta_begin/end (kk_cmd_meta.c:95,150): no split (kk_bb_vertex_dep only). Meta blit/fill/clear run
   through the common path into a compute encoder. EXCEPT kk_meta_resolve_rendering (kk_cmd_meta.c:327) =
   a real render pass for MSAA resolves (draw.c:644-645).
8. Queries: kk_CmdResetQueryPool (kk_query_pool.c:272, assert no render encoder at 278),
   kk_CmdWriteTimestamp2 (347), kk_CmdEndQuery (435 -> kk_cmd_write), CopyQueryPoolResults (553, cs_end 569).
9. kk_cmd_write (kk_cmd_buffer.c:1063-1068): pre_gfx = !gfx.encoder -> inside a render it opens the
   post_gfx compute encoder (2nd cmd buffer, pass stays open).
10. kk_CmdWriteMarkerToMemoryAMD (1271-1290): cs_end at 1285 when a render is open.
11. Boundaries: kk_BeginCommandBuffer (245, vertex_dep BEGIN), kk_EndCommandBuffer (262).
Stats counters: kk_device.h:160-175 (ENC_RENDER_LITE/FULL, ENC_COMPUTE, MTL_CMD_BUFS_CREATED,
DEP_BEGIN/BARRIER/OTHER/SPLIT, ENC_RENDER_WAITS(_ALL), VTX_FROM_MEMORY/IMAGE).

Per-frame from docs: ~170-186 render encoders ended/frame (62-70k per 10 s at ~373 frames/10 s;
KK_GPU_WALL §3: 186), ~323-348 Metal cmd buffers/frame => ~150 compute cmd buffers. 92 of the 186 render
encoders are <20 us (0.68 ms total) = the split-off tail.
Assessment: sites 1,2,4,5 are not per-command splits (a copy/dispatch stretch shares one encoder);
site 3 genuine except the unconditional MSAA clause; site 6 candidate; site 7 adds a whole extra render
pass per resolved MSAA attachment because the port never sets SUSPENDING/RESUMING (draw.c:638).

## Q2 Port pass-break callers
Mechanism: Scheduler::BeginRendering (render/scheduler.cpp:170-236): if the state being begun equals the
last RenderState and a pass ended -> Scheduler::TracePassBreak (scheduler.cpp:254); caller captured at
scheduler.cpp:246 (__builtin_return_address(0) inside EndRendering, [[gnu::noinline]]).
Names in the trace are the nearest EXPORTED symbol; static callers (MultiCopy, DispatchRecord) resolve to a
neighbouring exported name -> treat names as approximate.
Measured (KK_GPU_WALL §3, 5 s, ~211 frames): 6250/5 s = ~28-30 breaks/frame:
  Runtime::CopyBuffer 4450/5s ~21/f, ExecuteShaderHLE 815/5s ~3.9/f, Runtime::FlushBarriers 230/5s ~1.1/f,
  SceneTargets::Transition / PrepareTransfer / DownloadImage ~177/5s ~0.84/f each (sum 6026 of 6250).

| caller | site | what forces the end | merge? + risk |
| Runtime::CopyBuffer | render/runtime.cpp:209-260, EndRendering 211 | vkCmdCopyBuffer illegal in a render pass; guest DMA_DATA/CP copies (cp/cp_tasks.cpp:235-282 DmaDataTask/CopyDataTask -> raster_memory.cpp:52-75 -> runtime.CopyBuffer:75) and copy-shader HLE (shader_hle.cpp:250) in guest stream order | NO reorder (a draw may read the copy destination); the DRIVER could hold the render encoder open and record the copy in pre_gfx, splitting only if an attachment is touched |
| ExecuteShaderHLE (MultiCopy) | render/shader_hle.cpp:30-145, EndRendering 109 (fallback runtime.CopyBuffer at 250) | same, batched HLE copies (>=8 regions) | same |
| Runtime::FlushBarriers | render/runtime.cpp:141-171, EndRendering 148 | unconditional; a memory-only barrier is LEGAL in a render pass and the driver has an in-pass path (kk_cmd_buffer.c:673-699) | YES merge when image_barriers empty. Risk: dstStageMask=eAllCommands (stage::eAllCommands, runtime.cpp:154-157) so the in-pass path cannot order other command buffers; fine in legacy (every encoder ends ALL->ALL) |
| SceneTargets::Transition | upscale/vk_scene_resolution.cpp:175-190, EndRendering 177; callers Copy 215/233, 480/502/525, EndClear 166 | image layout transition (often a render target/proxy) | genuine if it is a bound attachment; else mergeable. Risk: layout of a bound attachment |
| Runtime::PrepareTransfer | render/runtime.cpp:354-366, EndRendering 356 | two Transits to TransferSrc/Dst, no copy yet | mergeable when neither image is the open pass's attachment. Risk: same |
| Runtime::DownloadImage | render/runtime.cpp:328-352, EndRendering 331 | Transit to TransferSrc + copyImageToBuffer | genuine if it is the current RT; else mergeable. Risk: readback vs pending writes |
| DispatchRecord / DispatchIndirectRecord | render/rasterizer.cpp:656, 713 | guest compute dispatch between draws | could stay in pre/post_gfx. Risk: dispatch writes a later draw's input |
| DrawIndirectRecord (indirect guard) | render/rasterizer.cpp:529 | vk_indirect_guard check is a dispatch outside the pass | genuine; same pass reads the result |
| WriteLabelOnGpu | render/raster_signals.cpp:295 | portable labels are transfer commands | genuine, few/frame |
| attachment feedback loop | render/raster_targets.cpp:444-451 | Transit to FEEDBACK_LOOP/GENERAL of a bound RT | genuine |
| BindTextures | render/raster_textures.cpp:317 | inside BB_UI_TEXTURE_LOG (debug only) | not per-frame |
| SceneTargets::Copy | upscale/vk_scene_resolution.cpp:201-233 | proxy resolve/fill | genuine-ish, ~1/f |

The port never sets VK_RENDERING_SUSPENDING_BIT/RESUMING_BIT: scheduler.cpp:186-236 builds RenderingInfo with
flags=0, emulates resume with loadOp=eLoad (:199) and always storeOp=eStore (:201,214). In KK,
kk_get_attachment_store_op (kk_cmd_buffer.c:1117-1145) then always STOREs, and the restart path forces
MTL_LOAD_ACTION_LOAD (draw.c:449-469) => every pass break = full attachment store + load.
Attachment bytes: not derivable per break from source. G-buffer at 2560x1440 = 6 targets ~88 MB + D32S8 ~29 MB
(docs/upscaler.md:92); one store+load of that set ~235 MB (~0.5-0.6 ms at 400-450 GB/s). Most breaks are
presumably smaller targets - unknown from source.

## Q3 FSR 3.1 chain
Port: upscale/vk_temporal_upscaler.cpp TemporalUpscaler::Run (1215), EndRendering 1243, 2 Transits +
FlushBarriers (1244-1252), FSR dispatch recorded into cmdbuf = scheduler.CommandBuffer() (1328-1394 via
ffxVkPortableUpscaleContextRecordDispatch, third_party/fsr-vulkan/src/ffx_vk_fsr3_portable.cpp:565 ->
ffxFsr3UpscallerContextDispatch :617), port merge/composite dispatch (1475-1480), own_barrier + Transit +
FlushBarriers (1428-1434), ExtraSharpen (940-1040, called 1400: if sharpness>applied -> 1 blit + 1 dispatch).
FFX: upstream/ffx-1.1.4/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp:1142-1160 - 6 aliasable DISCARD
jobs + 1 CLEAR_FLOAT job, then scheduleDispatch for PrepareInputs, LumaPyramid, ShadingChangePyramid,
ShadingChange, PrepareReactivity, LumaInstability, Accumulate(Sharpen), RCAS if sharpening;
DebugView only with FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW => 8 compute dispatches/frame, preset/scale
independent (+1 debug).
Barriers: backends/vk/ffx_vk.cpp flushBarriers (:1291, called :4080 at the top of every executeGpuJobCompute)
=> one vkCmdPipelineBarrier before each of the 8 dispatches, plus per clear/copy job (:4134,:4244,:4262,:4271)
and one at the end of the job list (:2980).
Recording: ONE Vulkan command buffer; every dispatch takes cs_get_compute(cmd,true) (kk_cmd_dispatch.c:83)
which REUSES the open pre_gfx encoder. With no gfx encoder open the FFX barriers emit nothing in legacy
(kk_cmd_buffer.c:614-668 only acts in lite/vk) and the `else if (pre_gfx)` branch (:700-707) makes an
intra-encoder DISPATCH|BLIT barrier.
=> FSR3 adds ~1 extra pre_gfx MTL4 cmd buffer / ~1 compute encoder per frame (2-3 with ExtraSharpen and the
final composite), NOT one per dispatch. Not an encoder-merge candidate; merging further would mean replacing
FFX's flushBarriers with a single memoryBarrier, which its resource-state tracking forbids.

## Q4 Object motion vectors (feasibility note)
Object motion is ALREADY IMPLEMENTED (identical in both repos): gpu/upscale/vk_object_motion.{h,cpp},
gpu/upscale/motion_history.h. The "vertex shader replay with the previous frame's constants"
(docs/upscaler.md:122) was superseded by a position-history scheme (store this frame's clip position,
load the previous frame's, same draw/vertex, by buffer device address).
Existing hooks:
- selection: render/pipeline_cache.cpp:990-1004 (vs has a 16-byte-stride buffer whose size classifies via
  Motion::ClassifyBuffer; BB_OBJECT_MOTION_ALL=1 old broad rule; BB_MOTION_SELECT_LOG=1) -> runtime_info.h:87-121.
- SPIR-V: shader/backend/spirv/spirv_emit_context.h:258-270 (motion_out_cur/prev, motion_in_cur/prev,
  motion_frag_out, VertexMotion()/FragmentMotion()); backend/spirv/emit_spirv_special.cpp:72 EmitVertexMotion
  (atomic u32x4 store + load, FlagStore/FlagLoad, index range), :154 EmitFragmentMotion (previous-current in
  pixels), :189 UI edge stretch; shader/resource.h:248-259 (PushData::MotionParamIndex / motion_param).
- per-draw: render/rasterizer.cpp:259-320 (Moving(), IndexRange(), PrepareDraw -> push_data.motion_param),
  render/raster_targets.cpp:672 (object_motion->Attach into the G-buffer motion attachment).
- camera motion: upscale/vk_camera_motion.cpp/.h - OnConstants (289), OnGBufferPass (377), CommitFrameCamera
  (388), OnDisplayPass (397), RecordMotion (242), Overlay (422), Near (205), VerticalFov (178),
  TaaDepthParameters (227), ObjectMotionView/Image (233/237), jitter phases, RenderSize.
Where it stops: indirect draws, tess/geometry shaders (rasterizer.cpp:506-508 motion_draw=false), MSAA,
non-G-buffer passes, transparency/particles/fog, UI -> camera vectors only; matches keyed by draw order can
break when culling reorders; dynamic vertex buffer address changes lose the match. A true replay (re-running
the VS with last frame's constants) is NOT implemented; the push-constant plumbing to feed it exists, but
nothing re-executes the vertex stage.

## Ranked merge candidates
1. The 28 store/load round trips per frame (EndRendering+BeginRendering with the same RenderState; storeOp
   always eStore, loadOp eLoad on resume, SUSPENDING/RESUMING never set). Risk: an attachment written then
   read in between must still be stored.
2. Runtime::FlushBarriers EndRendering (runtime.cpp:148) for memory-only flushes, ~1.1/frame. Risk: in-pass
   ordering only covers this encoder (needs legacy ALL->ALL for cross-cmd-buf ordering).
3. MSAA unconditional split (kk_cmd_buffer.c:678) + kk_meta_resolve_rendering extra render pass
   (kk_cmd_meta.c:327). Risk: MTLLoadAction semantics for MSAA attachments.
4. PrepareTransfer / DownloadImage / SceneTargets::Transition breaks (~0.84/frame each) when neither image
   is the open pass's attachment. Risk: layout transition of a bound attachment.
5. Guest compute dispatch breaks (rasterizer.cpp:656/713). Risk: dispatch writing a later draw's input.

## Measured 2026-10-17: the probe result, and the refinement

`BB_BREAK_STORE_PROBE=1` (branch `fix/encoder-merge`, commit 7368a49) — gated in
`Scheduler::BeginRendering`, the single funnel every break site passes through: a pass that comes
back after a break (same RenderState re-begun) uses `loadOp=eDontCare` + `storeOp=eDontCare`, and
instances of a RenderState already seen broken also drop `storeOp`. Gate off = byte-identical.
2+2 alternated hidden runs, 2560x1440 upscaler-off (the worktree's ini is 16:9/2560, not 3440 —
both arms identical, so the delta stands; magnitudes are config-dependent):

| arm | p50 (pair 1) | p50 (pair 2) |
|---|---|---|
| base | 23.57 ms | 23.40 ms |
| probe (DONT_CARE) | 25.61 ms | 25.62 ms |
| delta | **+2.04 ms** | **+2.22 ms** |

The gate **did** remove real GPU work — GPU-thread time per draw 6.99 -> 6.50 µs, `kernel_ms`
-2% — yet the frame got ~9% SLOWER, with the extra time appearing entirely in the
present/tick waits (+2.75 ms) and `b_busy` (+2.27 ms) while CPU recording got faster. The frame is
not GPU-memory-bound at this config (kernel ~2.6 ms of a ~23.6 ms frame): **the store/reload round
trips are not the bottleneck, and candidate 1 — as measured — is closed.** (A DONT_CARE load can
also hit a slow path; either way there is no recovery here.)

**Refinement (Fable review, same day):** the probe still emitted two encoders, so it tested the
store/load *bandwidth*, not the encoder boundary itself. The ~6.7 ms/frame of boundary idle is
almost exactly ~340 encoders x ~20 µs — the per-encoder "kick" cost on Apple GPUs (each render
encoder = vertex kick + fragment kick, each compute encoder = a compute kick). Because the driver
is 1 encoder : 1 MTL4CommandBuffer, the traces cannot distinguish per-kick from per-command-buffer
cost; the hardware prior says per-kick. Consequences: repackaging submission objects is likely a
no-op (and `kk_queue_submit` already batches commits per VkCommandBuffer); the only lever on that
6.7 ms is **fewer kicks** — i.e. eliminating breaks/passes outright (why do the ~28 breaks exist,
can each be avoided — port-side, hazard-gated), not how they are packaged.

Revised attack order: (1) the once-per-frame `DownloadImage` drain
(`BB_DOWNLOAD_SYNC_PROBE`, measured next); (2) a Metal 4 micro-benchmark (300 small dependent
encoders: one CB vs 300 CBs in one commit array vs 300 individual commits) to settle whether
packaging matters at all before any driver surgery; (3) break-elimination analysis against the
ranked list above, with its risk rules (never reorder a copy past a draw that reads its
destination or writes its source; an attachment written then read must still be stored;
suspend/resume only for pure command-buffer-boundary splits with an identical attachment set).
