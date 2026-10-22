// bbport MetalFX quality spike: runs MTLFXTemporalScaler over REAL frames dumped by the port
// (BB_DUMP_TRIGGER, tools/macos/metalfx-quality-capture.sh) instead of flat clears, and writes the
// scaler's output back out as .raw for the offline comparison (tools/metalfx_quality.py).
//
// The zero-copy plumbing question is already answered (spike_vk, 0.49-0.51 ms on KosmicKrisp's own
// VkImages). This tool answers the OTHER question: what does MetalFX put on the screen at a 3x
// scale. It therefore runs pure Metal (spike_metal showed Metal-on-KK-exported textures and Metal
// on private textures produce the same output and the same cost), reading the port's own dumps.
//
//   spike_quality --dir out/quality/dump-st --out out/quality/mfx-st \
//                 --jitter out/quality/jitter-st.txt [options]
//
// Options (defaults in brackets):
//   --w/--h/--ow/--oh          render and output size [1146 480 3440 1440]
//   --in-fmt rgba16f|r11g11b10f|rgba8|bgra8  colour dump format [rgba16f]; 8-bit inputs use an
//                               RGBA8Unorm colour texture (the port's LDR path dumps 8-bit)
//   --auto-exposure 0|1         MetalFX exposure estimation [1 for float, 0 for 8-bit input]
//   --depth-reversed 0|1        isDepthReversed [1]  (the port's depth is reversed-Z: 1 = near)
//   --motion-scale 1|uv         motionVectorScale 1.0 (vectors in render pixels, the port's
//                               convention) or 1/w (vectors as a fraction of the render size) [1]
//   --motion-sign +/-1          multiplies the motion vectors [1]
//   --jitter FILE               one "<frame> <jx> <jy>" per dumped frame, from the run log
//   --sharpen 0                 CAS-lite strength applied to the output (the port's
//                               taa_sharpen.comp after MetalFX, its default 0.3) [0]
//   --frames N                  dumped frames to use, 0 = all [0]
//   --no-out                    do not write output raws (metrics and timing only)
//   --label TEXT                printed with the summary
//   --print-grad                mean |grad| of each output (needs --no-out off, cheap either way)
//
// Prints: GPU ms per scale (p50/p10/p90) and, per frame, the readback mean |grad|.
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

// uint 10/11-bit float (B10G11R11_UFLOAT) -> float, the same decode as tools/dump_view.py.
float SmallFloat(unsigned bits, int mantissa_bits) {
    const unsigned exponent = (bits >> mantissa_bits) & 31u;
    const unsigned mantissa = bits & ((1u << mantissa_bits) - 1u);
    const float scale = float(1u << mantissa_bits);
    if (exponent == 0) return std::ldexp(float(mantissa) / scale, -14);
    if (exponent == 31) return INFINITY;
    return std::ldexp(1.0f + float(mantissa) / scale, int(exponent) - 15);
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0) {
            data.resize(size_t(size));
            if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        }
        std::fclose(f);
    }
    return data;
}

bool WriteFile(const std::string& path, const void* data, size_t size) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    return ok;
}

/// The scale pass's own cost, measured on the GPU (like spike_metal): the input uploads are not in
/// it, because the port's real path hands MetalFX the images with no copy at all.
struct Frame {
    std::vector<uint8_t> color, depth, motion;
    float jitter_x = 0, jitter_y = 0;
};

// CAS-lite: contrast-adaptive sharpen with local min/max clamping (an approximate stand-in for the
// port's taa_sharpen.comp RCAS at the same strength; labelled in the report as approximate).
const char* kSharpen = R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void sharpen(texture2d<float, access::read> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant float& amount [[buffer(0)]],
                    uint2 p [[thread_position_in_grid]]) {
    if (p.x >= dst.get_width() || p.y >= dst.get_height()) return;
    const int2 size = int2(dst.get_width(), dst.get_height());
    const auto at = [&](int2 q) -> float3 {
        q = clamp(q, int2(0), size - 1);
        return max(src.read(uint2(q)).rgb, 0.0f);
    };
    const float3 c = at(int2(p));
    const float3 l = at(int2(p) + int2(-1, 0)), r = at(int2(p) + int2(1, 0));
    const float3 t = at(int2(p) + int2(0, -1)), b = at(int2(p) + int2(0, 1));
    const float3 lo = min(min(l, r), min(t, b));
    const float3 hi = max(max(l, r), max(t, b));
    // Unsharp on the 4-neighbour cross, clamped to the local range (no overshoot).
    const float3 blur = (l + r + t + b) * 0.25f;
    float3 sharp = c + amount * (c - blur);
    sharp = clamp(sharp, min(lo, c), max(hi, c));
    const float peak = max(1.0f, max(c.r, max(c.g, c.b)));
    dst.write(float4(clamp(sharp, 0.0f, peak * 4.0f), 1.0f), p);
}
)MSL";

