# Does FSR get correct motion vectors? (2026-10-21, branch `probe/motion-correctness`)

M4 Max, macOS 27.2, hidden runs only (`BB_WARM_ONLY=1`), 3440x1440, FSR 3.1 preset 4
(1146x480 render), frame_generation=0, the owner's ini values (sharpen 0.40, jitter 1,
reactive 0). Checkout `bbport-mfxq-build` (dd9705b + the capture-only dump patch).

## Answer

**No. Moving objects get no object vectors at all.** The port computes them correctly, but they
are written 8 bytes off inside the motion attachment. The validity flag and depth are lost, so the
compose pass rejects every one. FSR then reads the camera-only field everywhere. On a moving
character that vector is wrong by a median **1.75 px/frame**. That is a smear on exactly the moving
edges the owner describes.

| | before (a2, shipped path) | after (f1, `BB_MOTION_SLOT_FIX=1`) |
|---|---|---|
| object attachment: x,y non-zero | 0 px | 550,080 px (all) |
| object attachment: valid (z > 0.99) | 13-136k px, garbage (z is the vector's x) | 492k px |
| object attachment: w == scene depth | 0-1 px | 550,080 px (all) |
| compose accepts an object vector | **0-6 px/frame** | ~18k moving-object px/frame |
| moving-object px with correct resolved vector (err < 0.5 px, cos > 0.9) | **0.01 %** | **100 %** |
| luma warp residual on moving objects, resolved vector (lower = better) | 0.0192 (= camera-only 0.0192) | **0.0092** (= object vector) |
| FSR output, character crop, mean \|grad\| / p99 | 0.00566 / 0.0432 | 0.00597 / 0.0446 (+5.5 % / +3.3 %) |

The crop A/B is two runs on the same route at the same session age, so the poses match closely
but not bit for bit. The owner's eyes are the remaining gate. Crops:
`docs/motion-correctness/{before,after}_f006_fsr_crop.png`.

## Task 1: the moving capture

- Driver: `tools/macos/motion_correctness/drive.sh`.
  - Title Continue presses in a closed loop on the frame log's draw count.
  - Then the walk `ly=0,rx=60:8 ly=0,rx=200:8` (walk forward while turning), with
    `BB_CAMERA_LOG=1` as the movement oracle.
  - `BB_DUMP_TRIGGER` is armed 3 s into the walk and dumps 10 consecutive frames: input,
    depth, motion, objects, FSR output, plus `Dump camera:` lines.
- Why the earlier MetalFX captures were static: their dump fired during the studio logos and
  the loading screens, before the walk applied. That capture's `final` frame is the FROM SOFTWARE
  splash. It was a timing fault in the driver, not a pad fault. The scripted pad does move the
  camera: the position changes from 16.4 s after the walk starts.
- Attempts:
  - **a1** was voided by the free+inactive < 4 GiB watchdog (other load on the Mac).
  - **a2** succeeded: 799 draws/frame, 10 frames, `prev_view[i] == view[i-1]` (consecutive).
- Motion stats for a2 frames 0-9:

| metric | range |
|---|---|
| camera move | 0.029-0.038 units/frame |
| camera rotation | 0.38-0.49 deg/frame |
| resolved motion median | 0.71-1.03 px/frame |
| resolved motion p99 | 3.8-5.0 px/frame |
| resolved motion max | 4.1-5.3 px/frame |
| pixels with a non-zero vector | 549,828-550,080 of 550,080 |

- Asserts: 0 `gcn_to_vk.cpp:422` and 0 `cp_graphics.cpp:203` in every run (a1-a6, f1).

## Task 2

### (a) Do moving objects have non-zero vectors in the image FSR reads?

They are non-zero, but they are only the camera's.

- On 100 % of pixels the resolved image equals the camera-only field (median 0.0015-0.0039 px,
  p99 < 0.0065 px). That field was recomputed offline from depth plus the dumped matrices,
  mirroring `camera_motion.comp`.
- On moving-object pixels the vectors point the wrong way: median cos 0.44 against the true
  object vector, median error 1.43-1.75 px.

### (b) Does the attachment layout match what the port expects?

**Refuted as a shader bug; confirmed as a layout bug.**

- **The shaders are correct.**
  - The fragment epilogue writes (dx, dy, valid, FragCoord.z) to location 7:
    `emit_spirv_special.cpp:218`, SPIR-V verified in `0x9e3394c8_1.spv`.
  - The driver's MSL is correct too (`MESA_KK_DEBUG=msl`):
    `out.color_7.xyzw = float4(dx, dy, valid, gl_FragCoord.z)`.
- **The dump is shifted by two floats.**
  - x,y read exactly 0.
  - z,w hold the true (dx, dy): they match the camera field to 0.003 px on static geometry.
  - The object's own vector wins the warp test on the character: residual 0.0059 vs 0.0094 for
    zero.
- **What the Metal debug layer reports:** 316,140 × `For color attachment 6, the
  renderPipelineState pixelFormat must be MTLPixelFormatInvalid, as no texture is set`.
- **Root cause:** `PipelineCache::RefreshGraphicsKey`, `gpu/render/pipeline_cache.cpp:1122-1128`.
  - The loop clears the formats of bound-but-unwritten targets only for
    `cb < key.num_color_attachments`, where the count is `bit_width(fs mrt_mask)` = 6.
  - The game binds a 7th target (slot 6, RGBA16F) that this pixel shader does not write, so
    slot 6 keeps its format.
  - The motion block (`:1142-1153`) then raises `num_color_attachments` to 8.
  - `pipeline.cpp:541` therefore builds the pipeline with formats
    `[RGBA8 x2, RGBA8 sRGB, RGBA8, B10G11R11, RGBA16F, RGBA16F, RGBA32F]` (`BB_MOTION_FMT_LOG`).
  - The pass, built from the mrt mask (`raster_targets.cpp:149-153`), binds no texture at slot 6.
- **How Metal ends up shifting the output:** KosmicKrisp copies the declared formats straight
  into the MTLRenderPipelineDescriptor (`kk_shader.c:1478-1483`). Metal lays out the tile from
  the pipeline formats, so the attachment-7 output lands 8 bytes off.
- **Earlier notes this explains:**
  - The "z/w match neither layout" note.
  - Why compose accepted 0 pixels in bench/motion-fast.
  - Why every A/B of object motion so far measured nothing.
- `vk_object_motion.cpp` and `vk_camera_motion.cpp` are fine.

### (c) Do the camera vectors match the camera delta?

**Yes.**

- With the compose accepting nothing, the resolved field equals the camera-only reprojection of
  the dumped `view`/`prev_view`/`proj`/jitter on every pixel (median ≤ 0.004 px).
- The direction is right on static geometry:

| vectors used for the warp | luma residual |
|---|---|
| resolved | 0.0039 |
| negated | 0.0086 |
| zero | 0.0062 |

- After the fix, 96.7 % of pixels still equal the camera field. The other 3.3 % are the accepted
  object pixels.

### (d) Is the reactive mask set on the pixels that change?

- **It is not set on any pixel.** The owner's ini has `reactive=0`, so `ReactiveOn()`
  (`vk_temporal_upscaler.cpp:290`) is false and FSR gets an empty `reactiveMask`
  (`vk_temporal_upscaler.cpp:2138-2142`). The mask image is never generated, so there was
  nothing to dump.
- It is not the mechanism here: a reactive mask is for transparency and particles. Opaque moving
  geometry needs correct vectors, which (b) is about.

## Task 3: the fix, and its bar

- **The fix:** clear the unwritten slots up to the motion slot before raising the count.
  `pipeline_cache.cpp` in the `if (sel.motion)` block:
  `for (cb = key.num_color_attachments; cb < mv; ++cb) key.color_buffers[cb] = {}, key.write_masks[cb] = {};`
- **Gating:** it is gated behind `BB_MOTION_SLOT_FIX=1` (default off, per the no-defaults rule).
  It changes the motion pipelines' keys, so their pipelines recompile once.
- **Bar (i):** moving-object pixels with a correct vector go from 0.01 % to **100 %** (f1, 10
  frames, ~18k px/frame). Met.
- **Bar (ii):** the FSR output's character crop gains +5.5 % mean |grad| and +3.3 % p99. The
  visible A/B crops are in `docs/motion-correctness/`. **The owner's visible check is the
  remaining gate.**
- **Shipping:** turn the line on unconditionally. A driver-side guard in KosmicKrisp (an
  attachment slot with no view → `MTLPixelFormatInvalid`) would harden every pipeline against the
  same class of bug, but the port's key is the actual fault.

## Files

- `tools/macos/motion_correctness/{drive.sh,analyze.py,moving.py,sharp.py}`
- Run data in the checkout's `out/mc/` (not committed): `dump-a2`, `dump-f1`, `run-*.log`,
  `png-*`.
- `gpu/render/pipeline.cpp`: `BB_MOTION_FMT_LOG` (log only).
- `gpu/upscale/vk_temporal_upscaler.cpp`: input/output dump on the scaled path (capture only).
