# Direct vs composited on the AW3425DW — attempt 1 is VOID (2026-10-20)

## Result of attempt 1: both arms were composited

The owner watched the Metal HUD during both runs: **run 2 was not Direct either.** The two arms
therefore measured the same path, and the 0.33 ms p50 difference between them is run-to-run noise,
not the cost of compositing. Nothing about compositing is established by attempt 1.

Keep the raw data (`out/direct-ab/frames-ab1.csv`, `frames-ab2.csv`, `run-ab1.out`, `run-ab2.out`)
only as a matched-pair noise floor: two identical runs of the same spot agree to 0.4 ms p50, 0.4 ms
p99 and 0.5 ms worst frame. That is the resolution any future A/B has to beat.

## Why run 2 was still composited: the harness, not the display path

The A/B was driven directly through `tools/macos/run.sh`, which reads the **checkout's** `bbport.ini`,
while the owner launches through the app, which reads
`~/Library/Application Support/bbport/bbport.ini`. Those two files drift — the documented pitfall —
and the key that matters here is `layer_opaque`: the app's ini has `layer_opaque=1`, the checkout's
does not.

The consequence is in the logs, plainly:

| | boot layer dump | `Layer check (+4 s)` |
|---|---|---|
| attempt-1 runs (run.sh, checkout ini) | `opaque 1` (override applied at boot) | **`opaque 0`** |
| owner's app run 15:17 (app ini) | `opaque 1` | `opaque 1 (as overridden)` |

A non-opaque layer cannot be scanned out directly; WindowServer must composite it. So both attempt-1
arms were composited by configuration, and the BetterDisplay filter was not the variable under test
at all.

## What attempt 1 does prove

- **The layer dump's `fullscreen` field is not the SDL fullscreen flag.** All four runs logged
  `Window: fullscreen flag 1` at entry and after the switch, and every layer dump read
  `fullscreen 0`. Whatever that field reports, it does not discriminate the paths — stop using it as
  the Direct/Composited tell.
- **The Metal HUD is the only reliable tell.** It disagreed with the logs twice today.
- The composited baseline is stable to ~0.4 ms across 1,800 frames per run, at 1,483 draws/frame,
  `window_mode=fullscreen`, `fullscreen_mode=exclusive`, panel on mode 727 (1x 3440x1440 @120,
  1:1 scanout), upscaler and frame generation off, 26.5 fps.

## Attempt 2 — the harness fix

Before any further comparison:

1. Drive the runs with the **app's** settings, not the checkout's: copy
   `~/Library/Application Support/bbport/bbport.ini` over the checkout's copy (or export
   `BB_LAYER_OPAQUE=1` and every other key the app sets) and assert `opaque 1` at the `Layer check`
   line before trusting the run.
2. Verify **per run, from the HUD**, that the intended path was reached: Composited for arm A,
   Direct for arm B. A run that does not reach its arm is discarded, not averaged.
3. Only then compare; anything under the 0.4 ms matched-pair floor is noise.

The BetterDisplay question (does its compositor filter force compositing?) is still open and still
plausible — it was simply not isolated, because layer opacity already forced compositing in both
arms.
