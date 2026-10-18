# Stage D — bespoke 2-pass temporal upscaler (`BB_UPSCALE_CUSTOM=1`)

Stage D of `docs/PERF_LOOP.md`. Gate implemented 2026-10-17 in `~/repos/bbport-encmerge-build`
(branch `fix/encoder-merge`), default **off**. Read with `UPSCALER_CONFIG_BENCH.md` (Stage A: the FSR
chain is ~2.0 ms and the live path was never the cost), `ENCODER_CENSUS.md` (B4: the encoder line is
killed), `upscaler.md`.

**Status: implemented, builds clean, measured — the line is CLOSED.** −0.615 ms / +1.8 fps, below the
pre-registered 0.8 ms bar, and the custom path is measurably less stable frame-to-frame than FSR. The
gate ships default-off with its numbers, as `COPY_MERGE` did.

## Why this and not something else

B4's kill leaves the ~2.0 ms upscaler chain as the largest *single* piece of the frame that is known
and attributable. FSR 3.1 preset 4 spends it in eight FFX dispatches: six at render resolution
(cheap, 1/9 of the pixels) and two at **output** resolution — `accumulate` (16+-tap separable Lanczos
history sample + 2 stores) and the separate RCAS sharpening pass. The bespoke version replaces those
with one output-resolution pass (9 cached render-res taps + 4 depth-validated history taps) plus the
already-existing RCAS. The saving is **not** "fewer encoders" — FFX's eight dispatches already share
one encoder (`ENCODER_MERGE_SURVEY.md` Q3) — it is fewer, cheaper output-resolution taps and no
full-res copy.

## Expected value, stated honestly before measuring

- Pass 1 ~0.45-0.85 ms, pass 2 ~0.10-0.20 ms -> **0.6-1.05 ms** replacing 1.5-2.0 ms, i.e.
  **0.9-1.4 ms saved**; **17.2-17.7 ms (56-58 fps)** from 18.5.
- Tempered twice: (a) at 19.37 ms the GPU is only ~65% busy with 6.75 ms idle, so a 1.2 ms GPU saving
  need not deliver 1.2 ms of frame time — plan on **0.6-1.1 ms of p50**; (b) the patched
  upscaler-**off** anchor is 16.50 ms, so even a free upscaler lands at the 60 fps budget with zero
  margin. **Stage D is margin and quality, not the path to 60.** It is worth keeping only if it
  measures >= 1.0 ms *and* survives the capture comparison at the noise floor.
- Decision rule agreed before the run: **< 0.8 ms saved, or visible ghosting on fog/particles/
  transparents, closes the line** and the gate ships default-off with its numbers, as `COPY_MERGE`
  did.

## Design (2 passes, not 3)

Inputs, all already produced every frame at render size on the startup-patched path
(`scaled_session`): scene colour `R8G8B8A8Unorm` (must stay UNORM — `vk_temporal_upscaler.cpp:1549`),
depth `D32Sfloat(+S8)` (same view FFX gets, `:1874`), motion `RG16F` render-res in pixels
previous-minus-current, **unjittered**, with object vectors already merged inside
(`CameraMotion::RecordMotion` `:1916`, `camera_motion.comp:36-59,90-104`), jitter in **render** pixels
from Halton(2,3) (`:487`), and the `{cur proj, prev proj, prev_z}` push-constant payload
(`vk_camera_motion.cpp:227-231`).

**Pass 1 — `gpu/upscale/shaders/upscale_custom.comp` (new, 178 lines).** One dispatch at
`((ow+7)/8,(oh+7)/8)` = 320x180 groups at 2560x1440, 80-byte push constants. Per output pixel:
unjittered render position, then a 3x3 Blackman-Harris weighted reconstruction at the jittered
position (this *is* the upscale — no separate spatial pass), nearest depth with 3x3 foreground
dilation and slope correction, previous **output** position computed in uv from the motion vectors
(never by a single scalar: 2560/854 = 2.998), 4-tap history with per-tap depth validation and the
"nearer history stays" rule, RGB clamp box, `mix(0.98,0.85,moving)*coverage` blend, writes result +
`vec4(result, depth)` history. Every helper is copied verbatim from `taa.comp` (the validated
reference, `:22-73`, `:90-130`, `:159-179`) with a keep-in-sync note. Four barriers, following
`RecordTaa` (`:847-872`, `:916-922`).

**Pass 2 — existing shader, no new code.** The port's `ExtraSharpen(output_image,false,ow,oh,0.0f)`
(RCAS, `taa_sharpen.comp`) plus the existing output blit at `:2055-2079`, entered through the same
group that TAA/MetalFX already use.

Rejected: a render-resolution motion-dilation prepass (the 3x3 gather reads a 1.6 MB image that stays
in cache; a prepass would add a dispatch and a barrier to save work that is not bandwidth-bound).
Revisit only if pass 1 profiles as dominated by the 3x3 loop.

## The gate

