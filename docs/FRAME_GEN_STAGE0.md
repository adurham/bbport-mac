# FRAME_GEN_STAGE0 — the FG path runs; its colour is wrong, its geometry is fine

Stage 0 of `FRAME_GENERATION_FEASIBILITY.md` §7, run 2026-10-18. Two hidden runs at the standing
config, one with `BB_FRAME_GEN=1` and one without, plus a 16-image `real`/`interp` sequence dump.
No source change, no default change: **FG is still default off and this pass added no gate.**

## Verdict, first

**The path executes end to end, first time it has ever been enabled, and it does not crash,
validate-fail or lose swapchain images.** In gameplay the pairs are complete:

```
Frame generation stats: captured 5041 interpolated 5040 ready 5039 taken 5039 missed 4960
```

5041 captured, 5039 handed to the presenter — two frames out of 5041 dropped from a pair (0.04 %).
The 4960 `missed` are all boot: the counter reads `missed 1999` while `captured 0` (title,
loading, menus — presents with no scene to interpolate), and it does not move again once gameplay
starts.

**Cost and rate:** real frame 18.83 → 21.53 ms p50 (53.1 → 46.4 fps), i.e. **+2.7 ms per real
frame** for OF+FI+the second encode; **presented 92.8 images/s** (two per real frame), measured on a
1512x982 @ **120.00 Hz** panel — so this number is *not* panel-clamped. It lands on the feasibility
estimate (91-96 fps), not on "locked 120".

**Artifact verdict: the geometry is right and the colour is wrong.**

- Interp frames land pixel-exact on the real frame they pair with: best alignment shift is
  **(0, 0)**, and no other shift improves the difference (2 px search). No doubled edges, no tear
  seam, no missing HUD: 0.0 % of pixels differ by more than 32/255 between the pair.
- But the interp image is a **uniform gamma transform of the real frame**: `interp ≈ 255·(real/255)^0.817`,
  residual 0.40/255 over the whole image. Mean luma 49.5 → 65.3. Blacks are lifted, mid-tones
  brightened — visibly washed out, and it is the *same everywhere* (top/middle/bottom bands
  14.3/16.3/16.7, HUD corner 16.6, sky corner 15.8), i.e. one systematic encode error, not a
  spatially varying FI artifact.
- Consequence in motion: **the two images of a pair alternate between normal and washed-out**, at
  the pair rate (46 Hz here). On a static camera that is a brightness flicker; with the camera
  moving it will read as pulsing exposure. That is the artifact to fix first, and it is a
  colour-space parameter, not FI failing.
- **2.81 % of frames cost >25 ms** (max 44.2 ms) with FG on, against **0** in the FG-off run. No
  `Skipping a frame: no swapchain image` warnings, so this is not an acquire stall of the kind
  §8 feared; it is a periodic +7..25 ms hit, consistent with the extra encode/dispatch landing on
  the presenter thread that also does the pair sleep.

**Honest next step: fixable, and cheap to confirm.** One diagnostic run separates the two candidate
causes of the gamma error (FI's own output vs the presenter's `EncodePass`), then one motion test
answers the only question this pass cannot: what interpolation does with a *moving* camera. Estimate
**1-2 days** to a defensible Stage 1 verdict, not the 5-8 days of the whole feature.

---

## 1. The enable path, exactly

Set **`BB_FRAME_GEN=1`** in the environment of the run. That is the only switch; it is sufficient
with the standing config (`BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4`), because FG needs the scaled run.

| step | file:line | what it does |
|---|---|---|
| settings load | `gpu/shim/bbport_settings.cpp:355` | `v.frame_generation = false;` — forced off on every load, so a saved `bbport.ini` **cannot** enable it |
| env override | `gpu/shim/bbport_settings.cpp:348` | `{"BB_FRAME_GEN", "frame_generation"}` in `env_keys`, applied after the force-off at `:356-360` |
| parser | `gpu/shim/bbport_settings.cpp:97` | `frame_generation = i != 0` (so `BB_FRAME_GEN=1`, any non-zero) |
| effect gate | `gpu/upscale/vk_frame_generation.cpp:97` | `Wanted() = supported && !failed && settings.frame_generation` |
| capture | `gpu/upscale/vk_temporal_upscaler.cpp:2338` (`CaptureFrameGenScene`) and `:2371` (`Interpolate` at the display pass) | both `return` unless `Wanted()` |
| present | `gpu/render/presenter.cpp:348-353` | `TakeOutput` → `EncodeInterpolated`; `:624-635` presents the interp image first, then `sleep(interval·0.5)`, then the real frame |
| menu | `gpu/shim/bbport_overlay.cpp:649` | the row only exists when `std::getenv("BB_FRAME_GEN")` is set; greyed out unless `frame_generation_supported` |

