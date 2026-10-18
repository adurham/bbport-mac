# Frame generation (FSR 3.1 FG) — feasibility on the macOS port

Feasibility pass, 2026-10-18. No behavior change, no default change, no GPU run.
Scope: what the vendored FidelityFX SDK gives us, what the presenter already does, what is
missing, what it would cost, and what could break.

---

## Verdict, first

**FG is not a greenfield integration in this tree. It is already wired end to end, compiled
into the shipped `libbbgpu.dylib`, and switched off behind `BB_FRAME_GEN`.**

`docs/PERF_LOOP.md` line 169 says "FSR 3.1 FG is compiled in, unwired". That is stale. The
wiring exists in five files (SDK shim, upscaler, presenter, settings, menu) and links into the
current build:

```
$ nm -gU out/gpu-arm64/libbbgpu.dylib | grep -E 'ffxVkPortableFrameGenerationContextCreate|ffxFrameInterpolationDispatch|ffxOpticalflowContextDispatch'
00000000003922a4 T _ffxFrameInterpolationDispatch
000000000038f9e0 T _ffxOpticalflowContextDispatch
000000000034b228 T _ffxVkPortableFrameGenerationContextCreate
(103 ffx* symbols total in the dylib; same in ~/repos/bbport-mac-native/out/gpu-arm64/libbbgpu.dylib)
```

The same log line the campaign quotes — `Frame generation: FSR 3.1 available` — is a **device
capability** claim (`fsr3ComputePrerequisites && timelineSemaphore`), not a statement that the
effect has ever run. In every run log on disk the FG counters are zero:

```
$ grep -h 'Frame generation' out/*.log | sort | uniq -c
  15 Frame generation stats: captured 0 interpolated 0 ready 0 taken 0 missed 0
   3 Frame generation: FSR 3.1 available
```

Nothing has ever executed the path. So the honest question is not "can we integrate FG" but
"why was it hidden, and what does it take to trust it". That is what this document answers.

Two headline answers up front:

1. **The usual FG blocker is absent.** `VK_KHR_present_id` / `VK_KHR_present_wait` are both
   advertised *and implemented* by KosmicKrisp (evidence in §4). The port does not use them,
   which is the real pacing gap — but it is an application-side gap, not a driver task.
2. **The remaining work is artifacts and pacing, not plumbing.** Ballpark 4-7 working days,
   gated on whether the interpolated image is acceptable at 854x480 → 2560 or 3440. If the
   artifacts cannot be fixed, the answer is no regardless of how good the pacing is.

---

## 1. Vendored SDK inventory

Vendored at `gpu/third_party/fsr-vulkan` (submodule `FireBurn/FSR-Vulkan`, commit
`c64f093`; identical in `bbport-mac-native` and `bbport-encmerge-build`).

| item | value |
|---|---|
| Provenance | `upstream/ffx-1.1.4` = AMD FidelityFX SDK **v1.1.4**, commit `c6efa6b` (2025-05-08) |
| FG components present | `frameinterpolation` (**FI 1.1.3**), `opticalflow` (**OF 1.1.2**) |
| Second FG copy | `upstream/ffx-2.3.0` = FSR SDK **v2.3.0**, FI/OF **3.1.6** (2026-06-24) |
| Shader blobs | `generated/ffx-1.1.4/vk/fsr3framegeneration/` — embedded SPIR-V permutation headers (reconstruct+dilate, disocclusion mask, inpainting, optical-flow compute/filter/SCD/luminance pyramid), 16-bit and wave64 variants |
| Vendor patches | 9 documented in `UPSTREAM.md`; two matter here: `ffx_frameinterpolation_shaderblobs.cpp` FP16 table selection, and the descriptor-pool storage-buffer count fix without which FI fails descriptor allocation |
| Port-side shim | `src/ffx_vk_fsr3_portable.cpp` (1025 lines) — the portable C API on top of the 1.1.4 host |
| Public API | `include/ffx_vk_portable.h`: `ffxVkPortableFrameGenerationContext{Create,Prepare,RecordDispatch,Destroy}` + 3 validators |

### Is it built?

Yes — both FG implementations compile, and the one the port uses is linked:

| target | built in `out/gpu-arm64` | contains |
|---|---|---|
| `libffx_vulkan_portable.a` | yes (linked by bbgpu) | portable C API; the real FG impl comes from `ffx_vk_fsr3_portable.cpp` (`FFX_VK_PORTABLE_HAS_FSR3_UPSCALER=1` selects it over the stubs at `ffx_vk_portable.c:468-548`) |
| `libffx_vulkan_fsr3_host_1_1_4.a` | yes | `ffxFrameInterpolation{ContextCreate,Prepare,Dispatch}`, `ffxOpticalflow*` |
| `libffx_vulkan_fsr3_vk_backend_1_1_4.a` | yes | Vulkan backend for both |
| `libffx_vulkan_fsr3_vk_framegeneration_3_1_6.a` | **was not built; I built it** | the SDK 2.3 FI 3.1.6 + OF + 3.1.6 blobs behind `ffxVkFsr3_3_1_6FrameGenerationContext*` |

The 3.1.6 target is `EXCLUDE_FROM_ALL` and nothing links it, so it had never been compiled.
Build check (this pass, no source change):

```
cmake --build out/gpu-arm64 --target ffx_vulkan_fsr3_vk_framegeneration_3_1_6 -j 8
[9/10] Linking CXX static library third_party/fsr-vulkan/libffx_vulkan_fsr3_vk_framegeneration_3_1_6.a
-> 1,891,024 bytes, 6 exported entry points (Create/RecordPrepare/RecordDispatch/RetireFrame/Destroy/GetMemoryUsage)
```

It compiles clean against our setup (warnings only, all inside AMD's own 2.3 sources:
`-Wmissing-field-initializers`, `-Wunknown-pragmas`). **It is not on the port's path and should
not be adopted now**: the two implementations keep separate symbol namespaces precisely because
the 2.3 `FfxInterface` ABI differs from the 1.1.4 backend, and the 3.1.6 scaffold cannot be
linked beside the 3.1.5 upscaler scaffold without duplicate private helpers (CMake comment at
`CMakeLists.txt:516`). The port's FG runs on **1.1.4 / FI 1.1.3 / OF 1.1.2**.

Net: **FG component present = yes; buildable = yes (both); linked and wired = yes, through the
1.1.4 portable shim.**

---

## 2. Integration surface, as it actually stands

| piece | where | state |
|---|---|---|
| SDK entry points | `gpu/third_party/fsr-vulkan/src/ffx_vk_fsr3_portable.cpp:719` create, `:845` prepare, `:925` dispatch, `:995` destroy | real: creates OF + FI contexts, runs `ffxOpticalflowContextDispatch` then `ffxFrameInterpolationPrepare` then `ffxFrameInterpolationDispatch` |
| Port FG object | `gpu/upscale/vk_frame_generation.{h,cpp}` (119 + 476 lines) | `CaptureScene`, `CaptureHudless`, `Interpolate`, `TakeOutput`, context rebuild on size/format change, stats counters |
| Scene capture | `vk_temporal_upscaler.cpp:2494` (`CaptureFrameGenScene`) | at end of the upscaler's scaled run: output-size colour (`ui_image`), render-size depth (copied immediately, because the game reuses its depth buffer before the display pass), render-size RG16F motion, jitter, frame ms, near/far/fov, camera basis |
| HUD-less capture | `vk_temporal_upscaler.cpp:2364` via `RedirectColor` | at the **first UI draw** into the output-size image: the image is copied to a private `hudless` (no UI mask texture; "what is there now is the frame without its UI"). A/B kill switch: `BbToggle::Disabled(1u << 30)` in `vk_frame_generation.cpp:342` |
| Interpolation | `vk_temporal_upscaler.cpp:2499` (`OnDisplayPassStart`) | recorded into the draw stream at the game's display pass, before the next frame's work |
| Present path | `presenter.cpp:346-354`, `EncodeInterpolated` at `421-516`, `Present` at `599-641` | takes one output per real frame, encodes it through the same `EncodePass` into a per-`Frame` `interp_image`, presents it **first**, then the real frame |
| Gate | `bbport_settings.cpp:353`, `bbport.ini:28`, `bbport_overlay.cpp:649` | `frame_generation` is forced to `false` on load and reset from settings; only `BB_FRAME_GEN=1` turns it on; menu row hidden unless the env var is set |
| Diagnostics | `BB_FRAME_GEN_ONLY`, `BB_SEQUENCE_DUMP_TRIGGER=<file>`, the stats line, `BB_TOGGLE_FILE` bit 30 | already written for exactly the triage this needs |

