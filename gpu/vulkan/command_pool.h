// SPDX-License-Identifier: GPL-2.0-or-later
// bbport vulkan: command buffers for submissions, reused once the GPU is done with the
// submission that last used them (grown four at a time).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <vector>

#include "common/types.h"
#include "vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Semaphore;

class CommandPool {
public:
    CommandPool(const Instance& instance, Semaphore* timeline);
    ~CommandPool();

    /// A command buffer free for the current submission. BB_CMDBUF_LIFO=1 makes it the most
    /// recently used of the free ones (a stack) instead of the next one in turn.
    vk::CommandBuffer Commit();

    /// BB_CMDBUF_RELEASE_IDLE: asks this pool to free its idle command buffers at its next Commit.
    /// The sweep runs on the pool's own thread because a command pool must not be used from two
    /// threads at once, and recording into buffers allocated from it is such a use
    /// (Vulkan: "Command pools are externally synchronized"); the per-frame hook's thread is not
    /// this pool's thread and may even be inside another pool's Commit. ReleaseIdleCommandBuffers
    /// calls this at most once a second.
    void RequestIdleRelease() noexcept {
        release_requested.store(true, std::memory_order_relaxed);
    }

    /// BB_CMDBUF_STATS: how many of the pool's command buffers were last handed out <=1 s, <=5 s,
    /// <=10 s and more than 10 s ago, as published by this pool's last Commit (atomics: the hook's
    /// thread reads them while the pool's own thread records).
    [[nodiscard]] std::array<u64, 4> IdleBuckets() const noexcept;

private:
    struct Entry {
        vk::CommandBuffer buffer;
        u64 tick; ///< the submission that used it last
        std::chrono::steady_clock::time_point last_use; ///< when Commit handed it out last
    };
    void Grow();
    /// BB_CMDBUF_RELEASE_IDLE: frees the entries the GPU is done with and that have been idle for
    /// longer than the gate, then erases them (a later Commit grows more, as it already does).
    /// Called from Commit, on this pool's thread.
    void ReleaseIdle();
    /// BB_CMDBUF_STATS: recounts IdleBuckets from `entries`. Called from Commit, on this pool's
    /// thread, which is the only writer of `entries`.
    void PublishIdleBuckets();

    const Instance& instance;
    Semaphore* timeline;
    vk::UniqueCommandPool pool;
    std::vector<Entry> entries;
    size_t next = 0; ///< where the search starts: buffers are reused in turn (BB_CMDBUF_LIFO=1
                     ///< picks by last_use instead; the cursor then only keeps the fallback in range)
    std::atomic<bool> release_requested{false}; ///< a release is pending (set by the frame hook)
    std::array<std::atomic<u64>, 4> idle_buckets{}; ///< published by Commit, read by the stats line
};

/// BB_CMDBUF_STATS=1: one line per 10 s window on the pools (commits, reuse, growth, live
/// buffers, and the age of each live buffer's last use). Prints nothing while the gate is off.
void ReportCommandBufferPools();

/// BB_CMDBUF_RELEASE_IDLE=N seconds: once a second, asks every live pool to free the command
/// buffers it has not handed out for more than N s and whose GPU work is done (unset or 0: does
/// nothing at all). The experiment: the driver only releases a command buffer's allocators when
/// the buffer is destroyed, so a pool that grew during a load holds them for the session.
void ReleaseIdleCommandBuffers();

} // namespace Vulkan
