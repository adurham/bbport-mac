// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): what the capture hotkey (window.cpp, Ctrl+Shift+C) needs from the platform: a
// PNG written from pixels the port already has, and the swapchain layer's own state.
//
// The pixels are the frame bbport_overlay.cpp read back from the presented swapchain image, so
// nothing here asks for the screen-recording permission CoreGraphics' own window capture would
// (macOS 10.15 and later), and the image is exactly the frame the port drew, not what the
// compositor happened to have on screen at another moment.
#pragma once

namespace BbCapture {

/// The swapchain's CAMetalLayer as bbport_layer.mm's dump has it, for the capture's log line.
struct LayerState {
    int opaque = -1;          ///< the layer's opaque flag (-1: no layer)
    char colorspace[64] = {}; ///< the layer's colour space, short name (bbport_layer.mm's Name)
    int drawable_width = 0;   ///< the drawable's pixel size (retina density included)
    int drawable_height = 0;
    int pixel_format = 0;     ///< MTLPixelFormat as an int
    float contents_scale = 0.0f;
    int framebuffer_only = -1;
};

} // namespace BbCapture

#if defined(__APPLE__)

/// bbport_capture.mm: writes `pixels` (32 bits per pixel, `stride` bytes per row) as a PNG at
/// `path`; `bgra` 1: the rows are B,G,R,A (MoltenVK's swapchain order), 0: R,G,B,A. The pixels
/// are copied first, so the caller's buffer may go at once. Returns without waiting for the
/// file; the encode of a 5-megapixel frame would otherwise stall the presenting thread.
extern "C" void bb_capture_write_png_async(const char* path, const void* pixels, int width,
                                           int height, long stride, int bgra);
/// The same, synchronously (tests, and any caller that wants the result in hand). True when the
/// file was written.
extern "C" int bb_capture_write_png(const char* path, const void* pixels, int width, int height,
                                    long stride, int bgra);
/// bbport_capture.mm: fills `out` from the swapchain's layer; a null layer leaves it cleared.
extern "C" void bb_capture_layer_state(void* metal_layer, BbCapture::LayerState* out);

#else

// Outside macOS there is no capture (the port's capture hotkey is a macOS debugging aid): the
// log line still says what it knows, and no file is written.
static inline void bb_capture_write_png_async(const char*, const void*, int, int, long, int) {}
static inline int bb_capture_write_png(const char*, const void*, int, int, long, int) { return 0; }
static inline void bb_capture_layer_state(void*, BbCapture::LayerState* out) {
    if (out) {
        *out = BbCapture::LayerState{};
    }
}

#endif
