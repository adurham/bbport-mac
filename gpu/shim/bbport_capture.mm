// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): the capture hotkey's PNG and the swapchain layer's state (bbport_capture.h).
//
// The pixels come from the frame the port presented (bbport_overlay.cpp copies the swapchain
// image back), so ImageIO here only encodes; nothing asks for the screen-recording permission
// CGWindowListCreateImage/CGDisplayCreateImage would. ImageIO is a system framework (no
// dependency to find, no PNG encoder to carry) and tags the file sRGB, which is what the
// swapchain's colour space says the bytes are.
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bbport_capture.h"

namespace {

/// bbport_layer.mm's short colour-space name ("kCGColorSpaceSRGB" -> "SRGB").
std::string Name(CGColorSpaceRef space) {
    if (!space) {
        return "nil";
    }
    CFStringRef name = CGColorSpaceCopyName(space);
    if (!name) {
        return "(unnamed)";
    }
    std::string out = [(__bridge NSString*)name UTF8String];
    CFRelease(name);
    if (out.rfind("kCGColorSpace", 0) == 0) {
        out = out.substr(13);
    }
    return out;
}

/// Wraps the caller's pixels as a PNG of `path`; `rgba` is a tightly packed R,G,B,A buffer.
int WritePng(const char* path, const std::vector<unsigned char>& rgba, int width, int height) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // The provider borrows the vector: it outlives this call.
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(nullptr, rgba.data(), rgba.size(), nullptr);
    CGImageRef image = nullptr;
    if (space && provider) {
        image = CGImageCreate(width, height, 8, 32, size_t(width) * 4, space,
                              (CGBitmapInfo)(kCGImageAlphaNoneSkipLast), provider, nullptr, false,
                              kCGRenderingIntentDefault);
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path,
                                                           std::strlen(path), false);
    CGImageDestinationRef destination =
        url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr) : nullptr;
    int written = 0;
    if (image && destination) {
        CGImageDestinationAddImage(destination, image, nullptr);
        written = CGImageDestinationFinalize(destination) ? 1 : 0;
    }
    if (destination) {
        CFRelease(destination);
    }
    if (image) {
        CGImageRelease(image);
    }
    if (provider) {
        CFRelease(provider);
    }
    if (url) {
        CFRelease(url);
    }
    if (space) {
        CFRelease(space);
    }
    return written;
}

} // namespace

extern "C" int bb_capture_write_png(const char* path, const void* pixels, int width, int height,
                                    long stride, int bgra) {
    if (!path || !path[0] || !pixels || width <= 0 || height <= 0) {
        return 0;
    }
    @autoreleasepool {
        const size_t row = stride > 0 ? size_t(stride) : size_t(width) * 4;
        std::vector<unsigned char> rgba(size_t(width) * size_t(height) * 4);
        const unsigned char* source = (const unsigned char*)pixels;
        for (int y = 0; y < height; ++y) {
            const unsigned char* in = source + row * size_t(y);
            unsigned char* out = rgba.data() + size_t(width) * 4 * size_t(y);
            for (int x = 0; x < width; ++x, in += 4, out += 4) {
                out[0] = bgra ? in[2] : in[0];
                out[1] = in[1];
                out[2] = bgra ? in[0] : in[2];
                out[3] = 255; // the swapchain's alpha is opaque anyway; the modal never is the file
            }
        }
        if (!WritePng(path, rgba, width, height)) {
            std::printf("Capture: PNG write failed (%s)\n", path);
            std::fflush(stdout);
            return 0;
        }
        return 1;
    }
}

extern "C" void bb_capture_write_png_async(const char* path, const void* pixels, int width,
                                           int height, long stride, int bgra) {
    if (!path || !path[0] || !pixels || width <= 0 || height <= 0) {
        return;
    }
    const size_t row = stride > 0 ? size_t(stride) : size_t(width) * 4;
    // The presenting thread's copy: the swapchain buffer is released and reused as soon as the
    // frame is done, so the encode reads its own.
    auto bytes = std::make_shared<std::vector<unsigned char>>(row * size_t(height));
    std::memcpy(bytes->data(), pixels, bytes->size());
    const std::string file = path;
    const long copied_stride = long(row), copied_width = width, copied_height = height;
    const int copied_bgra = bgra;
    std::thread([file, bytes, copied_stride, copied_width, copied_height, copied_bgra] {
        if (bb_capture_write_png(file.c_str(), bytes->data(), int(copied_width), int(copied_height),
                                 copied_stride, copied_bgra)) {
            std::printf("Capture: wrote %s (%ldx%ld, %.1f MB)\n", file.c_str(), copied_width,
                        copied_height, double(bytes->size()) / 1e6);
        }
        std::fflush(stdout);
    }).detach();
}

extern "C" void bb_capture_layer_state(void* metal_layer, BbCapture::LayerState* out) {
    if (!out) {
        return;
    }
    *out = BbCapture::LayerState{};
    if (!metal_layer) {
        return;
    }
    @autoreleasepool {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer;
        if (![layer isKindOfClass:CAMetalLayer.class]) {
            return;
        }
        out->opaque = int(layer.opaque);
        // The colour space a tagged layer is colour-matched by (bbport_layer.mm's dump).
        const std::string space = Name(layer.colorspace);
        std::snprintf(out->colorspace, sizeof(out->colorspace), "%s", space.c_str());
        out->drawable_width = int(layer.drawableSize.width);
        out->drawable_height = int(layer.drawableSize.height);
        out->pixel_format = int(layer.pixelFormat);
        out->contents_scale = float(layer.contentsScale);
        out->framebuffer_only = int(layer.framebufferOnly);
    }
}
