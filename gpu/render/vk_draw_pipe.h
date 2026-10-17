// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: two-stage draw pipeline (docs/parallel_gpu.md, "Two-stage draw pipeline").
//
// The GPU command thread (stage A) decodes PM4, keeps the register file and selects pipelines;
// for a direct draw it writes a packet (the register blocks changed since the previous packet,
// the stages' user data, the draw parameters) into this ring and goes on decoding. The draw
// recording thread (stage B) owns the caches, the barriers and the command recording while
// packets are in flight: it applies each packet to its own copy of the registers and runs the
// rest of the draw. Anything else stage A does on that state waits for stage B to run dry
// (Drain) and then runs on stage A as before.

#pragma once

#include <cstdlib>
#include "bbport_threads.h"
#include "bbport_sections.h"
#include "bbport_timeline.h"
#include "bbport_toggles.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include "bbport_cpu.h"

#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class DrawPipe {
public:
    /// Runs a packet on stage B.
    using Handler = void (*)(void* context, const u8* packet, u32 size);

    DrawPipe(Handler handler_, void* context_) : handler{handler_}, context{context_} {
        ring = std::make_unique<u8[]>(Capacity);
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~DrawPipe() {
        Drain();
        thread.request_stop();
        published.fetch_add(0, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// True on the draw recording thread.
    [[nodiscard]] static bool OnStageB() noexcept {
        return on_stage_b;
    }

    [[nodiscard]] u32 StageBThreadId() const noexcept {
        return stage_b_tid.load(std::memory_order_acquire);
    }

    /// Stage A: space for a packet of `size` bytes (8-byte aligned), valid until Commit().
    u8* Begin(u32 size) {
        size = Align(size + sizeof(Header));
        ASSERT(size <= Capacity / 4);
        u64 at = head;
        const u64 offset = at % Capacity;
        if (offset + size > Capacity) {
            // Wrap: the rest of the ring is skipped (a header with zero size marks it).
            WaitForSpace(at, Capacity - offset);
            reinterpret_cast<Header*>(ring.get() + offset)->size = 0;
            at += Capacity - offset;
            head = at;
            published.store(at, std::memory_order_release);
        }
        WaitForSpace(at, size);
        pending_size = size;
        auto* header = reinterpret_cast<Header*>(ring.get() + at % Capacity);
        header->size = size;
        return reinterpret_cast<u8*>(header + 1);
    }

    /// Stage A: hands the packet from Begin() to stage B.
    void Commit(u32 payload_size) {
        reinterpret_cast<Header*>(ring.get() + head % Capacity)->payload = payload_size;
        head += pending_size;
        published.store(head, std::memory_order_seq_cst);
        ++packets;
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    /// Why stage A drained (statistics): a PM4 opcode, or one of these.
    enum Reason : u32 {
        ReasonConstRam = 256,
        ReasonCommands,
        ReasonCompute,
        ReasonSubmissionEnd,
        ReasonDraw,
        ReasonRasterizer,
        NumReasons,
    };
    std::array<u64, NumReasons> drains_by_reason{};
    std::array<u64, NumReasons> cycles_by_reason{};

    /// Stage A: waits until stage B has run every committed packet.
    void Drain(u32 reason = ReasonRasterizer) {
        if (consumed.load(std::memory_order_acquire) == head) {
            return;
        }
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = BbCpu::Ticks();
        const u32 spins_done =
            WaitUntil(head, 0, [this] { return consumed.load(std::memory_order_seq_cst) == head; });
        const u64 waited = BbCpu::Ticks() - start;
        drain_cycles += waited;
        cycles_by_reason[slot] += waited;
        NoteWait(waited, spins_done);
    }

    /// Stage A: the number the next committed packet gets (packets are numbered from 1), and
    /// whether stage B has run the packet with that number.
    [[nodiscard]] u64 NextPacket() const noexcept {
        return packets + 1;
    }
    [[nodiscard]] bool ReachedPacket(u64 number) const noexcept {
        return consumed_packets.load(std::memory_order_acquire) >= number;
    }
    /// Stage A: waits until stage B has run the packet `number` (Drain: all of them).
    void WaitForPacket(u64 number, u32 reason) {
        if (ReachedPacket(number)) {
            return;
        }
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = BbCpu::Ticks();
        const u32 spins_done = WaitUntil(0, number, [this, number] {
            return consumed_packets.load(std::memory_order_seq_cst) >= number;
        });
        const u64 waited = BbCpu::Ticks() - start;
        drain_cycles += waited;
        cycles_by_reason[slot] += waited;
        NoteWait(waited, spins_done);
    }

    /// Stage A: position after the last committed packet; Reached(position) once B ran it.
    [[nodiscard]] u64 Head() const noexcept {
        return head;
    }
    [[nodiscard]] bool Reached(u64 position) const noexcept {
        return consumed.load(std::memory_order_acquire) >= position;
    }

    [[nodiscard]] bool Idle() const noexcept {
        return consumed.load(std::memory_order_acquire) == head;
    }
    /// Stage B: the position of the packet it runs (packets before it have run).
    [[nodiscard]] u64 Consumed() const noexcept {
        return consumed.load(std::memory_order_acquire);
    }
    /// Stage B: bytes of packets committed and not run yet (the work queued for this thread).
    [[nodiscard]] u64 Backlog() const noexcept {
        return published.load(std::memory_order_acquire) - consumed.load(std::memory_order_relaxed);
    }

    /// Statistics (stage A): packets, drains that had to wait, cycles waited, cycles stage B
    /// spent running packets.
    u64 packets = 0, drains = 0, drain_cycles = 0;
    std::atomic<u64> busy_cycles{0};

private:
    /// The frame probe (BB_FRAME_LOG ga_drain_ms/ga_drains/ga_yields): a drain that waited, and
    /// how often it went past its spin (a sleep, or with BB_PIPE_DRAIN_YIELD=1 a sched_yield each).
    static void NoteWait(u64 waited, u32 slow) {
        if (!BbStats::enabled) {
            return;
        }
        BbStats::ga_drain_ticks.fetch_add(waited, std::memory_order_relaxed);
        BbStats::ga_drains.fetch_add(1, std::memory_order_relaxed);
        BbStats::ga_yields.fetch_add(slow, std::memory_order_relaxed);
    }

    /// Stage A: spins briefly for `done`, then sleeps until stage B has run another packet
    /// (DrainWake). Returns the passes past the spin (sleeps, or yields). Stage A waits here for
    /// stage B at every guest submission's end (Rasterizer::OnSubmit), ~14 ms a frame in gameplay:
    /// the sched_yield loop this replaces made ~74,000 system calls a frame there - all of the GPU
    /// command thread's ~10 ms/frame of system time (the frame log's kernel_ms), a P-core held at
    /// 100% doing nothing. BB_PIPE_DRAIN_YIELD=1 brings the yield loop back (A/B).
    /// Stage B wakes this thread only once it reached `position` (a ring position) or ran packet
    /// `packet` (0: not that kind of wait): one wake a wait, not one a packet - stage B is the
    /// frame's critical path, a futex wake per packet would cost it ~1 ms a frame.
    template <typename Done>
    u32 WaitUntil(u64 position, u64 packet, Done done) {
        static const bool yield_loop = [] {
            const char* env = std::getenv("BB_PIPE_DRAIN_YIELD");
            return env && env[0] == '1';
        }();
        for (u32 spins = 0; spins < 4096; ++spins) {
            if (done()) {
                return 0;
            }
            BbCpu::Pause();
        }
        u32 slow = 0;
        while (!done()) {
            ++slow;
            if (yield_loop) {
                std::this_thread::yield();
                continue;
            }
            // seq_cst on both sides: stage B stores `consumed` and then reads `drain_waiting`;
            // this thread stores `drain_waiting` and then reads `consumed` (done()). One of them
            // sees the other's store: B wakes this thread, or this thread sees the packet ran.
            const u32 seen = drain_wake.load(std::memory_order_seq_cst);
            wake_position.store(position, std::memory_order_relaxed);
            wake_packet.store(packet, std::memory_order_relaxed);
            drain_waiting.store(true, std::memory_order_seq_cst);
            if (!done()) {
                drain_wake.wait(seen, std::memory_order_seq_cst);
            }
            drain_waiting.store(false, std::memory_order_seq_cst);
        }
        return slow;
    }

    /// Stage B: after `consumed` moved to `at` (and `consumed_packets` to `ran`), wakes stage A if
    /// it sleeps in WaitUntil for this point. The targets are written before `drain_waiting`
    /// (seq_cst), so a `true` read here sees them.
    void DrainWake(u64 at, u64 ran) {
        if (!drain_waiting.load(std::memory_order_seq_cst)) {
            return;
        }
        const u64 position = wake_position.load(std::memory_order_relaxed);
        const u64 packet = wake_packet.load(std::memory_order_relaxed);
        if ((position != 0 && at >= position) || (packet != 0 && ran >= packet) ||
            (position == 0 && packet == 0)) {
            drain_wake.fetch_add(1, std::memory_order_seq_cst);
            drain_wake.notify_one();
        }
    }

    struct Header {
        u32 size;    ///< bytes to the next packet (0: wrap to the ring start)
        u32 payload; ///< bytes written by the producer
    };
    static constexpr u64 Capacity = 16ull << 20;

    static constexpr u32 Align(u32 size) {
        return (size + 63) & ~63u;
    }

    void WaitForSpace(u64 at, u64 size) {
        while (at + size - consumed.load(std::memory_order_acquire) > Capacity) {
            BbCpu::Pause();
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:DrawRec");
        BbStats::record_thread_port.store(BbThreads::CurrentThreadPort(), std::memory_order_release);
        on_stage_b = true;
        BbSections::recording_thread = true;
        stage_b_tid.store(static_cast<u32>(BbThreads::HostTid()), std::memory_order_release);
        u64 at = 0;
        while (true) {
            // Packets follow each other within microseconds while a frame is decoded: spin,
            // and sleep only after a longer pause (between frames).
            u64 available = published.load(std::memory_order_acquire);
            if (available == at) {
                BbTimeline::Note(BbTimeline::RecorderIdleBegin);
                // With few hardware threads (Steam Deck: 8) a long spin takes time from the
                // game's own threads. BB_PIPE_SPIN_US overrides.
                static const auto spin_time = std::chrono::microseconds([] {
                    if (const char* env = std::getenv("BB_PIPE_SPIN_US")) {
                        return std::max(0, std::atoi(env));
                    }
                    return BbThreads::Available() >= 12 ? 200 : 50;
                }());
                const auto spin_until = std::chrono::steady_clock::now() + spin_time;
                for (u32 spins = 1; available == at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    BbCpu::Pause();
                    if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.store(true, std::memory_order_seq_cst);
                        if (published.load(std::memory_order_seq_cst) == at &&
                            !stop.stop_requested()) {
                            wake.wait(seen, std::memory_order_seq_cst);
                        }
                        sleeping.store(false, std::memory_order_relaxed);
                    }
                    available = published.load(std::memory_order_acquire);
                }
                BbTimeline::Note(BbTimeline::RecorderIdleEnd);
            }
            const auto* header = reinterpret_cast<const Header*>(ring.get() + at % Capacity);
            if (header->size == 0) {
                at += Capacity - at % Capacity;
                consumed.store(at, std::memory_order_seq_cst);
                DrainWake(at, consumed_packets.load(std::memory_order_relaxed));
                continue;
            }
            const u64 start = BbCpu::Ticks();
            handler(context, reinterpret_cast<const u8*>(header + 1), header->payload);
            const u64 ran = BbCpu::Ticks() - start;
            busy_cycles.fetch_add(ran, std::memory_order_relaxed);
            if (BbStats::enabled) {
                BbStats::b_busy_ticks.fetch_add(ran, std::memory_order_relaxed);
            }
            at += header->size;
            const u64 packets_run = consumed_packets.fetch_add(1, std::memory_order_seq_cst) + 1;
            consumed.store(at, std::memory_order_seq_cst);
            DrainWake(at, packets_run);
        }
    }

    static inline thread_local bool on_stage_b = false;

    Handler handler;
    void* context;
    std::unique_ptr<u8[]> ring;
    u64 head = 0; ///< stage A's write position
    u32 pending_size = 0;
    alignas(64) std::atomic<u64> published{0};
    alignas(64) std::atomic<u64> consumed{0};
    /// Stage A asleep in WaitUntil, and the word it sleeps on (DrainWake).
    alignas(64) std::atomic<bool> drain_waiting{false};
    std::atomic<u32> drain_wake{0};
    std::atomic<u64> wake_position{0}, wake_packet{0}; ///< what stage A waits for (DrainWake)
    std::atomic<u64> consumed_packets{0}; ///< packets stage B has run (NextPacket numbers)
    alignas(64) std::atomic<u32> wake{0};
    std::atomic<bool> sleeping{false};
    std::atomic<u32> stage_b_tid{0};
    std::jthread thread;
};

} // namespace Vulkan
