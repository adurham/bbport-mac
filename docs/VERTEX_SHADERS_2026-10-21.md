# Do the scene G-buffer vertex shaders do work their draws do not need?
(2026-10-21, branch `bench/vertex-shaders`, base `bench/vertex-passes` @ e239858)

## Answer

**No, not the guest's shaders.** Measured one shader at a time, the per-vertex cost is about the
same for all of them (0.15-0.5 ns per index-vertex, with ±0.2 ms of noise in one trace), and the
shaders do not run a weighted skinning palette on static meshes. **A static-geometry vertex variant
is a NO-GO.** What the same series did find is that **the port's own object-motion vertex epilogue
costs ~1.1 ms of the ~3.5 ms vertex block** (`BB_OBJECT_MOTION=0`: vertex 3.49/3.66 -> 2.42 ms/frame,
flat p50 19.2 -> 17.64, one run). That is the next candidate (below).

## Half 1: per-shader cost, one `BB_SKIP_VS` hash at a time

Runs: `tools/macos/vertex_ceiling.sh` (420 s hidden standing route, 3440x1440, fsr3 preset 4,
`frame_generation=0`, sandbox user dir, no VRAM limit). `skiptrace-*` runs now arm `BB_PASS_TRACE`
together with the 3 s Metal trace (SIGINT finalisation), so **the index-vertices each skip removes are
exact**: control pass trace minus the skip run's pass trace. Before, a multi-shader encoder's indices
had to be split evenly. Vertex ms/frame comes from `mtl_encoders.py`. The delta is computed two ways:
(a) total vertex over the trace's frames, and (b) vertex us per render encoder x the control's
encoders per frame. Control = mean of `trace-1` (3.657) and `trace-t200` (3.485) = **3.571
ms/frame**, the same window of the run (trace 200 s into gameplay, i.e. the late phase).
Tool: `tools/macos/vertex_per_shader.py`.

| VS | removed draws/fr | removed M idx/fr | skip vertex ms/fr | delta ms (a / b) | **ns / index-vertex (a / b)** |
|---|---|---|---|---|---|
| 9da6aacb | 60.7 | 2.908 (split est.) | 2.769 | 0.80 / 0.88 | **0.28 / 0.30** |
| 9da6aacb (repeat, exact count) | 105.0 | 3.972 | 2.985 | 0.59 / 0.60 | **0.15 / 0.15** |
| 6a833138 | 31.4 | 2.270 | 2.797 | 0.77 / 0.65 | **0.34 / 0.29** |
| 3eaa5555 | 12.8 | 2.019 | 2.682 | 0.89 / 0.92 | **0.44 / 0.45** |
| 3eaa5555 (flat, vs `trace-f80` 3.379) | 14.3 | 2.019 | 2.838 | 0.54 / 0.70 | **0.27 / 0.35** |
| c829ee58 | 5.8 | 0.274 | 3.262 | 0.31 / 0.30 | 1.1 (removed geometry under the noise) |
| 63220f67 | 1.8 | 0.005 | 3.352 | 0.22 / 0.19 | n/a: 5 k indices; the delta is noise |
| 3deaa573 (late) | 28.4 | 0.297 | 3.343 | 0.23 / 0.26 | 0.77 / 0.88 |
| 3deaa573 (flat, vs `trace-f80`) | 28.8 | 0.300 | 3.289 | 0.09 / 0.02 | 0.30 / 0.07 |

The noise floor comes from the table itself: 63220f67 removes 0.005 M indices and still "saves"
0.19-0.22 ms, and 3deaa573 reads 0.77 late and 0.07-0.30 flat. **One 3 s trace has about ±0.2 ms
of noise on vertex.**

**Uniformity verdict: uniform.** The three shaders carrying 85% of the removable geometry
(9da6aacb, 6a833138, 3eaa5555; 7-9 M index-vertices) sit at 0.15-0.45 ns per index-vertex, which is
the class mean (0.34) within the noise. The wind shader 3eaa5555 is the dearest at 0.27-0.45 ns,
1.3-1.5x the rigid ones, nowhere near the 2-5x that would point to unneeded work. The shaders above
1 ns/idx remove too little geometry for their deltas to be readable. Summed, the single-skip deltas
come to ~2.2-3.1 ms against the 2.06-2.29 ms measured when all six are dropped together
(`skiptrace-1`), consistent with the noise.

