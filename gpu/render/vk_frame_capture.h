// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame analyzer. Creating the file named by BB_CAPTURE_TRIGGER records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented. Input for placing a temporal
// upscaler (scene color, depth, motion vectors, UI) in Bloodborne's frame.

#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"

namespace VideoCore {
struct ImageInfo;
}

namespace Vulkan {

class FrameCapture {
public:
    /// VideoOut thread, per flip: frame boundary, presented buffer, trigger file check.
    static void OnFlip(VAddr presented_address);

    /// VideoOut: a display buffer the game registered. A pass drawing into one starts a frame
    /// in the GPU command stream (the flip itself runs a frame behind).
    static void AddDisplayBuffer(VAddr address);
    static bool IsDisplayBuffer(VAddr address);

    /// GPU thread: whether passes are being looked at (armed or recording; cheap).
    static bool Active() {
        return state.load(std::memory_order_relaxed) != Idle;
    }
    /// GPU thread, at every draw/dispatch: starts or ends a recording at frame boundaries.
    static void Poll();

    static void BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                          const VideoCore::ImageInfo* depth);

    // bbport BB_PASS_TRACE (bench/vertex-passes, default off): one line per RENDER ENCODER (not per
    // pass the guest asked for: a break, a split or a resume makes a new encoder and a new line),
    // carrying the identity the Metal trace cannot see - target size and formats, draw and
    // primitive counts, and the vertex/pixel shader hashes - so a trace's per-encoder vertex time
    // can be joined to a named pass. Written only inside a triggered window (BB_PASS_TRACE_TRIGGER
    // + BB_PASS_TRACE_FRAMES); with BB_PASS_TRACE unset every call is one cached-false branch.
    static bool PassTraceEnabled();
    /// Rasterizer::BeginRendering: the attachments of the pass about to be opened (stashed for the
    /// next encoder that actually begins; a draw that reuses an open pass overwrites nothing).
    static void PassTraceTargets(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                                 const VideoCore::ImageInfo* depth);
    /// Scheduler::BeginRendering, after the "same state, already rendering" early return: an
    /// encoder begins here.
    static void PassTraceBegin(u32 width, u32 height, u32 num_colors);
    static void PassTraceDraw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances);
    /// Scheduler::EndRendering: the encoder ends, its line goes out.
    static void PassTraceEnd();
    /// VideoOut, per flip: the recording window's frame count and trigger file.
    static void PassTraceFlip();
    /// An encoder opened outside Scheduler::BeginRendering (the port's present pass, the scene
    /// resample, the overlay): one line of its own, so the pass sequence keeps the trace's count.
    static void PassTraceExternal(u32 width, u32 height, u32 draws, const char* what);
    static void Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    static void Sampled(const VideoCore::ImageInfo& info, bool storage);
    /// Contents of a bound buffer (first 1 KiB), kept for small passes and a pass's first draw.
    static void Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size);
    static void Note(const char* text);

private:
    enum : u32 { Idle, Armed, Recording };
    static inline std::atomic<u32> state{Idle};
    static inline std::atomic<u64> flips{0};
    static inline std::atomic<VAddr> last_presented{0};
};

} // namespace Vulkan
