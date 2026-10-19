// SPDX-License-Identifier: GPL-2.0-or-later
// The split's graphics-submission path under a guest that reuses its command buffer
// (docs/PM4_TORN_SUBMISSIONS.md, docs/PM4_PREREG_2026-10-19.md, docs/PM4_SYNTHETIC_2026-10-19.md).
//
// What runs here is the port's own code, not a model of it:
//   - BbRemote::Channel (bb_channel.h) between the two sides, with the real message ring, reply
//     slots and wakeups;
//   - BbRemote::SubmitPath (bb_submit_path.h): WaitsForCopy, NoteSubmissionHashes, Send (the
//     wait-or-not branch remote_front.cpp uses), DelayUs, CheckSubmitted and HandleSubmission
//     (bb-gpu's order: delay, check, then the copy);
//   - BbCp::Diagnostics::TakeCopy (cp/cp_diagnostics.h), the same function
//     CommandProcessor::SubmitGfx calls to make the BB_COPY_GPU_BUFFERS copy.
// The 3000-line drivers on both sides are not linked in (remote_front.cpp starts bb-gpu and a
// window; remote_back.cpp is the whole GPU core), so the front's frame loop and the back's
// message dispatch are the few lines written here - see the note in the doc.
//
// The model of the game is the in-process contract: it writes a command buffer, submits it, and
// writes it again as soon as submit returns. The buffer is filled with NOPs whose body differs
// between the two versions, so a copy is always decodable and still shows the tear: every copied
// dword is compared with both versions and a copy that is not exactly the submitted version is
// counted, with the mixed ones (words of both versions) reported separately - the crash dumps'
// signature.
//
//   pm4-submit-test --arm old   --delay 500 --iters 2000     # v8 (BB_SUBMIT_ASYNC=1)
//   pm4-submit-test --arm fixed --delay 500 --iters 20000    # v9 (the fix)
//   pm4-submit-test --arm fixed --delay 0 --iters 2000 --writer   # a game that ignores the contract
//
// Prints one JSON line. One process per arm: the port caches its env decisions per process.
#include <algorithm>
#include <atomic>
#include <new>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "cp/cp_diagnostics.h"
#include "remote/bb_channel.h"
#include "remote/bb_protocol.h"
#include "remote/bb_submit_path.h"

using namespace BbRemote;
using BbCp::Diagnostics::SubmittedCopy;

namespace {

constexpr std::uint32_t NopHeader = 0xC0001000u; // type 3, one body dword, opcode 0x10 (NOP)

std::uint64_t NowNs() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

/// dwords: NOPs, body value `tag | i`.
void Fill(std::span<std::uint32_t> words, std::uint32_t tag) {
    for (std::size_t at = 0; at + 1 < words.size(); at += 2) {
        words[at] = NopHeader;
        words[at + 1] = tag | std::uint32_t(at / 2);
    }
}

struct Arm {
    bool async = false;      ///< BB_SUBMIT_ASYNC=1 (v8)
    long delay_us = 0;       ///< BB_SUBMIT_DELAY_US
    int iters = 1000;
    std::size_t dwords = 4096; ///< the command buffer's size (16 KiB)
    bool writer = false;     ///< a guest that keeps rewriting during submit (contract broken)
};

/// How one copy compares with the two versions of the buffer.
struct Verdict {
    std::uint64_t copies = 0;
    std::uint64_t torn = 0;    ///< not exactly the submitted version
    std::uint64_t mixed = 0;   ///< words of both versions in one copy (the dumps' signature)
    std::uint64_t whole_other = 0; ///< entirely the version written after submit returned
    std::uint64_t first_torn_changed = 0;
    std::uint64_t max_changed = 0;
};

} // namespace

