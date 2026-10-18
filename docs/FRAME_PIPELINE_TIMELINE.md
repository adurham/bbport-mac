# FRAME_PIPELINE_TIMELINE — Stage E1: is the recording thread serialised behind the GPU?

Established 2026-10-17, continuing `PERF_LOOP.md` stage E (the one lever stages A-D left open: "the
recording thread being late — 3.68 ms of the 6.75 ms idle"). Worktree `~/repos/bbport-encmerge-build`
(branch `fix/encoder-merge`), config 2560x1440 FSR3_1 preset 4 (854x480 internal), standing camera
`ly=128,rx=128:5`, 190 s runs, hidden, one at a time.

## TL;DR

- **Recording IS serialised behind the GPU in the shipping configuration — and the blocker is the
  guest-side pacing lock, not the presenter or the submit path.** In the default mode the first
  submission of frame N reaches bb-gpu **0.07 ms after the GPU went idle**, and its recording
  (8.7 ms median) starts 2.69 ms before the guest's `sceGnmSubmitDone`. Nothing of frame N is
  recorded while the GPU executes frame N-1: `first_submit -2.69`, `gpu_idle -2.76` (ms before
  the frame's own submit-done, medians of 9.8k steady frames).
- **Removing it buys nothing.** Four ways to unpipeline were measured; every one moves the GPU
  idle around (decoder idle 0.1 -> 0.6-8.9 ms/frame) and NONE moves the frame time: p50
  18.74 (default) / 19.03 (honest labels) / 18.84 (`ahead`) / 19.06 (pipeline, 2 ahead) /
  19.03 (pipeline, 100 ahead) / 18.77 (pipeline, 100 ahead, presenter gate off). With a full frame
  of run-ahead achieved (recording of frame N begins 18.6 ms before its submit-done, 6.6-6.8 ms of
  recording) the frame stays **18.77 vs 18.74 ms** and the guest still waits 15.8 ms/frame for the
  GPU to retire the previous frame — the frame is **GPU-paced at ~18.7 ms**, not CPU-starved.
- Consequence: the `3.68 ms CPU-late` in `ENCODER_CENSUS.md` (B5) is **not** a recoverable stall in
  this build — it is the shape of a GPU-bound frame whose submissions are held by the console lock.
  The next attack is the GPU's own 18.7 ms (which does not reconcile with B5's 12.53 ms GPU-busy
  figure - see "Next step"), and NOT the recorder's per-draw cost (6.6-8.7 ms/frame fits inside a
  frame with ~10 ms to spare).

## How it was measured — `BB_FRAME_TIMELINE=1` (default off)

`gpu/shim/bbport_frame_timeline.h`, wired into bb-gpu's own side of the pipeline (the process that
decodes, records and presents). One line per frame, printed at the frame's retire — or, in the
console's lock mode, where no frame is ever "retired", when the next frame's `sceGnmSubmitDone`
arrives. All times are ms relative to **this frame's `sceGnmSubmitDone`** (the guest's own frame
boundary); negative is before it.

```
Frame timeline: 12803 nsub 65 | frame +19.08 ms | first_submit -2.79, decode -2.78..+7.87 (10.66),
     present_wait -11.61 (8.72), gpu_idle -2.85, retire n/a | overlap n/a
```

| field | what it is | where it is taken |
|---|---|---|
| `nsub` | graphics submissions the frame was decoded in | `CommandProcessor::SubmitGfx` |
| `first_submit` | the frame's first submission reached bb-gpu | bb-gpu's reader thread (`SubmitGfx`) |
| `decode a..b (span)` | the GPU command thread (`bb:GpuComm`) started / finished decoding the frame, and the span | `CommandProcessor::Run` around `RunGraphics` |
| `present_wait p (span)` | the presenter's `BB_FRAMES_AHEAD` wait: the command thread blocked on the GPU | `Presenter::PrepareFrame` (`FramesAhead()`) |
| `gpu_idle` | the GPU went idle, releasing the pacing lock (the guest may submit again) | `BbGnm::ReleaseSubmissionLock` |
| `retire` | the GPU finished the frame (honest-labels / pipeline modes only) | `BbGnm::NoteFramesRetired` |
| `overlap` | decode start minus the previous frame's retire: **+** = recording began only after the GPU was done with the frame before, **-** = recording overlapped the previous frame | derived |

