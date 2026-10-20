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

/// Whether the display's current mode is scanned out 1:1 or scaled on the way to the panel. A
/// mode's pixel size (CGDisplayModeGetPixelWidth/Height) is the framebuffer the WindowServer
/// scans out; the panel has one physical size, the largest mode flagged native
/// (kDisplayModeNativeFlag, IOGraphicsTypes.h). A 1x mode smaller than that (2560x1440 on a
/// 3440x1440 panel) is a framebuffer the display pipe (Apple silicon: DCP) scales up to the
/// panel's pixels — or the monitor's own scaler does — so text is resampled (soft) whichever
/// does it; only a native-flagged mode is 1:1. A pixel size above the panel (6880x2880 for
/// "looks like 3440x1440") is the HiDPI backing store, scaled down.
std::string ScanoutVerdict(CGDirectDisplayID display) {
    constexpr uint32_t NativeFlag = 0x02000000;     // kDisplayModeNativeFlag
    constexpr uint32_t StretchedFlag = 0x00000020;  // kDisplayModeStretchedFlag
    constexpr uint32_t SimulscanFlag = 0x00000100;  // kDisplayModeSimulscanFlag
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (!mode) {
        return "unknown (no current mode)";
    }
    const size_t w = CGDisplayModeGetPixelWidth(mode), h = CGDisplayModeGetPixelHeight(mode);
    const uint32_t flags = CGDisplayModeGetIOFlags(mode);
    const int32_t mode_id = CGDisplayModeGetIODisplayModeID(mode);
    CGDisplayModeRelease(mode);
    size_t native_w = 0, native_h = 0;
    NSDictionary* options = @{(__bridge id)kCGDisplayShowDuplicateLowResolutionModes : @YES};
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, (__bridge CFDictionaryRef)options);
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); ++i) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        const size_t mw = CGDisplayModeGetPixelWidth(m), mh = CGDisplayModeGetPixelHeight(m);
        if ((CGDisplayModeGetIOFlags(m) & NativeFlag) && mw * mh > native_w * native_h) {
            native_w = mw;
            native_h = mh;
        }
    }
    if (modes) {
        CFRelease(modes);
    }
    char text[320];
    const char* verdict;
    if (native_w == 0) {
        verdict = "no mode is flagged native: cannot tell";
    } else if (w == native_w && h == native_h) {
        verdict = "1:1 scanout (the panel's native pixels)";
    } else if (w > native_w || h > native_h) {
        verdict = "NOT 1:1: a HiDPI backing store scaled down to the panel";
    } else {
        verdict = "NOT 1:1: a framebuffer smaller than the panel, scaled up to it by the display "
                  "pipe or the monitor (resampled: text is soft)";
    }
    std::snprintf(text, sizeof(text), "%s; mode %zux%zu ioflags 0x%08x (native %d stretched %d simulscan %d) id %d, panel native %zux%zu",
                  verdict, w, h, flags, int((flags & NativeFlag) != 0), int((flags & StretchedFlag) != 0),
                  int((flags & SimulscanFlag) != 0), mode_id, native_w, native_h);
    return text;
}

/// The display's own colour space (its ICC profile, CGDisplayCopyColorSpace) and whether the
/// layer's tag equals it: a tag different from the display's makes the WindowServer colour-match
/// the drawable (a pass of its own: Composited).
std::string DisplaySpaceText(CGDirectDisplayID display, CGColorSpaceRef layer_space) {
    CGColorSpaceRef space = CGDisplayCopyColorSpace(display);
    if (!space) {
        return "unknown";
    }
    std::string text = Name(space);
    if (text == "(unnamed)") {
        // An ICC profile without a CoreGraphics name: the profile's own description.
        for (NSScreen* screen in NSScreen.screens) {
            NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
            if (number && CGDirectDisplayID(number.unsignedIntValue) == display && screen.colorSpace.localizedName) {
                text = std::string("profile \"") + screen.colorSpace.localizedName.UTF8String + "\"";
            }
        }
    }
    text += layer_space ? (CFEqual(space, layer_space) ? ", the layer's tag equals it"
                                                                               : ", the layer's tag differs (colour-matched)")
                        : ", the layer is untagged (no matching)";
    CGColorSpaceRelease(space);
    return text;
}

