# GPU_TIMELINE_RECONCILIATION — Stage E2: the GPU executes 12.5 ms, not 18.6

Established 2026-10-17, answering the one question `PERF_LOOP.md` left at the top of its rank list
and `FRAME_PIPELINE_TIMELINE.md` (E1) left open: the stored trace said **GPU busy 12.53 ms + idle
6.75 ms**, while E1's wall-clock reading said the guest waits 15.8 ms/frame for the GPU to retire
and that the frame is **GPU-paced at ~18.6 ms**. Two fresh Metal System Traces settle it.

Worktree `~/repos/bbport-encmerge-build` (E1's build, `BB_FRAME_TIMELINE` + `BB_SUBMIT_PIPELINE`);
doc on `build/macos-native-0.6`. Config 2560x1440, FSR 3.1 preset 4 (854x480 internal), standing
camera `ROUTE='ly=128,rx=128:5'` (677-679 draws/frame), hidden run, one GPU workload at a time,
200 s runs with the trace attached mid-gameplay.

## TL;DR

- **The GPU executes 12.5 ms per frame. The stored trace's 12.53 ms was right; E1's ~18.6 ms
  inference was not.** Both new traces, in the two configs that E1's A/B compared, give the same
  GPU-side picture to within noise:

  | config | frame | GPU busy | GPU idle | occupancy |
  |---|---|---|---|---|
  | default (console pacing lock) | 18.41 ms | **12.73 ms** | 6.01 ms | 67.9% |
  | `BB_SUBMIT_PIPELINE=1 BB_SUBMIT_AHEAD=100 BB_FRAMES_AHEAD=0` | 18.40 ms | **12.54 ms** | 5.86 ms | 68.1% |

- **The ~6 ms of idle is not a CPU-starved recorder and not one block at the end of the frame: it
  is ~300 command-buffer handoffs.** 307.1 (default) / 301.6 (pipe_max) cross-submit gaps per frame
  at 19.5 / 19.3 us mean = 5.98 / 5.84 ms — i.e. **99.5% of the idle is the GPU waiting at a
  command-buffer boundary** (`same-cmdbuf` gaps, the ones between encoders inside one buffer, are
  0.03 ms/frame total, 0.2 us each). Nothing in the frame is a single long stall except an
  occasional 1.3-1.4 ms bubble (0.3-0.4/frame).
- **Where E1's reasoning broke:** in the timeline, `gpu_idle` and `retire` are a **full frame
  apart** (pipe_max: `gpu_idle -18.35`, `retire +18.35` relative to the same frame's submit-done).
  The guest's "15.8-16.8 ms waiting for the previous frame" is a wait for the *present-cadence
  retire tick*, not for GPU execution. E1 read the wait as GPU time; the trace says the GPU is
  doing 12.5 ms of work and idling 5.9 ms *inside* that window.
- **60 fps at 2560 is arithmetically reachable (12.5 < 16.67) but NOT by GPU-work reduction
  alone.** The deficit is 1.73 ms (18.40 -> 16.67). Removing it from GPU work means cutting 14% of
  everything the GPU does while the handoff drain stays exactly where it is; every priced item in
  the inventory (encoder merges 0.6, bespoke upscaler 0.615, relaxed hazard rule 0.3-0.4) is a
  pass-transition/CPU saving, not 1.7 ms of execution. The 2.9 ms lives in the **commit cadence**
  (302 CBs/frame at ~19.4 us of GPU-visible drain, a quarter of them committed 16-17 us before the
  GPU needs them), which the pipeline experiments never touched.

## Method

Two traces of the same scene, same build, same standing camera, one at a time:

```
BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_RENDER_RES=854x480 BB_OUTPUT_RES=2560x1440 \
BB_FRAME_TIMELINE=1 BB_FRAME_STATS=1 ROUTE='ly=128,rx=128:5' \
  bash tools/macos/warm_route.sh 200                       # default
  ... plus BB_SUBMIT_PIPELINE=1 BB_SUBMIT_AHEAD=100 BB_FRAMES_AHEAD=0   # pipe_max
```

`out/e2/e2_trace2.sh` drives it: waits for bb-gpu's pid from the run log, waits for 10 s of
`>= 500 draws/frame` (real gameplay, past the load), then attaches:

```
xcrun xctrace record --template 'Metal System Trace' --attach <bb-gpu pid> --output out/e2/<tag>.trace
# stopped with SIGINT after 4 s (xctrace's own "Ctrl-C to stop the recording" path)
xcrun xctrace export --input out/e2/<tag>.trace \
  --xpath '/trace-toc/run[1]/data/table[@schema="metal-gpu-intervals"]' > out/e2/<tag>-gpu.xml
```

**Pitfall, cost 3 min this session:** `--time-limit 3s` reaches "Reached specified time limit, ending
recording..." and then hangs at 0% CPU with the capture file still growing (3.0 GB in 180 s, no
`toc.xml`, unexportable). SIGINT after a short fixed duration completes and finalizes every time
(4/4 attempts), and the export then takes ~10 s for a ~1 s window (26-49 MB XML). Do not use
`--time-limit` on this host; do keep a hard `timeout` around the record call. Second gotcha:
xctrace's SIGINT also cuts warm_route.sh's quit path short, so the game can outlive the harness --
after a traced run, `pgrep -fl 'bb-probe|bb-gpu'` and kill only the pids whose command line names
this worktree (a stray pair sat spinning at ~100% CPU and held the load average at 11 until killed).

Analysis: `tools/macos/mtl_gaps.py` (unchanged, for comparability with B4/B5) plus
`out/e2/e2_frame.py`, which normalizes to the **wall clock** instead of the trace's frame numbers --
necessary because Metal's `gpu-frame-number` column is per command buffer here: 98.7% of depth-0
rows carry `-1` and the numbered ones interleave (a "frame" bucket spans 20+ ms of other frames).
Per-frame figures below are `total / (window / wall_frame_ms)`, with the wall frame time from the
same run's own `BB_FRAME_TIMELINE` lines (default 18.41 ms, pipe_max 18.40 ms, 1870 and 3938 lines
inside the traced windows; FPS during the traces 54.0-54.4, matching the earlier stages).