Capability line, printed in both runs: `Frame generation: FSR 3.1 available` (`run-fgon.log:66`).
The diagnostics are env-only too: `BB_SEQUENCE_DUMP_TRIGGER=<file>` (the next 16 presented images,
named `real`/`interp`, written as BGRA raw to `out/dump/`) and `BB_FRAME_GEN_ONLY=1`.

Nothing had to be fixed to make the path execute — **no gate change was needed and none was made.**

## 2. The two runs

Standing config, identical apart from the gate:

```
BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173 BB_WARM_SOURCE_USER=$HOME/repos/bbport-mac-native/user \
ROUTE='ly=128,rx=128:5' CAPTURES=3 BB_PASS_BREAK_TRACE=1 BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 \
BB_RENDER_RES=854x480 BB_OUTPUT_RES=2560x1440 [BB_FRAME_GEN=1] \
bash tools/macos/gpu_ab_run.sh <tag> 190
```

| | FG off (`fg0b`) | FG on (`fgon`) |
|---|---|---|
| frame time p50 (steady: `t_s>=60 && draws>=500`) | **18.83 ms** (p95 19.38, p99 19.73) | **21.53 ms** (p95 22.53, p99 32.91) |
| real fps | **53.1** (log: 53.0-53.2) | **46.4** (log: 46.3-46.9) |
| presented | 53.1 | **92.8** (2 per real frame) |
| frames >25 ms | 0 of 6889 | 167 of 5949 (2.81 %), max 44.2 ms |
| FG counters | `captured 0 interpolated 0 ready 0 taken 0 missed 0` (×6) | `captured 5041 interpolated 5040 ready 5039 taken 5039 missed 4960` |
| interpolated frames shown | — | `Frame generation: 6000 interpolated frames shown` |
| swapchain-skips / device loss | 0 / none | 0 / none |
| overlap (`bb-probe` not from this checkout) | empty | empty |
| captures | `out/cap-fg0b-1..3.png` (1512x982) | `out/cap-fgon-1..3.png` (1512x982) |
| sequence dump | — | `out/dump/f000_real … f015_interp` (16 × 1512x850 BGRA) |

Logs: `out/run-fg0b.log`, `out/run-fgon.log`, frame logs `out/frames-fg0b.csv`, `out/frames-fgon.csv`.
The FG-off reference was run twice (`fg0`, `fg0b`); `fg0` measured 18.61 ms / 53.7 fps, so the base
figure is stable to ~1 %.

### Log warnings

Neither run produced an FG-specific warning. `run-fgon.log` has **no** line matching
`Frame generation: (the|invalid|dispatch)`, i.e. no `invalid context`, no
`the interpolation could not be recorded`. The warnings present are the standing set, identical in
both runs: `Present mode Mailbox is not supported, using Fifo`, the format-capability list, the
pipeline-store preload note, and the usual `Object motion:` counters. Nothing new appeared with FG
on.

## 3. What the dumped frames say

16 consecutive presented images, alternating `interp`/`real`, from gameplay at t≈150 s:

```
$ python3 - <<'PY'   # mean luma per dumped image
real   49.4   interp 65.2      real 49.5  interp 65.3   (… all 8 pairs alike)
PY
```

- **Same scene, same HUD.** Real and interp frames show the same Central Yharnam frame, hunter on
  the stairs, full HUD (bars, item counts, souls counter). The port overlay/FPS text is present on
  both — `BbOverlay::Render` runs after FG, as §8 predicted.