/// Layers composited above the CAMetalLayer: its own sublayers, then at every level up to the
/// window's root the siblings ordered after it (drawn on top), visible ones only.
std::string LayersAbove(CALayer* metal) {
    std::string out;
    int count = 0;
    const auto add = [&](CALayer* layer, const char* where) {
        if (layer.hidden || layer.opacity <= 0.0f) {
            return;
        }
        if (++count <= 6) {
            // The view owning it (a layer-backed view is its layer's delegate): which AppKit part it is.
            id delegate = layer.delegate;
            const char* owner = delegate ? [delegate className].UTF8String : "no view";
            char text[256];
            std::snprintf(text, sizeof(text), "%s%s %s (%s) %.0fx%.0f opaque %d opacity %.2f", out.empty() ? "" : ", ",
                          where, layer.className.UTF8String, owner, layer.bounds.size.width, layer.bounds.size.height,
                          int(layer.opaque), layer.opacity);
            out += text;
        }
    };
    for (CALayer* sub in metal.sublayers) {
        add(sub, "sublayer");
    }
    for (CALayer* child = metal, *parent = metal.superlayer; parent; child = parent, parent = parent.superlayer) {
        NSArray<CALayer*>* siblings = parent.sublayers;
        const NSUInteger index = [siblings indexOfObjectIdenticalTo:child];
        for (NSUInteger i = index == NSNotFound ? siblings.count : index + 1; i < siblings.count; ++i) {
            add(siblings[i], "sibling");
        }
    }
    if (count > 6) {
        out += ", " + std::to_string(count - 6) + " more";
    }
    return count ? std::to_string(count) + " [" + out + "]" : "none";
}

std::string PresentationOptionsText(NSApplicationPresentationOptions o) {
    char text[200];
    std::snprintf(text, sizeof(text), "0x%lx (hideMenuBar %d autoHideMenuBar %d hideDock %d autoHideDock %d fullScreen %d)",
                  (unsigned long)o, int((o & NSApplicationPresentationHideMenuBar) != 0),
                  int((o & NSApplicationPresentationAutoHideMenuBar) != 0), int((o & NSApplicationPresentationHideDock) != 0),
                  int((o & NSApplicationPresentationAutoHideDock) != 0), int((o & NSApplicationPresentationFullScreen) != 0));
    return text;
}