## The traces, key lines

```
# default-gpu.xml, window 971.6 ms, 51.8 frames at 18.74 ms
busy 12.73 ms/frame (67.9%), >=2 channels 0.2% of busy, idle 6.01 ms/frame
  Fragment 5.51 ms/frame 186.8 enc/frame | Vertex 3.68 187.6 | Compute 3.55 119.7
  command buffers 306.4/frame, submissions 494.1/frame, encoders 492.4/frame
idle gaps 486/frame, 6.01 ms: same-cmdbuf 0.03 / same-submit 0.00 / cross-submit 5.98 (mean 19.5 us)
attribution: queued 2.73 ms/frame (412.5 gaps) / cpu-late 3.28 ms/frame (73.2 gaps)
idle by size: <20us 0.94, 20-150us 1.65, >=150us 3.42
timeline: frame 18.41 | first_submit -2.68, recording 8.71, present_wait 6.80, gpu_idle -2.74, retire n/a
```

```
# pipe_max-gpu.xml, window 1763.0 ms, 95.8 frames at 18.40 ms
busy 12.54 ms/frame (68.1%), >=2 channels 0.2% of busy, idle 5.86 ms/frame
  Fragment 5.41 ms/frame 184.0 enc/frame | Vertex 3.64 184.6 | Compute 3.52 117.9
  command buffers 301.7/frame, submissions 486.5/frame, encoders 484.7/frame
idle gaps 477/frame, 5.86 ms: same-cmdbuf 0.03 / same-submit 0.00 / cross-submit 5.84 (mean 19.3 us)
attribution: queued 2.62 ms/frame (403.0 gaps) / cpu-late 3.24 ms/frame (74.0 gaps)
idle by size: <20us 0.93, 20-150us 1.62, >=150us 3.31
timeline: frame 18.40 | first_submit -18.30, recording 6.69, present_wait 0.00, gpu_idle -18.35, retire +18.35
```

