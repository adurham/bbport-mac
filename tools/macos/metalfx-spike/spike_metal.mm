// bbport MetalFX tail spike, part A: pure Metal, no Vulkan.
//
// Allocates the inputs EXACTLY the way KosmicKrisp allocates a VkImage's backing (placement
// MTLHeap, KK_MTL_RESOURCE_OPTIONS = shared storage | default cache | untracked hazards,
// allowGPUOptimizedContents, usage from vk_image_usage_flags_to_mtl_texture_usage), then runs
// MTLFXTemporalScaler from the render size to the output size and times the GPU side with
// MTLCommandBuffer GPUStartTime/GPUEndTime. It also times the alternatives the port would face:
//   - inputs in private storage (what a "MetalFX-native" app would use)        -> the ceiling
//   - output: private (MetalFX requires private) + a blit into a KK-style shared texture
//   - output: KK-style shared texture directly (expected to be rejected by MetalFX)
//   - the old path's buffer->texture uploads (what vk_metalfx.cpp pays today, GPU side only)
// Usage: spike_metal [in_w in_h out_w out_h frames]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// KosmicKrisp's resource options (kk_private.h:48).
constexpr MTLResourceOptions kKkOptions = MTLResourceStorageModeShared |
                                          MTLResourceCPUCacheModeDefaultCache |
                                          MTLResourceHazardTrackingModeUntracked;

struct Stats {
    double p50, p10, p90, mean;
};
Stats Summ(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    double s = 0;
    for (double x : v) s += x;
    auto q = [&](double f) { return v[std::min(v.size() - 1, size_t(f * (v.size() - 1) + 0.5))]; };
    return {q(0.5), q(0.1), q(0.9), s / v.size()};
}

id<MTLHeap> KkHeap(id<MTLDevice> dev, NSUInteger size) {
    MTLHeapDescriptor* d = [MTLHeapDescriptor new];
    d.type = MTLHeapTypePlacement;
    d.resourceOptions = kKkOptions;
    d.size = size;
    d.sparsePageSize = MTLSparsePageSize16;
    d.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize16;
    return [dev newHeapWithDescriptor:d];
}

// A texture the way kk_image.c:843 / mtl_heap.m:76 builds it.
id<MTLTexture> KkTexture(id<MTLDevice> dev, MTLPixelFormat fmt, NSUInteger w, NSUInteger h,
                         MTLTextureUsage usage) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    td.allowGPUOptimizedContents = YES;
    td.usage = usage;
    td.resourceOptions = kKkOptions;
    MTLSizeAndAlign sa = [dev heapTextureSizeAndAlignWithDescriptor:td];
    id<MTLHeap> heap = KkHeap(dev, (sa.size + sa.align) * 1);
    if (!heap) return nil;
    id<MTLTexture> t = [heap newTextureWithDescriptor:td offset:0];
    return t;
}

id<MTLTexture> PrivateTexture(id<MTLDevice> dev, MTLPixelFormat fmt, NSUInteger w, NSUInteger h,
                              MTLTextureUsage usage) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = usage;
    return [dev newTextureWithDescriptor:td];
}

// Vulkan usage COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC|TRANSFER_DST -> KK's MTL usage.
constexpr MTLTextureUsage kKkAttachmentUsage =
    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

void FillScene(id<MTLCommandQueue> q, id<MTLTexture> color, id<MTLTexture> depth,
               id<MTLTexture> motion, float t) {
    // Render-pass clears: plausible non-trivial content is not needed for timing (MetalFX's
    // cost is data-independent), but the content must be defined.
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = color;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25 + 0.1 * std::sin(t), 0.5, 0.75, 1);
    rp.colorAttachments[1].texture = motion;
    rp.colorAttachments[1].loadAction = MTLLoadActionClear;
    rp.colorAttachments[1].storeAction = MTLStoreActionStore;
    rp.colorAttachments[1].clearColor = MTLClearColorMake(0.5, -0.25, 0, 0);
    rp.depthAttachment.texture = depth;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.depthAttachment.clearDepth = 0.5;
    if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
        rp.stencilAttachment.texture = depth;
        rp.stencilAttachment.loadAction = MTLLoadActionClear;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
    }
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
}

struct Config {
    const char* name;
    bool kk_inputs;          // KK-style shared/untracked/heap inputs (else private)
    MTLPixelFormat depth_fmt;
    int output_mode;         // 0 private output; 1 private + blit to KK-shared; 2 KK-shared output
    bool old_uploads;        // + the current port's 3 buffer->texture uploads and 1 download
};

struct Result {
    bool ok = false;
    std::string why;
    Stats scale{}, total{};
};

