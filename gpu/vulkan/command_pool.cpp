// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan/command_pool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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

/// BB_CMDBUF_RELEASE_IDLE=N seconds: free a command buffer the pool has not handed out for N s
/// (unset or 0: off; read once). The experiment the reference model asked for: the driver only
/// releases a command buffer's command allocators when the buffer is destroyed, so the buffers a
/// pool grew during an area load stay resident for the session.
u32 ReleaseIdleSeconds() {
    static const u32 seconds = [] {
        const char* env = std::getenv("BB_CMDBUF_RELEASE_IDLE");
        return static_cast<u32>(env ? std::max(0, std::atoi(env)) : 0);
    }();
    return seconds;
}

/// BB_CMDBUF_TRIM=1: after the frees, trim each pool once (vkTrimCommandPool; 0: never). This
/// exists only to measure the trim apart from the free: the earlier timer experiment's fps cost
/// came from trimming, not from freeing.
bool TrimOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_CMDBUF_TRIM");
        return env && env[0] == '1';
    }();
    return on;
}

/// BB_CMDBUF_LIFO=1: Commit hands out the most recently used free buffer (a stack) instead of the
/// next one in turn (read once). The round-robin rotates every buffer, so no entry is ever idle for
/// the release gate to take (measured: 64 live, all reused within a second, nothing released); with
/// LIFO the buffers a load grew fall out of use, stay cold, and the idle release can free them.
bool LifoOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_CMDBUF_LIFO");
        return env && env[0] == '1';
    }();
    return on;
}

/// BB_CMDBUF_LIFO=1: the index of the free entry that was used most recently, or entries.size()
/// when the GPU is not done with any of them. An entry never handed out has tick 0, so it is free.
template <typename Entries>
size_t NewestFreeIndex(const Entries& entries, u64 done) {
    size_t best = entries.size(); // no free buffer yet
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].tick > done) {
            continue;
        }
        // On a tie the earliest entry wins: the comparison is strict, so the first index holding
        // the newest last_use is the one kept.
        if (best == entries.size() || entries[i].last_use > entries[best].last_use) {
            best = i;
        }
    }
    return best;
}

/// The pools alive right now. The per-frame hook owns none of the recording threads' pools, so the
/// stats line and the release sweep can only reach them through this list; with both gates off
/// nothing is tracked and the list stays empty.
bool TrackPools() {
    return StatsOn() || ReleaseIdleSeconds() != 0;
}
std::mutex registry_mutex;
std::vector<CommandPool*> registry;

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
    if (TrackPools()) {
        const std::lock_guard lock{registry_mutex};
        registry.push_back(this);
    }
}

CommandPool::~CommandPool() {
    if (TrackPools()) {
        const std::lock_guard lock{registry_mutex};
        const auto it = std::find(registry.begin(), registry.end(), this);
        if (it != registry.end()) {
            registry.erase(it);
        }
    }
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
        // tick 0: never handed out (the idle sweep only takes tick != 0, so a new buffer is never
        // released); last_use starts at the allocation, the oldest a buffer's idleness can be.
        entries.push_back({buffer, 0, std::chrono::steady_clock::now()});
    }
    if (StatsOn()) {
        CountPeak(entries.size());
    }
}