/// The window side (AppKit wants the main thread): the view tree and the windows on screen.
void DumpWindow(NSWindow* window, NSView* metal_view, CAMetalLayer* layer) {
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
    const CGDirectDisplayID display = DisplayOf(window);
    std::printf("Layer dump: window level %ld hasShadow %d; layer presentsWithTransaction %d, layers above it %s\n",
                long(window.level), int(window.hasShadow), int(layer.presentsWithTransaction),
                LayersAbove(layer).c_str());
    std::printf("Layer dump: display %u colour space %s\n", display, DisplaySpaceText(display, layer.colorspace).c_str());
    std::printf("Layer dump: app presentationOptions %s, current system %s\n",
                PresentationOptionsText(NSApp.presentationOptions).c_str(),
                PresentationOptionsText(NSApp.currentSystemPresentationOptions).c_str());
    // Game Mode keys on the frontmost app's bundle (gamepolicyd: LSApplicationCategoryType).
    NSBundle* bundle = NSBundle.mainBundle;
    NSString* category = [bundle objectForInfoDictionaryKey:@"LSApplicationCategoryType"];
    NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;
    std::printf("Layer dump: bundle %s category %s; frontmost app %s (%s)\n",
                bundle.bundleIdentifier ? bundle.bundleIdentifier.UTF8String : "none",
                category ? category.UTF8String : "none",
                front.bundleIdentifier ? front.bundleIdentifier.UTF8String : "?",
                front && front.processIdentifier == getpid() ? "this process" : "another process");
    // Every on-screen window over the game's display: anything above the game (another app's
    // window, a utility panel, an overlay) makes the WindowServer composite.
    const CGRect bounds = CGDisplayBounds(display);
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
        const int level = [info[(id)kCGWindowLayer] intValue];
        if (level == kCGMainMenuWindowLevel) {
            // A menu bar strip: which display it belongs to (its origin's), and whether the game
            // (a borderless fill, not a fullscreen Space) is above it or merely beside it.
            CGDirectDisplayID owner_display = 0;
            uint32_t found = 0;
            CGGetDisplaysWithPoint(CGPointMake(frame.origin.x + 1, frame.origin.y + 1), 1, &owner_display, &found);
            std::printf("Layer dump: menu bar strip #%d %.0fx%.0f at %.0f,%.0f is on display %u (%s; display bounds "
                        "%.0fx%.0f at %.0f,%.0f), game window level %ld %s it\n",
                        [info[(id)kCGWindowNumber] intValue], frame.size.width, frame.size.height, frame.origin.x,
                        frame.origin.y, found ? owner_display : 0,
                        found && owner_display == display ? "the game's" : "another", bounds.size.width,
                        bounds.size.height, bounds.origin.x, bounds.origin.y, long(window.level),
                        window.level > kCGMainMenuWindowLevel ? "above" : "NOT above");
        }
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
                    level, [info[(id)kCGWindowAlpha] doubleValue], frame.size.width,
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
    // This runs on the presenting thread, which has no run loop: the driver's own changes
    // (wsi_metal_layer_configure) opened an implicit transaction there, our block above nests in
    // it, and nothing would ever commit it to the render server — the layer's model would read
    // opaque while the WindowServer still composites the old state. Flush it now.
    [CATransaction flush];
    static std::atomic<int> told{0};
    if (const int n = told.fetch_add(1); n < 8) {
        std::printf("Layer: override #%d (layer_opaque=%d layer_colorspace=%d): opaque %d -> %d, colorspace %s -> %s\n",
                    n + 1, opaque, colorspace, int(was_opaque), int(layer.opaque), was_space.c_str(),
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
        std::printf("Layer dump: scanout: %s\n", ScanoutVerdict(display).c_str());
        std::fflush(stdout);
        dispatch_async(dispatch_get_main_queue(), ^{
          DumpWindow(window, view, layer);
        });
        // A few seconds on (the mode switch and the fullscreen fill settled): is the layer still
        // as the overrides left it (nothing between swapchains reverted it), and are the menu bar
        // and Dock hidden. One line; the HUD's Direct/Composited reading goes with this state.
        const int want_opaque = BbSettings::Get().layer_opaque.load();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(4 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
          NSWindow* now = ViewOf(layer).window;
          std::printf("Layer check (+4 s): opaque %d%s colorspace %s drawableSize %.0fx%.0f; window %s level %ld "
                      "fullscreen %d; presentationOptions %s\n",
                      int(layer.opaque),
                      want_opaque < 0 ? "" : (int(layer.opaque) == want_opaque ? " (as overridden)" : " (REVERTED)"),
                      Name(layer.colorspace).c_str(), layer.drawableSize.width, layer.drawableSize.height,
                      now.visible ? "visible" : "hidden", long(now.level),
                      int((now.styleMask & NSWindowStyleMaskFullScreen) != 0),
                      PresentationOptionsText(NSApp.currentSystemPresentationOptions).c_str());
          std::fflush(stdout);
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
        std::printf("Display mode (%s): display %u now %s; scanout: %s\n", why, display, ModeText(display).c_str(),
                    ScanoutVerdict(display).c_str());
        std::fflush(stdout);
    }
}

/// The window's display: the pixel size of the mode it is on now (window.cpp: the mode-set settle).
extern "C" int bb_display_current_pixels(void* nswindow, int* width, int* height) {
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nswindow;
        const CGDirectDisplayID display = window ? DisplayOf(window) : CGMainDisplayID();
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
        if (!mode) {
            *width = *height = 0;
            return 0;
        }
        const int w = int(CGDisplayModeGetPixelWidth(mode));
        const int h = int(CGDisplayModeGetPixelHeight(mode));
        CFRelease(mode);
        *width = w;
        *height = h;
        return w > 0 && h > 0;
    }
}

/// Waits for a display-mode set to have landed: CGDisplaySetDisplayMode takes effect asynchronously
/// (SDL's fullscreen transition returns first), so the only evidence that it is done is that the
/// display reports a different pixel size — stable for two samples. 1 = changed and settled, 2 =
/// never left the old size (the pick was already the current mode, or the set did not take).
extern "C" int bb_display_wait_mode_change(void* nswindow, int old_width, int old_height, int timeout_ms) {
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nswindow;
        const CGDirectDisplayID display = window ? DisplayOf(window) : CGMainDisplayID();
        const NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeout_ms / 1000.0];
        int last_w = 0, last_h = 0, stable = 0;
        while ([deadline timeIntervalSinceNow] > 0) {
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
            int w = 0, h = 0;
            if (mode) {
                w = int(CGDisplayModeGetPixelWidth(mode));
                h = int(CGDisplayModeGetPixelHeight(mode));
                CFRelease(mode);
            }
            if (w != old_width || h != old_height) {
                stable = (w == last_w && h == last_h) ? stable + 1 : 0;
                last_w = w;
                last_h = h;
                if (stable >= 1) {
                    return 1;
                }
            }
            [NSThread sleepForTimeInterval:0.01];
        }
        return 2;
    }
}

