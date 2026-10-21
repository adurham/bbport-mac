# BB_NULL_RENDER: can renderer work be seen late in a session? (2026-10-21, branch bench/null-render)

Status: **gate, harness, trace tooling and report built and compiled; the decisive runs have NOT been
made.** The owner stopped all game launches (hidden runs included) while he is away from the
machine. Nothing in this doc is a null-renderer number yet.

Base: bench/protect-async @ 270a643.

## The question

Late in a standing run (after ~2-3 min) the game process becomes the pacer: its CPU steps 29 -> 47
ms/frame, bb-gpu's present_wait falls to ~0-2 ms, and removing 6-8 ms/frame of bb-gpu waiting moved
frame p50 by only -0.25 ms (docs/PROTECT_ASYNC_2026-10-21.md). If the guest paces at >= 18 ms with NO
GPU work at all, no renderer optimisation under that is visible in play.

Decision rule (late phase = last 75 s of a 420 s run): null-render frame p50 >= 18 ms -> the
renderer is capped, the finding is guest-side; <= 14 ms -> renderer work is in play; in between ->
report both and the GPU's share of the late frame.

## The gate (default off)

`BB_NULL_RENDER=1`: bb-gpu decodes every submission as before (labels, EOP fences, interrupts,
flips, the submit/copy protocol and protections are untouched) but `Rasterizer::Draw`,
`DrawIndirect`, `DispatchDirect` and `DispatchIndirect` return after counting; occlusion events fall
back to the fixed always-visible counters (no query could count); every flip presents the blank
frame (`Display::QueueFlip` -> `PrepareBlankFrame`, which still flushes the submission). The
swapchain, pacer and flip events stay alive. `BB_NULL_RENDER=2` drops draws but keeps dispatches,
the fallback if dropping the guest's compute changes what the game does.
Residual GPU work with the gate on: the blank present, CopyBuffer/DMA, WriteData-on-GPU and label
writes. These are small, but they are not zero.

Off = inert: `Level()` reads the env once and every call site is one cached-false branch. The only
new output is `Null render:` lines (one at start, one per 5 s stats window), printed only with the
gate on. **Proof that it is inert is pending:** the `ctrl-1` run must log zero `Null render` lines
(`null_render.sh` records `null_lines` per attempt in attempts.jsonl).

Files: `gpu/shim/bbport_null_render.h`, `gpu/render/rasterizer.cpp`,
`gpu/render/raster_signals.cpp`, `gpu/platform/videoout/display.cpp`,
`gpu/platform/videoout/frame_stats.cpp`. Build verified (both `Built:` lines, no STOP).

## Harness and analysis

- `tools/macos/null_render.sh TAG...`: hidden standing runs on the curve route/save (fsr3 preset 4,
  checkout ini with frame_generation=0, sandbox data, no BB_VRAM_LIMIT_MB, SECS=420), footprint
  watchdog 19 GiB / 4 GiB, overlap void, 3 attempts per tag, and per-attempt counts of the two
  known asserts (gcn_to_vk.cpp:422, cp_graphics.cpp:203). Tags: `null-N`, `null2-N`, `ctrl-N`,
  `trace-N` (gate off + a 4 s Metal System Trace finalised by SIGINT). `SAMPLE=1` takes 3 s
  `sample` stacks of bb-probe at gameplay+80 s and in the last minute (GXRenderThread running,
  sleeping or spinning).
- `tools/macos/gpu_ab_run.sh`: `TRACE_SIGINT_S=N` records without `--time-limit` and SIGINTs
  after N s, with a bounded TERM fallback (the time limit has hung on this Mac).
- `tools/macos/null_render_report.py [--out DIR] TAG...`: p50/p99 for 0-60, 60-110 and the last
  75 s, with game CPU/frame, present_wait, submissions, draws and GXRenderThread % of a core and
  ms/frame (from the per-5 s `Game process: CPU by thread` lines). Prints the verdict. Tested
  offline on protect-async's ctrl-1 (below).
- `tools/macos/mtl_encoders.py trace.xml`: per-encoder vertex/fragment/compute split, how flat or
  concentrated the vertex time is, the true vertex x previous-fragment interval intersection, and
  render encoders/frame.

## Guest render thread: what the existing data already shows (the real renderer, not null)

protect-async ctrl-1 (fsr3 preset 1, 3440, 300 s), via null_render_report.py:

