# The GPU wall on KosmicKrisp: barriers, serialization, pass count (M4 Max, 1440p 16:9)

Branch fix/kk-gpu-wall, 2026-10-17. Every number here comes from hidden warm-route runs of this checkout:
`tools/macos/gpu_ab_run.sh`, the same route, the owner's ini (16:9 2560x1440, exclusive fullscreen, uncapped,
preset 5, upscaler off). A watcher logged any bb-probe from another checkout during each run, and none appeared.
Frames are compared by draw-count bucket (`tools/macos/ab_compare.py`). Run-to-run noise in the same bucket is
about 0.3-1 ms. One run (legacy2) never got past the title screen. It is void and not quoted.

## How to build and swap the driver (KosmicKrisp)

- bb-gpu loads the driver from `<checkout>/deps-arm64/lib/kosmickrisp/` (remote_front.cpp sets
  `VK_DRIVER_FILES=<deps-arm64>/lib/kosmickrisp/kosmickrisp_mesa_icd.json`; `BB_GPU_DEPS` overrides it).
  The x86-64 game process never loads the driver in native mode.
- Source: `deps-arm64/src/mesa-kosmickrisp` is shadexternals/mesa-kosmickrisp at `KK_REV`
  3af112680499cc5eaf519007404a7366679e1c7f (setup_deps.sh). Its `externals/mesa` submodule is at dc41592
  ("kk: Add sparse buffer binding"), with `tools/macos/kosmickrisp-patches/*.patch` applied in order.
  The `.bbport-patches` stamp (shasum of the patch series) makes `setup_deps.sh` rebuild the driver
  whenever the series changes.
- Full rebuild: `BB_ARCH=arm64 bash tools/macos/setup_deps.sh`. This resets the submodule, applies
  0001-0005, configures, builds and copies the dylib and ICD into `deps-arm64/lib/kosmickrisp/`.
- Fast iteration on a patch (about 2 minutes, incremental):
  ```
  P=$PWD/deps-arm64; kk=$P/src/mesa-kosmickrisp
  # edit $kk/externals/mesa/src/kosmickrisp/...
  env -u PKG_CONFIG_LIBDIR PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:$(brew --prefix spirv-llvm-translator)/lib/pkgconfig" \
      PATH="$P/pyenv/bin:$PATH" cmake -S $kk -B $kk/build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0   # once
  env ...same... nice -n 19 cmake --build $kk/build-arm64 -j 10
  cp $kk/build-arm64/outputs/libvulkan_kosmickrisp.dylib $P/lib/kosmickrisp/
  (cd $kk/externals/mesa && git diff <base>) > tools/macos/kosmickrisp-patches/000N-....patch
  ```
  A build tree copied from another worktree carries that worktree's absolute paths in CMakeCache.txt.
  Delete `build-arm64` first.
- To swap back, keep the previous dylib next to the new one (`libvulkan_kosmickrisp.dylib.orig` in this
  checkout) and copy it over. The app bundle (package.sh) copies whatever sits in deps-arm64 at
  packaging time.

## 1. The end-of-encoder barrier (patch 0005, `BB_KK_BARRIER`)

### What the barrier protects

KosmicKrisp records each Metal 4 encoder in its own MTL4CommandBuffer. `kk_stop_encoder` ends every
encoder with `barrierAfterStages(ALL) beforeQueueStages(ALL)`, a queue-scoped producer barrier: every
stage of every later encoder waits for every stage of this one. This is not a blanket workaround. It is
the only cross-encoder synchronisation the driver has:

- `vkCmdPipelineBarrier2` emits nothing when no encoder is open. Inside a render pass it either splits
  the pass or emits an intra-encoder vertex->fragment barrier.
- `vkCmdWaitEvents2` is a no-op.
- KosmicKrisp's own hidden work (indirect, tessellation and geometry-shader unroll compute in the
  pre-gfx encoder, query resets and copies, `kk_cmd_write`, meta blit/resolve/clear, restarts of the
  same render pass for a store/load round trip) has no other ordering.
- Metal 4 adds no synchronisation between command buffers, so the barrier is also what orders the
  game's submissions against each other.