Two fields that were already there are used as cross-checks: the frame log's `present_wait_ms`
(identical to the timeline's span) and `Gnm: ... guest waited for the previous frame 1.0x N ms/frame`.

## The measured timeline (medians, steady frames: draws >= 500, t_s >= 60, ~9.8k frames/run)

2560x1440, FSR3_1 p4, 854x480 render, standing camera. Times in ms relative to each frame's
`sceGnmSubmitDone`; "recording" is the decode span.

| run | mode | frame ms | first_submit | decode begin..end | recording | present_wait (span) | gpu_idle | retire | p50 | p25 / p75 |
|---|---|---|---|---|---|---|---|---|---|---|
| `e1_base2` | **default** (console gpu-idle lock) | 18.55 | **-2.69** | -2.69 .. +5.86 | 8.70 | -9.46 (6.54) | **-2.76** | n/a | **18.74** | 18.55 / 18.93 |
| `e1_probe`-family | default, title screen (7 draws) | 8.3 | -0.62 | -0.58 .. +0.02 | 0.60 | -0.68 (0.00) | -0.66 | n/a | 8.2 | - |
| `e1_ahead1` | `BB_SUBMIT_LOCK=ahead` | 18.61 | -2.67 | -2.66 .. +5.82 | 8.62 | -9.59 (6.68) | -2.73 | n/a | 18.84 | 18.65 / 19.05 |
| `e1_honest1` | `BB_HONEST_LABELS=1` (decode pacing) | 18.79 | **-18.69** | -18.61 .. +0.02 | 18.70 | — (0.00) | -18.48 | **+16.65** | 19.03 | 18.50 / 19.59 |
| `e1_pipe1` | `BB_SUBMIT_PIPELINE=1` (2 ahead) | 18.87 | -18.78 | -15.83 .. +1.03 | 17.15 | — (0.00) | -9.91 | +16.75 | 19.06 | 18.41 / 19.75 |
| `e1_pipe_deep1` | `+ BB_SUBMIT_AHEAD=100` | 18.91 | -18.81 | -18.79 .. **-11.28** | **6.57** | -8.88 (8.17) | -18.86 | +18.86 | 19.03 | 18.81 / 19.28 |
| `e1_pipe_max1` | `+ BB_FRAMES_AHEAD=0` | 18.67 | -18.58 | -18.57 .. -10.81 | **6.84** | — (0.00) | -18.62 | +18.62 | **18.77** | 17.45 / 20.10 |

Raw data in the worktree: `out/frames-e1_*.csv` (frame log), `out/run-e1_*.log` (the timeline),
`out/cap-e1_pipe_max1-{1,2}.png`.

## Who waits on whom (default mode, per frame)

The cycle, read off the middle column of the table (frame period 18.55-19.1 ms):

1. **The GPU finishes frame N-1 and goes idle** (`gpu_idle`, 2.76 ms before the frame's
   submit-done). This releases the pacing lock (`ReleaseSubmissionLock`).
2. **The guest's first submission of frame N arrives 0.07 ms later** (`first_submit -2.69`
   against `gpu_idle -2.76`). The gap between "the GPU could take work" and "bb-gpu has work" is
   **0.07 ms — the guest was ready and waiting for exactly this event.** This is the whole
   serialisation: the guest is not slow, it is *held*, and it lets go the instant the GPU is idle.
3. 65 submissions arrive over ~2.7 ms (the guest's frame is already built: it runs ahead by the
   console lock's one-frame allowance, `BB_SUBMIT_AHEAD=2` for the first two, then it is inside
   `sceGnmSubmitDone`), `sceGnmSubmitDone` arrives, and the command thread keeps recording:
   `decode` ends **+5.86 ms after** the submit-done.
4. While it does, the guest sits in `WaitPreviousFrame`/`WaitGpuIdle`: **15.9 ms/frame** measured by
   the port's own counter (`guest waited for the previous frame 1.0x 15.86 ms/frame`).
5. The presenter's `BB_FRAMES_AHEAD=1` wait (6.54 ms) is the same GPU completion seen from the
   command thread: it ends where `gpu_idle` begins (-9.46 + 6.54 = -2.92 vs -2.76). It is *not* the
   bottleneck; it is the command thread waiting for the GPU to be done with what it already has.

So: **the recorder does not block on the GPU** (it never waits for the GPU itself before recording);
**the guest** is blocked, by the port's own pacing lock, from handing over frame N's commands until
the GPU has completely finished frame N-1. The lock is the console's semantics
(`gnm_driver.cpp` header comment), and the port takes it because `SubmitLockOnDecode()` requires
honest labels (`BB_HONEST_LABELS=1`) or in-place guest memory (`BB_GUEST_IN_PLACE=1`), neither of
which is on in these runs. That is why `BB_SUBMIT_LOCK=ahead` changes nothing either: without honest
labels `sceGnmSubmitDone` falls back to `WaitGpuIdle()` + "lock if the GPU is busy", so the guest is
released by the same idle interrupt (row 3 of the table is identical to row 1 to within noise).

## Queue depth, submit path, and copying (the questions the lever depended on)

- **Presenter queue: `BB_FRAMES_AHEAD=1`** (`presenter.cpp`, `FramesAhead()`; 0 = unbounded). It
  gates how far the *command thread* may run ahead of the GPU's tick; it does **not** limit the
  guest. With 1, the command thread blocks one frame behind the GPU after every flip (6.5-8.7 ms
  observed). Setting it to 2 or 0 is measured above: no gain (18.67/18.77).
