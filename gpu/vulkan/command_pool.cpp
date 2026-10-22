// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan/command_pool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "common/assert.h"
#include "vulkan/instance.h"
#include "vulkan/timeline.h"

namespace Vulkan {

namespace {
constexpr u32 GrowBy = 4;

/// BB_CMDBUF_STATS=1: the counters below are kept and ReportCommandBufferPools prints them (read
/// once; nothing is counted or printed otherwise).
bool StatsOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_CMDBUF_STATS");
        return env && env[0] == '1';
    }();
    return on;
}

/// BB_MAX_CMDBUFFERS=N: the most command buffers a pool grows to; at the cap Commit waits for the
/// GPU instead of growing (unset or 0: no cap; read once).
u32 MaxBuffers() {
    static const u32 cap = [] {
        const char* env = std::getenv("BB_MAX_CMDBUFFERS");
        return static_cast<u32>(env ? std::max(0, std::atoi(env)) : 0);
    }();
    return cap;
}

/// The pool counters. commits, misses (a Commit that grew) and allocs (a Grow) are the window's,
/// zeroed by the report; live (buffers allocated and not yet freed with their pool), pools
/// (constructed minus destroyed) and peak_pool (the largest entries.size() seen) are running.
struct CommandBufferCounts {
    std::atomic<u64> commits{0};
    std::atomic<u64> misses{0};
    std::atomic<u64> allocs{0};
    std::atomic<u64> live{0};
    std::atomic<u64> pools{0};
    std::atomic<u64> peak_pool{0};
};
CommandBufferCounts counts;

void CountPeak(size_t size) {
    const u64 wanted = size;
    u64 peak = counts.peak_pool.load(std::memory_order_relaxed);
    while (peak < wanted &&
           !counts.peak_pool.compare_exchange_weak(peak, wanted, std::memory_order_relaxed)) {
    }
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
    if (StatsOn()) {
        counts.pools.fetch_add(1, std::memory_order_relaxed);
    }
}

CommandPool::~CommandPool() {
    // BB_CMDBUF_STATS: a pool's buffers die with it.
    if (StatsOn()) {
        counts.pools.fetch_sub(1, std::memory_order_relaxed);
        counts.live.fetch_sub(entries.size(), std::memory_order_relaxed);
    }
}

void CommandPool::Grow() {
    if (StatsOn()) {
        // One Grow is one miss's worth of new buffers, always GrowBy of them.
        counts.allocs.fetch_add(1, std::memory_order_relaxed);
        counts.live.fetch_add(GrowBy, std::memory_order_relaxed);
    }
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
    if (StatsOn()) {
        CountPeak(entries.size());
    }
}

vk::CommandBuffer CommandPool::Commit() {
    const auto take = [&](size_t index) {
        entries[index].tick = timeline->CurrentTick();
        next = (index + 1) % entries.size();
        return entries[index].buffer;
    };
    // Every Commit counts once here, and only a Commit that grew counts a miss: reused is
    // commits - misses by construction, not by a second counter.
    const bool stats = StatsOn();
    if (stats) {
        counts.commits.fetch_add(1, std::memory_order_relaxed);
    }
    // From where the last one was taken to the end, again after asking the GPU, then from the
    // start; else a new one.
    for (int attempt = 0; attempt < 2 && !entries.empty(); ++attempt) {
        if (attempt == 1) {
            timeline->Refresh();
        }
        const u64 done = timeline->KnownGpuTick();
        for (size_t i = next; i < entries.size(); ++i) {
            if (entries[i].tick <= done) {
                return take(i);
            }
        }
    }
    const u64 done = timeline->KnownGpuTick();
    for (size_t i = 0; i < next && i < entries.size(); ++i) {
        if (entries[i].tick <= done) {
            return take(i);
        }
    }
    // BB_MAX_CMDBUFFERS=N: at the cap, wait for the GPU to finish with a submission and hand out
    // the buffer it frees instead of allocating more. The wait is bounded and falls through to
    // Grow(), so a cap can never deadlock the game (the one-time line says it was too low).
    const u32 cap = MaxBuffers();
    if (cap != 0 && entries.size() >= cap) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            timeline->Refresh();
            const u64 freed = timeline->KnownGpuTick();
            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].tick <= freed) {
                    return take(i);
                }
            }
        }
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            std::printf("GPU: BB_MAX_CMDBUFFERS=%u too low, every buffer in flight for 5 ms: "
                        "growing past it\n",
                        cap);
        }
    }
    if (stats) {
        counts.misses.fetch_add(1, std::memory_order_relaxed);
    }
    const size_t first_new = entries.size();
    Grow();
    return take(first_new);
}

void ReportCommandBufferPools() {
    if (!StatsOn()) {
        return;
    }
    static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) {
        return;
    }
    last = now;
    const u64 commits = counts.commits.exchange(0, std::memory_order_relaxed);
    const u64 misses = counts.misses.exchange(0, std::memory_order_relaxed);
    const u64 grew = counts.allocs.exchange(0, std::memory_order_relaxed);
    const u64 ever = counts.live.load(std::memory_order_relaxed);
    const u64 pools = counts.pools.load(std::memory_order_relaxed);
    const u64 peak = counts.peak_pool.load(std::memory_order_relaxed);
    // The three relations below are the counters' construction, not arithmetic to be trusted:
    // a commit that grew is one that counted a miss, Grow allocates exactly GrowBy buffers, and
    // live is Grow's additions minus the destructors' removals (so it is also ever).
    const u64 reused = commits - misses;
    const u64 new_buffers = grew * GrowBy;
    std::printf("GPU: command buffers (10 s): %llu commits, %llu reused, %llu grew (%llu new "
                "buffers, %llu ever); %llu pools, %llu live, largest pool %llu\n",
                static_cast<unsigned long long>(commits),
                static_cast<unsigned long long>(reused),
                static_cast<unsigned long long>(grew),
                static_cast<unsigned long long>(new_buffers),
                static_cast<unsigned long long>(ever),
                static_cast<unsigned long long>(pools),
                static_cast<unsigned long long>(ever),
                static_cast<unsigned long long>(peak));
}

} // namespace Vulkan