int main(int argc, char** argv) {
    Arm arm;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (arg == "--arm") {
            arm.async = std::string(next()) == "old";
        } else if (arg == "--delay") {
            arm.delay_us = std::strtol(next(), nullptr, 10);
        } else if (arg == "--iters") {
            arm.iters = std::atoi(next());
        } else if (arg == "--dwords") {
            arm.dwords = std::size_t(std::atol(next()));
        } else if (arg == "--writer") {
            arm.writer = true;
        } else {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    // The port reads these once per process, as the game and bb-gpu do.
    setenv("BB_COPY_GPU_BUFFERS", "1", 1);
    setenv("BB_SUBMIT_ASYNC", arm.async ? "1" : "0", 1);
    setenv("BB_SUBMIT_CHECK", "1", 1);
    const std::string delay = std::to_string(arm.delay_us);
    setenv("BB_SUBMIT_DELAY_US", delay.c_str(), 1);

    // The guest's command buffer (one DCB; no CCB), and the two versions of its contents.
    std::vector<std::uint32_t> guest(arm.dwords);
    std::vector<std::uint32_t> version_a(arm.dwords), version_b(arm.dwords);
    Fill(version_a, 0xA5A50000u);
    Fill(version_b, 0xB6B60000u);
    std::memcpy(guest.data(), version_a.data(), arm.dwords * 4);

    // One shared region, both sides on it: the same ring, reply slots and wakeups as the split.
    auto* region = static_cast<std::uint8_t*>(std::aligned_alloc(64, ChannelBytes));
    if (!region) {
        std::fprintf(stderr, "no room for the channel (%zu bytes)\n", ChannelBytes);
        return 3;
    }
    std::memset(region, 0, ChannelBytes);
    Channel::Init(region);
    Channel front(region, Side::Frontend);
    Channel back(region, Side::Backend);
    if (!front.Valid() || !back.Valid()) {
        std::fprintf(stderr, "channel invalid\n");
        return 3;
    }

    Verdict verdict;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> copies{0};

    std::thread backend([&] {
        while (!stop.load(std::memory_order_acquire)) {
            back.ReceiveOne(
                [&](const Message& message, auto&& reply) {
                    if (message.type != MsgSubmitGfx || message.size < sizeof(SubmitGfxArgs)) {
                        return;
                    }
                    SubmitGfxArgs args{};
                    std::memcpy(&args, message.data, sizeof(args));
                    const auto dcb = std::span<const std::uint32_t>(
                        reinterpret_cast<const std::uint32_t*>(std::uintptr_t(args.dcb)),
                        args.dcb_dwords);
                    // bb-gpu's order: the diagnostic delay, the check, then the copy. The reply
                    // goes out after this returns, as remote_back.cpp does.
                    SubmitPath::HandleSubmission(args, dcb, {}, NowNs(), [&] {
                        auto copy = BbCp::Diagnostics::TakeCopy(dcb, {});
                        // The dumps' shape: words of both versions in one copy. Headers are the
                        // same in both versions, so they say nothing either way (neutral).
                        std::uint64_t changed = 0, a_words = 0, b_words = 0, other_words = 0;
                        for (std::size_t at = 0; at < copy->data.size(); ++at) {
                            const std::uint32_t word = copy->data[at];
                            const bool in_a = word == version_a[at];
                            const bool in_b = word == version_b[at];
                            if (in_a && in_b) {
                                continue; // neutral (a NOP header: the same in both versions)
                            }
                            if (in_a) {
                                ++a_words; // the version the game submitted
                                continue;
                            }
                            ++changed; // not the submitted version: a tear
                            if (in_b) {
                                ++b_words;
                            } else {
                                ++other_words;
                            }
                        }
                        ++verdict.copies;
                        if (changed) {
                            ++verdict.torn;
                            if (a_words && b_words) {
                                ++verdict.mixed;
                            } else if (!a_words && !other_words) {
                                ++verdict.whole_other; // entirely the version written after submit
                            }
                            if (verdict.first_torn_changed == 0) {
                                verdict.first_torn_changed = changed;
                            }
                            verdict.max_changed = std::max(verdict.max_changed, changed);
                        }
                        copies.fetch_add(1, std::memory_order_release);
                    });
                    reply(nullptr, 0);
                },
                [&] { return stop.load(std::memory_order_acquire); });
        }
    });

    std::thread writer;
    std::atomic<bool> rewrite{false};
    if (arm.writer) {
        // Not the port's contract: the guest rewrites its buffer while the submission is in
        // flight. Run to show the detector sees a tear in the fixed path too, so a zero there is
        // not a zero without a working check.
        writer = std::thread([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (rewrite.load(std::memory_order_acquire)) {
                    std::memcpy(guest.data(), version_b.data(), arm.dwords * 4);
                }
            }
        });
    }

    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < arm.iters; ++i) {
        std::memcpy(guest.data(), version_a.data(), arm.dwords * 4); // the game builds its frame
        SubmitGfxArgs args{};
        args.dcb = reinterpret_cast<std::uint64_t>(guest.data());
        args.dcb_dwords = std::uint32_t(arm.dwords);
        SubmitPath::NoteSubmissionHashes(args, guest, {}, NowNs());
        rewrite.store(true, std::memory_order_release);
        // remote_front.cpp's path: waits for bb-gpu's copy unless BB_SUBMIT_ASYNC=1.
        SubmitPath::Send(front, &args, std::uint32_t(sizeof(args)), [&] {
            ResultReply taken{};
            front.Call(MsgSubmitGfx, &args, std::uint32_t(sizeof(args)), &taken, sizeof(taken));
        });
        rewrite.store(false, std::memory_order_release);
        // Submit returned: the game may use the buffer again, and does.
        std::memcpy(guest.data(), version_b.data(), arm.dwords * 4);
    }
    // The fixed path has answered every submission by now; the async path may still be behind.
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (copies.load(std::memory_order_acquire) < std::uint64_t(arm.iters) &&
           std::chrono::steady_clock::now() < drain_deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    stop.store(true, std::memory_order_release);
    back.WakeReader();
    backend.join();
    if (writer.joinable()) {
        writer.join();
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::free(region);

    std::printf(
        "{\"arm\":\"%s\",\"protocol\":%d,\"delay_us\":%ld,\"iters\":%d,\"dwords\":%zu,"
        "\"copies\":%llu,\"torn\":%llu,\"mixed\":%llu,\"whole_other\":%llu,"
        "\"torn_per_1000\":%.3f,\"first_torn_changed\":%llu,\"max_torn_dwords\":%llu,"
        "\"writer\":%s,\"secs\":%.2f}\n",
        arm.async ? "old" : "fixed", arm.async ? 8 : 9, arm.delay_us, arm.iters, arm.dwords,
        (unsigned long long)verdict.copies, (unsigned long long)verdict.torn,
        (unsigned long long)verdict.mixed, (unsigned long long)verdict.whole_other,
        1000.0 * double(verdict.torn) / double(arm.iters > 0 ? arm.iters : 1),
        (unsigned long long)verdict.first_torn_changed, (unsigned long long)verdict.max_changed,
        arm.writer ? "true" : "false", secs);
    return verdict.torn ? 1 : 0; // 1: the arm saw a tear (the caller reads the JSON, not the code)
}
