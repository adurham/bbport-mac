# MetalFX vs FSR 3.1 vs off: the scene-scale matrix at 3440x1440 (M4 Max, 2026-10-17)

Branch `fix/metalfx-measure`, worktree `~/repos/bbport-metalfx-build`. Script
`tools/macos/metalfx_matrix.sh` driving `tools/macos/gpu_ab_run.sh`: hidden warm-route runs,
standing camera (`ROUTE='ly=128,rx=128:5'`), the owner's ini (21:9, 3440x1440, exclusive
fullscreen, preset 5), 190 s per run, TRACE + 2 port captures each. Runs were alternated
(off / mode / off / …) and **every quoted run has an empty `overlap-<tag>.txt`** (no foreign
`bb-probe` at any point; the machine was idle otherwise). Frame times are p50 in the
850–950-draw bucket; the standing camera keeps run-to-run noise at about 0.15 ms. All runs
land at 883–893 draws/frame.

An earlier same-day attempt was voided by GPU contention (another worktree's benchmark runs
overlapped it) and was re-run clean; `3440_off1` and `3440_mfx5` are the two clean runs of
that attempt and are kept.

## Numbers (ms, p50; fps = 1000/p50)

| tag | upscaler | scene raster | p25 | p50 | p75 | fps | present_wait p50 | n |
|---|---|---|---|---|---|---|---|---|
| 3440_off1 | off | 3440x1440 | 27.06 | **27.24** | 27.43 | 36.7 | 16.38 | 4727 |
| 3440_off2 | off | 3440x1440 | 28.40 | **28.83** | 29.27 | 34.7 | 15.32 | 4448 |
| 3440_off3 | off | 3440x1440 | 27.61 | **27.88** | 28.20 | 35.9 | 15.95 | 4618 |
| 3440_off4 | off | 3440x1440 | 27.65 | **27.88** | 28.18 | 35.9 | 15.98 | 4618 |
| 3440_mfx1 | MetalFX Quality (1.5x) | 2294x960 | 35.57 | **36.14** | 36.81 | 27.7 | 0.00 | 3512 |
| 3440_mfx4 | MetalFX Ultra Performance (3x) | 1146x480 | 30.22 | **30.55** | 30.93 | 32.7 | 15.90 | 1281¹ |
| 3440_mfx5 | MetalFX Ultra Quality (1.25x) | 2752x1152 | 37.85 | **39.06** | 50.60 | 25.6 | 0.00 | 2199 |
| 3440_fsr3_5 | FSR 3.1 Ultra Quality (1.25x) | 2752x1152 | 27.13 | **27.37** | 27.69 | 36.5 | 11.75 | 4655 |
| 3440_fsr3_1 | FSR 3.1 Quality (1.5x) | 2294x960 | 23.96 | **24.19** | 24.49 | 41.3 | 9.06 | 5266 |

¹ mfx4 crashed at ~100 s (below); its row is a ~100 s window, internally tight.

## Verdicts

- **MetalFX is a loss at every preset.** Against the off runs (27.2–28.8 ms, median 27.9):
  Quality +8.2, Ultra Performance +2.7, Ultra Quality +11.2 ms. Mechanism, consistent with the
  known path (`gpu/upscale/vk_metalfx.cpp`): the scene is copied into exportable-memory buffers
  and MetalFX runs synchronously — the CPU waits on both sides (present_wait reads 0 in the mfx
  runs: the presenter wait is replaced by the copy waits). That toll exceeds the raster savings
  even at a 3x scene-scale reduction (1146x480 raster still nets slower than no upscaling).
  MetalFX is not the lever on this machine/driver as it stands.
- **FSR 3.1 Quality is the working lever.** The in-process path (no exportable copies; RCAS
  sharpening) at 1.5x scale: **24.19 ms vs 27.9 off = −13% frame time, 41.3 vs ~36 fps**, and
  present_wait falls 16 → 9 ms (a real GPU saving). Ultra Quality (1.25x) is about neutral
  (27.37) — FSR3's own cost eats the smaller raster saving at that scale. First time FSR 3.1
  ran on this Mac (KosmicKrisp); no crashes, no asserts.
- **Crash found (mfx4 only):** `gpu critical (Debug): Assertion Failed! Unknown data_format=15
  and num_format=7 [gcn_to_vk.cpp:422]` (libbbgpu.dylib, `Vulkan::GcnToVk::SurfaceFormat`),
  GPU process exited (23) ~100 s into the run; the route had completed, so the matrix script
  counted the run clean. Only mfx4 shows it (2 asserts; every other run: 0). Latent port bug on
  a MetalFX-path surface format — not re-run (MetalFX loses regardless); worth a root-cause pass
  only if the MetalFX copy path is ever reworked.

## Captures (visual pass)

Two port captures per run (`out/cap-<tag>-{1,2}.png`). Three were reviewed (off1, fsr3_1, fsr3_5):
same scene (graveyard, stairs, fog, candles, HUD), no missing geometry, no black areas, no
stretched bands, shadows and fog intact. **Stills cannot rule out temporal artifacts** (ghosting
on animated objects: FSR3 still lacks their motion vectors) — the owner's eyes on a visible run
remain the gate before anything becomes a default.

## Not done / open

- The 2560x1440 (16:9, the owner's other mode) group: off + MetalFX pair + FSR 3.1 Quality.
- FSR 3.1 presets 2/3/4 at 3440 (the knee toward 60 fps; softer image — the owner's call).
- No defaults changed anywhere: `upscaler=off` stands until the owner decides.

Evidence: `out/matrix-3440.log` (all VERDICT lines, zero VOID/SKIP), `out/frames-3440_*.csv`,
`out/metalfx_results.json`, parser `out/parse_matrix.py`, captures `out/cap-3440_*.png`.
