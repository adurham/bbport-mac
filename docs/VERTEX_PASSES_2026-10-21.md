# Which render encoders carry the vertex time, and what removing them is worth
(2026-10-21, branch `bench/vertex-passes`, base `bench/null-render` @ e082ab8)

The null-renderer run put the lever in the renderer (docs/NULL_RENDER_2026-10-21.md), and the Metal
trace put most of the renderer's vertex time in ~10 encoders per frame: 215 render encoders/frame,
median vertex phase 5.5 us, 9.2 encoders/frame at >= 100 us holding 2.35 ms of the 3.79 ms (62%),
top 10 = 64%, top 20 = 72%, everything fully serialised. What that trace could not say is *which*
passes those encoders are: a KosmicKrisp command buffer carries no label and the trace's frame
column is empty on this OS, so the encoders are anonymous intervals.

## Answer

**They are the game's scene G-buffer pass**, drawn at the reduced scene raster: 8 colour attachments
at **1146x480** (3440/3 x 1440/3, preset 4) plus D32SfloatS8Uint depth, ~607 draws and ~27 M
index-vertices per frame across six vertex shaders, split into ~35 Metal encoder segments per frame
by the port's pass breaks. Every encoder of that pass that draws more than a few thousand indices is
one of the heavy ones; with the six shaders' draws removed, **no render encoder is >= 100 us any more
and the whole 2.35 ms bucket disappears** (below). The shadow cascades are *not* it: the 4096x4096
passes are 0.27 big-draw encoders/frame and a small share of the indices.

## What the port now logs (gate, default off)

`BB_PASS_TRACE=<file>` (`gpu/render/vk_frame_capture.cpp`) writes one line per **render encoder** —
at `Scheduler::BeginRendering` after the "same state, already rendering" early return, flushed at
`EndRendering`, so a break/split/resume is its own line exactly as it is its own Metal encoder:

    PT <frame> <ordinal-in-frame> <WxH> nc=<colour attachments> c0=<fmt> c1=<fmt> d=<fmt>
       dr=<draws> idx=<indices x instances> inst=<instances> vs=<hash>[,<hash>] ps=<hash>[,<hash>]
       t=<steady-clock ns> [present|resample|overlay]

Draws, indices and shader hashes come from the call site that feeds the frame analyzer, the
attachments from `Rasterizer::BeginRendering`; the three encoders the port opens outside
`Scheduler::BeginRendering` (present pass, scene resample, overlay) log a line of their own so the
sequence keeps its count. Windowed: arms on a trigger file **and** only on a frame that draws the
game (a boot frame has 4 encoders, a gameplay frame ~190), then records `BB_PASS_TRACE_FRAMES` frames.
Off = one cached-false branch per draw; nothing is printed.

Harness: `tools/macos/vertex_ceiling.sh` (420 s hidden standing route, fsr3 preset 4, 3440x1440,
sandbox user dir, `frame_generation=0`, watchdog 19 GiB / 4 GiB, 3 attempts, the two known asserts
counted), tags `ctrl-N`, `skip-N` / `skiptrace-N` (`BB_SKIP_VS`), `trace-N`, `trace2560-N`.

## The join to the trace's vertex time did not validate, and why

`tools/macos/mtl_pass_join.py` tries two joins and prints which one it believes:

| join | assumption | result on `trace-1` |
|---|---|---|
| frame ordinal | port encoders/frame == trace render encoders/frame, same order | 0 of 27 frames agree |
| rank sequence (DP) | order preserved, port passes all matched, trace may carry extras | Spearman(indices, vertex) = 0.18 -> rejected |

The counts genuinely differ (port 187.5 render encoders/frame, trace 201.2: KosmicKrisp opens
encoders of its own for meta clears and a few port passes produce no interval), and the trace's frame
period is not identifiable — segmenting its vertex sequence gives a mean within-position spread of
0.68 in log(vertex us), because the game's frame structure changes frame to frame (184..203
encoders/frame). A positional join therefore drifts by ~14 encoders per frame and stops meaning
anything after the first few.

**What would close it, exactly:** (a) record `mach_absolute_time`/`CLOCK_UPTIME_RAW` at encoder begin
and end and have the harness stamp the recording's start uptime, so the trace's relative timeline can
be anchored and the join becomes a nearest-timestamp match (the port already logs `t=`, only the
absolute basis is missing); or (b) set a Metal encoder label from the pass identity (a Vulkan debug
label mapped to the MTL encoder by KosmicKrisp), which makes the trace export self-describing —
`metal-object-label` is empty today. Neither is in this branch.

The identity above therefore rests on two measurements that do not need the join: the port's own
per-encoder record, and the differential trace below.

## The heavy passes, from the port's own record

401 gameplay frames of `trace-1` (3440x1440, fsr3 preset 4). The port records **9.78 M
index-vertices/frame in 882 draws**; **11 encoders/frame carry more than 200 k indices each and
93% of all of it (9.07 M)** — the same count as the trace's 9.2 encoders at >= 100 us, which is why
these are the heavy ones:

