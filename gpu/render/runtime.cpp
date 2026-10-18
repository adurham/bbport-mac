// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <boost/container/small_vector.hpp>
#include <vulkan/vulkan_format_traits.hpp>

#include "bbport_toggles.h"
#include "render/gcn_to_vk.h"
#include "render/vk_gpu_profiler.h"
#include "vulkan/barrier_masks.h"
#include "textures/blitter.h"
#include "upscale/vk_scene_resolution.h"
#include "vulkan/instance.h"
#include "render/scheduler.h"

namespace Vulkan {

namespace {

using Stage = vk::PipelineStageFlagBits2;
using AccessBit = vk::AccessFlagBits2;
using Layout = vk::ImageLayout;

constexpr vk::AccessFlags2 ReadAccesses =
    AccessBit::eIndexRead | AccessBit::eVertexAttributeRead | AccessBit::eUniformRead |
    AccessBit::eShaderRead | AccessBit::eColorAttachmentRead |
    AccessBit::eDepthStencilAttachmentRead | AccessBit::eTransferRead | AccessBit::eMemoryRead;
constexpr vk::AccessFlags2 WriteAccesses =
    AccessBit::eShaderWrite | AccessBit::eColorAttachmentWrite |
    AccessBit::eDepthStencilAttachmentWrite | AccessBit::eTransferWrite | AccessBit::eMemoryWrite |
    AccessBit::eTransformFeedbackWriteEXT;

/// The Vulkan image type of a GCN one (1D, 2D or 3D).
vk::ImageType DimensionOf(Gcn::ImageType type) noexcept {
    switch (type) {
    case Gcn::ImageType::Color1D:
    case Gcn::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case Gcn::ImageType::Color2D:
    case Gcn::ImageType::Color2DMsaa:
    case Gcn::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case Gcn::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE_MSG("image type {}", u32(type));
    }
}

/// Layer counts for a copy that Vulkan accepts: 3D images have one; same types copy the same
/// number; between 2D and 3D the 2D layers match the 3D depth.
std::pair<u32, u32> CopyLayers(const VideoCore::ImageInfo& src, const VideoCore::ImageInfo& dst,
                               u32 depth) {
    const auto src_type = DimensionOf(src.type);
    const auto dst_type = DimensionOf(dst.type);
    u32 src_layers = src_type == vk::ImageType::e3D ? 1 : src.resources.layers;
    u32 dst_layers = dst_type == vk::ImageType::e3D ? 1 : dst.resources.layers;
    if (src_type == dst_type) {
        src_layers = dst_layers = std::min(src_layers, dst_layers);
    } else if (src_type == vk::ImageType::e2D && dst_type == vk::ImageType::e3D) {
        src_layers = depth;
    } else if (src_type == vk::ImageType::e3D && dst_type == vk::ImageType::e2D) {
        dst_layers = depth;
    }
    return {src_layers, dst_layers};
}

/// The bytes of the buffer a buffer <-> image copy reaches (from its offset).
u32 BufferImageCopySize(const vk::BufferImageCopy& copy, vk::Format format) {
    const u32 row_length = copy.bufferRowLength ? copy.bufferRowLength : copy.imageExtent.width;
    const u32 height = copy.bufferImageHeight ? copy.bufferImageHeight : copy.imageExtent.height;
    const auto block = vk::blockExtent(format);
    const u32 block_size = vk::blockSize(format);
    const u32 row_pitch = (row_length / block[0]) * block_size;
    const u32 slice_pitch = (height / block[1]) * row_pitch;
    const u32 width_blocks = (copy.imageExtent.width + block[0] - 1) / block[0];
    const u32 height_blocks = (copy.imageExtent.height + block[1] - 1) / block[1];
    const u32 slices = copy.imageExtent.depth * copy.imageSubresource.layerCount;
    return (slices - 1) * slice_pitch + (height_blocks - 1) * row_pitch + width_blocks * block_size;
}

/// The written image now holds what the source holds (or new GPU data) and no stale upload.
void MarkCopied(VideoCore::Image* dst, VideoCore::ImageFlagBits modified) {
    dst->flags |= modified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

/// BB_COPY_MERGE: copies that waited for a pass's end, per 10 s window (printed when the gate is
/// on only). `late` counts accesses a waiting copy touched that the pre-draw check did not end
/// the pass for: any of those is a copy reordered past that access (the gate then is unproven).
struct CopyMergeStats {
    u64 copies = 0, batches = 0, largest = 0, late = 0, breaks = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
CopyMergeStats merge_stats;
void ReportCopyMerge() {
    auto& s = merge_stats;
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last < std::chrono::seconds(10)) {
        return;
    }
    s.last = now;
    std::printf("Copy merge (10 s): %llu copies in %llu pass-end batches (%llu in the largest), "
                "%llu of the batches at a hazard break, %llu late conflicts\n",
                static_cast<unsigned long long>(s.copies),
                static_cast<unsigned long long>(s.batches),
                static_cast<unsigned long long>(s.largest),
                static_cast<unsigned long long>(s.breaks),
                static_cast<unsigned long long>(s.late));
    s.copies = s.batches = s.largest = s.late = s.breaks = 0;
}

/// BB_INPASS_BARRIERS: memory-only barriers recorded inside the open render pass, per 10 s window
/// (printed while the gate is on). `ended` counts the ones that still ended the pass (an image
/// transition, no pass open, or a destination other than ALL_COMMANDS).
struct InPassBarrierStats {
    u64 kept = 0, ended = 0, images = 0, idle = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
InPassBarrierStats inpass_stats;
void ReportInPassBarriers() {
    auto& s = inpass_stats;
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last < std::chrono::seconds(10)) {
        return;
    }
    s.last = now;
    std::printf("In-pass barriers (10 s): %llu memory barriers kept inside the open pass, %llu "
                "ended it (%llu carried an image transition, %llu with no pass open)\n",
                static_cast<unsigned long long>(s.kept), static_cast<unsigned long long>(s.ended),
                static_cast<unsigned long long>(s.images), static_cast<unsigned long long>(s.idle));
    s.kept = s.ended = s.images = s.idle = 0;
}
} // namespace

Runtime::Runtime(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_},
      blitter{std::make_unique<VideoCore::ImageBlitter>(instance_, scheduler_)},
      staging_pool{instance_, scheduler_} {}

Runtime::~Runtime() = default;

void Runtime::TickFrame() {
    staging_pool.TickFrame();
}

bool Runtime::IsBufferAccessed(const VideoCore::Buffer* buffer, u64 offset, u64 size,
                               bool check_read, vk::PipelineStageFlags2 consumer) {
    const u64 id = reinterpret_cast<u64>(buffer);
    const u64 last = offset + size - 1;
    const auto overlaps = [&](const AccessTracker& tracker) {
        return tracker.Overlaps(id, offset, last, AccessTracker::Kind::Write) ||
               (check_read && tracker.Overlaps(id, offset, last, AccessTracker::Kind::Read));
    };
    if (!NarrowBarriers()) {
        // a paged write may have touched it (AccessGlobal)
        return global_write || overlaps(accesses);
    }
    // BB_BARRIER_MASKS: accesses since the last barrier, or before a narrowed one that did not
    // order this consumer's stages after them.
    // (A paged read of any memory before a narrowed barrier: a later writer in other stages
    // waits for it. Legacy orders it only through whatever full barrier happens in between.)
    const bool hit = global_write || overlaps(accesses) ||
                     (bool(consumer & ~carried_visible) &&
                      ((!carried.Empty() && overlaps(carried)) ||
                       (check_read && carried_global_read)));
    if (hit) {
        flush_consumers |= consumer;
    }
    return hit;
}

void Runtime::AccessBuffer(const VideoCore::Buffer* buffer, u64 offset, u64 size,
                           vk::PipelineStageFlags2 stage, vk::AccessFlags2 access) {
    if (!deferred_copies.empty() && DeferredCopyOverlaps(buffer, offset, size)) {
        // BB_COPY_MERGE: a command that was recorded after a waiting copy reads or writes bytes
        // it touches. The pre-draw check (Rasterizer::BeginDraw) ends the pass for every such
        // draw, so this is a path that reaches buffers without going through the rasterizer's
        // bindings: the copy has been reordered past it (counted, and reported by
        // `Copy merge`). End the pass here so later accesses cannot pass the copies too.
        ++deferred_late_hits;
        merge_stats.late = deferred_late_hits;
        scheduler.EndRendering();
    }
    const u64 id = reinterpret_cast<u64>(buffer);
    u64 first = offset;
    u64 last = offset + size - 1;
    if (access & WriteAccesses) {
        accesses.Insert(id, first, last, AccessTracker::Kind::Write);
    }
    if (access & ReadAccesses) {
        // Reads at 4 KiB granularity: ring allocations of neighbouring draws join into one
        // range. Wider read ranges only add barriers, never drop one.
        if (!BbToggle::Disabled(BbToggle::CoarseReadTracking)) {
            first &= ~u64{0xFFF};
            last |= 0xFFF;
        }
        accesses.Insert(id, first, last, AccessTracker::Kind::Read);
    }
    pending_stages |= stage;
    pending_writes |= access & WriteAccesses;
}

void Runtime::AccessGlobal(vk::PipelineStageFlags2 stage, vk::AccessFlags2 access) {
    if (!deferred_copies.empty()) {
        // BB_COPY_MERGE: a paged access may touch any buffer of the game's memory, so it may
        // touch a waiting copy's bytes. The pre-draw check ends the pass for paged bindings;
        // this catches the ones that reach here anyway.
        ++deferred_late_hits;
        merge_stats.late = deferred_late_hits;
        scheduler.EndRendering();
    }
    pending_stages |= stage;
    pending_global_read = true;
    if (access & AccessBit::eShaderWrite) {
        pending_writes |= AccessBit::eShaderWrite;
        global_write = true;
    }
}

namespace {
/// BB_FRAME_STATS: what the barriers of Runtime waited in (vulkan/barrier_masks.h).
struct BarrierMaskStats {
    u64 memory_all = 0, memory_vertex = 0, memory_fragment = 0, images_vertex = 0,
        images_fragment = 0, image_only = 0, chains = 0, merged = 0;
    // Vertex-side flushes by the destination bits that made them so.
    u64 why_input = 0, why_vs = 0, why_indirect = 0, why_transfer = 0, why_image_vertex = 0,
        why_other = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
BarrierMaskStats mask_stats;
bool MaskStatsEnabled() {
    static const bool enabled = std::getenv("BB_FRAME_STATS") != nullptr;
    return enabled;
}
void NoteBarrier(bool memory, vk::PipelineStageFlags2 memory_dst,
                 std::span<const vk::ImageMemoryBarrier2> images) {
    if (!MaskStatsEnabled()) {
        return;
    }
    auto& s = mask_stats;
    if (memory) {
        ++(memory_dst & Stage::eAllCommands ? s.memory_all
           : IsVertexSide(memory_dst)        ? s.memory_vertex
                                             : s.memory_fragment);
    } else {
        ++s.image_only;
    }
    vk::PipelineStageFlags2 image_dst{};
    if (!images.empty()) {
        for (const auto& b : images) {
            image_dst |= b.dstStageMask;
            s.chains += b.oldLayout == b.newLayout && !b.srcAccessMask;
        }
        ++(IsVertexSide(image_dst) ? s.images_vertex : s.images_fragment);
    }
    const auto all = (memory ? memory_dst : vk::PipelineStageFlags2{}) | image_dst;
    if (IsVertexSide(all) && !(all & Stage::eAllCommands)) {
        using S = Stage;
        if (all & (S::eVertexInput | S::eIndexInput | S::eVertexAttributeInput)) ++s.why_input;
        if (all & S::eVertexShader) ++s.why_vs;
        if (all & S::eDrawIndirect) ++s.why_indirect;
        if (all & (S::eAllTransfer | S::eCopy | S::eClear | S::eResolve | S::eBlit)) ++s.why_transfer;
        if (image_dst & (S::eVertexShader | S::eAllGraphics)) ++s.why_image_vertex;
        if (all & ~(FragmentSideStages | S::eVertexInput | S::eIndexInput |
                    S::eVertexAttributeInput | S::eVertexShader | S::eDrawIndirect |
                    S::eAllTransfer | S::eCopy | S::eClear | S::eResolve | S::eBlit))
            ++s.why_other;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last >= std::chrono::seconds(10)) {
        s.last = now;
        std::printf("Barrier masks (10 s, %s): memory barriers waiting in all %llu, vertex side "
                    "%llu, fragment side %llu; image barrier batches vertex side %llu, fragment "
                    "side %llu (%llu without a memory barrier); %llu new-stage chains, %llu "
                    "merged into a queued barrier; vertex side because of: vertex input %llu, "
                    "vertex shader %llu, indirect %llu, transfer %llu, images in vertex/all "
                    "graphics %llu, other %llu\n",
                    GetBarrierMasks() == BarrierMasks::Legacy   ? "legacy"
                    : GetBarrierMasks() == BarrierMasks::Strict ? "strict"
                                                                : "safe",
                    (unsigned long long)s.memory_all, (unsigned long long)s.memory_vertex,
                    (unsigned long long)s.memory_fragment, (unsigned long long)s.images_vertex,
                    (unsigned long long)s.images_fragment, (unsigned long long)s.image_only,
                    (unsigned long long)s.chains, (unsigned long long)s.merged,
                    (unsigned long long)s.why_input, (unsigned long long)s.why_vs,
                    (unsigned long long)s.why_indirect, (unsigned long long)s.why_transfer,
                    (unsigned long long)s.why_image_vertex, (unsigned long long)s.why_other);
        const auto keep = s.last;
        s = {};
        s.last = keep;
    }
}
} // namespace

void Runtime::FlushBarriersFor(vk::PipelineStageFlags2 consumers) {
    if (!NarrowBarriers()) {
        FlushBarriers();
        return;
    }
    FlushNarrowed(consumers);
}

void Runtime::FlushNarrowed(vk::PipelineStageFlags2 consumers) {
    BeforeImageAccess();
    consumers |= flush_consumers;
    vk::PipelineStageFlags2 image_dst{};
    for (const auto& b : image_barriers) {
        image_dst |= b.dstStageMask;
    }
    // safe: a memory barrier whenever legacy emitted one (pending accesses), waiting in the
    // buffer consumers and the stages of the image barriers it goes with. strict: only for a
    // buffer conflict a consumer was named for (the tracked hazards).
    const bool strict = GetBarrierMasks() == BarrierMasks::Strict;
    const bool narrowed_any = bool(consumers | image_dst);
    // Every stage waits when the consumer is unknown, or a paged write may have touched anything.
    const bool full = bool(consumers & Stage::eAllCommands) || global_write ||
                      (!strict && !narrowed_any && bool(pending_stages));
    const vk::PipelineStageFlags2 src = pending_stages | carried_stages;
    const bool memory =
        full ? bool(pending_stages | carried_stages) || global_write
             : bool(src) && (strict ? bool(consumers) : bool(pending_stages) || bool(consumers));
    if (!memory && image_barriers.empty()) {
        flush_consumers = {};
        return;
    }
    const vk::PipelineStageFlags2 dst = full ? vk::PipelineStageFlags2(Stage::eAllCommands)
                                      : strict ? consumers
                                               : consumers | image_dst;
    // BB_INPASS_BARRIERS: the pass can stay open across a memory barrier (BarrierStaysInPass).
    const bool mem_only = image_barriers.empty();
    if (BarrierStaysInPass(memory && mem_only, dst)) {
        ++inpass_stats.kept;
    } else {
        if (in_pass_barriers) {
            ++inpass_stats.ended;
            if (!mem_only) {
                ++inpass_stats.images;
            } else if (!scheduler.IsRendering()) {
                ++inpass_stats.idle;
            }
        }
        scheduler.EndRendering();
    }
    if (in_pass_barriers) {
        ReportInPassBarriers();
    }
    NoteBarrier(memory, dst, {image_barriers.data(), image_barriers.size()});
    scheduler.Record([barrier = vk::MemoryBarrier2{
                          .srcStageMask = src,
                          .srcAccessMask = pending_writes | carried_writes,
                          .dstStageMask = dst,
                          .dstAccessMask = AccessBit::eMemoryRead | AccessBit::eMemoryWrite,
                      },
                      memory,
                      images = scheduler.RecordData(std::span<const vk::ImageMemoryBarrier2>(
                          image_barriers.data(), image_barriers.size()))](vk::CommandBuffer cmdbuf) {
        cmdbuf.pipelineBarrier2({
            .memoryBarrierCount = memory ? 1u : 0u,
            .pMemoryBarriers = &barrier,
            .imageMemoryBarrierCount = u32(images.size()),
            .pImageMemoryBarriers = images.data(),
        });
    });
    image_barriers.clear();
    flush_consumers = {};
    if (!memory) {
        return; // the buffer accesses still wait for a barrier that orders them
    }
    if (full) {
        accesses.Clear();
        carried.Clear();
        pending_stages = carried_stages = carried_visible = {};
        pending_writes = carried_writes = {};
        global_write = false;
        pending_global_read = carried_global_read = false;
        return;
    }
    // Ordered before (and visible to) `dst` only: a later consumer in another stage needs one
    // more barrier from these stages. Earlier carried accesses are in this barrier's first
    // scope too, so all of them are now ordered before `dst` (and no more is known).
    carried.MergeFrom(accesses);
    accesses.Clear();
    carried_stages |= pending_stages;
    carried_writes |= pending_writes;
    carried_visible = dst;
    carried_global_read |= pending_global_read;
    pending_global_read = false;
    pending_stages = {};
    pending_writes = {};
}

void Runtime::FlushBarriers() {
    if (NarrowBarriers()) {
        FlushNarrowed(Stage::eAllCommands);
        return;
    }
    BeforeImageAccess();
    const bool memory = bool(pending_stages);
    if (!memory && image_barriers.empty()) {
        return;
    }
    // BB_INPASS_BARRIERS (default off): a memory barrier is legal inside an open render pass, and
    // KosmicKrisp records it there (kk_CmdPipelineBarrier2 with cmd->gfx.encoder set) instead of
    // splitting the encoder, so ending the pass here is not required: the barrier orders what this
    // encoder records next, and every encoder ends ALL->ALL (BB_KK_BARRIER=legacy, kk_stop_encoder),
    // so everything before the pass is already visible to it. The invariant this relies on is
    // exactly that one: an in-pass barrier only orders within this encoder, so any work it must
    // order that ran in an earlier encoder is covered by that encoder's ALL->ALL end. Image
    // transitions, no open pass and a narrowed destination still end the pass (BarrierStaysInPass).
    const bool mem_only = image_barriers.empty();
    if (BarrierStaysInPass(memory && mem_only, Stage::eAllCommands)) {
        ++inpass_stats.kept;
    } else {
        if (in_pass_barriers) {
            ++inpass_stats.ended;
            if (!mem_only) {
                ++inpass_stats.images;
            } else if (!scheduler.IsRendering()) {
                ++inpass_stats.idle;
            }
        }
        scheduler.EndRendering();
    }
    if (in_pass_barriers) {
        ReportInPassBarriers();
    }
    scheduler.Record([barrier = vk::MemoryBarrier2{
                          .srcStageMask = pending_stages,
                          .srcAccessMask = pending_writes,
                          .dstStageMask = Stage::eAllCommands,
                          .dstAccessMask = AccessBit::eMemoryRead | AccessBit::eMemoryWrite,
                      },
                      memory,
                      images = scheduler.RecordData(std::span<const vk::ImageMemoryBarrier2>(
                          image_barriers.data(), image_barriers.size()))](vk::CommandBuffer cmdbuf) {
        cmdbuf.pipelineBarrier2({
            .memoryBarrierCount = memory ? 1u : 0u,
            .pMemoryBarriers = &barrier,
            .imageMemoryBarrierCount = u32(images.size()),
            .pImageMemoryBarriers = images.data(),
        });
    });
    pending_stages = {};
    pending_writes = {};
    image_barriers.clear();
    accesses.Clear();
    global_write = false;
}

bool Runtime::Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                      vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                      std::optional<VideoCore::SubresourceRange> range,
                      std::source_location caller) {
    BeforeImageAccess();
    if (scene_targets) {
        scene_targets->NativeAccess(*image, dst_access, caller);
    }
    const size_t before = image_barriers.size();
    image->GetBarriers(image_barriers, dst_layout, dst_access, dst_stage, range);
    if (NarrowBarriers() && before != 0) {
        MergeQueuedChains(before);
    }
    return image_barriers.size() != before;
}

namespace {
bool Covers(const vk::ImageSubresourceRange& outer, const vk::ImageSubresourceRange& inner,
            u32 levels, u32 layers) {
    const auto end = [](u32 base, u32 count, u32 total) {
        return count == VK_REMAINING_MIP_LEVELS ? total : base + count;
    };
    return (outer.aspectMask & inner.aspectMask) == inner.aspectMask &&
           outer.baseMipLevel <= inner.baseMipLevel &&
           end(outer.baseMipLevel, outer.levelCount, levels) >=
               end(inner.baseMipLevel, inner.levelCount, levels) &&
           outer.baseArrayLayer <= inner.baseArrayLayer &&
           end(outer.baseArrayLayer, outer.layerCount, layers) >=
               end(inner.baseArrayLayer, inner.layerCount, layers);
}
bool Overlap(const vk::ImageSubresourceRange& a, const vk::ImageSubresourceRange& b, u32 levels,
             u32 layers) {
    const auto end = [](u32 base, u32 count, u32 total) {
        return count == VK_REMAINING_MIP_LEVELS ? total : base + count;
    };
    return bool(a.aspectMask & b.aspectMask) && a.baseMipLevel < end(b.baseMipLevel, b.levelCount, levels) &&
           b.baseMipLevel < end(a.baseMipLevel, a.levelCount, levels) &&
           a.baseArrayLayer < end(b.baseArrayLayer, b.layerCount, layers) &&
           b.baseArrayLayer < end(a.baseArrayLayer, a.layerCount, layers);
}
} // namespace

void Runtime::MergeQueuedChains(size_t first_new) {
    // BB_BARRIER_MASKS: barriers in one vkCmdPipelineBarrier2 are not ordered against each other,
    // yet the image state treats a barrier queued after another for the same image as running
    // after it (its first scope is the earlier one's second). Each new barrier that overlaps a
    // queued one: the queued one also waits in the new one's stages (its layout transition and
    // visibility reach them), and the new one also starts from the queued one's first scope (so it
    // orders what that one ordered). A same-layout read the queued one covers is folded into it.
    // Under legacy every sampled destination is ALL_COMMANDS, which covered all of this.
    constexpr u32 Many = 1u << 16;
    for (size_t i = first_new; i < image_barriers.size();) {
        auto& added = image_barriers[i];
        const bool is_chain = added.oldLayout == added.newLayout && !added.srcAccessMask;
        bool covered = false;
        for (size_t j = 0; j < first_new; ++j) {
            auto& queued = image_barriers[j];
            if (queued.image != added.image ||
                !Overlap(queued.subresourceRange, added.subresourceRange, Many, Many)) {
                continue;
            }
            queued.dstStageMask |= added.dstStageMask;
            queued.dstAccessMask |= added.dstAccessMask;
            added.srcStageMask |= queued.srcStageMask;
            added.srcAccessMask |= queued.srcAccessMask;
            covered = covered || (is_chain && queued.newLayout == added.newLayout &&
                                  Covers(queued.subresourceRange, added.subresourceRange, Many,
                                         Many));
        }
        if (covered) {
            if (MaskStatsEnabled()) {
                ++mask_stats.merged;
            }
            image_barriers.erase(image_barriers.begin() + i);
        } else {
            ++i;
        }
    }
}

Runtime::TransferMark::TransferMark(Runtime& runtime, const char* what,
                                    const VideoCore::Image& image) {
    profiler = GpuProfiler::Get();
    if (!profiler || !profiler->Records(&runtime.scheduler)) {
        profiler = nullptr;
        return;
    }
    resume = profiler->Current();
    const auto& info = image.info;
    const u64 key = (u64(reinterpret_cast<uintptr_t>(what)) << 20) ^
                    (u64(info.pixel_format) << 40) ^ (u64(info.size.width) << 16) ^
                    info.size.height ^ 0x7A5Full;
    profiler->Mark(key, [&] {
        return fmt::format("{} {} {}x{}", what, vk::to_string(info.pixel_format),
                           info.size.width, info.size.height);
    });
}

Runtime::TransferMark::~TransferMark() {
    if (profiler && resume) {
        profiler->Resume(resume);
    }
}

bool Runtime::InPassBarriersEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_INPASS_BARRIERS");
        return env && env[0] == '1';
    }();
    return enabled;
}

