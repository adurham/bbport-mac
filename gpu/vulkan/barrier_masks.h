// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_BARRIER_MASKS=legacy|safe|strict - the destination stages of the port's barriers
// (docs/BARRIER_MASKS.md).
//   legacy (default): the global memory barrier of Runtime::FlushBarriers waits in ALL_COMMANDS,
//          sampled textures transition for ALL_COMMANDS (the behaviour before this switch).
//   safe:  every barrier waits only in the stages that consume what it orders: a buffer
//          conflict names its consumer (vertex input, index input, indirect, the shader stages of
//          the pipeline, transfer), sampled textures name the shader stages of the pipeline that
//          binds them, and a read in a stage not yet ordered after the last writer gets its own
//          barrier from the stages that were. Unknown consumers (explicit flushes, submits, paged
//          'any memory' bindings) keep ALL_COMMANDS.
//   strict: safe, with the shader stage of the binding itself instead of the pipeline's stages.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vulkan/vk_common.h"

namespace Vulkan {

enum class BarrierMasks { Legacy, Safe, Strict };

inline BarrierMasks GetBarrierMasks() {
    static const BarrierMasks mode = [] {
        const char* env = std::getenv("BB_BARRIER_MASKS");
        const BarrierMasks m = !env                        ? BarrierMasks::Legacy
                               : !std::strcmp(env, "safe")   ? BarrierMasks::Safe
                               : !std::strcmp(env, "strict") ? BarrierMasks::Strict
                                                             : BarrierMasks::Legacy;
        std::printf("GPU: barrier stage masks: %s (BB_BARRIER_MASKS)\n",
                    m == BarrierMasks::Safe     ? "safe"
                    : m == BarrierMasks::Strict ? "strict"
                                                : "legacy (ALL_COMMANDS)");
        return m;
    }();
    return mode;
}

inline bool NarrowBarriers() {
    return GetBarrierMasks() != BarrierMasks::Legacy;
}

/// Destination stages that do not make KosmicKrisp (BB_KK_BARRIER=vk) hold the next render
/// pass's vertex stage: the fragment side, compute, and the pseudo stages.
constexpr vk::PipelineStageFlags2 FragmentSideStages =
    vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eEarlyFragmentTests |
    vk::PipelineStageFlagBits2::eLateFragmentTests |
    vk::PipelineStageFlagBits2::eColorAttachmentOutput |
    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eHost |
    vk::PipelineStageFlagBits2::eTopOfPipe | vk::PipelineStageFlagBits2::eBottomOfPipe;

/// Vertex and index buffers: fixed-function input (and the vertex shader, in case the fetch is
/// done in it).
inline vk::PipelineStageFlags2 VertexInputConsumer() {
    return NarrowBarriers() ? vk::PipelineStageFlagBits2::eVertexInput |
                                  vk::PipelineStageFlagBits2::eVertexShader
                            : vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eAllCommands);
}

/// Indirect arguments and counts: the indirect stage, and compute for the GPU check of them
/// (render/vk_indirect_guard.h).
inline vk::PipelineStageFlags2 IndirectConsumer() {
    return NarrowBarriers() ? vk::PipelineStageFlagBits2::eDrawIndirect |
                                  vk::PipelineStageFlagBits2::eComputeShader
                            : vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eAllCommands);
}

/// Copies, clears, resolves and blits.
inline vk::PipelineStageFlags2 TransferConsumer() {
    return NarrowBarriers() ? vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eAllTransfer)
                            : vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eAllCommands);
}

/// Compute shaders only (the port's own dispatches: detiling, copy shader HLE).
inline vk::PipelineStageFlags2 ComputeConsumer() {
    return NarrowBarriers() ? vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eComputeShader)
                            : vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eAllCommands);
}

inline bool IsVertexSide(vk::PipelineStageFlags2 stages) {
    return bool(stages & ~FragmentSideStages);
}

} // namespace Vulkan
