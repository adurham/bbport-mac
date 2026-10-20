// SPDX-License-Identifier: GPL-2.0-or-later
// bbport vulkan: command buffers for submissions, reused once the GPU is done with the
// submission that last used them (grown four at a time).
#pragma once

#include <array>
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

    /// A command buffer free for the current submission.
    vk::CommandBuffer Commit();

private:
    struct Entry {
        vk::CommandBuffer buffer;
        u64 tick; ///< the submission that used it last
    };
    void Grow();

    /// bbport (BB_KK_TRIM_POOL): free the buffers the GPU is done with and trim the pool, so
    /// KosmicKrisp destroys the per-command-buffer MTL4CommandAllocators behind them. `done` is
    /// the same GPU-completion tick this pool already trusts for reuse, so nothing in flight is
    /// touched. Off unless the gate is set; see the .cpp for why trim alone is not enough.
    void MaybeTrim(u64 done);

    const Instance& instance;
    Semaphore* timeline;
    vk::UniqueCommandPool pool;
    std::vector<Entry> entries;
    size_t next = 0; ///< where the search starts: buffers are reused in turn

    /// bbport (BB_KK_STATS): how many of this pool's buffers were still in flight (tick > the GPU's)
    /// at each Commit, over a 10 s window, for the allocator-ring projection.
    void NoteInFlight(u64 done);
    u32 stats_id = 0;
    u64 window_commits = 0, window_busy_sum = 0, window_grows = 0;
    u32 window_busy_max = 0;
    std::array<u32, 33> window_busy_hist{};
    std::chrono::steady_clock::time_point window_start{};
};

} // namespace Vulkan
