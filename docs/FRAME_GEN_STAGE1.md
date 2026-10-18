# FRAME_GEN_STAGE1 — the gamma is in the frame generator, not in the encode

Stage 1a of `FRAME_GENERATION_FEASIBILITY.md` §7, run 2026-10-18. One new diagnostic dump (env-gated,
`BB_SEQUENCE_DUMP_AT` / `BB_FG_DUMP_TRIGGER`), two hidden runs at the standing config, one on a
moving route. **No default changed: FG is still default off and `BB_FRAME_GEN` is still the only
switch.**

## Verdict, first

**The systematic `interp ≈ 255·(real/255)^0.817` curve is already in the frame generator's own
output, before the presenter's `EncodePass` touches it.** The pre-encode dump measures the
interpolation at **luma 63.80** where its real pair is **48.41**; the presented image after the
encode is **63.90**. The encode moves the image by **0.10/255** — the resample, nothing else.

So `presenter.cpp:453-456` (the sRGB view of the interp image) is **exonerated**: it is a
decode/re-encode round trip and therefore an identity. The colour error is on the FI side of the
split — inside the frame generator's path, not in the presenter.

**But the one-line parameter the Stage 0 doc nominated as the likely FI-side culprit —
`transferFunction = SRGB` at `vk_frame_generation.cpp:363/405` — is *not* the mechanism, and no fix
is landed in this pass.** Detail in §3: the portable enum has no "none" (SRGB, PQ, sRGB-2020 scRGB
only), the FSR 3.1 FI pass blends its two sources in **raw** colour (`ffxLerp(fPrevColorGame.fRaw,
fCurrColorGame.fRaw, …)`, `ffx_frameinterpolation.h:122` of both the 1.1.4 and 2.3.0 kits) so the
transfer function only reaches luminance/static-content maths, and no combination of sRGB
decode/encode on the input and output views reproduces a 0.80 exponent — those give 1.0 (identity),
0.42 (bright) or 2.4 (dark), never 0.80. The wash is one pointwise curve of ~1.24 gamma between what
the FI emits and what the display buffer holds, and pinning it needs one more dump: **the FI's own
input** (`scene.color` = the upscaler's UI image, `vk_temporal_upscaler.cpp:2341`). That dump decides
whether the image the FI *reads* is already washed (error upstream, in the upscaler's UI image, which
nothing else ever displays) or whether the FI *pass* writes it washed (error in the output
view/format). It is one `DumpPresented` call in `FrameGeneration::Interpolate` and one run.

## 1. The split, and how it was made

One new dump, in `Presenter::PrepareFrame`, immediately before `EncodeInterpolated`
(`presenter.cpp:349-372`): the `FrameGeneration::Output` image that `TakeOutput` hands over, copied
to `out/dump/f%03d_fi_<w>x<h>_bgra.raw` while it is still in `eGeneral` after the FI dispatch and
before the encode records into `frame->interp_image`. Armed by `BB_SEQUENCE_DUMP_AT=<frame>` (the
same gate arms the 16-image `real`/`interp` sequence dump one frame later, so `f000_fi` and
`f000_interp` are the **same** frame).

The existing sequence dump is, by construction, after the encode (`presenter.cpp:712-731` dumps
`frame->interp_image`), so the two dumps bracket exactly the pass in question:

| image | size | what it is | luma mean (8 frames) |
|---|---|---|---|
| `f000_real` | 1512x850 | display buffer through `EncodePass` | **48.41** |
| `f000_fi` | 2560x1440 | **FI output, before `EncodePass`** | **63.80** |
| `f000_interp` | 1512x850 | the same FI output through `EncodePass` | **63.90** |

Fit `out = 255·(real/255)^g` (median per real value, 8-bit, green channel):

```
fi     (pre-encode)   g = 0.8065   scale 0.988   residual 0.53/255
interp (post-encode)  g = 0.8124   scale 0.995   residual 0.30/255
```

The two are the same curve to within the downscale — **the encode adds nothing**. Per-value map,
real → (fi, interp): 16→(27, 27), 32→(47, 47), 64→(82, 82), 96→(115, 115), 128→(145, 145),
160→(174, 174). One global curve, present before the encode.

Dumps: `out/dump/` (still run, this pass), `out/dump-stage0/` (Stage 0, for the "before" column),
`out/dump-motion/` (moving route).

## 2. Pair metrics, before and after the split

| | Stage 0 (`out/dump-stage0`) | this pass (`out/dump`) |
|---|---|---|
| real luma | 49.53 | 48.41 |
| interp luma | 65.36 | 63.90 |
| FI luma (pre-encode) | *not dumped* | **63.80** |
| mean \|real−interp\| | 15.82/255 | 15.49/255 |
| pixels differing >32/255 | 0.00 % | 0.00 % |
| gamma fit residual | 0.41/255 | 0.30/255 (interp), 0.53 (fi) |
| geometry | best shift (0,0) | best shift (0,0) |

The residual did **not** collapse, because no fix was landed. What did change is the *location*:
the error is now measured on the FI side of the encode boundary, which is the thing this pass was
for. Two different camera positions (luma 49.5 vs 48.4) give the same 0.81 exponent, so this is not
scene-dependent.

## 3. Why `transferFunction` is not the line, stated plainly

- The portable enum (`ffx_vk_portable.h:211-215`) offers **SRGB, PQ, SCRGB** — there is no linear /
  passthrough value to switch to.
- In the FI pass the transfer function reaches `RawRGBToLinear` (`ffx_frameinterpolation_common.h:53`),
  which is used for `RawRGBToLuminance` / perceived-luma / static-content comparisons. The colour
  that is written out is `ffxLerp(fPrevColorGame.fRaw, fCurrColorGame.fRaw, t)`
  (`ffx_frameinterpolation.h:122`, identical in the 1.1.4 and 2.3.0 kits) — a **raw** blend, no
  transfer conversion at all, and the store (`ffx_frameinterpolation.h:174-181`) writes it straight
  out.
- Hardware sRGB views can only produce identity, ×pow(1/2.4) or ×pow(2.4) between the FI's input and
  output. The measured 0.80 is none of those, so the conversion happens *before* the FI reads its
  source, or the image it reads is not what the display buffer holds.
- The one candidate that fits a single pointwise 1.24-gamma step: **the FI's source image**. The FI
  reads `scene.color` = the upscaler's **UI image** (`vk_frame_generation.cpp:236`,
  `vk_temporal_upscaler.cpp:2341`), format `R8G8B8A8Unorm`; the presenter's real path reads
  `displays[address].image`, a **separate** host image in the game's own pixel format
  (`vk_temporal_upscaler.cpp:2263`). Nothing else in the port ever displays the UI image, so a
  colour-space error on it would have been invisible until FG was turned on — which is exactly the
  shape of this bug.