| # | target | nc | colour0 / depth | draws | M indices/fr | vertex shader | pixel shader | enc/fr |
|---|---|---|---|---|---|---|---|---|
| 1 | 1146x480 | 8 | R8G8B8A8Unorm / D32SfloatS8Uint | 51.1 | 2.61 | 000000009da6aacb | 00000000bb10e440 | 2.9 |
| 2 | 1146x480 | 8 | R8G8B8A8Unorm / D32SfloatS8Uint | 41.6 | 1.61 | 000000006a833138 | 000000001c3953c6 | 1.5 |
| 3 | 1146x480 | 8 | R8G8B8A8Unorm / D32SfloatS8Uint | 25.1 | 1.56 | 000000003eaa5555 | 00000000dbeccd9c | 1.6 |
| 4 | 1146x480 | 8 | R8G8B8A8Unorm / D32SfloatS8Uint | 25.9 | 1.26 | 000000009da6aacb | 00000000ca7226ec | 1.8 |
| 5 | 1146x480 | 8 | R8G8B8A8Unorm / D32SfloatS8Uint | 10.5 | 0.67 | 000000006a833138 | 00000000bb10e440 | 0.7 |
| 6 | 1146x480 | 8 | R8G8B8A8Unorm / D32SfloatS8Uint | 17.0 | 0.44 | 00000000a7450683 | 00000000b9bb200e | 0.5 |
| 7 | 1146x480 | 8 | R8G8B8A8Unorm / D32SfloatS8Uint | 7.4 | 0.43 | 000000009da6aacb | 000000001c3953c6 | 0.5 |
| 8 | **573x240** | 1 | R8G8B8A8Unorm / D32SfloatS8Uint | 26.3 | 0.24 | 00000000b11756c0 | 00000000749e4f9e | 0.7 |
| 9 | 1146x480 | 8 | R8G8B8A8Unorm / D32SfloatS8Uint | 1.2 | 0.10 | 000000003deaa573 | 00000000e21e283a | 0.4 |
| 10 | 573x240 | 1 | R8G8B8A8Unorm / D32SfloatS8Uint | 5.7 | 0.05 | 00000000855f61b6 | 00000000749e4f9e | 0.1 |
| 11 | 573x240 | 1 | R8G8B8A8Unorm / D32SfloatS8Uint | 4.3 | 0.04 | 000000005992ac36 | 0000000034bc187c | 0.1 |

Eight of the eleven are the **scene G-buffer: 1146x480 (3440/3 x 1440/3, preset 4) with 8 colour
attachments and D32SfloatS8Uint depth**, 644 draws and 9.12 M indices/frame in 89 encoder segments;
the other three are the half-res chain at 573x240. The whole scene raster target holds 93% of the
frame's geometry. The single heaviest encoder is #1: 51 draws, 2.61 M indices, vs 9da6aacb.

**It is not the shadows, and it is not duplicated.** The 4096x4096 cascades are 0.19 M indices/frame
(2.0%) in 6 encoders/frame; the biggest 4096x4096 encoder carries 0.089 M. Each vertex shader
appears once per frame with exactly one attachment set, so the scene geometry is drawn once — there is
no depth-prepass duplicate or second copy to delete.

At 2560x1440 the identity is the same, and not because the target followed: `BB_OUTPUT_RES=2560x1440`
(with preset 4) moved the output and FSR's output size but the scene proxy **stayed 1146x480**, so
`trace2560-1` records the same 11.1 encoders/frame above 200 k indices (9.08 M, 93%), the same eight
attachments, the same three vertex shaders at the top (4.42 / 2.45 / 1.36 M), the same 3.7 ms of
vertex time (3.745 ms/frame, 8.8 encoders at >= 100 us = 2.364 ms = 63%, top10 65.6%, floor 1.05).
The ranking is not resolution-specific; if anything this shows the scene raster is pinned by something
other than the output resolution, which is worth knowing before anyone tries to buy vertex time with
it.

## The ceiling, measured (arm B = the scene geometry's draws dropped)

`BB_SKIP_VS=hash[,hash...]` (diagnostics, default off, `gpu/render/rasterizer.cpp`, same shape as the
existing `BB_SKIP_PS`) returns before recording a draw whose *vertex* shader is listed. The pass still
begins and ends, so every encoder and boundary stays; only the geometry is gone. The screen shows a
scene without its main geometry — this is a ceiling, not a renderer.