The diagnostic mode `BB_KK_BARRIER=none` drops the barrier on render encoders. It renders a **black
world** with only the HUD visible (`out/cap-none1-1.png`), so the hazards are real.

### Modes (default `legacy` = upstream behaviour, unchanged)

| BB_KK_BARRIER | render encoder end | safety |
|---|---|---|
| legacy (default) | ALL -> ALL | upstream |
| lite | ALL -> ALL except vertex/object/mesh; any Vulkan barrier or command a vertex stage may depend on makes the next render encoder wait for all prior queue work before its vertex stage, and end with the full barrier | conservative |
| vk | no producer barrier. Each Vulkan barrier becomes a consumer barrier at the next encoder: all prior queue work before the destination stages (fragment-side destinations hold only fragment/tile/dispatch/blit work; anything else holds the whole pass). Compute/blit encoders wait on all prior work and end full. Command buffer start, meta ops, queries, events, markers and same-pass restarts wait on everything | follows the Vulkan contract: two passes with no barrier between them may overlap |
| nodep | lite without the vertex waits | diagnostic, UNSAFE |
| none | no barrier on render encoders | diagnostic, UNSAFE (black world) |

`BB_KK_STATS=1` prints, every 10 s, the encoders ended lite or full and the dependency counts by source.

### Measured (frame_ms p50 per draw bucket, ms)

| run | 650-750 | 750-850 | 850-950 | 950-1300 | GPU >=2 channels active (share of busy) |
|---|---|---|---|---|---|
| legacy1 | 21.39 | 22.18 | 24.09 | 26.12 | 0.3% |
| legacy3 | 21.10 | 22.21 | 23.95 | 24.04 | - |
| lite1 | 21.33 | 22.46 | 24.06 | - | 1.6% |
| vk1 | 20.51 | 21.97 | 23.20 | - | 1.5% |
| vk2 | 20.80 | 21.76 | 23.57 | 24.70 | - |
| vk3 | 20.78 (n=69) | 22.30 | 23.28 | 24.88 | - |
| nodep1 (unsafe) | 21.32 | 22.18 | 23.53 | 24.24 | 5.8% |
| none1 (unsafe, black) | 19.02 | 20.62 | 21.55 | 23.37 | 19.9% |

- **Ceiling of the whole lever:** `none` saves about 2-2.5 ms/frame (10%), and that run draws a black world.
  Even with no barrier at all on render encoders, only 20% of busy GPU time has two channels active. So
  the barrier is not the whole story: fragment work alone is 14 ms/frame.
- **lite:** no change. 77% of render encoders still end full, because 950k of about 1.4M passes had a
  vertex-side dependency.
- **vk:** about -0.5 to -0.9 ms in the 850-950 bucket on all three runs (23.20/23.57/23.28 vs
  24.09/23.95). Same sign every time, but at the edge of the noise. **Not a proven win.** The counters
  explain the small effect: 1.16M of 1.20M render encoders wait for all prior work before their vertex
  stage, because the port's barriers name vertex-side destinations (next section).
- **Captures:** the port's own capture (Ctrl+Shift+C path) at the same route point for legacy1, lite1, vk1
  and nodep1 (`out/cap-*-1.png`, `-2.png`) shows the same scene: graveyard, stairs, fog, HUD. A model
  description found no corruption in legacy, lite and vk. **This does not prove visual correctness.**
  A hazard in this class shows up as a flicker, a one-frame stale shadow or SSAO, or a sometimes-wrong
  post effect, and a still frame cannot rule those out. The owner's eyes, on a visible run in busy areas
  and combat, are the gate before `vk` or `lite` becomes anyone's default.

### Why the port defeats the narrower barriers (the next lever, on the port side, not done here)

