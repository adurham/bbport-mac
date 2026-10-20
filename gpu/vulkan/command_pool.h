// SPDX-License-Identifier: GPL-2.0-or-later
// bbport vulkan: command buffers for submissions, reused once the GPU is done with the
// submission that last used them (grown four at a time).
#pragma once

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
};

} // namespace Vulkan
