// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_FRAME_TIMELINE=1: one line per frame, from bb-gpu's side of the pipeline, to answer
// "who waits on whom" (docs/FRAME_PIPELINE_TIMELINE.md):
//
//   * when the frame's first submission arrived from the game process (the reader thread),
//   * when the GPU command thread began and finished decoding it (the recording),
//   * when sceGnmSubmitDone for it arrived,
//   * the presenter's FramesAhead wait inside it (the command thread blocked on the GPU),
//   * when the GPU went idle (the pacing lock's release, in the console's lock mode),
//   * when the GPU finished the frame (its work retired; the "honest labels" mode).
//
// A line prints at the frame's retire, or - in the console's pacing mode, where no frame is
// "retired" - when the next frame's sceGnmSubmitDone arrives. Times are ms relative to this
// frame's sceGnmSubmitDone (the guest's own frame boundary): negative is before it.
// `overlap` is the decode start against the moment the GPU finished the previous frame: positive
// = recording began only after the GPU was done with the frame before (serialised), negative =
// recording of this frame ran while the GPU was still working on the previous one.
//
// Off by default; the enable check is a function-local static.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace BbFrameTimeline {

using u64 = std::uint64_t;
using i64 = std::int64_t;

inline bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_FRAME_TIMELINE");
        return env && env[0] == '1';
    }();
    return enabled;
}

namespace Detail {

inline i64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Record {
    u64 frame = 0;
    i64 sub_first = 0;  ///< the frame's first submission arrived (bb-gpu's reader thread)
    i64 dec_begin = 0;  ///< the GPU command thread began decoding it
    i64 dec_end = 0;    ///< ... finished its last submission
    i64 sd = 0;         ///< sceGnmSubmitDone for it arrived
    i64 wait_begin = 0; ///< the presenter's FramesAhead wait (command thread)
    i64 wait_end = 0;
    i64 gpu_idle = 0; ///< the GPU went idle during this frame (pacing lock released)
    i64 retire = 0;   ///< the GPU finished the frame
    u64 subs = 0;
    bool printed = false;
};

inline constexpr std::uint32_t Slots = 8;

struct State {
    std::mutex mutex;
    Record records[Slots];
    i64 last_retire = 0; ///< when the GPU finished the previous frame (the overlap baseline)
    i64 prev_sd = 0;     ///< the previous printed frame's sceGnmSubmitDone (its period)
    u64 current = 1;     ///< the frame the guest is filling
};

inline State& Get() {
    static State state;
    return state;
}

inline u64 CurrentFrame() {
    return Get().current;
}

/// The slot for `frame` (under the mutex): reused for a new frame.
inline Record& Slot(State& state, u64 frame) {
    Record& slot = state.records[frame % Slots];
    if (slot.frame != frame) {
        slot = Record{};
        slot.frame = frame;
    }
    return slot;
}

/// One line; the frame's sceGnmSubmitDone is the zero.
inline void Print(State& state, Record& record) {
    const i64 anchor = record.sd;
    const auto at = [&](i64 value) {
        return anchor && value ? double(value - anchor) / 1e6 : 0.0;
    };
    const auto span = [&](i64 begin, i64 end) {
        return begin && end ? double(end - begin) / 1e6 : 0.0;
    };
    char overlap_text[32] = "n/a";
    if (state.last_retire && record.dec_begin) {
        std::snprintf(overlap_text, sizeof(overlap_text), "%+.2f",
                      double(record.dec_begin - state.last_retire) / 1e6);
    }
    char retire_text[32] = "n/a";
    if (record.retire) {
        std::snprintf(retire_text, sizeof(retire_text), "%+.2f", at(record.retire));
    }
    std::printf("Frame timeline: %llu nsub %llu | frame %+.2f ms | first_submit %+.2f, decode "
                "%+.2f..%+.2f (%.2f), present_wait %+.2f (%.2f), gpu_idle %+.2f, retire %s | "
                "overlap %s\n",
                (unsigned long long)record.frame, (unsigned long long)record.subs,
                anchor && state.prev_sd ? double(anchor - state.prev_sd) / 1e6 : 0.0,
                at(record.sub_first), at(record.dec_begin), at(record.dec_end),
                span(record.dec_begin, record.dec_end), at(record.wait_begin),
                span(record.wait_begin, record.wait_end), at(record.gpu_idle), retire_text,
                overlap_text);
    std::fflush(stdout);
    record.printed = true;
    if (anchor) {
        state.prev_sd = anchor;
    }
}

/// Prints `frame` if it is known and no retire has printed it (the console-lock pacing mode).
inline void PrintIfPending(State& state, u64 frame) {
    if (frame == 0) {
        return;
    }
    Record& record = state.records[frame % Slots];
    if (record.frame == frame && !record.printed && record.sd) {
        Print(state, record);
    }
}

} // namespace Detail

