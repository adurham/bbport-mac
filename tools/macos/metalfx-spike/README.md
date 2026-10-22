# MetalFX zero-copy tail: feasibility spike (2026-10-21, branch `spike/metalfx-tail`)

This is a standalone test. It does not touch the port's renderer, any default, or `tools/macos/kosmickrisp-patches/`.
It ran on an M4 Max (macOS 27.2) against KosmicKrisp 3af1126 with patches 0001-0006, plus the two patches in this folder.
No game run was needed.

## Answer, numbers first

| what | GPU ms (p50) | source |
|---|---|---|
| MTLFXTemporalScaler 1146x480 -> 3440x1440 on **KosmicKrisp's own VkImages** (exported, zero copies) | **0.49-0.51** | `spike_vk`, `results/spike_vk_3440_import.txt`, `results/spike_vk_sizes.txt` |
| same, pure Metal, private inputs (ceiling) | 0.50 | `spike_metal` A1 |
| same, pure Metal, KK-style inputs (placement heap, shared, untracked, D32 or D32S8) | 0.49 | A2 / A3 |
| 1720x720 / 2294x960 -> 3440x1440 | 0.65 / 0.80 | `spike_vk` |
| 854x480 -> 2560x1440 | 0.37 | `spike_vk` |
| private output + one blit into a KK (shared) texture | 1.26 (+0.76) | A4: the fallback without patch 0008 |
| today's port path, GPU side only (3 buffer->texture uploads, scale, 1 texture->buffer copy) | 1.54 | A6. The real path also pays two CPU waits on top. |
| FSR 3.1 chain, preset 4, at 3440 (for comparison) | ~1.97 | trace compute 4.70 vs 2.73 ms/frame, `bbport-metalfx-build/out/mtl-3440_{fsr3_4,off1}-gpu.xml` |

How "zero-copy" was proven in `spike_vk`:
- Vulkan creates the images.
- A Vulkan render pass writes them.
- MetalFX reads the exact exported `MTLTexture`s and writes the output.
- Vulkan `vkCmdCopyImageToBuffer`s the output VkImage itself.
- The value Vulkan wrote comes back on every frame checked (0.375 then 0.25 alternating).
- No copy is encoded anywhere.
- The GPU ordering is the Vulkan timeline's own `MTLSharedEvent` (patch 0002): Vulkan signals N, MetalFX waits N and signals N+1, Vulkan waits N+1.
- There are no CPU waits in between.

## API findings (paths relative to `deps-arm64/src/mesa-kosmickrisp/externals/mesa/`)

- **Stock state:** KK already advertises `VK_EXT_metal_objects` (`src/kosmickrisp/vulkan/kk_physical_device.c:184`, port patch 0002). But `kk_ExportMetalObjectsEXT` (`kk_sync.c:119`) only fills `VkExportMetalSharedEventInfoEXT`. Device and texture requests come back NULL. Verified: `results/spike_vk_stock_kk.txt`.
  - Patch 0003 ("prototype") only adds two `#include`s. It is not an export.