bool Runtime::BarrierStaysInPass(bool memory_only, vk::PipelineStageFlags2 dst) const {
    // A barrier with an image transition is not in-pass material: the layout change has to happen
    // between encoders. A narrowed destination (BB_BARRIER_MASKS=safe|strict) names stages the
    // driver's split rule looks at on its own, so the gate is limited to the ALL_COMMANDS barrier
    // of the default mask mode.
    if (!in_pass_barriers || !memory_only || !bool(dst & Stage::eAllCommands)) {
        return false;
    }
    return scheduler.IsRendering();
}

bool Runtime::CopyMergeEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_COPY_MERGE");
        return env && env[0] == '1';
    }();
    return enabled;
}

void Runtime::NoteDeferredCopyBreak() {
    ++merge_stats.breaks;
}

bool Runtime::DeferredCopyOverlaps(const VideoCore::Buffer* buffer, u64 offset, u64 size) const {
    if (deferred_copies.empty() || size == 0) {
        return false;
    }
    const u64 last = offset + size - 1;
    for (const auto& copy : deferred_copies) {
        if ((copy.src == buffer && copy.src_first <= last && offset <= copy.src_last) ||
            (copy.dst == buffer && copy.dst_first <= last && offset <= copy.dst_last)) {
            return true;
        }
    }
    return false;
}