| window | frame p50 | p99 | game CPU/frame | present_wait | GXRenderThread % core | GXRT ms/frame |
|---|---|---|---|---|---|---|
| 0-60 | 23.47 | 25.69 | 29.8 | 9.6 | 11 | 2.6 |
| 60-110 | 23.54 | 26.01 | 29.8 | 9.7 | 12 | 2.8 |
| last 75 | 24.88 | 39.47 | 47.2 | 1.1 | 16 | 4.0 |

GXRenderThread (the guest's submitting thread) is on-CPU only 2.6-4.0 ms of each frame, i.e. it
is blocked/sleeping, not spinning, for most of it. The game-wide Gnm counter shows the guest
waiting 15.3-15.6 ms/frame for the previous frame (the submit lock). The counter is not tagged per
thread, but the submit path is where it is taken. The per-5 s thread lines map to gameplay time
only approximately (one line per >= 5 s stats window). The late growth is in the other guest threads (GXWorker x5, Havok x5, FMOD,
CSChr), not in the submit thread. This is a proxy with the real renderer. It cannot tell whether
the frame is gated by the GPU's retire or by the workers; the null run answers that.

## Metal trace (from existing traces; no new trace taken)

Existing exports, 3440x1440 FSR 3.1 p4 (fix/metalfx-measure, 2026-10-17, 179 frames, 19.69 ms/frame),
cross-checked against 2560 p4 and 3440 p3. `mtl_encoders.py` gives:

- Busy 13.62 ms/frame: fragment 5.20, vertex 3.79, compute 4.70 (incl. the FSR pass).
- **Vertex is mostly CONCENTRATED, with a per-encoder floor under it.** 215 render encoders/frame. The
  median encoder's vertex phase is 5.5 us. 190/frame sit under 10 us (1.00 ms/frame); the floor
  (5.2 us x 215) = 1.11 ms/frame. 9.2/frame are >= 100 us and carry 2.35 ms (62%). The top 10
  encoders hold 64% of vertex time and the top 20 hold 72%. Same shape at 2560 p4 (top10 66%, floor
  1.04 ms) and 3440 p3 (65%, 1.07 ms). So ~2.4-2.7 ms is geometry in ~10 heavy passes (shadow
  cascades are the usual suspect; not yet identified by label), and ~1.1 ms is per-encoder floor.
  Vertex vs fragment per encoder correlates at r = 0.53.
- **Overlap is essentially none.** The real vertex x previous-fragment interval intersection is
  0.007 ms/frame (3.5 of 215 pairs per frame touch at all). Any two channels at once = 0.066 ms/frame.
  16 pairs/frame have N's first vertex stamp before N-1's last fragment stamp, but their intervals
  do not intersect. The median gap from N-1's fragment end to N's vertex start is 21.8 us. Encoders
  are fully serialised.
- **Encoder count vs guest passes:** 215 render + 136 compute = 351.5 encoders/frame against the
  guest's 138-162 render passes/frame (docs/ENCODER_ATTRIBUTION.md). Render encoders run about 1.4x
  the guest's passes. Encoders/command buffer = 1.0 (351.5 vs 350.5). That excess matches the
  port's pass breaks attributed in ENCODER_ATTRIBUTION.md (CopyBuffer ~37/frame and others), so
  there is no sign of KK splitting beyond the port's structure. But this trace alone cannot
  separate a KK split from a port break: that needs `BB_KK_STATS` begin-rendering counts next to
  the trace (the trace-1 run logs both).

Caveat: these traces are 4 days and several commits old, at ~888 draws. The `trace-1` tag re-takes
the trace on this build.

## Resume (one command, when launches are allowed again)

    cd ~/repos/bbport-nullrender-build && SAMPLE=1 nohup bash tools/macos/null_render.sh null-1 ctrl-1 trace-1 >/dev/null 2>&1 &

then `python3 tools/macos/null_render_report.py null-1 ctrl-1` and
`xcrun xctrace export --input out/mtl-trace-1.trace --xpath '/trace-toc/run[1]/data/table[@schema="metal-gpu-intervals"]' > out/mtl-trace-1-gpu.xml && python3 tools/macos/mtl_encoders.py out/mtl-trace-1-gpu.xml`.
Progress: out/null-render/series.log (ends with ALLDONE).