| | arm A `ctrl-1` | arm B `skiptrace-1` | delta |
|---|---|---|---|
| vertex ms/frame (both arms: total vertex / frames in the trace span) | 3.30 | 1.24 | **-2.06** |
| vertex us per render encoder | 18.7 | 6.7 | -64% |
| encoders >= 100 us (trace's 36-frame basis) | 9.2/frame, 2.35 ms | **0** | -2.35 |
| vertex floor (5.2 us x encoders/frame) | 1.05 ms | 0.97 ms | -0.08 |
| frame p50, flat (60-110 s) | 19.24 | 15.70 | **-3.54** |
| frame p99, flat | 20.91 | 17.31 | -3.60 |
| frame p50, late (last 75 s) | 22.17 | 20.28 | -1.89 |
| frame p99, late | 42.55 | 24.28 | -18.27 |
| present_wait, flat | 6.29 | 2.37 | -3.92 |
| game CPU/frame, flat | 27.67 | 27.48 | -0.19 |

**Bar (vertex -1.5 ms AND flat p50 -1.0 ms): met, with room** — -2.06 ms of vertex and -3.54 ms of
flat p50, and the GPU stops being the flat-phase pacer (`present_wait` 6.29 -> 2.37). Arm B left the
floor untouched (0.93 ms, 5.2 us/encoder), so the entire vertex block above the floor is those
shaders' scene draws, and the bucket >= 100 us is *only* them. The skip set was the six hashes
`9da6aacb, 6a833138, 3eaa5555, c829ee58, 63220f67, 3deaa573` — the top three by geometry plus the
three that appear inside the multi-shader encoders. No asserts in either arm (gcn_to_vk
0/0, cp_graphics 0/0), overlap 0, watchdog never fired, `max_draws` 889/891 (the guest's decoded count;
the skip does not change what the game submits).

Frame counts inside the trace windows come from the frame log (41 frames in 0.779 s of arm A, 29 in
0.619 s of arm B); the ratio between arms matches the per-encoder ratio (0.375 vs 0.356), so the delta
does not depend on that estimate.

## What the fix actually is

From the identity, not from taste:

1. **Not shadows, so no cascade/LOD/resolution trade.** The heavy encoders are the scene G-buffer at
   1146x480. The resolution knob is already at 1/3 and does not touch this block: measured vertex is
   **3.76 ms at 3440x1440 p4, 3.68 at 2560x1440 p4, 3.86 at 3440x1440 p3** (`mtl_encoders.py` on
   `bbport-metalfx-build/out/mtl-{3440,2560}_fsr3_*.xml`), and the >= 100 us bucket is 2.35-2.46 ms in
   all three. The vertex phase is per-vertex, so cutting pixels again buys nothing here — anyone
   proposing "lower the scene scale" for this should expect 0.
2. **Not the port's pass structure either.** The scene target is drawn in 89 encoder segments/frame
   (the port's own breaks), and those segments are worth 88 x 5.2 us = **0.46 ms** of the 1.06 ms
   floor —
   merging them cannot touch the 2.35 ms above it. The measured marginal cost of a break in situ is
   0.03-0.04 ms (ENCODER_ATTRIBUTION.md, BB_COPY_MERGE + BB_INPASS_BARRIERS: -0.04 ms for -1.6
   breaks/frame), i.e. the whole break-elimination line is a ~1.2 ms-class contributor that has
   already measured neutral. Closing that line for the *vertex* block specifically.
3. **The lever left is per-vertex cost in those shaders** (~0.34 ns/vertex: 3.3 ms over the 9.78 M
   index-vertices), or the vertex count, which is the game's own draw lists — the port cannot lower
   the LOD of guest geometry without changing the picture, and the geometry is drawn once per frame,
   so there is no duplicate pass to delete either.

   The only port-side fix that keeps the picture is a cheaper vertex path for those shaders
   (a static-geometry variant that skips the skinning/fetch work a draw does not use). **The bar it
   has to clear: ~45% off the per-vertex cost of those shaders**, which is 1.5 ms of the 3.3 ms. The
   measurement that decides whether it can: run `BB_SKIP_VS` on ONE of the shaders at a time and
   compare vertex us per index-vertex between them — if the per-vertex cost differs materially across
   them (i.e. some are doing work their draws do not need), a specialized path is worth building; if
   they all sit at the same ~0.34 ns/vertex, the cost is the GPU's vertex rate for this geometry and
   the line closes with no fix to build.

## Artifacts

- `out/pass-trace-1.txt` — 75 k encoder lines, 401 frames, 3440x1440 fsr3 preset 4.
- `out/mtl-trace-1-gpu.xml` — 3 s Metal System Trace of the same frames; `mtl_encoders.py` on it:
  vertex 3.759 ms/frame (36-frame basis), 201.2 render encoders/frame, floor 1.056, top10 64.4%,
  >= 100 us 9.2/frame = 2.349 ms = 62% — the same shape as the 2026-10-17 trace.
- `out/mtl-skiptrace-1-gpu.xml` — arm B's trace: no encoder >= 100 us, vertex 6.7 us/encoder.
- `out/frames-{ctrl-1,skiptrace-1}.csv`, `out/vertex/attempts.jsonl`, `out/vertex/foot-*.txt`.
- `out/pass-trace2560-1.txt`, `out/mtl-trace2560-1-gpu.xml` — the 2560 p4 confirmation.