Frame time per skip (flat 60-110 s p50; control 19.24/19.37/19.19): 9da6aacb 18.13-18.45,
6a833138 18.51, 3eaa5555 18.07-18.17, 3deaa573 16.06 (late-window trace) / 18.99 (flat trace),
c829ee58 19.14, 63220f67 19.24. No single shader's draws are worth more than ~1.1 ms of flat p50.

## Half 2: what the vertex stages do (translated SPIR-V the port feeds)

Source: the port's pipeline cache, `out/curve3440/user/cache/CUSA03173-arm64/0x<hash>_<perm>.spv`.
Disassembled with `spirv-dis`. Each hash has a plain permutation and a motion permutation (`_3..5`,
`3eaa5555_1`). md5 of `9da6aacb_0.spv` = da8cab41..., `_3.spv` = f8d66eb6...

- **9da6aacb** (906 ops; `_0`): 5 inputs, all loaded: position f32x4; two u32x4 byte-packed
  normal/tangent, decoded `(b-127)/127`; f32x4 UV; i32x4 16-bit UV `*1/2048`. One u32 from ssbo_8
  (a per-draw base) is added to `attr1.w`. That index selects **one 3x4 matrix** (3 vec4 rows, 12
  floats) from ssbo_9, applied to position/normal/tangent; then a 4x4 view-projection from ssbo_10
  constants. **No weights, no second bone, no loop.** This is rigid single-matrix ("one bone")
  transform, not a blended skinning palette. Outputs: position + 5 vec4 varyings. No per-vertex
  branch. Two `OpFDiv`, both in the port's `EmitUiEdgeStretch` epilogue (motion_param=0 there, so
  that is select-only ALU).
- **6a833138** (939 ops): the same shape. The 3x4 matrix is indexed by `gl_InstanceIndex` (an
  instanced world matrix) instead of a vertex attribute. Same 5 inputs, all consumed, same outputs.
- **3eaa5555** (4118 ops; 4.5x the others): the same rigid 3x4 transform plus a **procedural wind
  / foliage-sway block**: 108 constant-buffer loads (ssbo_7), 16 `Sin`, 32 `Fract`, 12 `Sqrt`, 12
  `InverseSqrt`, 13 `FClamp`, 84 conditional branches (all uniform-ish `> 0` guards on constants and
  distance tests), 196 `OpPhi`. Four varyings out. This is work the picture needs (vegetation
  animation), and its measured per-vertex cost is only ~1.3-1.5x the rigid shaders: the GPU hides
  most of the ALU.
- **Motion permutations** (`_3`/`_1`, the variant `PipelineCache` selects for draws whose VS binds a
  bone-palette-sized stride-16 buffer, `pipeline_cache.cpp` `motion_possible`): the port appends
  `EmitVertexMotion` (`emit_spirv_special.cpp`). Per vertex this is 2 BDA `u32x4` param loads,
  **4 device-scope `OpAtomicExchange`** of the clip position into a 64 MB history buffer, one
  `f32x4` BDA load of last frame's position, and **2 extra vec4 varyings** (`motion_cur` /
  `motion_prev`, locations 30/31) through the tiler. It also adds an 8th colour attachment
  (RGBA32F). The `Object motion:` log line shows ~85 draws/frame storing ~2.34 M vertices/frame.
  These permutations are what the scene draws run.

So the classic "skinned VS over static meshes" case is not here: the guest's "palette" is one
matrix row-fetch per vertex (12 floats, cache-resident), with no weights. Removing it for static
draws saves 3 loads and ~12 FMAs of a ~100-ALU-op shader, far from the 45% bar.

## Go / no-go: static-geometry vertex variant

**NO-GO.** The bar (-45% of these shaders' per-vertex cost, i.e. vertex -1.5 ms and flat p50 -1.0 ms
from 2+2 alternated runs) cannot be met by specialising the guest shaders. Their per-vertex cost is
uniform, they fetch only what they use, and they have no weighted skinning to drop. The line of
**guest** vertex work is closed.

