// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): the swapchain's CAMetalLayer and the display mode, for Direct presentation.
//
// The Metal HUD's "Direct" means the WindowServer scans the game's drawable out as is; anything
// that makes it composite ("Composited") costs a copy (and latency) per frame. What decides it:
// the drawable's pixel size against the display mode's, the layer being opaque, its colour space
// (a tagged one is colour-matched to the display's profile), and other layers or windows on top.
// This module logs those (one dump per drawable size; BB_LAYER_DUMP=0 turns it off) and applies
// the experiments the settings ask for after the Vulkan driver configured the layer:
//   layer_opaque / BB_LAYER_OPAQUE        = 1 | 0 | keep
//   layer_colorspace / BB_LAYER_COLORSPACE = nil | srgb | display | keep
// KosmicKrisp (Mesa's Metal WSI) leaves the layer non-opaque on purpose for hardware devices
// (wsi_common_metal.c: "work around a Metal bug when presenting through Metal4") and tags it
// sRGB for VK_COLOR_SPACE_SRGB_NONLINEAR_KHR — both before every swapchain, so these run after
// each one.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bbport_settings.h"

namespace {

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
    // kCGColorSpaceSRGB is "kCGColorSpaceSRGB": the short form reads better in a log.
    if (out.rfind("kCGColorSpace", 0) == 0) {
        out = out.substr(13);
    }
    return out;
}

const char* PixelFormatName(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatBGRA8Unorm: return "BGRA8Unorm";
    case MTLPixelFormatBGRA8Unorm_sRGB: return "BGRA8Unorm_sRGB";
    case MTLPixelFormatRGBA16Float: return "RGBA16Float";
    case MTLPixelFormatRGB10A2Unorm: return "RGB10A2Unorm";
    case MTLPixelFormatBGR10A2Unorm: return "BGR10A2Unorm";
    default: return "other";
    }
}

CGDirectDisplayID DisplayOf(NSWindow* window) {
    NSScreen* screen = window.screen ? window.screen : NSScreen.mainScreen;
    NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
    return number ? CGDirectDisplayID(number.unsignedIntValue) : CGMainDisplayID();
}

NSView* ViewOf(CAMetalLayer* layer) {
    id delegate = layer.delegate; // a layer-backed view is its layer's delegate
    return [delegate isKindOfClass:NSView.class] ? (NSView*)delegate : nil;
}

std::string ModeText(CGDirectDisplayID display) {
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (!mode) {
        return "unknown";
    }
    char text[160];
    std::snprintf(text, sizeof(text), "%zux%zu pixels (%zux%zu points) %.0f Hz", CGDisplayModeGetPixelWidth(mode),
                  CGDisplayModeGetPixelHeight(mode), CGDisplayModeGetWidth(mode), CGDisplayModeGetHeight(mode),
                  CGDisplayModeGetRefreshRate(mode));
    CGDisplayModeRelease(mode);
    return text;
}

/// The window side (AppKit wants the main thread): the view tree and the windows on screen.
void DumpWindow(NSWindow* window, NSView* metal_view) {
    if (!window) {
        std::printf("Layer dump: window: none (the layer has no view/window yet)\n");
        return;
    }
    NSView* content = window.contentView;
    std::string subviews;
    for (NSView* view in content.subviews) {
        subviews += std::string(subviews.empty() ? "" : ", ") + view.className.UTF8String +
                    (view == metal_view ? " (the swapchain's)" : "") + (view.hidden ? " hidden" : "");
    }
    CALayer* content_layer = content.layer;
    std::printf("Layer dump: window #%ld visible %d opaque %d fullscreen %d backing scale %.2f; "
                "contentView %s, layer %s (opaque %d, sublayers %lu), subviews [%s]\n",
                long(window.windowNumber), int(window.visible), int(window.opaque),
                int((window.styleMask & NSWindowStyleMaskFullScreen) != 0), window.backingScaleFactor,
                content.className.UTF8String, content_layer ? content_layer.className.UTF8String : "none",
                int(content_layer.opaque), (unsigned long)content_layer.sublayers.count, subviews.c_str());
    // Every on-screen window over the game's display: anything above the game (another app's
    // window, a utility panel, an overlay) makes the WindowServer composite.
    const CGRect bounds = CGDisplayBounds(DisplayOf(window));
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                 kCGNullWindowID);
    const pid_t self = getpid();
    int shown = 0, total = 0;
    for (NSDictionary* info in (__bridge NSArray*)list) {
        CGRect frame{};
        if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(id)kCGWindowBounds], &frame) ||
            !CGRectIntersectsRect(frame, bounds)) {
            continue;
        }
        ++total;
        if (shown >= 12) {
            continue;
        }
        ++shown;
        const int pid = [info[(id)kCGWindowOwnerPID] intValue];
        // The owning app only, not the window's title: logs are shared in reports.
        NSString* owner = info[(id)kCGWindowOwnerName];
        std::printf("Layer dump: on screen: #%d %s%s level %d alpha %.2f %.0fx%.0f at %.0f,%.0f\n",
                    [info[(id)kCGWindowNumber] intValue], owner ? owner.UTF8String : "?",
                    pid == self ? " (this process)" : "",
                    [info[(id)kCGWindowLayer] intValue], [info[(id)kCGWindowAlpha] doubleValue], frame.size.width,
                    frame.size.height, frame.origin.x, frame.origin.y);
    }
    if (total > shown) {
        std::printf("Layer dump: on screen: %d more\n", total - shown);
    }
    if (list) {
        CFRelease(list);
    }
    std::fflush(stdout);
}