// Taking the display for Direct scan-out. WindowServer only hands a window the direct path when the
// app owns the display: the menu bar and the Dock hidden, the window the only thing there. The
// 2026-10-16 run that reached Direct logged app presentationOptions 0x40a (hideMenuBar 1 hideDock 1
// fullScreen 1) with the window at level 0; the 2026-10-20 runs log 0x0, the window at level 25, the
// menu bar strip still on the display, and composite — SDL reported the window fullscreen in both,
// so its own flag does not tell the two apart.
extern "C" int bb_window_take_display(void* nswindow) {
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nswindow;
        if (!window) {
            return 0;
        }
        const NSApplicationPresentationOptions want =
            NSApplicationPresentationHideMenuBar | NSApplicationPresentationHideDock;
        const NSApplicationPresentationOptions before = NSApp.presentationOptions;
        if ((before & want) != want) {
            @try {
                [NSApp setPresentationOptions:want];
            } @catch (NSException* e) {
                std::printf("Window: presentation options refused (%s)\n", [[e reason] UTF8String]);
                return 0;
            }
        }
        const NSApplicationPresentationOptions after = NSApp.presentationOptions;
        const int took = (after & want) == want;
        std::printf("Window: %s the display: presentationOptions 0x%lx (was 0x%lx), window level %ld\n",
                    took ? "took" : "did NOT take", (unsigned long)after, (unsigned long)before,
                    (long)window.level);
        return took;
    }
}

extern "C" void bb_window_release_display(void) {
    @autoreleasepool {
        if (NSApp.presentationOptions != NSApplicationPresentationDefault) {
            @try {
                [NSApp setPresentationOptions:NSApplicationPresentationDefault];
            } @catch (NSException* e) {
                std::printf("Window: presentation options not restored (%s)\n", [[e reason] UTF8String]);
            }
        }
    }
}