void Runtime::FlushDeferredCopies() {
    if (deferred_copies.empty()) {
        return;
    }
    // Away first: the accesses recorded below must not be checked against their own copy.
    std::vector<DeferredCopy> batch;
    batch.swap(deferred_copies);
    for (const auto& copy : batch) {
        const std::span<const vk::BufferCopy> regions{copy.regions.data(), copy.regions.size()};
        scheduler.Record([src = copy.src_handle, dst = copy.dst_handle,
                          regions = scheduler.RecordData(regions)](vk::CommandBuffer cmdbuf) {
            cmdbuf.copyBuffer(src, dst, u32(regions.size()), regions.data());
        });
        AccessBuffer(copy.src, copy.src_first, copy.src_last - copy.src_first + 1, Stage::eCopy,
                     AccessBit::eTransferRead);
        AccessBuffer(copy.dst, copy.dst_first, copy.dst_last - copy.dst_first + 1, Stage::eCopy,
                     AccessBit::eTransferWrite);
    }
    ++merge_stats.batches;
    merge_stats.copies += batch.size();
    merge_stats.largest = std::max<u64>(merge_stats.largest, batch.size());
    ReportCopyMerge();
}

void Runtime::CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                         std::span<const vk::BufferCopy> copies) {
    if (!copy_merge) {
        CopyBufferRecord(src, dst, copies);
        return;
    }
    // BB_COPY_MERGE=1. A copy cannot be recorded inside a render pass (Vulkan forbids it), so the
    // port ends the pass, records it and starts the pass again: two render encoders per copy, 81%
    // of this frame's pass breaks (docs/ENCODER_ATTRIBUTION.md, cutlist item 1). With the gate,
    // the copy waits and is recorded when the pass ends anyway - no extra render encoder.
    //
    // A copy recorded then runs after *every* draw of the pass, so it may only wait when nothing
    // recorded after it touches its bytes. With `accesses` (what the port's commands read and
    // write since the last barrier) and a draw's own binding ranges, the rule is:
    //   - any access, read or write, to the destination: a later reader would see the bytes from
    //     before the copy, a later writer would be overwritten by it;
    //   - any write to the source: the copy would read bytes written after it in stream order;
    //   - any read of the source: that reader would miss the bytes the copy writes;
    //   - a paged access (BB_LAYER_MEMORY, `AccessGlobal`): a shader reaching memory through the
    //     page table may touch any buffer of the game's memory, so the range is unknown.
    // Accesses of commands already recorded are found by the same `IsBufferAccessed` the barrier
    // check uses (a hit means the copy does not wait at all, below); accesses of draws recorded
    // *after* it are caught before those draws are recorded (Rasterizer::BeginDraw ->
    // DeferredCopyOverlaps ends the pass there, which is exactly the break the copy would have
    // caused). Anything else that touches a waiting range is counted in `deferred_late_hits`.
    const bool bounded = copies.size() > 4;
    u64 src_min = ~0ULL, src_max = 0, dst_min = ~0ULL, dst_max = 0;
    for (const auto& copy : copies) {
        src_min = std::min<u64>(src_min, copy.srcOffset);
        src_max = std::max<u64>(src_max, copy.srcOffset + copy.size);
        dst_min = std::min<u64>(dst_min, copy.dstOffset);
        dst_max = std::max<u64>(dst_max, copy.dstOffset + copy.size);
    }
    // Overlapping a copy already waiting keeps their stream order: that one goes out first.
    const bool after_waiting = !copies.empty() &&
                               (DeferredCopyOverlaps(src, src_min, src_max - src_min) ||
                                DeferredCopyOverlaps(dst, dst_min, dst_max - dst_min));
    if (!copies.empty() && scheduler.IsRendering() && !after_waiting) {
        // The barrier this copy needs anyway (whatever was written before it is read here).
        bool needs_flush = false;
        if (bounded) {
            needs_flush = IsBufferAccessed(src, src_min, src_max - src_min, false,
                                           TransferConsumer()) ||
                          IsBufferAccessed(dst, dst_min, dst_max - dst_min, true,
                                           TransferConsumer());
        } else {
            for (const auto& copy : copies) {
                needs_flush = needs_flush ||
                              IsBufferAccessed(src, copy.srcOffset, copy.size, false,
                                               TransferConsumer()) ||
                              IsBufferAccessed(dst, copy.dstOffset, copy.size, true,
                                               TransferConsumer());
            }
        }
        if (!needs_flush) {
            // Consecutive copies of one buffer pair share the batch: one vkCmdCopyBuffer. The
            // hazard check above ruled out overlapping regions (Vulkan forbids them in one call).
            if (!deferred_copies.empty() && deferred_copies.back().src == src &&
                deferred_copies.back().dst == dst) {
                auto& group = deferred_copies.back();
                group.regions.insert(group.regions.end(), copies.begin(), copies.end());
                group.src_first = std::min(group.src_first, src_min);
                group.src_last = std::max(group.src_last, src_max - 1);
                group.dst_first = std::min(group.dst_first, dst_min);
                group.dst_last = std::max(group.dst_last, dst_max - 1);
            } else {
                deferred_copies.push_back({.src = src,
                                           .dst = dst,
                                           .src_handle = src->Handle(),
                                           .dst_handle = dst->Handle(),
                                           .regions = {copies.begin(), copies.end()},
                                           .src_first = src_min,
                                           .src_last = src_max - 1,
                                           .dst_first = dst_min,
                                           .dst_last = dst_max - 1});
            }
            return;
        }
        // A barrier is needed: it cannot be recorded inside the pass either, so end it here and
        // record this copy the old way, after the copies that were waiting (guest order).
        scheduler.EndRendering();
        FlushBarriersFor();
        CopyBufferTrack(src, dst, copies, bounded, src_min, src_max, dst_min, dst_max);
        return;
    }
    // The old way: ending the pass records whatever waited, before this copy (order kept).
    scheduler.EndRendering();
    bool needs_flush = bounded ? (IsBufferAccessed(src, src_min, src_max - src_min, false,
                                                   TransferConsumer()) ||
                                  IsBufferAccessed(dst, dst_min, dst_max - dst_min, true,
                                                   TransferConsumer()))
                               : false;
    if (!bounded) {
        for (const auto& copy : copies) {
            needs_flush = needs_flush ||
                          IsBufferAccessed(src, copy.srcOffset, copy.size, false,
                                           TransferConsumer()) ||
                          IsBufferAccessed(dst, copy.dstOffset, copy.size, true,
                                           TransferConsumer());
        }
    }
    if (needs_flush) {
        FlushBarriersFor();
    }
    CopyBufferTrack(src, dst, copies, bounded, src_min, src_max, dst_min, dst_max);
}