/// Applies the opaque/colour-space experiments to the layer; logs what changed.
void ApplyOverrides(CAMetalLayer* layer) {
    const auto& s = BbSettings::Get();
    const int opaque = s.layer_opaque.load();
    const int colorspace = s.layer_colorspace.load();
    if (opaque < 0 && colorspace < 0) {
        return;
    }
    const BOOL was_opaque = layer.opaque;
    const std::string was_space = Name(layer.colorspace);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (opaque >= 0) {
        layer.opaque = opaque != 0;
    }
    if (colorspace == 0) {
        layer.colorspace = nil;
    } else if (colorspace == 1) {
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer.colorspace = srgb;
        CGColorSpaceRelease(srgb);
    } else if (colorspace == 2) {
        NSView* view = ViewOf(layer);
        NSScreen* screen = view.window.screen ? view.window.screen : NSScreen.mainScreen;
        layer.colorspace = screen.colorSpace.CGColorSpace;
    }
    [CATransaction commit];
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 4) {
        std::printf("Layer: override (layer_opaque=%d layer_colorspace=%d): opaque %d -> %d, colorspace %s -> %s\n",
                    opaque, colorspace, int(was_opaque), int(layer.opaque), was_space.c_str(),
                    Name(layer.colorspace).c_str());
        std::fflush(stdout);
    }
}

} // namespace

/// After each swapchain (vulkan/swapchain.cpp, the presenting thread): the overrides, then one
/// dump per drawable size.
extern "C" void bb_layer_after_swapchain(void* metal_layer) {
    if (!metal_layer) {
        return;
    }
    @autoreleasepool {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer;
        if (![layer isKindOfClass:CAMetalLayer.class]) {
            return;
        }
        ApplyOverrides(layer);
        static const bool dump = [] {
            const char* env = std::getenv("BB_LAYER_DUMP");
            return !env || env[0] != '0';
        }();
        static CGSize dumped{};
        const CGSize drawable = layer.drawableSize;
        if (!dump || CGSizeEqualToSize(drawable, dumped)) {
            return;
        }
        dumped = drawable;
        NSView* view = ViewOf(layer);
        NSWindow* window = view.window;
        const CGDirectDisplayID display = window ? DisplayOf(window) : CGMainDisplayID();
        std::printf("Layer dump: CAMetalLayer opaque %d colorspace %s pixelFormat %s contentsScale %.2f "
                    "drawableSize %.0fx%.0f bounds %.0fx%.0f framebufferOnly %d wantsEDR %d "
                    "displaySync %d maxDrawables %lu sublayers %lu superlayer %s\n",
                    int(layer.opaque), Name(layer.colorspace).c_str(), PixelFormatName(layer.pixelFormat),
                    layer.contentsScale, drawable.width, drawable.height, layer.bounds.size.width,
                    layer.bounds.size.height, int(layer.framebufferOnly), int(layer.wantsExtendedDynamicRangeContent),
                    int(layer.displaySyncEnabled), (unsigned long)layer.maximumDrawableCount,
                    (unsigned long)layer.sublayers.count,
                    layer.superlayer ? layer.superlayer.className.UTF8String : "none");
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
        const size_t mode_w = mode ? CGDisplayModeGetPixelWidth(mode) : 0;
        const size_t mode_h = mode ? CGDisplayModeGetPixelHeight(mode) : 0;
        if (mode) {
            CGDisplayModeRelease(mode);
        }
        std::printf("Layer dump: display %u mode %s; drawable %s the mode's pixels%s\n", display,
                    ModeText(display).c_str(),
                    size_t(drawable.width) == mode_w && size_t(drawable.height) == mode_h ? "matches" : "differs from",
                    window && window.visible ? "" : " (window hidden: windowed, no mode switch)");
        std::fflush(stdout);
        dispatch_async(dispatch_get_main_queue(), ^{
          DumpWindow(window, view);
        });
    }
}

/// The panel's native pixel size (window.cpp: the output's default and the monitor size): the
/// largest mode CoreGraphics flags native (kDisplayModeNativeFlag), counting the scaled HiDPI
/// variants by their pixels. 0 when no mode carries the flag (some projectors, AirPlay).
extern "C" int bb_display_native_pixels(void* nswindow, int* width, int* height) {
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nswindow;
        const CGDirectDisplayID display = window ? DisplayOf(window) : CGMainDisplayID();
        constexpr uint32_t NativeFlag = 0x02000000; // IOGraphicsTypes.h kDisplayModeNativeFlag
        NSDictionary* options = @{(__bridge id)kCGDisplayShowDuplicateLowResolutionModes : @YES};
        CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, (__bridge CFDictionaryRef)options);
        size_t best_w = 0, best_h = 0;
        for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); ++i) {
            CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            const size_t w = CGDisplayModeGetPixelWidth(mode), h = CGDisplayModeGetPixelHeight(mode);
            if ((CGDisplayModeGetIOFlags(mode) & NativeFlag) && w * h > best_w * best_h) {
                best_w = w;
                best_h = h;
            }
        }
        if (modes) {
            CFRelease(modes);
        }
        *width = int(best_w);
        *height = int(best_h);
        return best_w > 0 ? 1 : 0;
    }
}

/// The display mode of the window's display, logged after a mode switch (window.cpp).
extern "C" void bb_log_display_mode(void* nswindow, const char* why) {
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nswindow;
        const CGDirectDisplayID display = window ? DisplayOf(window) : CGMainDisplayID();
        std::printf("Display mode (%s): display %u now %s\n", why, display, ModeText(display).c_str());
        std::fflush(stdout);
    }
}
