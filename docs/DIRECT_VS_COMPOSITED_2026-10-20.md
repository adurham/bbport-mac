# Direct vs composited on the AW3425DW: measured, and why it stays composited (2026-10-20)

## The question

The owner's desktop runs the AW3425DW through **BetterDisplay's colour filter** (brightness 1.25,
gamma -0.25, saturation 0.95, temperature -0.02 — `~/bin/display-hdr-desktop`, the approved
"gold" look). BetterDisplay implements those values as a full-screen compositor pass, its window
`BetterDisplay Compositor Filter Overlay for Display 3` at level 2147483629, alpha 1.00, covering
3440x1440. While that overlay exists, WindowServer composites every frame of everything on that
display, so the port's Metal layer can never take the direct scan-out path (Metal HUD: **Composited**).

So: what does composited actually cost, and is it worth dropping the filter for?

## What was measured, and how

Two identical visible, fullscreen runs of the port, driven by a scripted pad (`BB_PAD_FILE`, the
same mechanism `tools/macos/warm_route.sh` uses): press the title's Continue until the run's log
shows the area, then stand still 60 s at the lamp with no input. Same settings both runs:
3440x1440 output, `window_mode=fullscreen`, `fullscreen_mode=exclusive` (panel on mode 727,
1x 3440x1440 @120, 1:1 scanout), upscaler **off** and frame generation **off** (so the signal is
the compositing path and not the FSR stack; 1,483 draws/frame, GPU-bound at ~26.5 fps).

Only variable: the BetterDisplay filter.

| | composited (filter on) | direct (filter neutralised) | delta |
|---|---|---|---|
| frame_ms p50 | 37.79 | 37.46 | **-0.33** |
| frame_ms mean | 37.78 | 37.49 | -0.29 |
| frame_ms p90 | 38.60 | 38.22 | -0.38 |
| frame_ms p99 | 39.41 | 39.04 | -0.37 |
| frame_ms max | 40.45 | 39.94 | -0.51 |
| present_wait p50 | 21.00 | 21.00 | 0.00 |
| gpu_idle p50 | 0.07 | 0.06 | -0.01 |
| draws/frame p50 | 1484 | 1483 | matched |

Frames: 1,800 per arm, taken from the standing window (the last 1,800 frames with >=500 draws/frame
in each run). Raw data: `out/direct-ab/frames-ab1.csv`, `frames-ab2.csv`, runs `run-ab1.out`,
`run-ab2.out`.

## Verdict

**The compositing pass costs under half a millisecond of frame time** — 0.33 ms at p50, 0.37 ms at
p99 — and it is systematic (every percentile and the worst frame move the same way), not noise: the
two runs otherwise agree to 0.4 ms across 1,800 frames.

That is below the 1 ms line the owner set for "worth doing anything about". So the colour filter
stays on and the game stays composited for now.

## Caveats, stated so a later reader does not over-read this

- **This measures throughput, not latency.** The frame is paced by a 21 ms vblank wait, so one extra
  compositing stage is largely hidden inside it. Direct scan-out still removes a stage from the
  input-to-photon path; nothing in this tree measures that, so the latency claim is unmeasured here,
  not disproven.
- **The layer dump's `fullscreen` field is not the SDL fullscreen flag.** Both runs logged
  `Window: fullscreen flag 1` at entry and after the switch, and both layer dumps read
  `fullscreen 0`. Whatever that field reports, it does not discriminate the two paths — the overlay
  did. Do not use it as the Direct/Composited tell; the Metal HUD is the tell.
- The FSR stack was off for both arms. With the upscaler and frame generation on the frame is
  ~16 ms instead of ~38 ms, so the compositing share per second roughly halves; if it is 0.33 ms at
  26.5 fps it is not going to grow at 60.
- Both arms are the same single spot in the same area, standing still. Combat, streaming and
  cutscenes were not sampled.

## What would change the answer

A game-side equivalent of the filter: the same brightness/gamma/saturation in the port's present
shader, so the BetterDisplay filter can stay off permanently and the desktop look lives somewhere
that does not own the compositor. That is real work for ~0.4 ms unless the latency matters more than
this measurement says.
