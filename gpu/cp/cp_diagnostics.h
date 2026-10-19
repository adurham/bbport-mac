// SPDX-License-Identifier: GPL-2.0-or-later
// bbport cp: diagnostics of the command stream. BB_DCB_STATS=1: the structure of the graphics
// command buffers every 5 s (input for decoding them on several threads); bad packets reported
// with their surroundings; BB_FREE_CHECK: submissions the guest changed before they were decoded.
#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <xxhash.h>

#include "common/types.h"

namespace BbCp::Diagnostics {

/// A type-3 opcode's name (its number in hex if unknown).
std::string OpName(u32 op);

bool DcbStatsEnabled();
/// A graphics buffer about to be decoded (`depth` > 0: an indirect buffer).
void ScanDcb(std::span<const u32> dcb, int depth);
/// WAIT_REG_MEM outcomes: 0 display buffer label, 1 pending fence, 2 met at once, 3 waited.
void NoteWait(int outcome, double ms = 0);

struct SubmittedCopy;
/// A packet that cannot be decoded: where, in which buffer, what surrounds it, the packets the
/// walk decoded before it in that buffer, and - for the submission's own buffer decoded from its
/// copy (BB_COPY_GPU_BUFFERS) - where the game's memory now differs from the copy (a submission
/// torn while it was copied shows the two versions side by side).
void ReportBadPacket(uintptr_t base, std::size_t dwords, const u32* at, u64 seq, int depth,
                     const SubmittedCopy* copy = nullptr);

/// Where a label for the guest is (a fence, an end-of-pipe or release event's value).
enum class LabelStep : u8 {
    ComputeDecoded, ///< RELEASE_MEM decoded (compute)
    ComputeOnGpu,   ///< RELEASE_MEM recorded as a GPU write
    ComputeWritten, ///< RELEASE_MEM written by a CPU thread
    GraphicsOnGpu,  ///< EOP/EOS recorded as a GPU write
    GraphicsWritten ///< EOP/EOS written by a CPU thread
};
/// Kept in a ring of the last 256 (any thread; a few nanoseconds).
void NoteLabel(LabelStep step, u64 address, u64 value, u32 bytes);
/// The last `count` labels (oldest first), each with its age and what its memory holds now.
std::string RecentLabels(size_t count);
/// Guest memory read through the backing view (no fault, any protection); false when unmapped.
bool ReadGuest(const void* address, void* out, u32 bytes);

/// A submission's command buffers as copied when submitted (BB_COPY_GPU_BUFFERS).
struct SubmittedCopy {
    std::vector<u32> data; ///< dcb, then ccb
    const u32* guest_dcb{};
    std::size_t dcb_dwords{};
    const u32* guest_ccb{};
    std::size_t ccb_dwords{};
    std::chrono::steady_clock::time_point submitted;
    /// BB_PARSE_CHECK=1 (diagnostics): XXH3 of `data` when the copy was taken and again just
    /// before the walk reads it (0: not taken). Equal hashes mean the copy was not damaged after
    /// it was taken: a bad packet in it was torn at the copy, or already wrong in guest memory.
    u64 copy_hash{};
    u64 parse_hash{};
};
/// BB_PARSE_CHECK=1: hash the copy at the two points the bad-packet report compares. Off by
/// default: two XXH3 passes over every submission's copy cost more than the rest of the walk.
inline bool ParseCheckEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PARSE_CHECK");
        return env && (env[0] == '1' || env[0] == 'y' || env[0] == 't');
    }();
    return enabled;
}
inline void NoteCopyTimeHash(SubmittedCopy& copy) {
    if (ParseCheckEnabled() && !copy.data.empty()) {
        copy.copy_hash = XXH3_64bits(copy.data.data(), copy.data.size() * sizeof(u32));
    }
}
inline void NoteParseTimeHash(SubmittedCopy& copy) {
    if (ParseCheckEnabled() && !copy.data.empty()) {
        copy.parse_hash = XXH3_64bits(copy.data.data(), copy.data.size() * sizeof(u32));
    }
}
/// BB_COPY_GPU_BUFFERS: one copy per submission, taken from the guest's words at this moment and
/// alive until it is decoded. This is the copy the split's submit must not return before
/// (docs/PM4_TORN_SUBMISSIONS.md); tests/test_pm4_submit.cpp calls it through the same function.
inline std::shared_ptr<SubmittedCopy> TakeCopy(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto copy = std::make_shared<SubmittedCopy>();
    copy->data.resize(dcb.size() + ccb.size());
    if (!dcb.empty()) {
        std::memcpy(copy->data.data(), dcb.data(), dcb.size_bytes());
    }
    if (!ccb.empty()) {
        std::memcpy(copy->data.data() + dcb.size(), ccb.data(), ccb.size_bytes());
    }
    copy->guest_dcb = dcb.data();
    copy->dcb_dwords = dcb.size();
    copy->guest_ccb = ccb.data();
    copy->ccb_dwords = ccb.size();
    copy->submitted = std::chrono::steady_clock::now();
    NoteCopyTimeHash(*copy);
    return copy;
}
/// After it was decoded: whether the guest changed its command buffers since.
void CheckSubmittedCopy(const SubmittedCopy& copy, u64 seq);

} // namespace BbCp::Diagnostics