void Runtime::CopyBufferTrack(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                              std::span<const vk::BufferCopy> copies, bool bounded, u64 src_min,
                              u64 src_max, u64 dst_min, u64 dst_max) {
    scheduler.Record([src = src->Handle(), dst = dst->Handle(),
                      regions = scheduler.RecordData(copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyBuffer(src, dst, u32(regions.size()), regions.data());
    });
    if (bounded) {
        AccessBuffer(src, src_min, src_max - src_min, Stage::eCopy, AccessBit::eTransferRead);
        AccessBuffer(dst, dst_min, dst_max - dst_min, Stage::eCopy, AccessBit::eTransferWrite);
        return;
    }
    for (const auto& copy : copies) {
        AccessBuffer(src, copy.srcOffset, copy.size, Stage::eCopy, AccessBit::eTransferRead);
        AccessBuffer(dst, copy.dstOffset, copy.size, Stage::eCopy, AccessBit::eTransferWrite);
    }
}

/// The copy path without BB_COPY_MERGE (and the fallback of every path that cannot wait for the
/// pass's end): end the pass, wait for the barrier the tracker asks for, record. Unchanged.
void Runtime::CopyBufferRecord(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                               std::span<const vk::BufferCopy> copies) {
    scheduler.EndRendering();
    // Many regions (HLE copy shaders) are tracked as one bounding range per buffer:
    // conservative for barriers, and two lookups instead of two per region.
    const bool bounded = copies.size() > 4;
    u64 src_min = ~0ULL, src_max = 0, dst_min = ~0ULL, dst_max = 0;
    bool needs_flush = false;
    for (const auto& copy : copies) {
        if (bounded) {
            src_min = std::min<u64>(src_min, copy.srcOffset);
            src_max = std::max<u64>(src_max, copy.srcOffset + copy.size);
            dst_min = std::min<u64>(dst_min, copy.dstOffset);
            dst_max = std::max<u64>(dst_max, copy.dstOffset + copy.size);
        } else {
            needs_flush = needs_flush || IsBufferAccessed(src, copy.srcOffset, copy.size, false, TransferConsumer()) ||
                          IsBufferAccessed(dst, copy.dstOffset, copy.size, true, TransferConsumer());
        }
    }
    if (bounded) {
        needs_flush = IsBufferAccessed(src, src_min, src_max - src_min, false, TransferConsumer()) ||
                      IsBufferAccessed(dst, dst_min, dst_max - dst_min, true, TransferConsumer());
    }
    if (needs_flush) {
        FlushBarriersFor();
    }
    scheduler.Record([src = src->Handle(), dst = dst->Handle(),
                      regions = scheduler.RecordData(copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyBuffer(src, dst, u32(regions.size()), regions.data());
    });
    if (bounded) {
        AccessBuffer(src, src_min, src_max - src_min, Stage::eCopy, AccessBit::eTransferRead);
        AccessBuffer(dst, dst_min, dst_max - dst_min, Stage::eCopy, AccessBit::eTransferWrite);
        return;
    }
    for (const auto& copy : copies) {
        AccessBuffer(src, copy.srcOffset, copy.size, Stage::eCopy, AccessBit::eTransferRead);
        AccessBuffer(dst, copy.dstOffset, copy.size, Stage::eCopy, AccessBit::eTransferWrite);
    }
}

void Runtime::FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value) {
    scheduler.EndRendering();
    if (IsBufferAccessed(dst, offset, size, true, TransferConsumer())) {
        FlushBarriersFor();
    }
    scheduler.Record([dst = dst->Handle(), offset, size, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.fillBuffer(dst, offset, size, value);
    });
    AccessBuffer(dst, offset, size, Stage::eClear, AccessBit::eTransferWrite);
}

void Runtime::CopyFromGuestChunk(vk::Buffer src, const VideoCore::Buffer* dst,
                                 std::span<const vk::BufferCopy> copies) {
    if (copies.empty()) {
        return;
    }
    scheduler.EndRendering();
    const bool needs_flush = std::ranges::any_of(copies, [&](const vk::BufferCopy& copy) {
        return IsBufferAccessed(dst, copy.dstOffset, copy.size, true, TransferConsumer());
    });
    if (needs_flush) {
        FlushBarriersFor();
    }
    scheduler.Record([src, dst = dst->Handle(),
                      regions = scheduler.RecordData(copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyBuffer(src, dst, u32(regions.size()), regions.data());
    });
    for (const auto& copy : copies) {
        AccessBuffer(dst, copy.dstOffset, copy.size, Stage::eCopy, AccessBit::eTransferWrite);
    }
}

void Runtime::UpdateBuffer(const VideoCore::Buffer* dst, u64 offset, std::span<const u8> data) {
    if (data.empty()) {
        return;
    }
    scheduler.EndRendering();
    if (IsBufferAccessed(dst, offset, data.size(), true, TransferConsumer())) {
        FlushBarriersFor();
    }
    constexpr size_t Piece = 32 * 1024; // vkCmdUpdateBuffer takes 64 KiB at most
    for (size_t at = 0; at < data.size(); at += Piece) {
        const auto piece = data.subspan(at, std::min(Piece, data.size() - at));
        scheduler.Record([dst = dst->Handle(), to = offset + at,
                          bytes = scheduler.RecordData(piece)](vk::CommandBuffer cmdbuf) {
            cmdbuf.updateBuffer(dst, to, bytes.size_bytes(), bytes.data());
        });
    }
    AccessBuffer(dst, offset, data.size(), Stage::eClear, AccessBit::eTransferWrite);
}

void Runtime::UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                          std::span<const vk::BufferImageCopy> copies) {
    SetBackingSamples(dst, dst->info.num_samples, false);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image upload", *dst};
    bool needs_flush =
        Transit(dst, Layout::eTransferDstOptimal, Stage::eCopy, AccessBit::eTransferWrite);
    for (const auto& copy : copies) {
        needs_flush = needs_flush ||
                      IsBufferAccessed(src, copy.bufferOffset,
                                       BufferImageCopySize(copy, dst->info.pixel_format), false,
                                       TransferConsumer());
    }
    if (needs_flush) {
        FlushBarriersFor();
    }
    scheduler.Record([src = src->Handle(), image = dst->GetImage(),
                      regions = scheduler.RecordData(copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyBufferToImage(src, image, Layout::eTransferDstOptimal, u32(regions.size()),
                                 regions.data());
    });
    for (const auto& copy : copies) {
        AccessBuffer(src, copy.bufferOffset, BufferImageCopySize(copy, dst->info.pixel_format),
                     Stage::eCopy, AccessBit::eTransferRead);
    }
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                            std::span<const vk::BufferImageCopy> copies) {
    SetBackingSamples(src, src->info.num_samples);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image download", *src};
    bool needs_flush =
        Transit(src, Layout::eTransferSrcOptimal, Stage::eCopy, AccessBit::eTransferRead);
    for (const auto& copy : copies) {
        needs_flush = needs_flush ||
                      IsBufferAccessed(dst, copy.bufferOffset,
                                       BufferImageCopySize(copy, src->info.pixel_format), true,
                                       TransferConsumer());
    }
    if (needs_flush) {
        FlushBarriersFor();
    }
    scheduler.Record([image = src->GetImage(), dst = dst->Handle(),
                      regions = scheduler.RecordData(copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(image, Layout::eTransferSrcOptimal, dst, u32(regions.size()),
                                 regions.data());
    });
    for (const auto& copy : copies) {
        AccessBuffer(dst, copy.bufferOffset, BufferImageCopySize(copy, src->info.pixel_format),
                     Stage::eCopy, AccessBit::eTransferWrite);
    }
}

void Runtime::PrepareTransfer(VideoCore::Image* src, VideoCore::Image* dst,
                              vk::PipelineStageFlags2 stage, bool extra_flush) {
    scheduler.EndRendering();
    bool needs_flush = Transit(src, Layout::eTransferSrcOptimal, stage, AccessBit::eTransferRead);
    needs_flush |= Transit(dst, Layout::eTransferDstOptimal, stage, AccessBit::eTransferWrite);
    if (needs_flush || extra_flush) {
        FlushBarriersFor();
    }
}

void Runtime::CopyImageRegions(VideoCore::Image* src, VideoCore::Image* dst,
                               std::span<const vk::ImageCopy> regions) {
    scheduler.Record([src = src->GetImage(), dst = dst->GetImage(),
                      regions = scheduler.RecordData(regions)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src, Layout::eTransferSrcOptimal, dst, Layout::eTransferDstOptimal,
                         u32(regions.size()), regions.data());
    });
}

void Runtime::CopyImage(VideoCore::Image* src, VideoCore::Image* dst) {
    if (src->info.pixel_format != dst->info.pixel_format) {
        LOG_DEBUG(Render_Vulkan, "image copy between formats {} and {}",
                  vk::to_string(src->info.pixel_format), vk::to_string(dst->info.pixel_format));
    }
    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);

    const auto src_type = DimensionOf(src->info.type);
    const auto dst_type = DimensionOf(dst->info.type);
    const vk::ImageAspectFlags src_aspect = src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    const vk::ImageAspectFlags dst_aspect = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    const u32 depth =
        dst->info.type == Gcn::ImageType::Color3D ? dst->info.size.depth : src->info.size.depth;
    const u32 levels = std::min(src->info.resources.levels, dst->info.resources.levels);
    boost::container::small_vector<vk::ImageCopy, 16> regions;
    for (u32 mip = 0; mip < levels; ++mip) {
        const u32 width = std::max(src->info.size.width >> mip, 1u);
        const u32 height = std::max(src->info.size.height >> mip, 1u);
        const u32 mip_depth = std::max(depth >> mip, 1u);
        auto [src_layers, dst_layers] = CopyLayers(src->info, dst->info, mip_depth);
        u32 extent_depth = 1;
        if (src_type == vk::ImageType::e2D && dst_type == vk::ImageType::e3D) {
            extent_depth = src_layers; // 2D layers into depth slices
        } else if (src_type == vk::ImageType::e3D && dst_type == vk::ImageType::e2D) {
            extent_depth = dst_layers;
        } else if (src_type == vk::ImageType::e3D) {
            src_layers = dst_layers = 1;
            extent_depth = mip_depth;
        } else {
            src_layers = dst_layers = std::min(src_layers, dst_layers);
        }
        regions.push_back({
            .srcSubresource = {src_aspect, mip, 0, src_layers},
            .dstSubresource = {dst_aspect, mip, 0, dst_layers},
            .extent = {width, height, extent_depth},
        });
    }
    PrepareTransfer(src, dst, Stage::eCopy);
    CopyImageRegions(src, dst, regions);
    MarkCopied(dst, src->flags & VideoCore::ImageFlagBits::GpuModified);
}

void Runtime::CopyImageThroughBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                                     const VideoCore::Buffer* buffer, u64 offset) {
    ASSERT(src->info.resources.layers == dst->info.resources.layers &&
           std::min(src->info.resources.levels, dst->info.resources.levels) == 1);
    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);
    vk::BufferImageCopy download{
        .bufferOffset = offset,
        .imageSubresource = {src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil, 0, 0,
                             src->info.resources.layers},
        .imageExtent = {src->info.size.width, src->info.size.height, src->info.size.depth},
    };
    const u32 size = BufferImageCopySize(download, src->info.pixel_format);
    PrepareTransfer(src, dst, Stage::eCopy, IsBufferAccessed(buffer, offset, size, true, TransferConsumer()));
    vk::BufferImageCopy upload = download;
    upload.imageSubresource.aspectMask = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    scheduler.Record([src = src->GetImage(), dst = dst->GetImage(), buffer = buffer->Handle(),
                      download, upload](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(src, Layout::eTransferSrcOptimal, buffer, download);
        const vk::MemoryBarrier2 written{
            .srcStageMask = Stage::eCopy,
            .srcAccessMask = AccessBit::eTransferWrite,
            .dstStageMask = Stage::eCopy,
            .dstAccessMask = AccessBit::eTransferRead,
        };
        cmdbuf.pipelineBarrier2({
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &written,
        });
        cmdbuf.copyBufferToImage(buffer, dst, Layout::eTransferDstOptimal, upload);
    });
    AccessBuffer(buffer, offset, size, Stage::eCopy,
                 AccessBit::eTransferWrite | AccessBit::eTransferRead);
    MarkCopied(dst, src->flags & VideoCore::ImageFlagBits::GpuModified);
}

