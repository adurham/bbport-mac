# Upscaler configuration bench — Stage A

Stage A of `docs/PERF_LOOP.md`. Measured 2026-10-17 21:13-21:46 CDT in
`~/repos/bbport-encmerge-build` (branch `fix/encoder-merge`, build @ 18dd2d2 + Stage D gate),
all gates default off, output 2560x1440 / 16:9, FSR 3.1 preset 4 unless stated, hidden warm route
`ROUTE='ly=128,rx=128:5'`, standing camera, 190 s, 2 port captures per run.

## Headline

1. **A1 answered: FSR 3.1 already works on the startup-patched path** (`BB_RENDER_RES` /
   `live_resolution=0`) — no code change needed, and the interrupted run's numbers reproduce. But it
   buys **0.24 ms**, not the ~2.9 ms the live path was assumed to cost. **The >= 1 ms gate FAILS**:
   startup-patched + FSR is not a candidate default.
2. **The 2.9 ms was never live-path overhead.** Patched+FSR = 18.66-18.69 ms against the patched
   *upscaler-off* anchor of 16.50 ms, so **the FSR chain itself is ~2.0 ms** on the patched path too.
   Removing the live 1080p post and the proxy copy-backs only removes 0.24 ms of it.
3. **The patch is not a neutral A/B**: it changes what the *guest* submits — 681-682 draws/frame vs
   886-890 live (-23%), because the guest runs its own post at 480p instead of at 1080p off proxies.
   The 0.24 ms therefore includes removed guest work as well as removed host work, and the comparison
   is "smaller frame" not "same frame, less upscaler".
4. **A2 (preset sweep, presets 2-4 at 2560 and 3440) is measured and monotonic**: preset 4 is fastest
   at both resolutions (2560: 21.02 / 20.11 / **18.92**; 3440: 22.75 / 21.93 / **20.81** for presets
   2 / 3 / 4), so the shipped default is confirmed rather than changed. 3440 costs +1.89 ms over 2560
   at the same preset. One gross outlier tag (`2560_a2_p4 = 24.01`, same config as 18.92) is
   superseded by A1's two live runs, not averaged with them.

## A1 — live vs startup-patched, 2560, FSR 3.1 preset 4, ABBA

Command (`tools/macos/upscaler_config_bench.sh`, added this session):

```
bash tools/macos/upscaler_config_bench.sh 2560 a1_live1=live:4 a1_patch1=patch:4 \
                                            a1_patch2=patch:4 a1_live2=live:4
```

`live:P` = guest at 1080p, host scene targets scaled at run time (`BB_RENDER_RES` unset, what
`live_resolution=1` does). `patch:P` = guest render size patched at start (`BB_RENDER_RES=854x480
BB_OUTPUT_RES=2560x1440`). ABBA order so a linear drift cancels; steady rows only
(`draws>=500 && t_s>=60`).

| tag | mode | n rows | p25 | **p50** | p75 | fps | draws | present_wait p50 |
|---|---|---|---|---|---|---|---|---|
| 2560_a1_live1 | live | 6827 | 18.81 | **19.02** | 19.23 | 52.6 | 886 | 6.15 |
| 2560_a1_patch1 | patch | 6949 | 18.46 | **18.66** | 18.87 | 53.6 | 681 | 7.40 |
| 2560_a1_patch2 | patch | 6947 | 18.48 | **18.69** | 18.90 | 53.5 | 682 | 7.51 |
| 2560_a1_live2 | live | 6877 | 18.62 | **18.81** | 19.01 | 53.2 | 890 | 6.02 |

- live mean **18.915 ms / 52.9 fps**; patch mean **18.675 ms / 53.5 fps**; **delta -0.240 ms**.
- Within-path spread: live 0.21 ms, patch 0.03 ms. The 0.24 ms delta is the **same size as the
  live-path run-to-run spread**, so it is a marginal effect, not a lever.
- All four runs valid: `overlap-*.txt` empty, `took=1`, draws >= 500, p50 <= 40 ms.
- The earlier interrupted series (`frames-2560_a1_patch1/patch2.csv`, cut mid-run) gave the same
  sign and size (-0.3 ms); this series supersedes it and both old tags are discarded.
- `present_wait` rises on the patched path (6.0-6.2 -> 7.4-7.5 ms): the patched frame does less GPU
  work and spends longer waiting on presentation, consistent with 205 fewer draws.

## Why FSR works there (mechanism, from source)