Result Run(id<MTLDevice> dev, id<MTLCommandQueue> q, const Config& c, NSUInteger iw,
           NSUInteger ih, NSUInteger ow, NSUInteger oh, int frames) {
    Result r;
    MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
    d.inputWidth = iw;
    d.inputHeight = ih;
    d.outputWidth = ow;
    d.outputHeight = oh;
    d.colorTextureFormat = MTLPixelFormatRGBA16Float;
    d.depthTextureFormat = c.depth_fmt;
    d.motionTextureFormat = MTLPixelFormatRG16Float;
    d.outputTextureFormat = MTLPixelFormatRGBA16Float;
    d.autoExposureEnabled = YES; // the port's HDR path (config.hdr)
    d.requiresSynchronousInitialization = YES;
    id<MTLFXTemporalScaler> s = [d newTemporalScalerWithDevice:dev];
    if (!s) {
        r.why = "newTemporalScalerWithDevice returned nil";
        return r;
    }
    auto mk = [&](MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage need) {
        MTLTextureUsage u = kKkAttachmentUsage | need;
        return c.kk_inputs ? KkTexture(dev, f, w, h, u) : PrivateTexture(dev, f, w, h, u);
    };
    id<MTLTexture> color = mk(MTLPixelFormatRGBA16Float, iw, ih, s.colorTextureUsage);
    id<MTLTexture> depth = mk(c.depth_fmt, iw, ih, s.depthTextureUsage);
    id<MTLTexture> motion = mk(MTLPixelFormatRG16Float, iw, ih, s.motionTextureUsage);
    id<MTLTexture> out_private =
        PrivateTexture(dev, MTLPixelFormatRGBA16Float, ow, oh, s.outputTextureUsage);
    id<MTLTexture> out_kk = KkTexture(dev, MTLPixelFormatRGBA16Float, ow, oh,
                                      kKkAttachmentUsage | s.outputTextureUsage);
    if (!color || !depth || !motion || !out_private || !out_kk) {
        r.why = "texture creation failed";
        return r;
    }
    // The current port path's staging buffers (vk_metalfx.cpp), for the old_uploads arm.
    id<MTLBuffer> bc = nil, bd = nil, bm = nil, bo = nil;
    if (c.old_uploads) {
        id<MTLHeap> h = KkHeap(dev, iw * ih * 16 + ow * oh * 8 + (1 << 20));
        bc = [h newBufferWithLength:iw * ih * 8 options:kKkOptions offset:0];
        bd = [h newBufferWithLength:iw * ih * 4 options:kKkOptions offset:iw * ih * 8];
        bm = [h newBufferWithLength:iw * ih * 4 options:kKkOptions offset:iw * ih * 12];
        bo = [dev newBufferWithLength:ow * oh * 8 options:kKkOptions];
    }
    FillScene(q, color, depth, motion, 0);
    s.colorTexture = color;
    s.depthTexture = depth;
    s.motionTexture = motion;
    s.outputTexture = c.output_mode == 2 ? out_kk : out_private;
    s.inputContentWidth = iw;
    s.inputContentHeight = ih;
    s.motionVectorScaleX = 1;
    s.motionVectorScaleY = 1;
    s.preExposure = 1;

    std::vector<double> scale_ms, total_ms;
    const int warm = 30;
    @try {
        for (int f = 0; f < warm + frames; ++f) {
            @autoreleasepool {
                // Halton(2,3) jitter like FSR/MetalFX expect.
                auto halton = [](int i, int b) {
                    double r = 0, fct = 1;
                    while (i > 0) { fct /= b; r += fct * (i % b); i /= b; }
                    return r;
                };
                s.jitterOffsetX = float(halton(f % 32 + 1, 2) - 0.5);
                s.jitterOffsetY = float(halton(f % 32 + 1, 3) - 0.5);
                s.reset = f == 0;
                id<MTLCommandBuffer> pre = nil, cb = nil, post = nil;
                if (c.old_uploads) {
                    pre = [q commandBuffer];
                    id<MTLBlitCommandEncoder> b = [pre blitCommandEncoder];
                    auto up = [&](id<MTLBuffer> src, id<MTLTexture> dst, NSUInteger bpp) {
                        [b copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:iw * bpp
                            sourceBytesPerImage:iw * ih * bpp sourceSize:MTLSizeMake(iw, ih, 1)
                                  toTexture:dst destinationSlice:0 destinationLevel:0
                            destinationOrigin:MTLOriginMake(0, 0, 0)];
                    };
                    up(bc, color, 8);
                    if (c.depth_fmt == MTLPixelFormatDepth32Float) up(bd, depth, 4);
                    up(bm, motion, 4);
                    [b endEncoding];
                    [pre commit];
                }
                cb = [q commandBuffer];
                [s encodeToCommandBuffer:cb];
                [cb commit];
                if (c.output_mode == 1 || c.old_uploads) {
                    post = [q commandBuffer];
                    id<MTLBlitCommandEncoder> b = [post blitCommandEncoder];
                    if (c.old_uploads)
                        [b copyFromTexture:out_private sourceSlice:0 sourceLevel:0
                              sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(ow, oh, 1)
                                  toBuffer:bo destinationOffset:0
                         destinationBytesPerRow:ow * 8 destinationBytesPerImage:ow * oh * 8];
                    else
                        [b copyFromTexture:out_private toTexture:out_kk];
                    [b endEncoding];
                    [post commit];
                }
                id<MTLCommandBuffer> last = post ? post : cb;
                [last waitUntilCompleted];
                if (cb.status == MTLCommandBufferStatusError) {
                    r.why = std::string("command buffer error: ") +
                            cb.error.localizedDescription.UTF8String;
                    return r;
                }
                if (f >= warm) {
                    double s0 = cb.GPUStartTime, s1 = cb.GPUEndTime;
                    scale_ms.push_back((s1 - s0) * 1e3);
                    double t0 = (pre ? pre : cb).GPUStartTime, t1 = last.GPUEndTime;
                    total_ms.push_back((t1 - t0) * 1e3);
                }
            }
        }
    } @catch (NSException* e) {
        r.why = std::string("exception: ") + e.reason.UTF8String;
        return r;
    }
    // Readback sanity: output must be finite and near the input colour (constant input).
    {
        id<MTLBuffer> rb = [dev newBufferWithLength:8 * 16 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        id<MTLTexture> src = c.output_mode == 0 ? out_private : out_kk;
        if (c.old_uploads) src = out_private;
        [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(ow / 2, oh / 2, 0)
                sourceSize:MTLSizeMake(16, 1, 1) toBuffer:rb destinationOffset:0
            destinationBytesPerRow:16 * 8 destinationBytesPerImage:16 * 8];
        [b endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const __fp16* px = (const __fp16*)rb.contents;
        std::printf("    readback centre px: %.3f %.3f %.3f %.3f\n", float(px[0]), float(px[1]),
                    float(px[2]), float(px[3]));
    }
    r.ok = true;
    r.scale = Summ(scale_ms);
    r.total = Summ(total_ms);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        NSUInteger iw = 1146, ih = 480, ow = 3440, oh = 1440;
        int frames = 300;
        if (argc >= 5) {
            iw = std::atoi(argv[1]); ih = std::atoi(argv[2]);
            ow = std::atoi(argv[3]); oh = std::atoi(argv[4]);
        }
        if (argc >= 6) frames = std::atoi(argv[5]);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        std::printf("device %s, MetalFX temporal supported=%d, scale range %.2f..%.2f\n",
                    dev.name.UTF8String, [MTLFXTemporalScalerDescriptor supportsDevice:dev],
                    [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:dev],
                    [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:dev]);
        std::printf("render %lux%lu -> output %lux%lu (%.2fx), %d timed frames after 30 warm\n",
                    (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh,
                    double(ow) / iw, frames);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const Config configs[] = {
            {"A1 private inputs, private output (ceiling)", false, MTLPixelFormatDepth32Float, 0, false},
            {"A2 KK-style inputs (D32), private output", true, MTLPixelFormatDepth32Float, 0, false},
            {"A3 KK-style inputs (D32S8), private output", true, MTLPixelFormatDepth32Float_Stencil8, 0, false},
            {"A4 KK-style inputs, private output + blit to KK-shared", true, MTLPixelFormatDepth32Float, 1, false},
            {"A5 KK-style inputs, KK-shared output directly", true, MTLPixelFormatDepth32Float, 2, false},
            {"A6 today's path GPU side: 3 buf->tex uploads + scale + tex->buf", true, MTLPixelFormatDepth32Float, 0, true},
        };
        for (const Config& c : configs) {
            std::printf("%s\n", c.name);
            Result r = Run(dev, q, c, iw, ih, ow, oh, frames);
            if (!r.ok) {
                std::printf("    FAILED: %s\n", r.why.c_str());
                continue;
            }
            std::printf("    scale cmdbuf GPU ms  p50 %.3f  p10 %.3f  p90 %.3f  mean %.3f\n",
                        r.scale.p50, r.scale.p10, r.scale.p90, r.scale.mean);
            std::printf("    whole chain GPU ms   p50 %.3f  p10 %.3f  p90 %.3f  mean %.3f\n",
                        r.total.p50, r.total.p10, r.total.p90, r.total.mean);
        }
    }
    return 0;
}