void Runtime::CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice) {
    const u32 dst_shift = dst->info.props.is_block ? 2 : 0;
    const u32 src_shift = src->info.props.is_block ? 2 : 0;
    ASSERT(std::max(dst->info.size.width >> (mip + dst_shift), 1u) ==
           (src->info.size.width >> src_shift));
    ASSERT(std::max(dst->info.size.height >> (mip + dst_shift), 1u) ==
           (src->info.size.height >> src_shift));
    ASSERT(std::max(dst->info.mips_layout[mip].pitch >> dst_shift, 1u) ==
           (src->info.pitch >> src_shift));
    const auto [src_layers, dst_layers] = CopyLayers(src->info, dst->info, src->info.size.depth);
    const vk::ImageCopy region{
        .srcSubresource = {src->aspect_mask, 0, 0, src_layers},
        .dstSubresource = {src->aspect_mask, mip, slice, dst_layers},
        .extent = {src->info.size.width, src->info.size.height, src->info.size.depth},
    };
    SetBackingSamples(dst, dst->info.num_samples);
    SetBackingSamples(src, src->info.num_samples);
    PrepareTransfer(src, dst, Stage::eCopy);
    CopyImageRegions(src, dst, {&region, 1});
    MarkCopied(dst, src->flags & VideoCore::ImageFlagBits::GpuModified);
}