- **Guest queue: 1 frame, in 2-submission granules.** `BB_SUBMIT_AHEAD` (default 2): the first 2
  submissions of a frame go straight through; the rest wait in `WaitAfterSubmit` →
  `WaitPreviousFrame` until the previous frame has retired. Combined with the lock this is why the
  bulk of a frame arrives in one burst.
- **Submissions per frame: 65** (`nsub`), of which bb-gpu records **3.0 vkQueueSubmit/frame, 5.0
  command buffers each** in the default mode (8.8/2.2 in the honest mode). The frame reaches the
  GPU in a handful of queue submissions, so a deeper pipeline does not need a new submit path.
- **The submit path already copies the command buffers**: `run.sh` exports `BB_COPY_GPU_BUFFERS=1`,
  so `CommandProcessor::SubmitGfx` copies the whole DCB+CCB into a `SubmittedCopy` before the reply,
  and bb-gpu's reader thread sends that reply (`SubmitWaitsForCopy()`, the split-process rule: a
  submit may only return after bb-gpu has the words). The run logs confirm it:
  `GPU process: graphics submissions return once bb-gpu has copied them`. The pipeline experiment
  therefore had no aliasing hazard to fix first.

## The A/B (all 5 modes, same scene, same build, ABBA not needed because the gate loses)

Gate rule from `PERF_LOOP.md`: >= 1 ms and clean captures -> candidate default. **Every pipelined
mode fails**: best case 18.77 vs 18.74 (-0.03 ms, and worse p75: 20.10 vs 18.93; stddev 2.11 vs
0.29 — the default mode is by far the steadiest). `BB_SUBMIT_PIPELINE=1` ships **default off**, as a
measurement tool rather than a candidate win.

Correctness: `out/cap-e1_pipe_max1-1.png` / `-2.png` (pipelined, 100 ahead, presenter gate off) are
the same scene, camera and HUD as `out/cap-2560_a2_p4-1.png` from the earlier baseline (same config,
FSR3_1 p4 at 2560): no tearing, no missing geometry, no HUD damage. A pixel-diff was not run (the
camera is a standing route but the two runs are separate sessions; the earlier stage's noise floor
applies).

Background load during the runs: the owner's desktop (Telegram ~120% CPU, Chrome/Safari/WindowServer,
load average ~8.5) — the project's standing rule is to note it, not void for it.

## What was built

Worktree `fix/encoder-merge`:

- `gpu/shim/bbport_frame_timeline.h` — **new**, `BB_FRAME_TIMELINE=1`, default off: the per-frame
  timeline above. Off = one predictable branch per submission/decode/present.
- `gpu/cp/command_processor.{h,cpp}` — the frame tag on each submission, decode begin/end, the
  submit-done arrival, and frame numbering for the modes where the guest sends frame 0.
- `gpu/render/presenter.cpp` — the `FramesAhead` wait's begin/end.
- `gpu/platform/gnm/gnm_driver.cpp` — the GPU-idle and retire stamps, plus **`BB_SUBMIT_PIPELINE=1`**
  (default off): the retire-based pacing of the honest-labels mode with the labels as they are, so
  a frame may run ahead of the GPU. It measured no win; it stays as the experiment that answers
  "what if the guest could run ahead" without touching the fence semantics.

## Next step (ranked, honest)

1. **Reconcile the two GPU numbers before touching the pipeline again.** With zero decoder
   starvation (pipeline, 100 ahead, presenter unbounded) the frame is 18.77 ms and the guest waits
   15.8 ms/frame for the GPU to retire the previous frame — i.e. the GPU appears to own ~18.6 ms of
   the frame, not the 12.53 ms of `ENCODER_CENSUS.md` B4/B5. Either that trace figure was taken on
   a frame whose CPU-bound phase hid GPU work, or the GPU's work grows when the CPU records
   continuously (bandwidth/contention). One Metal System Trace (`TRACE=1`) of
   `BB_SUBMIT_PIPELINE=1 BB_SUBMIT_AHEAD=100 BB_FRAMES_AHEAD=0` parsed with the stage-B tooling
   settles it, and it is cheap.
2. **If the GPU really needs 18.6 ms**, the loop's remaining levers are all GPU-side and already
   ranked: encoder count (B4 says no), the upscaler's dispatches + post pass (stage D measured
   -0.6 ms), draw/vertex work (3.68 ms vertex of the 12.53). The pipeline line is closed by this
   stage — do not re-walk it.
3. **The recorder's per-draw cost (8.4-10 us/draw, 6.6-8.7 ms/frame) is not the wall** and should
   not be attacked next: with a full frame of run-ahead the recording takes 6.6 ms of an 18.7 ms
   frame. It only becomes interesting if (1) shows the GPU at ~12.5 ms after all.