`BB_UPSCALE_CUSTOM=1`, read once in the constructor (`vk_temporal_upscaler.cpp:262`), member
`custom_enabled`; predicate `UseCustom() = custom_enabled && scaled_session && Active() && !failed`
(`:298`). `scaled_session` is required — the lever is the startup-patched path.

**Off is a no-op**, argued from the diff: `UseCustom()` false -> the `RunScaled` block condition
(`:1933`) is unchanged, `CreatePipelines` skips the custom branch (`if (custom_enabled &&
scaled_session)`), `EnsureResources` creates nothing and its cache key (`:512-516`) is unchanged, the
`taa_history` allocation condition (`:667-676`) is unchanged, no printf, no scheduler call. The one
unavoidable cost is the 3.3 KB SPIR-V array linked into `libbbgpu`, as for every other shader.

**On**: `settings.upscaler` stays `fsr3` on purpose, so `run.sh`/`patches.py` still patch the guest to
preset 4's 854x480 and `SceneResolution::ForPreset` is identical — that is what makes the A/B
matched-raster. The FSR 3 context is skipped (`:563`). One printf under the gate proves engagement:
`Upscaler: BB_UPSCALE_CUSTOM engaged 854x480 -> 2560x1440 (2-pass)`.

## Implementation

```
 gpu/CMakeLists.txt                       |   1 +
 gpu/upscale/vk_temporal_upscaler.cpp     | 139 ++++++++++++++++++--
 gpu/upscale/vk_temporal_upscaler.h       |  14 +++
 gpu/upscale/shaders/upscale_custom.comp  | 178 (new)
```

Build: `bash tools/macos/build.sh` (this worktree's macOS entry point — the root `build.sh` is the
Linux script and fails at its host-probe compile under Apple clang, pre-existing). Exit 0, no
`: error:` lines; `[1/5] Generating include/bbgpu/shaders/upscale_custom_comp.h`; SPIR-V verified to
have `LocalSize 8 8 1`, bindings 0-3 sampled / 4-5 storage at set 0, and push-constant offsets
`0,8,12,16,32,48,64` = 80 bytes (matching `static_assert(sizeof(Params) == 80)`).

Deviations from the design, all deliberate: `RecordCustom` returns `bool` so a missing pipeline cannot
fall through to a blit of a stale image; pipeline creation is keyed on
`custom_enabled && scaled_session` rather than `UseCustom()` because `CreatePipelines` is one-shot
(`if (merge_pipeline) return;`) and a `UseCustom()`-keyed branch would never build if the runtime A/B
toggle were off on the first frame; reconstruction averages in tonemapped space (as `taa.comp:99-108`)
while the clamp box is RGB (v1, no YCoCg/moments).

Untouched, deliberately: `taa.comp`, `camera_motion.comp`, `vk_object_motion.*`,
`vk_scene_resolution.*` — the FSR and TAA paths stay byte-identical.

## A/B — measured

Run 2026-10-17 22:08-22:25 CDT, matched raster (`BB_RENDER_RES=854x480 BB_OUTPUT_RES=2560x1440`,
preset 4 for both arms so the guest patch and the FSR preset label are identical), interleaved
**base, cust, base, cust**, 190 s each, standing camera, 2 captures per run, no trace (traces perturb
timing). Machine: `pgrep -x bb-probe` empty before every run, load average ~8 at the start of the
series (the earlier contaminated attempt at 22:05 was voided: its `base1` read 20.72 ms with
`present_wait 0.00`, and the canary re-run of the same config read 18.57 with `present_wait 6.86` —
a profile change, not a provider effect).

| tag | arm | n | p25 | **p50** | p75 | fps | present_wait | draws |
|---|---|---|---|---|---|---|---|---|
| 2560_stageD_base1r2 | FSR 3.1 p4 | 6993 | 18.37 | **18.57** | 18.78 | 53.9 | 6.86 | 680 |
| 2560_stageD_base2 | FSR 3.1 p4 | 6738 | 18.60 | **18.99** | 20.03 | 52.7 | 7.58 | 682 |
| 2560_stageD_cust1 | BB_UPSCALE_CUSTOM | 7063 | 18.04 | **18.26** | 18.57 | 54.8 | 6.50 | 679 |
| 2560_stageD_cust2 | BB_UPSCALE_CUSTOM | 7108 | 17.88 | **18.07** | 18.27 | 55.3 | 6.16 | 678 |

- **base 18.780 ms / 53.2 fps; custom 18.165 ms / 55.1 fps; delta -0.615 ms, +1.8 fps.**
- Spreads: base 0.42 ms, custom 0.19 ms; the delta is larger than the base spread but the pairs do
  not overlap (both custom runs beat both base runs).
- Draws are matched (678-682), so this is a like-for-like upscaler comparison, and `present_wait`
  falls on the custom arm (6.16-6.50 vs 6.86-7.58) — consistent with less per-frame GPU work.