`Runtime::FlushBarriers` (render/runtime.cpp) emits its global memory barrier with
`dstStageMask = eAllCommands`. Sampled-texture transitions (`Rasterizer::TransitSampled` and
`BindTextures`) also use `eAllCommands`. Under Vulkan semantics this means "every later stage waits",
so any driver honouring the barrier has to hold the next pass's vertex stage. Counters from vk2:
1.24M vertex-side barriers come from memory/buffer barriers and 1.11M from image barriers. Narrowing
these to the stage that actually consumes the data (fragment for fragment-only samples, from the
translated shader's real usage, not descriptor-set-layout flags) is what would let `vk` overlap passes.
The reference model's review flagged the trap: when a reader in a new stage needs no layout change,
the port must still emit an execution barrier from the last writer. It must not just OR the stage into
the tracked state, or vertex-texture-fetch reads race. This is a careful change across the barrier
tracker, and it was not started on this branch.

## 2. Why only one GPU channel is active

- It is the barrier, not the submission pattern. `none1` has the same submissions (about 60
  vkQueueSubmit/frame from 3 guest submissions, ~320 Metal command buffers and ~520 GPU submissions per
  frame) and reaches 20% overlap. With the barrier, overlap is 0.3%.
- Fewer, larger submissions: the batching knob that exists, `BB_HONEST_FLUSH_US` (default 2000 µs), was
  tested at 500 (more, smaller submissions): no frame-time change (23.74 vs 24.09/23.95 at 850-950), and
  idle stayed about 5.4 ms/frame. An earlier measurement in docs/CHANGES_after_0.5.md found 4000-8000
  (fewer, larger) worse. Bigger batches cannot create overlap that the barrier forbids. There is no
  cheap structural fix in the port's submission pattern.

## 3. Pass count and idle gaps (`tools/macos/mtl_gaps.py`)

legacy1 trace, 151 frames, 23.7 ms/frame:

- GPU busy 18.7 ms/frame (79%). Fragment 13.7 ms over 186 encoders/frame: 92 are under 20 µs (0.68 ms in
  total), 44 take 0.1-0.5 ms (9.65 ms). Vertex 2.2 ms, compute 2.8 ms.
- Idle 5.0 ms/frame. Every gap sits between Metal command buffers (cross-submit, 4.90 ms). Gaps inside a
  command buffer total 0.08 ms. By size and cause (cpu-late = the next work reached the GPU during the
  gap, so the GPU was starved; queued = the work was already queued):
  - about 420 gaps under 5 µs at encoder boundaries: 0.5 ms/frame (the encoder-per-command-buffer
    structure plus the barrier drain)
  - 5 µs-0.5 ms: 2.1 ms/frame, about 60% cpu-late (the recording side was late)
  - over 0.5 ms: 2.3 ms/frame, about 2 per frame: frame-boundary gaps (present/acquire and the HUD's own
    command buffer) and waits after the last submission of a frame
- Pass breaks (`BB_PASS_BREAK_TRACE=1`): the same render pass begun again about 6,250 times in 5 s (about
  28/frame). The callers are `Runtime::CopyBuffer` (4,450/5 s: DMA_DATA and CP copies in cp_tasks.cpp,
  copy-shader HLE), `ExecuteShaderHLE` (815), `FlushBarriers` (230), `SceneTargets::Transition`,
  `PrepareTransfer` and `DownloadImage` (about 177 each). Each of these is a transfer in the middle of
  a pass (Vulkan forbids copies inside a render pass), so the pass has to end. Merging would mean
  moving the guest's copies ahead of the pass. The GNM translation keeps guest command order and has no
  such merge point; reordering a guest DMA past draws that may read its destination is a correctness
  risk. **Left as is.**

## What the numbers do NOT prove

- The `vk` gain (about 0.5-0.9 ms) is within about 2x the noise. Three vk runs against two clean legacy
  runs agree in sign, but no run alternated A/B/A/B on a fixed camera.
- The captures show no gross corruption in one still frame per mode. They say nothing about temporal
  hazards (flicker, stale buffers), combat, or other areas. The owner's eyes decide.
- The `none` ceiling (2-2.5 ms) is an upper bound for removing the end-of-encoder barrier, measured with
  broken rendering. A correct scheme gets less. Most of the frame is fragment work itself
  (13.7-14.4 ms/frame), and only resolution or scene scale (PERF_STEADY_STATE.md: 720p = 14.4 ms)
  moves that.
- The futex drain (BB_PIPE_DRAIN_YIELD) and the frame-log probe columns are untouched (port code is
  unchanged on this branch).