- **One env var selects the path.** `scaled_session` is literally "`BB_RENDER_RES` is set"
  (`gpu/upscale/vk_temporal_upscaler.cpp:243-252`); `Scaled()` is `scaled_session || target !=
  1920x1080` (`:1539-1541`). `BB_OUTPUT_RES` only seeds the FSR output size (`:234-240`).
- **On the patched path FSR consumes the guest's own images.** `RunScaled()` derives `w,h` from the
  scene constants (`:1522-1530`) and, when `scaled_session`, takes `CachedView()` of the guest
  colour/depth directly (`:1873-1881`); only the live branch calls `scene_targets.Read()`
  (`:1861-1872`). Host proxies are only ever created for **1920x1080** guest images
  (`gpu/upscale/vk_scene_resolution.cpp:95-97`), so a patched 854x480 guest has no proxy, no
  resolve and no copy-back at all — that is the entire 0.24 ms.
- **TAA is what refuses the patched path, not FSR:**
  `if (use_taa && (w != ow || h != oh)) { printf("TAA: remove BB_RENDER_RES…"); return false; }`
  (`:508-511`). The FSR 3 branch has no equivalent check; its only gates are `depth extent ==
  colour extent` and `w <= ow && h <= oh` (`:1847-1849`). The FSR layer itself just forwards
  `renderSize`/`upscaleSize` (`gpu/third_party/fsr-vulkan/src/ffx_vk_fsr3_portable.cpp:604-605`) and
  validation only requires input extent **>=** render size (`ffx_vk_portable.c:47-48`), so the
  guest's 8-aligned 856x480 target passes.
- **Two traps for a future bench, both silent today.** (a) `RunScaled` returns without a log line
  if depth/colour extents disagree or scene > output (`:1847-1849`) — the frame is then merely
  stretched by `Background::Copy` and looks normal in the log. (b) On the patched path the output
  size comes **only** from `BB_OUTPUT_RES`; set `BB_RENDER_RES` without it and FSR targets
  1920x1080 while presenting 2560x1440. Hardening (a one-shot printf, and falling back to the
  settings' output size) is **not implemented** — recorded as hygiene, no default change.
- **Discriminating log lines** (used to verify every run above):
  `Patches: scene 854x480; UI 1920x1080` (patched only), `Scene resolution: … (live)` and
  `Output resolution: … (live)` (live only), `Upscaler: FSR 3 context 856x480 -> 2560x1440
  (tonemapped frame)` (proves a real upscale on either path), `UI: native composition 2560x1440
  (scene 854x480)`. `Upscaler: FSR 3.1 available` is printed regardless of path — not a
  discriminator, and `Display: host buffer … (live)` prints on the patched run too.

## Correctness

`out/cmp_caps.py` on the four runs' captures (shot 1 and shot 2, `mean` / `%>8` / `%>32` / worst
64x64 tile):

| pair | kind | mean | >32 | worst tile |
|---|---|---|---|---|
| live1 vs live2 (shot 1 / shot 2) | same-path repeat = noise floor | 6.26 / 5.12 | 1.73% / 0.22% | 50.2 / 30.6 |
| patch1 vs patch2 (shot 1 / shot 2) | same-path repeat | 6.13 / 8.01 | 0.53% / 2.35% | 47.4 / 53.7 |
| live1 vs patch1, live2 vs patch2 (both shots) | cross-path | 6.24-8.99 | 0.68-3.03% | 31.4-67.5 |

**Reading: no structural artifact** — no crop, no offset band, no double-scale, no black frame
(a geometry error shows as a large mean with a saturated worst tile, not as this). But the
comparison **cannot** prove pixel equivalence, because the two paths differ in the *input* by
construction: 856x480 (guest-aligned) vs 854x480 render size, guest post at 480p vs at 1080p off
proxies, and a different jitter phase count (`JitterPhases(render_w,target_w)` vs
`(scene_w,1920)`, `:484-486`). Cross-path captures are informational only; within-path repeat pairs
are the real noise floor, and both paths reproduce within it.

## A2 — preset sweep, measured (live mode, both resolutions)

Owner's standing rule for this loop: measure under **real-world** machine conditions — background CPU
load (Spotlight, Telegram, a browser, an unrelated `exo` cluster) is normal and is recorded rather
than treated as a reason to void. Only the hard rules invalidate a run (overlap non-empty, draws <
500, p50 > 40 ms, no `bb-probe` of yours) or a gross outlier on an identical config.
Load during this sweep: **1-min average 7.30 at the start, 10.57 at the end**, with Telegram at ~130%
and a WebKit content process at ~194% of a core; recorded in `out/a2-load.txt`. `overlap-*.txt` empty
on every tag, draws 884-890, `took=1`.