vk::CommandBuffer CommandPool::Commit() {
    // The two experiments' per-pool hooks, both on this pool's own thread: a command pool must
    // not be used from two threads at once, and recording into its buffers counts as using it, so
    // the hook's thread only ever asks for these (RequestIdleRelease / the stats line) and the
    // work is done here.
    if (release_requested.load(std::memory_order_relaxed)) {
        ReleaseIdle();
    }
    if (StatsOn()) {
        PublishIdleBuckets();
    }
    const auto take = [&](size_t index) {
        entries[index].tick = timeline->CurrentTick();
        entries[index].last_use = std::chrono::steady_clock::now();
        next = (index + 1) % entries.size();
        return entries[index].buffer;
    };
    // Every Commit counts once here, and only a Commit that grew counts a miss: reused is
    // commits - misses by construction, not by a second counter.
    const bool stats = StatsOn();
    if (stats) {
        counts.commits.fetch_add(1, std::memory_order_relaxed);
    }
    // BB_CMDBUF_LIFO=1: take the most recently used buffer the GPU is done with (a stack) instead
    // of the round-robin's next in turn, so the buffers a load grew stop being rotated through,
    // stay cold, and BB_CMDBUF_RELEASE_IDLE has something to free. Same predicate and same retry
    // as the searches below (ask the GPU once between the two passes); if no buffer is free it
    // falls through to them unchanged, so a miss still grows exactly as it does with the gate off.
    if (LifoOn()) {
        for (int attempt = 0; attempt < 2 && !entries.empty(); ++attempt) {
            if (attempt == 1) {
                timeline->Refresh();
            }
            const u64 done = timeline->KnownGpuTick();
            size_t best = entries.size(); // no free buffer yet
            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].tick > done) {
                    continue;
                }
                // On a tie the earliest entry wins: the comparison is strict, so the first index
                // holding the newest last_use is the one kept.
                if (best == entries.size() || entries[i].last_use > entries[best].last_use) {
                    best = i;
                }
            }
            if (best != entries.size()) {
                return take(best);
            }
        }
    }
    // From where the last one was taken to the end, again after asking the GPU, then from the
    // start; else a new one.
    for (int attempt = 0; attempt < 2 && !entries.empty(); ++attempt) {
        if (attempt == 1) {
            timeline->Refresh();
        }
        const u64 done = timeline->KnownGpuTick();
        // BB_CMDBUF_LIFO=1: the pre-pass's selection, asked of the retry after this Refresh instead
        // of the rotation: the most recently used buffer the Refresh shows the GPU is done with,
        // not the next one in turn. The rotation here is what kept the whole pool hot (measured:
        // 58 of 64 buffers idle <= 1 s) and left BB_CMDBUF_RELEASE_IDLE nothing to age. Only when
        // no buffer at all is free do the searches below run, so a reuse stays a reuse and a miss
        // still grows exactly as it does with the gate off.
        if (attempt == 1 && LifoOn()) {
            const size_t best = NewestFreeIndex(entries, done);
            if (best != entries.size()) {
                return take(best);
            }
        }
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

void CommandPool::ReleaseIdle() {
    release_requested.store(false, std::memory_order_relaxed);
    const u32 idle_seconds = ReleaseIdleSeconds();
    if (idle_seconds == 0) {
        return;
    }
    const std::chrono::seconds idle{idle_seconds};
    const auto now = std::chrono::steady_clock::now();
    // The free predicate, and nothing more: the submission that last used the buffer must be done
    // on the GPU (an unreached tick is work still in flight, so it is never freed), and the buffer
    // must have sat unused for the whole gate (one the GPU finished with a moment ago is exactly
    // the one the next frame reuses). Entries never handed out have tick 0 and stay too.
    const auto is_idle = [&](const Entry& entry) {
        return entry.tick != 0 && entry.tick <= timeline->KnownGpuTick() &&
               now - entry.last_use > idle;
    };
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true, std::memory_order_relaxed)) {
        std::printf("GPU: BB_CMDBUF_RELEASE_IDLE=%u: releasing command buffers idle for more than "
                    "%u s%s\n",
                    idle_seconds, idle_seconds,
                    TrimOn() ? ", then trimming each pool (BB_CMDBUF_TRIM=1)" : "");
    }
    std::vector<vk::CommandBuffer> freed;
    freed.reserve(entries.size());
    size_t kept = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (is_idle(entries[i])) {
            freed.push_back(entries[i].buffer);
            continue;
        }
        entries[kept++] = entries[i];
    }
    if (freed.empty()) {
        return;
    }
    const vk::Device device = instance.GetDevice();
    device.freeCommandBuffers(*pool, static_cast<u32>(freed.size()), freed.data());
    // Only the command buffers go: their entries go with them, so no entry is left holding a
    // handle that no longer exists, and a later Commit grows more as it already does.
    entries.resize(kept);
    // The cursor may have pointed at an entry that was just freed (or past the new end): send it
    // back to the front. It is only where the search starts, so that is all it needs.
    if (next >= entries.size()) {
        next = 0;
    }
    if (StatsOn()) {
        counts.live.fetch_sub(freed.size(), std::memory_order_relaxed);
    }
    if (TrimOn()) {
        device.trimCommandPool(*pool, {});
    }
    std::printf("GPU: command buffers release: %llu freed, %llu live, idle > %us\n",
                static_cast<unsigned long long>(freed.size()),
                static_cast<unsigned long long>(entries.size()), idle_seconds);
}

void CommandPool::PublishIdleBuckets() {
    const auto now = std::chrono::steady_clock::now();
    std::array<u64, 4> buckets{};
    for (const Entry& entry : entries) {
        const auto age = now - entry.last_use;
        const size_t bucket = age <= std::chrono::seconds(1)    ? 0
                              : age <= std::chrono::seconds(5)  ? 1
                              : age <= std::chrono::seconds(10) ? 2
                                                                : 3;
        ++buckets[bucket];
    }
    for (size_t i = 0; i < buckets.size(); ++i) {
        idle_buckets[i].store(buckets[i], std::memory_order_relaxed);
    }
}

std::array<u64, 4> CommandPool::IdleBuckets() const noexcept {
    std::array<u64, 4> buckets{};
    for (size_t i = 0; i < buckets.size(); ++i) {
        buckets[i] = idle_buckets[i].load(std::memory_order_relaxed);
    }
    return buckets;
}

void ReleaseIdleCommandBuffers() {
    if (ReleaseIdleSeconds() == 0) {
        return;
    }
    // At most once a second, whatever the frame rate: one sweep a second is enough to see the
    // footprint move, and a sweep per frame would put the driver's frees in every frame.
    static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1)) {
        return;
    }
    last = now;
    const std::lock_guard lock{registry_mutex};
    for (CommandPool* pool : registry) {
        pool->RequestIdleRelease();
    }
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
    // BB_CMDBUF_STATS: the age of each live buffer's last use, summed over the pools (each pool
    // publishes it from its own thread on its last Commit, so this reads no entries directly).
    std::array<u64, 4> idle{};
    {
        const std::lock_guard lock{registry_mutex};
        for (const CommandPool* pool : registry) {
            const std::array<u64, 4> buckets = pool->IdleBuckets();
            for (size_t i = 0; i < idle.size(); ++i) {
                idle[i] += buckets[i];
            }
        }
    }
    std::printf("GPU: command buffers (10 s): %llu commits, %llu reused, %llu grew (%llu new "
                "buffers, %llu ever); %llu pools, %llu live, largest pool %llu; idle<=1s:%llu "
                "<=5s:%llu <=10s:%llu older:%llu\n",
                static_cast<unsigned long long>(commits),
                static_cast<unsigned long long>(reused),
                static_cast<unsigned long long>(grew),
                static_cast<unsigned long long>(new_buffers),
                static_cast<unsigned long long>(ever),
                static_cast<unsigned long long>(pools),
                static_cast<unsigned long long>(ever),
                static_cast<unsigned long long>(peak),
                static_cast<unsigned long long>(idle[0]),
                static_cast<unsigned long long>(idle[1]),
                static_cast<unsigned long long>(idle[2]),
                static_cast<unsigned long long>(idle[3]));
}

} // namespace Vulkan
