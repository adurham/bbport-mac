// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan/command_pool.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "common/assert.h"
#include "vulkan/instance.h"
#include "vulkan/timeline.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

namespace Vulkan {

namespace {
constexpr u32 GrowBy = 4;

/// bbport (BB_KK_TRIM_POOL): why this gate frees command buffers instead of just trimming.
///
/// KosmicKrisp gives every VkCommandBuffer three MTL4CommandAllocators, one per encoder state
/// (kk_cmd_buffer.c kk_init_encoder_state). In steady state the driver only ever calls
/// [MTL4CommandAllocator reset] on them (kk_reset_encoder_state), and reset does NOT return the
/// encoding memory: measured on this machine, 8 allocators that had encoded commands kept
/// allocatedSize at 789.57 MiB across reset and the process phys_footprint did not move
/// (301.6 MiB before and after); releasing the allocator objects dropped it to 21.1 MiB. So the
/// allocators hold their high-water mark until the buffer that owns them is destroyed, which
/// kk_destroy_encoder_state does on vkFreeCommandBuffers.
///
/// This pool also sets VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, so vk_common_FreeCommandBuffers
/// only *recycles* into the pool's free list (vk_command_pool.c vk_command_buffer_recycle_or_destroy)
/// and vkTrimCommandPool is what then destroys them (destroy_free_command_buffers). Both calls are
/// needed: vkTrimCommandPool on its own returns nothing here, because every buffer this pool owns is
/// still allocated. Freeing is only done for entries the GPU has finished with (tick <= done), the
/// same predicate Commit() uses for reuse, at the frame boundary where that tick is read.
#ifdef __APPLE__
u64 FootprintBytes() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
            KERN_SUCCESS ||
        count < TASK_VM_INFO_REV3_COUNT) {
        return 0;
    }
    return info.phys_footprint;
}
#else
u64 FootprintBytes() {
    return 0;
}
#endif

/// One clock shared by every pool, so all of them trim on the same 3 s sampler tick rather than
/// each on its own phase.
struct TrimGate {
    bool read = false;
    bool on = false;
    double secs = 0.0;
    std::chrono::steady_clock::time_point next{};
    u64 rounds = 0;
};
TrimGate gate;

bool GateOn() {
    if (gate.read) {
        return gate.on;
    }
    const char* value = std::getenv("BB_KK_TRIM_POOL");
    gate.on = value != nullptr && std::atof(value) > 0.0;
    if (gate.on) {
        gate.secs = std::atof(value);
        gate.next = std::chrono::steady_clock::now();
        std::printf("PROBE TRIM: BB_KK_TRIM_POOL on, every %g s\n", gate.secs);
        std::fflush(stdout);
    }
    gate.read = true;
    return gate.on;
}
} // namespace

CommandPool::CommandPool(const Instance& instance_, Semaphore* timeline_)
    : instance{instance_}, timeline{timeline_} {
    const vk::Device device = instance.GetDevice();
    auto [result, created] = device.createCommandPoolUnique({
        .flags = vk::CommandPoolCreateFlagBits::eTransient |
                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    });
    ASSERT_MSG(result == vk::Result::eSuccess, "command pool: {}", vk::to_string(result));
    pool = std::move(created);
    SetObjectName(device, *pool, "CommandPool");
}

CommandPool::~CommandPool() = default;

void CommandPool::Grow() {
    std::array<vk::CommandBuffer, GrowBy> buffers;
    const vk::CommandBufferAllocateInfo info{
        .commandPool = *pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = GrowBy,
    };
    const vk::Device device = instance.GetDevice();
    ASSERT(device.allocateCommandBuffers(&info, buffers.data()) == vk::Result::eSuccess);
    for (const vk::CommandBuffer buffer : buffers) {
        SetObjectName(device, buffer, "CommandPool: Command Buffer {}", entries.size());
        entries.push_back({buffer, 0});
    }
}

vk::CommandBuffer CommandPool::Commit() {
    const auto take = [&](size_t index) {
        entries[index].tick = timeline->CurrentTick();
        next = (index + 1) % entries.size();
        return entries[index].buffer;
    };
    // bbport (BB_KK_TRIM_POOL): the tick read here is this pool's GPU-caught-up point, so the trim
    // happens on a frame boundary and only over entries the reuse search would itself accept.
    // `done` is used for the trim only: the search below re-reads the tick exactly as it did
    // before this gate existed, so a run with the gate unset behaves identically.
    MaybeTrim(timeline->KnownGpuTick());
    // From where the last one was taken to the end, again after asking the GPU, then from the
    // start; else a new one.
    u64 done = timeline->KnownGpuTick();
    for (int attempt = 0; attempt < 2 && !entries.empty(); ++attempt) {
        if (attempt == 1) {
            timeline->Refresh();
            done = timeline->KnownGpuTick();
        }
        for (size_t i = next; i < entries.size(); ++i) {
            if (entries[i].tick <= done) {
                return take(i);
            }
        }
    }
    done = timeline->KnownGpuTick();
    for (size_t i = 0; i < next && i < entries.size(); ++i) {
        if (entries[i].tick <= done) {
            return take(i);
        }
    }
    const size_t first_new = entries.size();
    Grow();
    return take(first_new);
}

void CommandPool::MaybeTrim(u64 done) {
    if (!GateOn()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < gate.next) {
        return;
    }
    gate.next = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<double>(gate.secs));

    std::vector<vk::CommandBuffer> idle;
    std::vector<Entry> keep;
    const size_t before_entries = entries.size();
    idle.reserve(entries.size());
    keep.reserve(entries.size());
    for (const Entry& entry : entries) {
        // tick <= done: the submission that last used it has completed, exactly as Commit() decides
        // before handing the buffer out again. A buffer never submitted (tick 0) is idle too.
        if (entry.tick <= done) {
            idle.push_back(entry.buffer);
        } else {
            keep.push_back(entry);
        }
    }
    if (idle.empty()) {
        return;
    }

    const u64 before = FootprintBytes();
    const vk::Device device = instance.GetDevice();
    device.freeCommandBuffers(*pool, u32(idle.size()), idle.data());
    // Not redundant: the pool has RESET_COMMAND_BUFFER_BIT, so the free above only put the buffers
    // on the pool's free list (recycled). The trim is what destroys them, and destroying them is
    // what makes KosmicKrisp release the MTL4CommandAllocators they own.
    device.trimCommandPool(*pool, vk::CommandPoolTrimFlags{});
    entries = std::move(keep);
    next = 0;

    const u64 after = FootprintBytes();
    constexpr u64 MiB = 1u << 20;
    ++gate.rounds;
    std::printf("PROBE TRIM %llu: pool trimmed, buffers freed %u, entries %zu->%zu, "
                "footprint %llu->%llu MiB (delta %lld MiB)\n",
                (unsigned long long)gate.rounds, unsigned(idle.size()), before_entries,
                entries.size(), (unsigned long long)(before / MiB), (unsigned long long)(after / MiB),
                (long long)((std::int64_t(after) - std::int64_t(before)) / std::int64_t(MiB)));
    std::fflush(stdout);
}

} // namespace Vulkan