void Runtime::CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst) {
    if (src->info.num_samples == 1 && dst->info.num_samples == 1) {
        if (instance.IsMaintenance8Supported() ||
            src->info.props.is_depth == dst->info.props.is_depth) {
            CopyImage(src, dst);
            return;
        }
        // Depth and color images cannot be copied into each other without maintenance8.
        constexpr size_t CopyBufferSize = 128_MB;
        const auto staging = staging_pool.Request(CopyBufferSize, VideoCore::MemoryType::DeviceLocal);
        CopyImageThroughBuffer(src, dst, staging.buffer, staging.offset);
        return;
    }
    if (src->info.num_samples == 1 && dst->info.props.is_depth) {
        // A draw puts the source's channels into the samples of the destination.
        bool needs_flush = Transit(src, Layout::eShaderReadOnlyOptimal, Stage::eFragmentShader,
                                   AccessBit::eShaderRead);
        needs_flush |= Transit(dst, Layout::eDepthStencilAttachmentOptimal,
                               Stage::eEarlyFragmentTests | Stage::eLateFragmentTests,
                               AccessBit::eDepthStencilAttachmentWrite);
        if (needs_flush) {
            FlushBarriersFor();
        }
        blitter->ColorAsMsDepth(dst->info.size.width, dst->info.size.height,
                                dst->info.num_samples, src->info.pixel_format,
                                dst->info.pixel_format, src->GetImage(), dst->GetImage());
        return;
    }
    LOG_WARNING(Render_Vulkan, "copy between multisampled color and depth images: not done");
}