## Next candidate (measured once, not built): the object-motion vertex epilogue

`motoff-1` = the same run with `BB_OBJECT_MOTION=0`, an env override with no ini write:

| | control (`trace-t200` / `trace-1` / `trace-f80`) | `motoff-1` |
|---|---|---|
| vertex ms/frame | 3.485 / 3.657 / 3.379 | **2.422** |
| encoders >= 100 us | 8.0-8.9/frame, 2.08-2.29 ms | 6.3/frame, 1.00 ms |
| fragment ms/frame | 5.14 / 5.16 / 4.92 | 4.82 |
| flat p50 / p99 | 19.24-19.37 / 20.9-21.7 | **17.64 / 19.95** |
| late p50 | 22.17-22.39 | 19.77 |
| G-buffer attachments | 8 | 6-7 |

So the port's motion path is ~1.0-1.2 ms of vertex: about half of the >= 100 us bucket, and as much
as the whole 9da6aacb geometry. Turning it off changes the picture (FSR3 loses object motion vectors
on animated meshes), so the switch itself is not the fix. **The fix is a cheaper epilogue that keeps
the vectors:**

1. Replace the 4 device-scope `OpAtomicExchange` per vertex with one plain 16-byte store. Every
   invocation of the same vertex writes identical bits; the atomics only exist to make that
   write/write race formally defined. On Apple GPUs, device-scope atomics are the dear part.
2. Do not run the motion permutation for draws the rasterizer has already decided are still. The
   gate `object_motion->Moving` in `rasterizer.cpp` sets `motion_param=0` for them (~137 "still"
   draws/frame vs ~85 stored), but they still pay the motion permutation's 2 extra varyings and
   param loads. Bind the plain permutation when `motion_param == 0`.

Code path: `gpu/shader/backend/spirv/emit_spirv_special.cpp` `EmitVertexMotion` (the atomics);
`gpu/render/pipeline_cache.cpp` `motion_possible` / `sel.motion` (the permutation choice);
`gpu/render/rasterizer.cpp` ~L271-320 (the per-draw gate).

**Bar:** vertex -0.7 ms (keep >= 2/3 of the 1.1 ms motoff saving) **and** flat p50 -1.0 ms, from 2+2
alternated runs, with motion vectors unchanged (an `object_motion` capture A/B of the motion image).
The motoff ceiling for that bar has only one run behind it so far: repeat it once before building.

## Incidents

- `gcn_to_vk.cpp:422` (`Unknown data_format=15/6 and num_format=5`) hit on 4 of ~17 attempts
  (`trace-t200`, `skiptrace-3eaa5555`, `skiptrace-63220f67` x2, `skiptrace-3deaa573`). They were
  retried by the harness, and the counted runs have 0. `cp_graphics.cpp:203`: 0.
- **`BB_OBJECT_MOTION=0` crashed bb-gpu once** (motoff-1 attempt 1, title screen, 24 draws/frame):
  SIGSEGV at 0x0 in `Vulkan::PipelineCache::GetProgram` <- `RefreshGraphicsStages` <-
  `RefreshGraphicsKey` <- `Rasterizer::Draw`. Attempt 2 ran clean. This is a real null deref on the
  motion-off path (the motion addresses are 0 there). It is not investigated here; worth a look before
  anyone ships a motion-off default.
- One Metal trace was lost (`skiptrace-6a833138` first try: "Target app exited", "Document Missing
  Template Error"). It was re-run by `out/vertex/rerun_after.sh`.
- Watchdog never fired. Overlap 0 in every run.

## Artifacts

`out/pass-{trace-t200,trace-f80,skiptrace-*,motoff-1}.txt`, `out/mtl-*-gpu.xml`, `out/frames-*.csv`,
`out/vertex/{attempts.jsonl,series.log,per_shader.json}`. All under the ignored `out/`.
Tools: `tools/macos/vertex_per_shader.py`, `tools/macos/pass_shader_share.py`,
`tools/macos/vertex_ceiling.sh` (`skiptrace-*` now arms the pass trace; new `motoff-N` tag).
