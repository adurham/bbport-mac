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
4. **A2 (preset sweep, presets 2-4 at 2560 and 3440) is VOID** — a foreign GPU/CPU job appeared mid
   series and the runs' frame profile changed regime before it. All six tags are discarded and no
   preset conclusion is drawn. Re-run when the machine is quiet (see "A2 — void" below).

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

## A2 — void (preset sweep, 2-4 at 2560 and 3440)

Launched 21:29 with `a2_p2=live:2 a2_p3=live:3 a2_p4=live:4` at 2560, then the same three at 3440.
Results as recorded, **all VOID, none reported as measurements**:

| tag | p50 | verdict | why void |
|---|---|---|---|
| 2560_a2_p2 | 21.02 | runs OK but discarded | regime change (see below) |
| 2560_a2_p3 | 21.05 | discarded | frame profile changed: `present_wait 0.00` (A1: 6.15), `ipc_ms 3.5` (1.2), `kernel_ms 0.58` (4.08) |
| 2560_a2_p4 | 24.01 | discarded | **same config as A1 live p4 = 19.02**, 5.0 ms slower 25 min later; not a preset effect |
| 3440_a2_p2 | 23.53 | discarded | ran through the foreign job's start |
| 3440_a2_p3 | — | killed by me at 21:46 | yield |
| 3440_a2_p4 | — | never ran | yield |

Evidence for the yield, and why the numbers are not usable:

- At **21:45:54** eleven `repos/exo/.venv/bin/python` processes appeared (not this worktree, not a
  `bb-probe`); by 21:57 the load average was **119** (1-min) with 11 foreign processes still alive.
  Per the owner rules I killed **only** my own bench chain (the `upscaler_config_bench.sh` wrapper,
  its `gpu_ab_run.sh` / `warm_route.sh` children and the `bb-probe` whose command line names this
  worktree) and left the foreign job running.
- The frame CSV shows the regime change predates the process start: runs from ~21:38 have
  `present_wait_ms = 0.00` and `tick_wait_ms = 0.00` where A1 has 6.02-6.15, `ipc_ms` ~3.5 vs 1.2,
  and `kernel_ms` ~0.6 vs 4.1 — i.e. the machine was already contended while `exo` was warming up.
  A 24 ms same-config frame cannot be a preset effect, and B4's census says 3.68 ms of this frame's
  idle is CPU-late, so CPU contention hits exactly the number being measured.
- The killed series left `bbport.ini` at 21:9 / 3440x1440; it was restored from
  `/tmp/bbport.ini.pristine` to 16:9 / 2560x1440 immediately (tracked file, `git status` clean).

**To finish A2** (one command pair, when `uptime` is quiet and `pgrep -x bb-probe` is empty):

```
bash tools/macos/upscaler_config_bench.sh 2560 a2_p2=live:2 a2_p3=live:3
bash tools/macos/upscaler_config_bench.sh 3440 a2_p2=live:2 a2_p3=live:3 a2_p4=live:4
```

2560 preset 4 needs no new run: A1's two live runs (18.81 / 19.02) are the same config, same series,
and are the anchor. **Gate on the sweep:** report only tags whose `present_wait_ms` is in the A1
band (~6-8 ms for live) — a zero present-wait row means the run is contended and must be re-run.

## What Stage A decided

- **No default change.** Startup-patched + FSR fails the >= 1 ms gate at 0.24 ms, and the capture
  comparison cannot certify equivalence across the paths. `upscaler=off` stays the shipped default
  (owner decides otherwise).
- The lever this stage was meant to find (remove the live-path 1080p post + copy-backs) exists but is
  **0.24 ms, not 2.9**: the live path was never the cost. The ~2.0 ms is the FSR chain itself, which
  is Stage D's target (`docs/STAGE_D_CUSTOM_UPSCALER.md`).
- Preset ordering is **not** established for this build; the pre-existing matrix in
  `docs/METALFX_MEASURE.md` (presets 1-4 at both resolutions) remains the only clean sweep, and the
  A2 numbers above must not be quoted against it.