void Runtime::CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                               const VideoCore::SubresourceRange& range) {
    PrepareTransfer(src, dst, Stage::eCopy);
    const vk::ImageSubresourceLayers layers{src->aspect_mask & dst->aspect_mask, 0,
                                            range.base.layer, range.extent.layers};
    const vk::ImageCopy region{
        .srcSubresource = layers,
        .dstSubresource = layers,
        .extent = {dst->info.size.width, dst->info.size.height, 1},
    };
    CopyImageRegions(src, dst, {&region, 1});
    MarkCopied(dst, VideoCore::ImageFlagBits::GpuModified);
}

void Runtime::ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                           const VideoCore::SubresourceRange& src_range,
                           const VideoCore::SubresourceRange& dst_range) {
    SetBackingSamples(dst, 1, false);
    const bool resolve = src->backing->num_samples != 1;
    PrepareTransfer(src, dst, resolve ? Stage::eResolve : Stage::eCopy);
    const auto [src_layers, dst_layers] = CopyLayers(src->info, dst->info, 1);
    const vk::ImageSubresourceLayers src_layers_info{vk::ImageAspectFlagBits::eColor, 0,
                                                     src_range.base.layer, src_layers};
    const vk::ImageSubresourceLayers dst_layers_info{vk::ImageAspectFlagBits::eColor, 0,
                                                     dst_range.base.layer, dst_layers};
    const vk::Extent3D extent{src->info.size.width, src->info.size.height, 1};
    if (resolve) {
        scheduler.Record([src = src->GetImage(), dst = dst->GetImage(),
                          region = vk::ImageResolve{.srcSubresource = src_layers_info,
                                                    .dstSubresource = dst_layers_info,
                                                    .extent = extent}](vk::CommandBuffer cmdbuf) {
            cmdbuf.resolveImage(src, Layout::eTransferSrcOptimal, dst, Layout::eTransferDstOptimal,
                                region);
        });
    } else {
        const vk::ImageCopy region{
            .srcSubresource = src_layers_info,
            .dstSubresource = dst_layers_info,
            .extent = extent,
        };
        CopyImageRegions(src, dst, {&region, 1});
    }
    MarkCopied(dst, VideoCore::ImageFlagBits::GpuModified);
}

