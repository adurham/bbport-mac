// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the control block shared by the game process and the native GPU process
// (docs/macos-native-gpu.md). It is part of the guest memory pool (its host span) and mapped at
// the same fixed address in both, just above the guest's range, so pointers into it are valid on
// both sides: the VideoOut buffer labels live here, and the guest's GPU commands write them.
#pragma once

#include <atomic>
#include <cstdint>

#include "remote/bb_channel.h"

namespace BbRemote {

inline constexpr std::uint64_t GuestBegin = 0x7000000000ull; ///< runtime_memory.c USER_MIN (macOS)
inline constexpr std::uint64_t GuestEnd = 0xfc00000000ull;   ///< USER_MAX
inline constexpr std::uint64_t LowBegin = 0x0800000000ull;   ///< the low region (game image)
inline constexpr std::uint64_t LowEnd = 0x0fc0000000ull;     ///< platform.h BB_LOW_MAX (macOS)

inline constexpr std::uint64_t ControlAddress = GuestEnd;
inline constexpr std::uint64_t ControlBytes = 8ull << 20;
/// Pool bytes after direct and flexible memory for the control block and shared image data.
inline constexpr std::uint64_t HostSpanBytes = 512ull << 20;

/// State both processes read and write directly (no messages).
struct SharedState {
    alignas(64) std::uint64_t vo_labels[16]; ///< BbVideoOut::Port::buffer_labels (guest-visible)
    alignas(64) std::atomic<std::uint32_t> overlay_captures_input{0}; ///< written by bb-gpu
    std::atomic<std::uint32_t> gpu_ready{0};
    /// The game window's keyboard (SDL scancodes, 1 while held), written by bb-gpu's window loop.
    alignas(64) std::uint8_t keyboard[512];
    /// Mouse look (bbport 0.51): written by bb-gpu's window loop, taken by the game process's pad
    /// (bbgpu_mouse_*). Motion in 1/1024 pixels and wheel notches pile up until taken.
    alignas(64) std::atomic<std::int64_t> mouse_dx{0}, mouse_dy{0};
    std::atomic<std::uint32_t> wheel_up{0}, wheel_down{0};
    std::atomic<std::uint32_t> mouse_captured{0}, mouse_buttons{0};
    /// The game's mouse_look setting for bb-gpu's window: 0 not said yet, 1 off, 2 on.
    std::atomic<std::uint32_t> mouse_look{0};
    /// Requests from the game process's pad to capture the mouse (bbgpu_mouse_capture), counted:
    /// bb-gpu's window loop takes them.
    std::atomic<std::uint32_t> mouse_capture_requests{0};
    /// The button prompts (bbport 0.6): runtime_pad.c's prompt state, published by the game
    /// process once a frame for bb-gpu's texture uploads (bbport_button_prompts.cpp).
    alignas(64) std::atomic<std::uint32_t> prompt_generation{0};
    std::atomic<std::int32_t> prompt_device{0};
    std::atomic<std::int32_t> pad_mouse_look{0};
    /// The Toy Pad's lights (runtime_toypad_lights), for bb-gpu's settings menu.
    std::atomic<std::uint32_t> toypad_lights[3]{};
    /// Frame phases of the game process (BB_FRAME_STATS), running totals for bb-gpu's frame log
    /// (frame_stats.cpp): its fault round trips to bb-gpu (page tracking: a guest thread stands
    /// still for each), its other calls, and, published once a frame (SubmitDone), its file reads
    /// (asset streaming), and its CPU time and page-ins (Rosetta translating new code shows here).
    alignas(64) std::atomic<std::uint64_t> game_fault_ns{0}, game_faults{0}, game_call_ns{0},
        game_calls{0};
    alignas(64) std::atomic<std::uint64_t> game_file_reads{0}, game_file_bytes{0},
        game_file_ns{0}, game_cpu_us{0}, game_majflt{0};
};

struct ControlBlock {
    ChannelLayout channel;
    /// Page protection only (MsgProtect, bb-gpu -> game process), served by a thread of its own:
    /// it needs nothing but the runtime's lock, so it is never stuck behind a VideoOut or event
    /// queue lock, and protections apply in the order bb-gpu decides them.
    ChannelLayout protect;
    alignas(4096) SharedState state;
};
static_assert(sizeof(ControlBlock) <= ControlBytes);
static_assert(ControlBytes % 16384 == 0 && HostSpanBytes % 16384 == 0);

} // namespace BbRemote