- **Every VkImage already *is* an MTLTexture:** `kk_image_plane.mtl_handle` (`kk_image.h:39`) is created at bind time (`kk_image.c:862`) with `newTextureWithDescriptor:offset:` on the memory's placement `MTLHeap` (`bridge/mtl_heap.m:76-82`).
- **Patch 0007** (`kk-patches/0007-…`, about 30 lines in `kk_sync.c`): fills `VkExportMetalDeviceInfoEXT` (the `kk_device.mtl_handle`) and `VkExportMetalTextureInfoEXT` (image plane `mtl_handle`, or the view's `mtl_handle_sampled`).
  - That alone is enough for MetalFX to **read** the colour, depth and motion images with no copy.
  - Usage is sufficient: KK maps sampled to ShaderRead and attachment to RenderTarget (`kk_image_layout.c:42-77`). Exported usage is 0x7 for colour and 0x17 for D32S8. MetalFX needs 0x1 for inputs and 0x7 for the output.
  - D32S8 (fmt 260) is accepted as the depth texture.
- **The one wall: the output.** MetalFX requires `outputTexture.storageMode == Private`.
  - Every KK heap is `MTLStorageModeShared`: `KK_MTL_RESOURCE_OPTIONS` (`kk_private.h:48-51`) is used for every heap (`kk_bo.c:43`) and inherited by every texture (`mtl_heap.m:81`).
  - So no KK VkImage can be a MetalFX output.
  - On a shared output the scale still runs and the bits are right without validation. Under `MTL_DEBUG_LAYER=1` it aborts: `MetalFXTemporalScalingEffectV4.mm:627: failed assertion 'outputTexture must have private storage mode'` (`results/spike_vk_3440_sharedout_debuglayer.txt`, rc 134). That is unsupported. Do not ship it.
- **Patch 0008** (`kk-patches/0008-…`, about 40 lines in `kk_image.{h,c}`): `VkImportMetalTextureInfoEXT` in `vkCreateImage`.
  - The image becomes a caller-made **private** MTLTexture. It follows the WSI drawable bind's model (`kk_wsi.c:52-70`), adds the texture to KK's residency set, and makes binds a no-op.
  - Validated: debug layer clean (rc 0), private storage round-trips, output correct, 0.49 ms.
  - Not CTS-grade: it doesn't handle multi-plane images, and doesn't query memory requirements for the imported image. It is a spike.
- **Queue:** KK's queue is an `MTL4CommandQueue` (`bridge/mtl_command_queue.m:53`), so there is no `MTLCommandQueue` to export.
  - MetalFX runs on its own legacy `MTLCommandQueue` on the exported device, ordered by shared events.
  - Encoding into KK's own MTL4 command buffer (`MTL4FXTemporalScaler`, macOS 26+) would remove the queue hop. Vulkan has no API for that, so it would need a private KK hook.

## Side finding: the old `3440_mfx4` row never ran MetalFX

`bbport-metalfx-build/out/run-3440_mfx4.log` says: `MetalFX unavailable: scale 3.00 outside the device's MetalFX range 1.00..3.00`, then `MetalFX failed; upscaler off`.
- 3440/1146 = 3.0017, which is over the port's 1e-3 tolerance (`gpu/shim/bbport_metalfx.mm:109`).
- MetalFX itself accepts it: this spike creates and runs that exact scaler.
- So the 30.55 ms row in `docs/METALFX_MEASURE.md` is "upscaler off at a 1146x480 raster", not MetalFX Ultra Performance.

## Cost arithmetic for the port

- FSR 3.1 preset 4 frame: 19.1-19.7 ms p50 (tonight's control 19.18; 2026-10-17 matrix 19.73).
- FSR chain on the GPU: about 1.97 ms at 3440 (trace compute delta). The 2560 docs put it at 1.4-2.0 ms.
- MetalFX tail: 0.49 ms scale + about 0.3 ms queue hand-off. `SPIKE_NO_MFX=1` measures the full Vulkan -> Metal -> Vulkan event round trip with no work at 0.31 ms wall. The real frame adds one more command-buffer boundary.
  - Total: about **0.8 ms**.
- Best-case GPU saving: 1.97 - 0.8 ≈ **1.2 ms**, so the frame goes 19.1 -> about **17.9 ms (~56 fps)**.
- Realistic frame saving: **0.4-1.2 ms**. The frame is not GPU-bound: GPU busy is about 65%, and roughly 6 ms of idle sits at submission boundaries (`docs/GPU_TIMELINE_RECONCILIATION.md`, `docs/STAGE_D_CUSTOM_UPSCALER.md`). The stage-D custom upscaler saved 0.615 ms of frame from a similar GPU cut.
  - So: about 18-18.7 ms, 53-56 fps. **It beats 19.1 ms on the arithmetic. It does not reach 60 fps (16.7 ms).**
- Sharpness is **not measured**. The spike's content is flat clears. MetalFX vs FSR 3.1 + RCAS at the same 3x raster is a visible-run question for the owner. The port could keep its RCAS pass after MetalFX for about 0.1-0.2 ms.

## Fallback without patch 0008

Leave KK's heaps alone and have MetalFX write a private output, then blit it into the KK output image: **+0.76 ms GPU** (A4), about 1.26 ms total.
Against the FSR chain's ~1.97 ms the GPU saving is then only about 0.7 ms, and frame time would likely be 0-0.7 ms better.

## Port API surface a real build would need

1. KK patches 0007 + 0008 (or an equivalent KK change: allocate private heaps for non-host-visible images).
2. A `MetalFxUpscaler` mode that, at resource creation:
   - exports the scene colour, depth and motion VkImages via `vkExportMetalObjectsEXT`;
   - creates the output as an imported private texture.
   It then encodes into its own `MTLCommandQueue` between two timeline values. That replaces today's four staging buffers and the CPU waits (`gpu/upscale/vk_metalfx.cpp`, `gpu/shim/bbport_metalfx.mm`).
3. Fix the 3.0017 scale guard (`bbport_metalfx.mm:109`).
4. Keep the existing jitter, motion-sign and depth-reversal conventions. They are untested on image content.

## Build and run

```
bash tools/macos/metalfx-spike/build.sh
./tools/macos/metalfx-spike/build/spike_metal [iw ih ow oh frames]
VK_DRIVER_FILES=<icd json of a KK with 0007+0008> SPIKE_IMPORT_OUTPUT=1 \
    ./tools/macos/metalfx-spike/build/spike_vk [iw ih ow oh frames]
# SPIKE_NO_MFX=1: hand-off cost only.  MTL_DEBUG_LAYER=1: Metal validation.
```

The KK used for the spike is a separate copy at `~/repos/bbport-metalfx-spike-kk/mesa-kosmickrisp`. It has its own `build-spike/` and no installed dylib is replaced. It is built with `meson compile -C build-spike vulkan_kosmickrisp`, using the deps pyenv for mako and the copied build-tools for the clc tools.