void Runtime::ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                         const vk::ClearValue& value) {
    scheduler.EndRendering();
    if (scene_targets) {
        scene_targets->BeginClear(*dst, range);
    }
    if (Transit(dst, Layout::eTransferDstOptimal, Stage::eClear, AccessBit::eTransferWrite,
                range)) {
        FlushBarriersFor();
    }
    const vk::ImageSubresourceRange vk_range{vk::ImageAspectFlagBits::eColor, range.base.level,
                                             range.extent.levels, range.base.layer,
                                             range.extent.layers};
    scheduler.Record([image = dst->GetImage(), color = value.color,
                      vk_range](vk::CommandBuffer cmdbuf) {
        cmdbuf.clearColorImage(image, Layout::eTransferDstOptimal, color, vk_range);
    });
    if (scene_targets) {
        scene_targets->EndClear(*dst, range, value.color);
    }
    MarkCopied(dst, VideoCore::ImageFlagBits::GpuModified);
}

void Runtime::SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing) {
    BeforeImageAccess();
    auto& backing = image->backing;
    if (!backing || backing->num_samples == num_samples) {
        return;
    }
    const auto& info = image->info;
    ASSERT_MSG(!info.props.is_depth, "only color images change sample counts");
    VideoCore::Image::BackingImage* target;
    const auto it = std::ranges::find(image->backing_images, num_samples,
                                      &VideoCore::Image::BackingImage::num_samples);
    if (it != image->backing_images.end()) {
        target = std::addressof(*it);
    } else {
        auto create_info = backing->image.image_ci;
        create_info.samples = GcnToVk::NumSamples(num_samples, image->supported_samples);
        target = &image->backing_images.emplace_back();
        target->num_samples = num_samples;
        target->image = VideoCore::HostImage{instance.GetDevice(), instance.GetAllocator()};
        target->image.Create(create_info);
        SetObjectName(instance.GetDevice(), target->image.image,
                      "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                      info.size.width, info.size.height, info.size.depth,
                      Gcn::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
                      info.guest_address, info.guest_size, info.resources.layers,
                      info.resources.levels, num_samples);
    }

    if (copy_backing) {
        scheduler.EndRendering();
        ASSERT(info.resources.levels == 1 && info.resources.layers == 1);
        Transit(image, Layout::eShaderReadOnlyOptimal, Stage::eFragmentShader,
                AccessBit::eShaderRead);
        // The new backing's old contents do not matter.
        constexpr auto Stages = Stage::eColorAttachmentOutput;
        constexpr auto Accesses = AccessBit::eColorAttachmentRead | AccessBit::eColorAttachmentWrite;
        constexpr auto TargetLayout = Layout::eColorAttachmentOptimal;
        image_barriers.push_back({
            .srcStageMask = Stage::eAllCommands,
            .srcAccessMask = AccessBit::eMemoryRead | AccessBit::eMemoryWrite,
            .dstStageMask = Stages,
            .dstAccessMask = Accesses,
            .oldLayout = Layout::eUndefined,
            .newLayout = TargetLayout,
            .image = target->image,
            .subresourceRange = {image->aspect_mask, 0, 1, 0, info.resources.layers},
        });
        FlushBarriers();
        blitter->CopyMsImage(info.size.width, info.size.height, target->num_samples,
                             info.pixel_format, backing->num_samples > 1, backing->image,
                             target->image);
        target->state.layout = TargetLayout;
        target->state.access_mask = Accesses;
        target->state.pl_stage = Stages;
    }
    backing = target;
}

} // namespace Vulkan