- **No ghosting measured, because there is no motion to ghost.** The standing route holds the
  camera still: consecutive *real* frames differ by 0.11/255. Diffing interp against its two real
  neighbours gives 15.79 and 15.75 — the same value — so the interp image is not "half way between"
  two different frames; it is the same picture at a different gamma.
- **The difference is one global curve.** Fitting `interp = 255·(real/255)^g` gives **g = 0.817**
  with residual **0.40/255**; bands and corners all shift by the same 14-17/255; 0.0 % of pixels
  differ by >32. High-frequency energy is 2.92 (real) vs 3.15 (interp): the interp image is not
  blurred, so it is not a resample or a stale-history blend.
- **No tearing.** Any row-wise discontinuity would show as a few-percent band of large diffs; the
  per-band diffs are flat.

## 4. Numbers, with the panel caveat

`system_profiler`: `1512 x 982 @ 120.00Hz` (3024x1964 Retina, built-in Liquid Retina XDR; no
external monitor attached). Presented 92.8 Hz is **below** the 120 Hz cap, so this measurement is
not panel-clamped and the number is the presenter's own: had the panel or FIFO clamped it, the real
rate would have collapsed toward 30 fps instead of settling at 46.4.

Frame-cost accounting for the +2.7 ms: OF at reduced resolution + FI at display size + one extra
`EncodePass` into `frame->interp_image`, per real frame. p99 32.9 ms vs 19.7 ms is the worrying
part: the hitch is periodic, not random, and it sits in the presenter path that also does the
`sleep_until(now + interval/2)` — which is exactly the coupling §8 lists first.

## 5. What Stage 1 should do, in order

1. **Split FI's output from the presenter's encode** (the one diagnostic that decides the gamma
   work). Dump the target image at `vk_frame_generation.cpp` right after the FI dispatch, and dump
   the same image again after `EncodeInterpolated`. If the pre-encode dump is already brighter than
   the real frame, the parameter is FI's (`transferFunction` is hardcoded `SRGB` at
   `vk_frame_generation.cpp:404-405` for 8-bit images that are *already* display-encoded — the
   likely double encode). If it is not, the fault is `presenter.cpp:453-456`, which views the
   interp image as the display **sRGB** format while the real frame's source view uses the image's
   own format. Both are one-line parameter changes; neither touches FI itself.
2. **Then a motion test** — the standing route's still camera makes interpolation quality
   unmeasurable. One route with the camera turning (`BB_SEQUENCE_DUMP_TRIGGER` + a walk) answers
   ghosting, disocclusion and the HUD-paste question that Stage 0's gate actually cares about.
3. **Then the hitch.** 2.81 % of frames >25 ms is the price of two presents per flip on a
   3-drawable swapchain plus a blocking sleep; `present_id`/`present_wait` pacing (§4 of the
   feasibility doc) is the measurement that names it. Do not tune anything before that number
   exists.

## 6. What this pass does not answer

- Interpolation quality under motion (camera still by design of the standing route).
- HDR: this config is SDR, so the hardcoded `SRGB`/`0..1` luminance path in
  `vk_frame_generation.cpp` was not exercised in the mode it breaks.
- Menus/fades as HUD-less sources, and resolution changes (no mode switch in a hidden run).
- Whether the 2.81 % hitch is acquire, encode, or the presenter sleep — only that it exists and is
  periodic.

## Appendix — commands

```
# FG off reference
BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173 BB_WARM_SOURCE_USER=$PWD/user ROUTE='ly=128,rx=128:5' \
CAPTURES=3 BB_PASS_BREAK_TRACE=1 BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_RENDER_RES=854x480 \
BB_OUTPUT_RES=2560x1440 bash tools/macos/gpu_ab_run.sh fg0b 190

# FG on, with the sequence dump armed 150 s in (the file's existence triggers it)
BB_...same... BB_FRAME_GEN=1 BB_SEQUENCE_DUMP_TRIGGER=$PWD/out/seq_trig \
bash tools/macos/gpu_ab_run.sh fgon 190

grep 'Frame generation' out/run-fgon.log
python3 -c "..."   # gamma fit + per-band diff + shift search over out/dump/*.raw (1512x850 BGRA)
```