void Usage() {
    std::printf("usage: spike_quality --dir D --out DIR [options]; see the file header\n");
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        std::string dir, out, jitter_path, label, in_fmt = "rgba16f";
        NSUInteger iw = 1146, ih = 480, ow = 3440, oh = 1440;
        bool depth_reversed = true, write_out = true, print_grad = false;
        int auto_exposure = -1; // -1: pick from the input format
        float motion_scale = 1.0f, motion_sign = 1.0f, sharpen = 0.0f;
        int frames_wanted = 0, repeat = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
            if (a == "--dir") dir = next();
            else if (a == "--out") out = next();
            else if (a == "--jitter") jitter_path = next();
            else if (a == "--label") label = next();
            else if (a == "--in-fmt") in_fmt = next();
            else if (a == "--auto-exposure") auto_exposure = std::atoi(next());
            else if (a == "--w") iw = std::atoi(next());
            else if (a == "--h") ih = std::atoi(next());
            else if (a == "--ow") ow = std::atoi(next());
            else if (a == "--oh") oh = std::atoi(next());
            else if (a == "--depth-reversed") depth_reversed = std::atoi(next()) != 0;
            else if (a == "--motion-scale") {
                const std::string v = next();
                motion_scale = v == "uv" ? 1.0f / float(iw) : float(std::atof(v.c_str()));
            } else if (a == "--motion-sign") motion_sign = float(std::atof(next()));
            else if (a == "--sharpen") sharpen = float(std::atof(next()));
            else if (a == "--frames") frames_wanted = std::atoi(next());
            else if (a == "--repeat") repeat = std::max(1, std::atoi(next()));
            else if (a == "--no-out") write_out = false;
            else if (a == "--print-grad") print_grad = true;
            else { Usage(); return 2; }
        }
        if (dir.empty()) { Usage(); return 2; }
        if (write_out && out.empty()) { Usage(); return 2; }
        if (write_out) [[maybe_unused]] int mk = std::system(("mkdir -p '" + out + "'").c_str());

        // The frames: fNNN_input_WxH_<fmt>.raw, fNNN_depth_WxH_f32.raw, fNNN_motion_WxH_rg16f.raw
        // The port names the dump suffix from the actual pixel format ("rgba"/"bgra" for 8-bit).
        const std::string file_fmt = in_fmt == "rgba8" ? "rgba" : in_fmt == "bgra8" ? "bgra" : in_fmt;
        char name[512];
        std::vector<Frame> frames;
        for (int f = 0; frames_wanted == 0 || f < frames_wanted; ++f) {
            Frame fr;
            std::snprintf(name, sizeof(name), "%s/f%03d_input_%lux%lu_%s.raw", dir.c_str(), f,
                          (unsigned long)iw, (unsigned long)ih, file_fmt.c_str());
            fr.color = ReadFile(name);
            if (fr.color.empty()) break;
            std::snprintf(name, sizeof(name), "%s/f%03d_depth_%lux%lu_f32.raw", dir.c_str(), f,
                          (unsigned long)iw, (unsigned long)ih);
            fr.depth = ReadFile(name);
            std::snprintf(name, sizeof(name), "%s/f%03d_motion_%lux%lu_rg16f.raw", dir.c_str(), f,
                          (unsigned long)iw, (unsigned long)ih);
            fr.motion = ReadFile(name);
            if (fr.depth.empty() || fr.motion.empty()) {
                std::printf("missing depth/motion for frame %d\n", f);
                return 1;
            }
            frames.push_back(std::move(fr));
        }
        if (frames.empty()) {
            std::printf("no frames in %s\n", dir.c_str());
            return 1;
        }
        // Jitter (and the rest of the camera state) from the port's own dumped frames.
        if (!jitter_path.empty()) {
            if (FILE* jf = std::fopen(jitter_path.c_str(), "r")) {
                int idx;
                float jx, jy;
                while (std::fscanf(jf, "%d %f %f", &idx, &jx, &jy) == 3) {
                    if (idx >= 0 && size_t(idx) < frames.size()) {
                        frames[size_t(idx)].jitter_x = jx;
                        frames[size_t(idx)].jitter_y = jy;
                    }
                }
                std::fclose(jf);
            }
        }

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const float scale = float(ow) / float(iw);
        std::printf("%sdevice %s; %zu frames %lux%lu -> %lux%lu (%.4fx); depth reversed %d; "
                    "motion scale %.6g sign %+.0f; sharpen %.2f\n",
                    label.empty() ? "" : (label + ": ").c_str(), dev.name.UTF8String, frames.size(),
                    (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh,
                    double(scale), int(depth_reversed), motion_scale, motion_sign, sharpen);

        const bool in_8bit = in_fmt == "rgba8" || in_fmt == "bgra8";
        const MTLPixelFormat color_fmt = in_8bit ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA16Float;
        const bool auto_exp = auto_exposure >= 0 ? auto_exposure != 0 : !in_8bit;
        MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
        d.inputWidth = iw;
        d.inputHeight = ih;
        d.outputWidth = ow;
        d.outputHeight = oh;
        d.colorTextureFormat = color_fmt;
        d.depthTextureFormat = MTLPixelFormatDepth32Float;
        d.motionTextureFormat = MTLPixelFormatRG16Float;
        d.outputTextureFormat = MTLPixelFormatRGBA16Float;
        // Matches the port's HDR/LDR switch (bbport_metalfx.mm: autoExposureEnabled = config.hdr).
        d.autoExposureEnabled = auto_exp ? YES : NO;
        id<MTLFXTemporalScaler> scaler = [d newTemporalScalerWithDevice:dev];
        if (!scaler) {
            std::printf("newTemporalScalerWithDevice returned nil\n");
            return 1;
        }
        std::printf("  usage: color 0x%lx depth 0x%lx motion 0x%lx output 0x%lx\n",
                    (unsigned long)scaler.colorTextureUsage, (unsigned long)scaler.depthTextureUsage,
                    (unsigned long)scaler.motionTextureUsage, (unsigned long)scaler.outputTextureUsage);

        // Private textures: what a ship-ready MetalFX path has (the KK export path is proven).
        const auto texture = [&](MTLPixelFormat format, NSUInteger w, NSUInteger h,
                                 MTLTextureUsage usage) -> id<MTLTexture> {
            MTLTextureDescriptor* td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                   width:w
                                                                  height:h
                                                               mipmapped:NO];
            td.storageMode = MTLStorageModePrivate;
            td.usage = usage;
            return [dev newTextureWithDescriptor:td];
        };
        id<MTLTexture> color = texture(color_fmt, iw, ih, scaler.colorTextureUsage);
        id<MTLTexture> depth = texture(MTLPixelFormatDepth32Float, iw, ih, scaler.depthTextureUsage);
        id<MTLTexture> motion = texture(MTLPixelFormatRG16Float, iw, ih, scaler.motionTextureUsage);
        id<MTLTexture> output = texture(MTLPixelFormatRGBA16Float, ow, oh, scaler.outputTextureUsage);
        id<MTLTexture> sharp_tex =
            sharpen > 0 ? texture(MTLPixelFormatRGBA16Float, ow, oh,
                                  MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)
                        : nil;
        if (!color || !depth || !motion || !output || (sharpen > 0 && !sharp_tex)) {
            std::printf("texture creation failed\n");
            return 1;
        }
        id<MTLComputePipelineState> sharpen_pipeline = nil;
        if (sharpen > 0) {
            NSError* err = nil;
            id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSharpen) options:nil error:&err];
            if (!lib) {
                std::printf("sharpen kernel: %s\n", err.localizedDescription.UTF8String);
                return 1;
            }
            sharpen_pipeline = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"sharpen"]
                                                                  error:&err];
            if (!sharpen_pipeline) {
                std::printf("sharpen pipeline: %s\n", err.localizedDescription.UTF8String);
                return 1;
            }
        }
        scaler.colorTexture = color;
        scaler.depthTexture = depth;
        scaler.motionTexture = motion;
        scaler.outputTexture = output;
        scaler.inputContentWidth = iw;
        scaler.inputContentHeight = ih;
        scaler.motionVectorScaleX = motion_sign * motion_scale;
        scaler.motionVectorScaleY = motion_sign * motion_scale;
        scaler.preExposure = 1.0f;
        scaler.depthReversed = depth_reversed;

        // Readback of the output (and of the sharpened copy) as float16 RGBA.
        const NSUInteger out_bytes = NSUInteger(ow) * oh * 8;
        id<MTLBuffer> readback = [dev newBufferWithLength:out_bytes options:MTLResourceStorageModeShared];

        std::vector<double> scale_ms;
        std::vector<double> grad_per_frame;
        const float half[2] = {float(ow) * 0.5f / float(ow), float(oh) * 0.5f / float(oh)};
        (void)half;
        // repeat > 1 replays the same frames so MetalFX's history can converge (a static or
        // looping sequence): the outputs written are those of the LAST pass.
        const size_t total = frames.size() * size_t(repeat);
        for (size_t pass = 0; pass < total; ++pass) {
            const size_t f = pass % frames.size();
            @autoreleasepool {
                Frame& fr = frames[f];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> up = [cb blitCommandEncoder];
                const auto upload = [&](const std::vector<uint8_t>& src, id<MTLTexture> dst,
                                        NSUInteger bpp, bool convert_ufloat) {
                    NSUInteger bytes = bpp;
                    if (convert_ufloat) {
                        // r11g11b10f -> RGBA16Float on the way in (the same values, no resampling:
                        // MetalFX has no 10/11-bit colour input format).
                        std::vector<__fp16> wide(NSUInteger(iw) * ih * 4);
                        const uint32_t* packed = (const uint32_t*)src.data();
                        for (NSUInteger i = 0; i < NSUInteger(iw) * ih; ++i) {
                            const uint32_t v = packed[i];
                            wide[i * 4 + 0] = (__fp16)SmallFloat(v & 0x7FFu, 6);
                            wide[i * 4 + 1] = (__fp16)SmallFloat((v >> 11) & 0x7FFu, 6);
                            wide[i * 4 + 2] = (__fp16)SmallFloat(v >> 22, 5);
                            wide[i * 4 + 3] = (__fp16)1.0f;
                        }
                        id<MTLBuffer> tmp = [dev newBufferWithBytes:wide.data()
                                                             length:wide.size() * 2
                                                            options:MTLResourceStorageModeShared];
                        bytes = 8;
                        [up copyFromBuffer:tmp sourceOffset:0 sourceBytesPerRow:iw * bytes
                            sourceBytesPerImage:iw * ih * bytes sourceSize:MTLSizeMake(iw, ih, 1)
                                  toTexture:dst destinationSlice:0 destinationLevel:0
                            destinationOrigin:MTLOriginMake(0, 0, 0)];
                        return;
                    }
                    id<MTLBuffer> tmp = [dev newBufferWithBytes:src.data() length:src.size()
                                                         options:MTLResourceStorageModeShared];
                    [up copyFromBuffer:tmp sourceOffset:0 sourceBytesPerRow:iw * bytes
                        sourceBytesPerImage:iw * ih * bytes sourceSize:MTLSizeMake(iw, ih, 1)
                              toTexture:dst destinationSlice:0 destinationLevel:0
                        destinationOrigin:MTLOriginMake(0, 0, 0)];
                };
                if (in_8bit) {
                    // BGRA dumps: a channel swizzle on the way in (no resampling, no scale).
                    const std::vector<uint8_t>* src = &fr.color;
                    std::vector<uint8_t> swizzled;
                    if (in_fmt == "bgra8") {
                        swizzled.resize(fr.color.size());
                        for (size_t i = 0; i + 3 < fr.color.size(); i += 4) {
                            swizzled[i + 0] = fr.color[i + 2];
                            swizzled[i + 1] = fr.color[i + 1];
                            swizzled[i + 2] = fr.color[i + 0];
                            swizzled[i + 3] = fr.color[i + 3];
                        }
                        src = &swizzled;
                    }
                    upload(*src, color, 4, false);
                } else {
                    upload(fr.color, color, 8, in_fmt == "r11g11b10f");
                }
                upload(fr.depth, depth, 4, false);
                upload(fr.motion, motion, 4, false);
                [up endEncoding];
                [cb commit]; // the scale's command buffer queues behind it

                id<MTLCommandBuffer> s = [q commandBuffer];
                scaler.jitterOffsetX = fr.jitter_x;
                scaler.jitterOffsetY = fr.jitter_y;
                scaler.reset = pass == 0;
                [scaler encodeToCommandBuffer:s];
                [s commit];
                [s waitUntilCompleted];
                if (s.status != MTLCommandBufferStatusCompleted) {
                    std::printf("frame %zu: scale failed: %s\n", f,
                                s.error.localizedDescription.UTF8String);
                    return 1;
                }
                scale_ms.push_back((s.GPUEndTime - s.GPUStartTime) * 1e3);

                // The sharpen arm runs on its own command buffer and is timed separately (its cost
                // is reported, never added to the scaler's).
                id<MTLTexture> result = output;
                double sharp_ms = 0;
                if (sharpen_pipeline) {
                    id<MTLCommandBuffer> sh = [q commandBuffer];
                    id<MTLComputeCommandEncoder> ce = [sh computeCommandEncoder];
                    [ce setComputePipelineState:sharpen_pipeline];
                    [ce setTexture:output atIndex:0];
                    [ce setTexture:sharp_tex atIndex:1];
                    float amount = sharpen;
                    [ce setBytes:&amount length:sizeof(amount) atIndex:0];
                    const NSUInteger tg = 8;
                    [ce dispatchThreads:MTLSizeMake(ow, oh, 1)
                        threadsPerThreadgroup:MTLSizeMake(tg, tg, 1)];
                    [ce endEncoding];
                    [sh commit];
                    [sh waitUntilCompleted];
                    sharp_ms = (sh.GPUEndTime - sh.GPUStartTime) * 1e3;
                    result = sharp_tex;
                }

                id<MTLCommandBuffer> rb = [q commandBuffer];
                id<MTLBlitCommandEncoder> dl = [rb blitCommandEncoder];
                [dl copyFromTexture:result sourceSlice:0 sourceLevel:0
                       sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(ow, oh, 1)
                           toBuffer:readback destinationOffset:0
                  destinationBytesPerRow:ow * 8 destinationBytesPerImage:out_bytes];
                [dl endEncoding];
                [rb commit];
                [rb waitUntilCompleted];

                if (print_grad) {
                    // Mean |grad| on the luma of the whole output, in the linear float space.
                    const __fp16* px = (const __fp16*)readback.contents;
                    double sum = 0;
                    size_t n = 0;
                    for (NSUInteger y = 1; y + 1 < oh; ++y) {
                        for (NSUInteger x = 1; x + 1 < ow; ++x) {
                            const size_t i = size_t(y) * ow + x;
                            const auto lum = [&](size_t j) {
                                return 0.2126f * float(px[j * 4]) + 0.7152f * float(px[j * 4 + 1]) +
                                       0.0722f * float(px[j * 4 + 2]);
                            };
                            const float dx = (lum(i + 1) - lum(i - 1)) * 0.5f;
                            const float dy = (lum(i + ow) - lum(i - ow)) * 0.5f;
                            sum += std::sqrt(double(dx * dx + dy * dy));
                            ++n;
                        }
                    }
                    const double g = n ? sum / double(n) : 0.0;
                    grad_per_frame.push_back(g);
                    std::printf("  frame %zu: scale %.3f ms, sharpen %.3f ms, mean|grad| %.6f\n",
                                f, scale_ms.back(), sharp_ms, g);
                } else {
                    std::printf("  frame %zu: scale %.3f ms, sharpen %.3f ms\n", f, scale_ms.back(),
                                sharp_ms);
                }
                if (write_out) {
                    std::snprintf(name, sizeof(name), "%s/f%03d_out_%lux%lu_rgba16f.raw", out.c_str(),
                                  int(f), (unsigned long)ow, (unsigned long)oh);
                    if (!WriteFile(name, readback.contents, out_bytes)) {
                        std::printf("cannot write %s\n", name);
                        return 1;
                    }
                }
            }
        }
        const Stats st = Summ(scale_ms);
        std::printf("RESULT scale GPU ms p50 %.3f p10 %.3f p90 %.3f mean %.3f, %zu frames\n", st.p50,
                    st.p10, st.p90, st.mean, scale_ms.size());
    }
    return 0;
}