Next diagnostic (30 s of work + one run): dump `scene.color` inside `FrameGeneration::Interpolate`
next to the output dump. `≈63.8` ⇒ the wash is upstream in the upscaler's UI image and the fix is
there; `≈48.4` ⇒ the FI pass or its output view writes it and the fix is in the dispatch's image
formats. Do not guess further before that dump exists.

## 4. Counters and cost (still run, FG on, standing config)

```
Frame generation stats: captured 5059 interpolated 5058 ready 5057 taken 5057 missed 4942
Frame generation: 6000 interpolated frames shown
```

5059 captured → 5057 taken: **two frames** lost from a pair over the whole gameplay run (0.04 %),
and both are the frame after a reset, not a swapchain miss. `missed 4942` is all boot (title,
loads, menus) and stops moving when gameplay starts. No `Skipping a frame: no swapchain image`,
no `Frame generation: (invalid|the interpolation could not be recorded|dispatch)` line.

Frame cost, steady rows (`t_s>=60 && draws>=500`, 5917 rows):

| | Stage 0 FG off | Stage 0 FG on | this pass FG on |
|---|---|---|---|
| p50 | 18.83 ms | 21.53 ms | **21.71 ms** |
| p95 | 19.38 | 22.53 | 22.58 |
| p99 | 19.73 | 32.91 | 27.18 |
| real fps | 53.1 | 46.4 | 45.8-46.4 |
| presented | 53.1 | 92.8 | ~91.6 |
| frames >25 ms | 0 | 167 (2.81 %) | **120 (2.03 %), max 49.7 ms** |