- Engagement proven per run: `Upscaler: BB_UPSCALE_CUSTOM engaged 856x480 -> 2560x1440 (2-pass)`.
- All four runs pass the hard validity rules (`overlap-*` empty, `took=1`, draws >= 500, p50 <= 40 ms).

### Verdict: CLOSE the line

Against the bar agreed before the run (**>= 1.0 ms to keep, < 0.8 ms closes it**), 0.615 ms is 62% of
the threshold, and the quality arm is worse:

| pair | kind | mean | >32 | worst tile |
|---|---|---|---|---|
| base1r2 vs base2 (shot 1 / 2) | FSR repeat = noise floor | 8.79 / 7.34 | 5.63% / 0.60% | 54.1 / 42.6 |
| cust1 vs cust2 (shot 1 / 2) | custom repeat | 12.00 / 13.77 | 6.98% / 13.77% | 60.1 / 51.4 |
| all base-vs-cust pairs (8) | the signal | 9.88-14.10 | 4.99-13.85% | 38.2-61.0 |

The **custom path's own run-to-run difference is ~60% larger than FSR's** (mean 12.9 vs 8.1, >32
10.4% vs 3.1%), and the base-vs-custom difference sits *inside* that self-difference — so this tool
cannot separate provider difference from the custom path's own frame-to-frame instability. On a
standing camera that points at shimmer/ghosting, exactly the risk the design flagged (4 history taps,
one clamp box, no luma-instability pass). It is a signal against the gate, not a proof of a defect.

So: **the bespoke upscaler is real and cheaper (0.615 ms), but it is not enough of the frame and it
loses on temporal stability.** Default stays `fsr3`; `BB_UPSCALE_CUSTOM` is kept default-off for
anyone who wants to continue it (the obvious next steps are the luma-instability/shading-change
terms FSR has and this does not, which cost ~0.1-0.2 ms of the 0.615 ms margin).

**Where the remaining gap is, in one line:** 18.17 ms at best here, 16.50 ms with the upscaler off
entirely, 16.67 ms needed. The upscaler chain is now measured at ~1.4-1.7 ms of the frame after this
change, and the other half of the gap is the 6.75 ms of idle that B4/B5 attributed to the recording
thread being late — which is where the next session should go, not back to encoders or upscalers.


### Reproducing it (exact command used)

```
cd ~/repos/bbport-encmerge-build
for t in base1 cust1 base2 cust2; do
  case $t in cust*) g="BB_UPSCALE_CUSTOM=1";; *) g="";; esac
  env BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173 \
      BB_WARM_SOURCE_USER=$HOME/repos/bbport-mac-native/user \
      ROUTE='ly=128,rx=128:5' CAPTURES=2 BB_PASS_BREAK_TRACE=1 \
      bash tools/macos/gpu_ab_run.sh 2560_stageD_$t 190 \
      BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_RENDER_RES=854x480 BB_OUTPUT_RES=2560x1440 $g
done
```

Deliberately no `TRACE=1`: a Metal System Trace perturbs frame time, so encoder counts for this pair
are not part of the timing claim. If they are wanted later, run the trace separately and compare it to
the census' 378.7 encoders/frame — the custom path should be *fewer* (one dispatch replacing eight)
but B4 already established that encoder count is not a frame-time lever.

Quality, done the way this doc recommends: captures through `out/cmp_caps.py` with the same-arm pair
as the noise floor (result above); the `BB_DUMP_TRIGGER` output-dump PSNR test (FSR reads ~45 dB vs
~28 dB for a jittered input on a standing camera, `upscaler.md:164-167`; custom should be >= ~42 dB)
was **not** run — it needs a dump-enabled run, and the capture evidence above already fails the
stability question. **HUD/text sharpness is not a discriminator**: on the patched path the UI is
composed at output resolution *after* the upscale (`:2109-2115`), so both providers give identical HUD
sharpness by construction.


## Known risks (ranked, for whoever measures this)

1. **Reprojection units at non-square 3x** (jitter render-px vs uv, motion px vs uv, the 0.5-texel
   offset, 2.998 vs 3.0). A mistake is silent: a constant 1-3 px ghost offset when panning, invisible
   standing still. The A/B above uses a *standing* camera, so **pan once during the run** or the risk
   will not show.
2. **Ghosting at 3x** — 4 history taps and one clamp box against FSR's 16+ taps plus luma-instability
   and shading-change heuristics. Thin geometry, foliage and particles are where it shows, and that is
   exactly the category the playtest brief kept open.
3. **Perturbing the control path** — `CreatePipelines` is one-shot and `EnsureResources` tears the
   device-synced FSR context down on a cache-key mismatch; two 59 MB history images at 1440p.
4. **Write-heavy 3.69 M-thread pass** across four barriers on KosmoKrisp/TBDR — measure pass 1 in
   isolation with `GpuProfiler::Mark` (already labels `RunScaled`, `:1835-1840`) before blaming the
   upscaler for a frame that did not move.