| res | preset | proxy raster | n | p25 | **p50** | p75 | fps | present_wait |
|---|---|---|---|---|---|---|---|---|
| 2560 | 2 | 1506x848 | 6163 | 20.84 | **21.02** | 21.25 | 47.6 | 8.17 |
| 2560 | 3 | 1280x720 | 6428 | 19.88 | **20.11** | 20.39 | 49.7 | 6.74 |
| 2560 | 4 | 854x480 | 13699 (2 runs) | 18.62-18.81 | **18.92** | 19.01-19.23 | 52.9 | 6.02-6.15 |
| 3440 | 2 | 2024x848 | 5645 | 22.38 | **22.75** | 23.43 | 44.0 | 8.04 |
| 3440 | 3 | 1720x720 | 5914 | 21.66 | **21.93** | 22.24 | 45.6 | 6.88 |
| 3440 | 4 | 1146x480 | 6158 | 20.48 | **20.81** | 21.29 | 48.1 | 6.68 |

- **Monotonic in proxy pixel count at both resolutions** — preset 4 (854x480 / 1146x480) is fastest,
  preset 2 (1506x848 / 2024x848) slowest, preset 3 between them, with clean separation (gaps of
  0.6-1.2 ms, spreads inside 0.3 ms). Preset 4 is the right default and is confirmed, not changed.
- **Live mode is insensitive to which raster the *guest* renders**: the guest stays at 1080p on this
  path and only the host proxy changes, so the spread from preset 2 to 4 (2.1 ms at 2560, 1.9 ms at
  3440) is the cost of the proxy resolution, not of guest work.
- 3440 costs **+1.89 ms** over 2560 at the same preset (20.81 vs 18.92) for 34% more output pixels —
  consistent with the FSR chain being output-resolution-bound.
- **One gross outlier, re-run:** the first `2560_a2_p3` read 21.05 ms with a different frame profile
  (`present_wait 0.00`, `ipc_ms 3.62`, `kernel_ms 0.65`); the re-run `2560_a2_p3r2` read 20.11 with
  the normal profile (`present_wait 6.74`, `ipc_ms ~1.2`, `kernel_ms ~3.4`). The re-run is the number
  in the table. Its 21.05 is consistent with the p2/p3 ordering anyway, so nothing hinges on it.
- `2560_a2_p4` is **not** re-run as part of this sweep: A1's two live preset-4 runs (18.81 / 19.02)
  are the same config, same hour, and are the anchor. The `2560_a2_p4 = 24.01` tag from the earlier
  contaminated attempt remains a gross outlier on an identical config (same raster, same preset,
  5 ms slower, `present_wait 0.00`) and is superseded by those two runs, not averaged with them.

**What the earlier "void" was.** The 21:33-21:46 attempt ran while a foreign `exo` cluster was
starting (11 processes, load average 119) and produced `present_wait 0.00` profiles and a 24.01 ms
same-config p4. That was recorded as VOID at the time; under the real-world rule the in-rule tags are
now kept and only the gross outlier is replaced. Both attempts' logs are on disk
(`out/a2-2560.log`, `out/a2-3440.log`, `out/a2-3440b.log`) so the difference is auditable.


## What Stage A decided

- **No default change.** Startup-patched + FSR fails the >= 1 ms gate at 0.24 ms, and the capture
  comparison cannot certify equivalence across the paths. `upscaler=off` stays the shipped default
  (owner decides otherwise).
- The lever this stage was meant to find (remove the live-path 1080p post + copy-backs) exists but is
  **0.24 ms, not 2.9**: the live path was never the cost. The ~2.0 ms is the FSR chain itself, which
  is Stage D's target (`docs/STAGE_D_CUSTOM_UPSCALER.md`).
- Preset ordering **is** established for this build and agrees with `docs/METALFX_MEASURE.md`'s
  matrix: preset 4 fastest, then 3, then 2, at both 2560 and 3440, monotonic in proxy pixel count.
- The startup-patched path is worth **0.24 ms** and costs a 23% draw-count change in the guest, so it
  is not a free win; it stays a bench-only mode (`BB_RENDER_RES` / `BB_OUTPUT_RES`), not a default.