The presenter already does the two hard bookkeeping things correctly:

- the interpolated present signals nothing; the **real** present is the one that signals
  `frame->present_done` (`presenter.cpp:705-709`), so the frame fence still means "both images
  of the pair are done";
- the interp image is created lazily, destroyed with the frame on resize/HDR change
  (`RecreateFrame`, `presenter.cpp:186-193`).

---

## 3. Have / lack

| requirement | state | evidence / note |
|---|---|---|
| FSR 3.1 FG effect (FI + OF) compiled | **have** | §1; 103 ffx symbols in `libbbgpu.dylib` |
| Portable API for FG, implemented (not stubbed) | **have** | `ffx_vk_fsr3_portable.cpp:719-1025`; stubs are `#if !defined(FFX_VK_PORTABLE_HAS_FSR3_UPSCALER)` |
| HUD-less colour (a UI mask) | **have, coarse** | capture-at-first-UI-draw, not a mask. Correct only if "everything drawn before the first UI draw is scene". Menus/fades that fill the screen are captured as HUD-less backgrounds |
| Per-frame motion vectors | **have** | render-size `R16G16_SFLOAT`, same image the upscaler consumes, `motionVectorScale = {1,1}` consistently (`vk_temporal_upscaler.cpp:1482`, `:2276`, `vk_frame_generation.cpp:397`) |
| Per-frame depth | **have** | render-size, copied at capture time into a private image |
| Jitter / frame time / camera basis | **have** | jitter from the upscaler (sign honoured), `frame_ms`, near/far, vertical FOV, position/up/right/forward from `CameraMotion::CameraVectors` |
| Distortion field | **lack** | not supplied; FI then uses its neutral 1x1 field. Only matters if post-processing displaces UVs (Bloodborne's vignette/CA do not) — low risk |
| HDR correctness | **lack** | `transferFunction` hardcoded `SRGB`, `min/maxLuminance` hardcoded `0/1` (`vk_frame_generation.cpp:363,404-405`). With HDR10 on the images are `A2B10G10R10` PQ — FI 1.1.3 does accept `R10G10B10A2_UNORM` (`upstream/ffx-1.1.4/sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp:369-371`), so this is a wrong-parameter bug, not a format wall |
| **present_id / present_wait in the driver** | **have** | see §4 |
| present_id / present_wait in the port | **lack** | no `present_id` anywhere in `gpu/`; `VkPhysicalDevicePresentWaitFeaturesKHR` is not in the device `pNext` chain (`instance.cpp:170-183` query, `:326+` enable); `Swapchain::Present` sends no `VkPresentIdKHR` (`swapchain.cpp:282-296`) |
| Pacing loop with a latency budget | **lack** | pacing is a `sleep_until(now + interval/2)` **on the presenter thread** (`presenter.cpp:634-637`), with the interval smoothed from wall-clock presents. No queue-depth bound, no budget knob, no measurement |
| Swapchain surface for the callback | **partial** | `AcquireNextImage`/`Present` only; needs `PresentId`, `WaitForPresent(id, timeout)`, and an in-flight count. `Frame` already carries `interp_image`/`has_interp` |
| Artifact triage tooling | **have** | `BB_SEQUENCE_DUMP_TRIGGER` dumps 16 alternating `real`/`interp` images; `BB_FRAME_GEN_ONLY`; stats (`captured/interpolated/ready/taken/missed`) |
| Any measured FG number | **lack** | zero counters in every log on disk; no run has ever enabled it |

---

## 4. present_wait / present_id in KosmicKrisp — available, unused

This is normally where an FG port dies. It does not here.

**Advertised** (`deps-arm64/src/mesa-kosmickrisp/externals/mesa/src/kosmickrisp/vulkan/kk_physical_device.c:150-155`):

```c
#ifdef KK_USE_WSI_PLATFORM
      .KHR_present_id = true,
      .KHR_present_id2 = true,
      .KHR_present_wait = true,
      .KHR_present_wait2 = true,
#endif
```
plus the matching feature entries `.presentId/.presentId2/.presentWait/.presentWait2 = true` at
`:382-394`. `KK_USE_WSI_PLATFORM` is gated on `VK_USE_PLATFORM_METAL_EXT`
(`kk_physical_device.h:101-104`), and the build defines it — visible in
`build-arm64/.../compile_commands.json` (`-DVK_USE_PLATFORM_MACOS_MVK -DVK_USE_PLATFORM_METAL_EXT`).

**Implemented** in the Metal WSI (`src/vulkan/wsi/wsi_common_metal.c`):
- `wsi_metal_swapchain_queue_present` (line 673) installs a per-present completion handler when
  `present_id && present_wait_enabled`;
- `wsi_metal_present_complete` (656) advances `present_info->present_id` and broadcasts a
  monotonic condvar;
- `wsi_metal_swapchain_wait_for_present` (718) waits on that condvar with the caller's timeout;
- the drawable side is real Metal: `wsi_metal_layer_add_presented_handler` →
  `[drawable addPresentedHandler:]` (`wsi_common_metal_layer.m:284-292`).
- Surface capabilities report `presentId2Supported` / `presentWait2Supported = VK_TRUE` (100-108).

**What the port must do to use it** (small, contained):
1. `instance.cpp`: add `vk::PhysicalDevicePresentWaitFeaturesKHR` to the query set and to the
   device `pNext` chain (`presentWait = true`).
2. `swapchain.cpp`: pass `VkPresentIdKHR` in `VkPresentInfoKHR.pNext` with a monotonic counter;
   add `bool WaitForPresent(u64 id, u64 timeout_ns)` calling `vkWaitForPresentKHR`.
3. Mesa needs nothing else: `wsi_common.c:511` sets `present_wait_enabled` from
   `device->enabled_features.presentWait` alone — no special swapchain create flag.

So: **not a driver task, not a blocker.** It is ~60-80 lines in two files, and it is what turns
the current blind `sleep` into a measurable, bounded pacing loop.

---

## 5. Latency budget and expected presented rate (240 Hz)

Measured base (perf campaign, do not re-walk): 18.4 ms at 2560 (54.3 fps), ~19.5 ms at 3440
(51.2 fps). Panel tick = 4.167 ms.

FG adds real GPU work (OF at reduced resolution + FI at display resolution + one extra
`EncodePass` for the interpolated image, all inside the same 302-commit frame). A defensible
estimate for the M4 Max, to be replaced by measurement in stage 0:

| case | real frame | real fps | presented | pair interval | ticks per image |
|---|---|---|---|---|---|
| 2560, no FG | 18.40 ms | 54.3 | 54.3 | — | 4.4 |
| 2560, FG +2.5 ms | 20.90 ms | 47.8 | **95.7** | 10.45 ms | 2.5 |
| 2560, FG +3.5 ms | 21.90 ms | 45.7 | **91.3** | 10.95 ms | 2.6 |
| 3440, FG +3.5 ms | 23.05 ms | 43.4 | **86.8** | 11.53 ms | 2.8 |

Expected presented rate at 2560: **91-96 fps**, i.e. roughly double the real rate, not a locked
108. Note the feedback loop FG introduces: the more the interpolation costs, the lower the real
rate, the longer the pair interval, the more latency FG adds. Every millisecond of FG GPU cost is
paid twice (once in rate, once in latency).

Latency, honestly. The interpolated image is by construction *between* two real frames, so it is
older than the newest real frame. The only choice is where in the pair the two presents land
relative to vblank:

| pacing scheme | real frame's on-screen delay | cadence |
|---|---|---|
| **A. current**: present interp, `sleep(interval/2)`, present real | +~9.2 ms (2-3 ticks) | even |
| B. present back-to-back, let 240 Hz FIFO take them 4.17 ms apart | +1 tick (4.17 ms) | uneven: second image held ~14 ms → a stutter every pair, exactly what FG is for |
| C. **present-id paced**: id per present, wait for id-2 before submitting, 2 pairs in flight | +~9.2 ms, deterministic | even, and the queue depth is bounded so no extra latency creeps in from a deep swapchain |

A is what the code does today (blind, unmeasurable, and it blocks the presenter thread for half
a frame). C is the target. B is a trap. **FG cannot go below ~+9 ms of newest-content latency at
54→95 fps; that is the price of the feature, not a defect to be fixed.** On a 240 Hz panel that
is 2-3 refreshes, which is why the panel helps perceived smoothness but cannot hide the lag.

Budget check: 240 Hz gives 4.167 ms of slack per tick; a 9.2 ms pair interval means each image
sits for 2-3 ticks, so the panel is not the limiter — the frame rate is. No tearing risk under
FIFO; `present_id` pacing is what keeps the pair evenly spaced instead of bunching.

---

## 6. Effort

| work item | size | confidence |
|---|---|---|
| Wiring | **0 — already done** | verified by symbols + code |
| Stage 0 triage (run it, capture, decide if it is fixable) | 2-4 h | high |
| present-id/present_wait pacing (instance + swapchain + presenter, remove the sleep) | 1-1.5 d, 150-250 lines | high |
| HUD-less / artifact fixes (colour space, capture timing, reset/first-frame, menu cases) | 2-4 d, unknown split | **low — this is the risk** |
| HDR correctness (transfer function, luminance from settings) | 0.5 d | high |
| UX/plumbing (menu row, ini default, 3440, exclusive fullscreen/mode switch, aspect) | 1 d | medium |
| Measurement + captures + doc | 1 d | high |
| **Total to a shippable, default-off→candidate feature** | **5-8 working days** | gated on stage 0 |

Files touched, expected: `gpu/upscale/vk_frame_generation.cpp` (the bulk), `gpu/render/presenter.cpp`,
`gpu/vulkan/swapchain.{h,cpp}`, `gpu/vulkan/instance.cpp`, `gpu/shim/bbport_settings.cpp`,
`gpu/shim/bbport_overlay.cpp`. No SDK changes expected; the 1.1.4 shim is already correct in
structure (it just has never been trusted).

---

## 7. Staged plan

**Stage 0 — does it run, and what does it look like (no code).**
Fixed camera, one hidden run, `BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_FRAME_GEN=1`, standing
route. Read: the stats line (`captured` vs `interpolated` vs `taken` — a gap means frames are
dropped from the pair), `BB_SEQUENCE_DUMP_TRIGGER` for 16 alternating `real`/`interp` images,
and the existing `BB_FRAME_TIMELINE` for pacing. Gate: if the interp image is unusable at 854x480
internal, stop here and write it off — everything downstream is wasted effort.

**Stage 1 — pipeline wired and validated, still off by default.**
Fix what stage 0 names, starting with the cheapest known-wrong things: HDR transfer function,
first-frame/reset handling, capture timing versus the display pass. Validate on the fixed camera
with pixel comparison of the dumped sequence. No present change yet.

**Stage 2 — pacing and latency, behind `BB_FRAME_GEN=1`.**
Add `presentWait` + `VkPresentIdKHR` + `Swapchain::WaitForPresent`; replace the presenter sleep
with id-paced submission (scheme C above); keep the sleep path behind a toggle for A/B. Measure:
presented interval p50/p99, real fps delta, added latency (input→photon proxy from the frame
timeline), and whether the presenter-thread sleep was silently costing base rate.

**Stage 3 — HUD/UI and the rest of the surface.**
Menus and fades (`UiComposition::MovieShader` classification is what decides "UI" — the title
menu already needed a fix list for flicker), letterbox/aspect at 3440, exclusive fullscreen and
mode switches (swapchain recreate → `EnsureContext` rebuild → must reset history, not interpolate
across the break), HDR on/off mid-session. Then the decision: menu row visible, ini default, or
keep hidden.

---

## 8. What could break the current presenter

| risk | why | mitigation |
|---|---|---|
| **Presenter-thread sleep** | `Present` blocks the present thread for `interval/2` (the queue that serves the next flip's present), so the next flip's acquire/present waits behind it. With `BB_FRAMES_AHEAD=1` guest pacing, this can couple into base rate — unmeasured | stage 2 replaces it with `present_id` pacing; measure base fps with FG on vs off before/after |
| **Two presents per flip on three drawables** | KK caps the swapchain at `minImageCount = maxImageCount = 3` (`wsi_common_metal.c:65-66`), so the port's `minImageCount+1` clamps to **3** images, and the layer sets `maximumDrawableCount = 3` (`wsi_common_metal_layer.m:168`). FG presents **two** images per flip: with one frame of run-ahead at most one drawable is free. This is the sharpest structural risk in the feature — an acquire stall shows up as `Skipping a frame: no swapchain image`, i.e. dropped pairs | count skip warnings in stage 0; if it bites, the fix is either a KK swapchain-capability change (driver side) or a presenter-side rule that presents of a pair may reuse the just-freed image |
| **Frame accounting** | `frame->present_done` is signalled only by the real present (correct today); any change that signals on the first present lets the next frame rewrite the interp image while it is still being scanned out | keep the fence semantics; assert in stage 2 |
| **`BbConfirm` modal / port overlay** | Safe by construction: the port's modal and FPS counter are drawn in `RecordPresent` onto the swapchain image, *after* FG has consumed `ui_image`. Both images of a pair get it | none needed; verify visually in stage 0 |
| **Exclusive fullscreen / mode switch** | swapchain recreate changes extent and surface format → `EnsureContext` rebuild; stale interp targets and history must be dropped, or the first pair after a switch interpolates across a resolution change | reset on recreate; dump the first 4 presents after a switch |
| **HDR** | transfer function hardcoded SRGB while HDR10 images are PQ, and `min/maxLuminance` are 0/1 → wrong-looking interp under HDR | 0.5 d fix, or hard-disable FG when HDR is on |
| **Menus/fades** | HUD-less capture assumes "before the first UI draw = scene". A full-screen menu background becomes the "HUD-less" frame and the UI is pasted over unmoved — acceptable for static menus, bad for animated ones | stage 3, per-case |
| **Upscaler preset Off** | FG needs the scaled run to exist; already excluded (`display.width` check in presenter, and the menu text says so) | none |

---

## 9. Blockers

1. **No blocker in the SDK or the driver.** FG component present, compiled, linked; present_wait
   and present_id available and implemented in KosmicKrisp.
2. **The real blocker is unmeasured quality.** The feature has never run. The gating question is
   whether FI over a 854x480-internal, output-size-composited frame (HUD pasted from the current
   frame) is acceptable in motion. Nothing in this pass can answer that without a run.
3. **Secondary:** the pacing loop is a blind sleep on the presenter thread, so today's FG would
   also be the least measurable configuration. Fix pacing only after stage 0 justifies it.
4. **Conditional driver item (not present_wait):** KosmicKrisp caps a swapchain at 3 images
   (`minImageCount = maxImageCount = 3`), and FG wants two presents per flip. If stage 0 shows
   acquire stalls, the first FG-side driver task is that cap, not the present extensions.
5. **Not on the critical path:** the SDK 2.3 / FI 3.1.6 library builds clean but is deliberately
   not linked (ABI and duplicate-private-helper constraints). Do not adopt it as part of this
   work; the 1.1.4 path is the one the port is built around.

---

## Appendix — commands used for this pass

```
nm -gU out/gpu-arm64/libbbgpu.dylib | grep ffx                          # 103 symbols, FG present
nm -gU out/gpu-arm64/third_party/fsr-vulkan/libffx_vulkan_fsr3_host_1_1_4.a | grep FrameInterpolation
cmake --build out/gpu-arm64 --target ffx_vulkan_fsr3_vk_framegeneration_3_1_6 -j 8   # builds clean
grep -rn 'present_wait|present_id' deps-arm64/src/mesa-kosmickrisp/.../src/kosmickrisp/vulkan
grep -h 'Frame generation' out/*.log | sort | uniq -c                   # counters all zero
```

Build artifact created by this pass (no source change): `out/gpu-arm64/third_party/fsr-vulkan/
libffx_vulkan_fsr3_vk_framegeneration_3_1_6.a`.