The FG-off reference is Stage 0's `fg0b` run: the whole FG path is behind `BB_FRAME_GEN`, and this
pass changed only that path (one dump block in `PrepareFrame` + the dump gate), so the FG-off numbers
cannot have moved; a fresh reference run was not spent on it. p99 improved from 32.9 to 27.2 ms and
the >25 ms share from 2.81 % to 2.03 % on a quieter scene — the hitch is still there, still
periodic, and still the presenter's `sleep_until(interval/2)` coupling, not a swapchain stall.

## 5. Ghosting, on a moving route

**Ghosting is still unmeasured, and the reason is the route, not the frame generator.** Two motion
runs were made with the pair dump armed mid-run — first the warm-fill `spin` walk
(`rx=128:6 rx=88:0.5 rx=128:5 rx=168:0.5 … ry=40:5 ry=215:5`, `tools/macos/warmfill.sh:95`), then a
route built to hold the right stick over for the whole walk (`rx=40:25 rx=128:2 rx=215:25 rx=128:2`),
FG on, `BB_SEQUENCE_DUMP_AT=6300`, dumps in `out/dump-motion/`. Both dumps landed on a **still
camera**: consecutive *real* frames differ by **0.08-0.09/255**, the same as Stage 0's standing route.
So the pairs in `out/dump-motion/` are the Stage 0 case again — `|interp − real|` 14.85/255, best
shift **(0, 0)**, 0.00 % of pixels >32 — and they say nothing about trailing edges, doubled geometry
or particle smearing. The route's pad writes did not turn the camera under this invocation (the
`rx=` axis values are the ones the verified warm pack uses, so the likely cause is the pad path in
these runs, not the values), and that needs checking before a motion test means anything. **Do not
read these frames as "no ghosting".**

Route for the next attempt (unverified here): `rx=40:25` held across the dump, with
`BB_PAD_FILE`/route verification first — the dump only proves motion if consecutive `real` frames
differ by more than a few /255.


## 6. What remains

- **The gamma fix** — one dump away from the line (§3). Until it lands, FG alternates normal/washed
  at the pair rate and is not visually usable.
- **The pacing hitch** — 2.03 % of frames >25 ms, p99 27 ms. That is two presents per flip on a
  3-drawable swapchain plus a blocking `sleep`, and `present_id`/`present_wait` is the measurement
  that names it (feasibility §4). Not a tuning problem.
- **HUD/menu edge cases** — the port's overlay is drawn before FG and is present on both images; a
  HUD-less source is not used (`hudlessColor` is only bound when the UI phase produced one), and
  menus/fades were not exercised here.
- **HDR** — this config is SDR; the `SRGB`/`0..1` luminance path is not exercised in the mode where
  it breaks.

## Appendix — commands

```
# still, FG on, with the FI-output dump armed at frame 6300
BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173 BB_WARM_SOURCE_USER=$PWD/user ROUTE='ly=128,rx=128:5' \
CAPTURES=3 BB_PASS_BREAK_TRACE=1 BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_RENDER_RES=854x480 \
BB_OUTPUT_RES=2560x1440 BB_FRAME_GEN=1 BB_SEQUENCE_DUMP_AT=6300 \
bash tools/macos/gpu_ab_run.sh fg1d 190

# moving route, same dumps
... ROUTE='rx=128:6 rx=88:0.5 rx=128:5 rx=168:0.5 rx=128:5 rx=40:6 rx=128:5 rx=215:6 rx=128:4 ry=40:5 ry=215:5' \
BB_DUMP_DIR=out/dump-motion BB_FRAME_GEN=1 BB_SEQUENCE_DUMP_AT=6300 bash tools/macos/gpu_ab_run.sh fgm 190

python3 tools/macos/fg_pair_metrics.py out/dump
```
