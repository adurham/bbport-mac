// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the one place the split's graphics submission decides whether it waits for bb-gpu's copy,
// and the one place bb-gpu checks what it took. Shared by remote_front.cpp (the game process) and
// remote_back.cpp (bb-gpu) so tests/test_pm4_submit.cpp drives the port's own code, not a copy of
// its protocol (docs/PM4_PREREG_2026-10-19.md, docs/PM4_SYNTHETIC_2026-10-19.md).
//
// Why the wait exists: the game may write a command buffer again as soon as it believes the GPU has
// it. In-process, the command processor copies the words inside sceGnmSubmitCommandBuffers
// (BB_COPY_GPU_BUFFERS, run.sh's default). Sent without waiting, bb-gpu's reader thread copied them
// whenever it got to the message - behind write faults and mappings that wait for its GPU thread
// during an area load - so the copy could mix two versions of the buffer: packets cut short, a NOP
// whose body holds the next packets, decoded as "PM4 type 0/1 packet"
// (docs/PM4_TORN_SUBMISSIONS.md). Waiting gives the native split the in-process guarantee: the
// words are copied before the call returns.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <thread>

#include <xxhash.h>

#include "remote/bb_protocol.h"

namespace BbRemote::SubmitPath {

/// Whether a graphics submission waits until bb-gpu has taken it. BB_SUBMIT_ASYNC=1 (diagnostics):
/// the old way, which returns before bb-gpu has copied the words.
inline bool WaitsForCopy() {
    static const bool wait = [] {
        const char* copy = std::getenv("BB_COPY_GPU_BUFFERS");
        const char* async = std::getenv("BB_SUBMIT_ASYNC");
        const bool copied = copy && (copy[0] == '1' || copy[0] == 'y' || copy[0] == 't');
        const bool on = copied && !(async && async[0] == '1');
        std::printf("GPU process: graphics submissions %s\n",
                    on       ? "return once bb-gpu has copied them (BB_COPY_GPU_BUFFERS)"
                    : copied ? "return before bb-gpu has copied them (BB_SUBMIT_ASYNC=1)"
                             : "are decoded from guest memory (BB_COPY_GPU_BUFFERS=0)");
        return on;
    }();
    return wait;
}

/// BB_SUBMIT_CHECK=1 (diagnostics): the words as the game submitted them, for bb-gpu to compare
/// with what it takes ("Submit check:" lines).
inline bool SubmitCheckEnabled() {
    static const bool check = [] {
        const char* env = std::getenv("BB_SUBMIT_CHECK");
        return env && env[0] == '1';
    }();
    return check;
}

/// Fills the submission's hashes and the time it was submitted (only with BB_SUBMIT_CHECK=1).
inline void NoteSubmissionHashes(SubmitGfxArgs& args, std::span<const std::uint32_t> dcb,
                                 std::span<const std::uint32_t> ccb, std::uint64_t now_ns) {
    if (!SubmitCheckEnabled()) {
        return;
    }
    args.dcb_hash = dcb.empty() ? 0 : XXH3_64bits(dcb.data(), dcb.size_bytes());
    args.ccb_hash = ccb.empty() ? 0 : XXH3_64bits(ccb.data(), ccb.size_bytes());
    args.submit_ns = now_ns;
}

/// The game's side: sends the submission, and with WaitsForCopy() returns only once bb-gpu's
/// handler has answered it (it answers after the copy). `call` is the port's timed call.
template <typename Channel, typename Call>
inline void Send(Channel& channel, const void* message, std::uint32_t size, Call&& call) {
    if (WaitsForCopy()) {
        call();
        return;
    }
    channel.Send(MsgSubmitGfx, message, size);
}

/// BB_SUBMIT_DELAY_US=n (diagnostics): each submission taken n us late - the window in which a
/// game reusing its command buffer tears a submission that does not wait (BB_SUBMIT_ASYNC=1).
inline long DelayUs() {
    static const long delay_us = [] {
        const char* env = std::getenv("BB_SUBMIT_DELAY_US");
        return env ? std::strtol(env, nullptr, 10) : 0L;
    }();
    return delay_us;
}

/// BB_SUBMIT_CHECK=1: whether the game's command words changed between its submission and this
/// process taking them (a torn submission). Counted; the first 16 and every 100th reported.
/// `now_ns` is on the same clock as SubmitGfxArgs::submit_ns.
inline void CheckSubmitted(const SubmitGfxArgs& args, std::span<const std::uint32_t> dcb,
                           std::span<const std::uint32_t> ccb, std::uint64_t now_ns) {
    static std::atomic<std::uint64_t> checked{0}, changed{0};
    const std::uint64_t n = checked.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool dcb_changed = !args.inline_dcb && !dcb.empty() &&
                             XXH3_64bits(dcb.data(), dcb.size_bytes()) != args.dcb_hash;
    const bool ccb_changed = !args.inline_ccb && !ccb.empty() &&
                             XXH3_64bits(ccb.data(), ccb.size_bytes()) != args.ccb_hash;
    if (n % 20000 == 0) {
        std::printf("Submit check: %llu of %llu graphics submissions changed before bb-gpu took "
                    "them\n",
                    (unsigned long long)changed.load(), (unsigned long long)n);
    }
    if (!dcb_changed && !ccb_changed) {
        return;
    }
    const std::uint64_t c = changed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c <= 16 || c % 100 == 0) {
        std::printf("Submit check: submission %llu's%s%s (%u+%u dwords at %#llx) changed %llu us "
                    "after the game submitted it, before bb-gpu took it (%llu of %llu)\n",
                    (unsigned long long)n, dcb_changed ? " dcb" : "", ccb_changed ? " ccb" : "",
                    args.dcb_dwords, args.ccb_dwords, (unsigned long long)args.dcb,
                    (unsigned long long)((now_ns - args.submit_ns) / 1000),
                    (unsigned long long)c, (unsigned long long)n);
        std::fflush(stdout);
    }
}

/// bb-gpu's side of MsgSubmitGfx: the diagnostic delay, the check, then the copy - in that order,
/// and the caller replies only after this returns.
template <typename Copy>
inline void HandleSubmission(const SubmitGfxArgs& args, std::span<const std::uint32_t> dcb,
                             std::span<const std::uint32_t> ccb, std::uint64_t now_ns, Copy&& copy) {
    if (const long delay_us = DelayUs(); delay_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
    }
    if (args.submit_ns != 0) {
        CheckSubmitted(args, dcb, ccb, now_ns);
    }
    copy();
}

} // namespace BbRemote::SubmitPath