/// The frame the guest is filling (advanced by sceGnmSubmitDone).
inline u64 CurrentFrame() {
    return Detail::CurrentFrame();
}

/// The frame's first submission reached bb-gpu (later submissions of it are counted only).
inline void NoteSubmit(u64 frame) {
    if (!Enabled()) {
        return;
    }
    if (frame == 0) {
        frame = Detail::CurrentFrame();
    }
    auto& state = Detail::Get();
    std::scoped_lock lock{state.mutex};
    auto& record = Detail::Slot(state, frame);
    if (record.sub_first == 0) {
        record.sub_first = Detail::Now();
    }
    ++record.subs;
}

inline void NoteDecodeBegin(u64 frame) {
    if (!Enabled()) {
        return;
    }
    if (frame == 0) {
        frame = Detail::CurrentFrame();
    }
    auto& state = Detail::Get();
    std::scoped_lock lock{state.mutex};
    auto& record = Detail::Slot(state, frame);
    if (record.dec_begin == 0) {
        record.dec_begin = Detail::Now();
    }
}

inline void NoteDecodeEnd(u64 frame) {
    if (!Enabled()) {
        return;
    }
    if (frame == 0) {
        frame = Detail::CurrentFrame();
    }
    auto& state = Detail::Get();
    std::scoped_lock lock{state.mutex};
    Detail::Slot(state, frame).dec_end = Detail::Now();
}

/// sceGnmSubmitDone for the frame arrived: its line prints now if this mode never retires frames.
inline void NoteSubmitDone(u64 frame) {
    if (!Enabled() || frame == 0) {
        return;
    }
    auto& state = Detail::Get();
    std::scoped_lock lock{state.mutex};
    auto& record = Detail::Slot(state, frame);
    record.sd = Detail::Now();
    Detail::PrintIfPending(state, frame - 1); // the frame before: nothing retired it
    state.current = frame + 1;
}

inline void NotePresentWaitBegin(u64 frame) {
    if (!Enabled()) {
        return;
    }
    if (frame == 0) {
        frame = Detail::CurrentFrame();
    }
    auto& state = Detail::Get();
    std::scoped_lock lock{state.mutex};
    auto& record = Detail::Slot(state, frame);
    if (record.wait_begin == 0) {
        record.wait_begin = Detail::Now();
    }
}

inline void NotePresentWaitEnd(u64 frame) {
    if (!Enabled()) {
        return;
    }
    if (frame == 0) {
        frame = Detail::CurrentFrame();
    }
    auto& state = Detail::Get();
    std::scoped_lock lock{state.mutex};
    auto& record = Detail::Slot(state, frame);
    if (record.wait_end == 0) {
        record.wait_end = Detail::Now();
    }
}

/// The GPU went idle (the console pacing lock's release: the guest may submit again).
inline void NoteGpuIdle(u64 frame) {
    if (!Enabled()) {
        return;
    }
    if (frame == 0) {
        frame = Detail::CurrentFrame();
    }
    auto& state = Detail::Get();
    std::scoped_lock lock{state.mutex};
    auto& record = Detail::Slot(state, frame);
    if (record.gpu_idle == 0) {
        record.gpu_idle = Detail::Now();
    }
}

/// The GPU finished the frame (its submissions retired): print it and drop it.
inline void NoteRetire(u64 frame) {
    if (!Enabled() || frame == 0) {
        return;
    }
    auto& state = Detail::Get();
    std::scoped_lock lock{state.mutex};
    auto& record = state.records[frame % Detail::Slots];
    if (record.frame != frame) {
        return; // never seen (the first frames after start, or a frame the game skipped)
    }
    record.retire = Detail::Now();
    state.last_retire = record.retire;
    if (!record.printed) {
        Detail::Print(state, record);
        record = Detail::Record{};
    }
}

} // namespace BbFrameTimeline