Moving-window occupancy (wall-length windows, 50% overlap — immune to the frame-number column):
**median 72% busy = 13.48 ms/frame (default), 72% = 13.22 (pipe_max)**, p10-p90 68-74%. The
aggregate 12.5-12.7 is slightly lower because both windows contain a handful of multi-ms hitch gaps
(60 ms, 29 ms, 12 ms in pipe_max — load/compile stalls, not steady state). Steady state is
**~13.0-13.2 ms of GPU work in an 18.4 ms frame**; the honest range for the steady figure is
12.5-13.5 ms and every variant of the arithmetic leaves ~5.5 ms of GPU idle in the frame.

One denominator note, for anyone re-running `mtl_gaps.py` unchanged: on the default export it counts
54 frames over 971.6 ms (17.99 ms/frame, against the run's own 18.41) and therefore prints busy
12.22 / idle 5.77 instead of 12.73 / 6.01. That ±4% is the whole spread between the tool and the
wall clock here; it does not move any conclusion below. Both traces also show **0.1-0.2% of busy
with two or more channels active**, i.e. the GPU timeline is fully serial — the union of encoder
intervals is a true busy/idle record, not a merge of concurrent queues.

## The reconciliation, in numbers

| per frame | stored trace (B5, metalfx build) | default (E2) | pipe_max (E2) |
|---|---|---|---|
| GPU busy | 12.53 | 12.73 (median window 13.48) | 12.54 (median window 13.22) |
| GPU idle | 6.75 | 6.01 | 5.86 |
| frame period | 19.28 | 18.41 | 18.40 |
| fragment / vertex / compute | 4.60 / 3.68 / 4.30 | 5.51 / 3.68 / 3.55 | 5.41 / 3.64 / 3.52 |
| encoders/frame | 340.9 | ~306 CBs (492 encoder-channel intervals) | ~302 CBs (485) |

1. **B5's 12.53 ms is reproduced at the max-pipeline config (12.54).** The stored trace was not
   taken on an unrepresentative frame, not CPU-bound-hidden, and not a different measurement
   window. The figure stands.
2. **The 6.75 ms idle is not one block.** It is 477-496 gaps/frame, all at *command-buffer*
   boundaries: `cross-submit` 5.84-5.98 ms at 19.3-19.5 us mean. Gaps *between encoders inside a
   command buffer* are 0.2 us each (0.03 ms/frame) — so the cost is per **queue commit**, not per
   pass, and not per encoder. This is the mechanical reason E1's presenter-gate/submit-ahead
   experiments were null: they move *the guest's* handoff, and the idle is in *bb-gpu's* handoff.
3. **Who was late (B5's rule, `start-latency >= gap` = queued):** queued 2.62-2.73 ms, cpu-late
   3.24-3.28 ms — the same ~45/55 split B5 measured on the stored trace, unaffected by a full frame
   of run-ahead. The latch distribution explains it: commit-to-GPU-start latency is **bimodal** —
   p25 = 16-17 us (a quarter of all intervals are committed essentially the moment the GPU needs
   them), median 0.8 ms, p75 1.7 ms, p90 3.3 ms. The recorder does run ahead for most of the frame,
   but a quarter of the work is handed over just-in-time, so recorder jitter (host copies blocked
   13-18% per `Frame stats`) becomes GPU dry time.
4. **Why E1's wall-clock conclusion has a hole, precisely:** E1 measured `guest waited for the
   previous frame 1.0x 15.8-16.8 ms/frame` and read it as GPU execution. But the timeline's own
   stamps say otherwise in the deepest pipeline mode: `gpu_idle -18.35` and `retire +18.35` are
   **36.7 ms apart**, one frame on either side of the frame's submit-done. `retire` is stamped at
   the frame's *presentation*, a frame behind the GPU's completion; `gpu_idle` is stamped when the
   submission lock is released, a frame ahead of the guest's submit-done. The guest is waiting for
   a tick that occurs at the frame period, and the frame period is set by GPU work (12.5) plus the
   commit drain (5.9) — not by GPU execution of 18.6 ms. E1's conclusion "GPU-paced at ~18.7 ms"
   is right about *who* paces (GPU/commit, not CPU-record) and wrong about *what* the GPU is doing
   with the time: two thirds executing, one third waiting for its own next command buffer.

## The 18.4 ms frame, by work type (pipe_max; default in parentheses)

| item | ms/frame | what it is |
|---|---|---|
| **Fragment** | **5.41** (5.51) | 184 render encoders, mean 29.4 us; the mass is 13.8 encoders/frame in the 100-500 us class = 2.99 ms (full-screen passes at 2560: post/RCAS/composite) plus 147.5 tiny ones at 1.15 ms |
| **Vertex** | **3.64** (3.68) | 184.6 vertex intervals, mean 19.7 us; 9.6/frame in the 100-500 us class = 2.39 ms (the heavy draws) |
| **Compute** | **3.52** (3.55) | 117.9 encoders, mean 29.9 us; **one pass of 1.50-1.63 ms every frame** (the largest single interval in both traces) is half of all compute time — the upscaler chain (FFX's 8 dispatches live in one encoder, `ENCODER_MERGE_SURVEY.md` Q3) |
| GPU idle at CB handoffs | 5.86 (6.01) | 301.6 (307.1) cross-submit gaps, 19.3 (19.5) us each |
| — of which ≥150 us bubbles | 3.31 (3.42) | ~3.9/frame, one recurring 1.33-1.38 ms (0.3-0.4/frame, queued: the CB was committed 1.7 ms before the GPU started it) |
| — of which 20-150 us | 1.62 (1.65) | ~57/frame |
| — of which <20 us | 0.93 (0.94) | ~392/frame, the floor of the handoff |
| **frame** | **18.40** (18.41) | timeline median; FPS 54.3 (54.0) |

Ranked reducible items, with honest pricing:

1. **The 1.5 ms compute pass (the upscaler chain)** — the single biggest block of GPU time in the
   frame, ~12% of all GPU work. Stage D's bespoke replacement measured **-0.615 ms** and was less
   temporally stable, so most of that 1.5 ms is resample work at 2560 that any upscaler pays; the
   *reachable* part from this item is ~0.6 ms unless the upscale is removed entirely (which costs
   the 2560 output).
2. **Fragment 5.41 ms** — 2.99 ms of it in 13.8 medium full-screen passes. This is the largest
   single category and the only one with no measured-out lever yet; cutting it 20% = 1.1 ms.
3. **Vertex 3.64 ms** — already flagged by B5; 2.39 ms in 9.6 draws/frame. No culling/LOD work has
   been scoped, so this is inventory, not a plan.
4. **The handoff drain, 5.86 ms** — the largest reducible item in the frame, and the only one that
   can carry the whole deficit. It is *not* encoder count: B4 priced that at 9.0 us/boundary and
   killed it, and `METAL4_PACKING_PROBE.md` showed packing 300 encoders into 1 CB is 6% *slower*.
   What this trace adds is that the drain is per **commit** (same-cmdbuf gaps are 0.2 us), that the
   frame makes 302 of them, and that a quarter of them arrive 16-17 us before the GPU wants them.

## Verdict: is 60 fps at 2560 reachable by GPU-work reduction alone?

**No — but the GPU is not why.**

- Budget at 60 fps: 16.67 ms. Measured frame: 18.40 (pipe_max) / 18.41 (default, p50 18.74 across
  E1's 9.8k steady frames). **Deficit 1.73-2.07 ms** (the "~2.9 ms" in `PERF_LOOP.md` was against a
  19.4 ms frame; the number to beat now is 1.7-2.1).
- GPU execution is **12.5-13.2 ms** — comfortably inside 16.67. If GPU work were the whole story,
  this config would already run at 60 fps. It does not, because **5.9 ms of every frame is the GPU
  waiting for the next command buffer**, and that waiting does not shrink when the work shrinks
  (it is per commit, and the commit count is set by the guest's pass structure, not by how heavy
  the passes are).
- Concretely, to reach 16.67 by GPU-work reduction alone you must remove **1.73 ms of execution
  (14% of 12.5)** *and* keep the 5.9 ms of drain constant. The entire priced inventory —
  encoder merges 0.6, bespoke upscaler 0.615, relaxed hazard rule 0.3-0.4 — sums to ~1.5 ms and is
  mostly pass-transition/CPU savings that the trace says do not touch the 19.4 us handoff. So the
  answer is no: not by GPU-work reduction alone, and not by any combination currently on the list.
- **Where the 2.9 ms would actually come from**, in rank order:
  1. **Commit cadence (5.86 ms of drain).** Halving the GPU-visible handoff cost returns more than
     the whole deficit. The concrete question the trace raises and B4 did not answer: why does
     bb-gpu hand the GPU 302 command buffers one at a time when, in pipeline mode, it has 18.3 ms
     of notice (`first_submit -18.30`) and only 6.7 ms of recording to do? A quarter of the work is
     still committed ~16 us before use. Pre-committing a frame's buffers before the GPU starts
     consuming them is the measurement to make next.
  2. **Fragment (5.41 ms)**: 2.99 ms in 13.8 medium passes at 2560 — the only large category with no
     measured-out lever. A 20% cut is 1.1 ms and would, together with (1) partially, close the gap.
  3. **The 1.5 ms upscaler chain**: 0.6 ms reachable (stage D), the rest is the cost of upscaling to
     2560 at all.

## Where E1's hole was, and the next measurement

The hole is in the *name* of the wait, not in the numbers: `retire` is a presentation-cadence
stamp one frame behind GPU completion (`gpu_idle -18.35`, `retire +18.35` in the same pipeline
mode), so "the guest waits 15.8 ms for the GPU to retire the previous frame" cannot be read as
"the GPU executes 18.6 ms". It is consistent with the GPU executing 12.5 ms and draining 5.9 ms
while the guest waits for a tick at the frame period. Every E1 conclusion that depends only on the
A/B (pipelining is null; the recorder's 6.6-8.7 ms/frame is not the wall; the console lock holds
the guest until the GPU is idle) survives; the conclusion "the GPU owns ~18.6 ms" does not.

Next measurement, cheapest first:

1. **`BB_FRAME_TIMELINE` extension: per-command-buffer commit time + GPU start time in bb-gpu**
   (one line per commit, or a summary: how far ahead of the GPU's consumption the frame's commits
   are). The trace already shows the shape (bimodal latch, p25 = 16 us); the recorder knows the
   exact commit times without a 3 s, 300 MB trace. If the commits really are just-in-time, the fix
   is recorder-side pre-commit (queue the frame's buffers before the GPU drains the previous one),
   not fewer encoders and not GPU work.
2. **A trace at `BB_UPSCALER=off`** to split the 1.5 ms compute pass from the rest of compute (one
   run, same harness) — this prices item 3 above in situ instead of by subtraction from stage D.
3. Only then the fragment inventory (13.8 medium passes at 2560): if item (1) returns nothing, that
   is the last big block of GPU work in the frame.

## Artifacts

Worktree `~/repos/bbport-encmerge-build`:

- `out/e2/default.trace` + `out/e2/default-gpu.xml` (26.7 MB export, 971.6 ms window, 51.8 frames)
- `out/e2/pipe_max.trace` + `out/e2/pipe_max-gpu.xml` (48.7 MB, 1763.0 ms, 95.8 frames)
- `out/e2/{default,pipe_max}-{frame,gaps}-report.txt`, `-report.txt`, `-timeline.txt`
- `out/e2/run-{default,pipe_max}.log` (full run logs incl. the per-frame timeline), `xctrace-*.log`
- `out/e2/e2_trace2.sh` (harness), `out/e2/e2_frame.py`, `out/e2/e2_attr.py`,
  `out/e2/gaps_anatomy.py`, `out/e2/an_probe.py` (analysis)

Background load during the runs: the owner's desktop (Telegram/Chrome/WindowServer), load average
4.1 at the start and ~7 during — the standing rule (note, do not void) applies; the moving-window
occupancy was 68-74% in both traces, i.e. stable.
